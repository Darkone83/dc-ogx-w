#!/usr/bin/env python3
"""DC-OGX-W remote: PySide6 desktop, web bridge, CLI and asyncio API.

Direct launches detect missing dependencies and install into a local virtual
environment. Importing this module never installs packages. +Y is up.
"""
from __future__ import annotations

import argparse
import asyncio
import contextlib
import dataclasses
import json
import math
import os
from pathlib import Path
import sys
import time
from typing import Any
from urllib.parse import urlsplit
import webbrowser

# Install missing desktop dependencies only for an explicit program launch.
# Module consumers (and the test suite) are never subject to package installs.
if __name__ == "__main__":
    from ogx_bootstrap import ensure_and_exec
    ensure_and_exec(sys.argv[1:])

try:
    import aiohttp
    from aiohttp import web
except ImportError:
    raise SystemExit("Install the dependencies first: python -m pip install -r requirements.txt")


BUTTONS = {"a": 1, "b": 2, "x": 4, "y": 8, "l3": 16, "r3": 32,
           "back": 64, "start": 128, "lb": 256, "rb": 512,
           "guide": 1024, "misc": 2048}
DPAD = {"up": 1, "down": 2, "left": 4, "right": 8}
API_PATHS = {"/api/status", "/api/network", "/api/network/config", "/api/network/retry",
             "/api/network/scan", "/api/network/mdns/retry",
             "/api/profile", "/api/profile/save", "/api/profiles", "/api/profile/select",
             "/api/led", "/api/led/apply", "/api/led/save", "/api/led/test",
             "/api/control/acquire", "/api/control/release", "/api/control/heartbeat",
             "/api/control/mode", "/api/calibration",
             "/api/update", "/api/update/begin", "/api/update/chunk",
             "/api/update/finish", "/api/update/install", "/api/update/cancel"}
DEFAULT_HOST = "192.168.4.1"
DEFAULT_RATE = 60
RECONNECT_DELAYS = (.5, 1, 2, 4, 8, 8, 8, 8)


class RemoteError(RuntimeError):
    """A rejected request or a broken controller connection."""

    def __init__(self, message: str, *, code: str | None = None, status: int | None = None):
        super().__init__(message)
        self.code = code
        self.status = status


def device_url(host: str) -> str:
    value = host.strip()
    if "://" not in value:
        value = "http://" + value
    parsed = urlsplit(value)
    if (parsed.scheme not in ("http", "https") or not parsed.hostname
            or parsed.username or parsed.password or parsed.query or parsed.fragment
            or parsed.path not in ("", "/")):
        raise ValueError("Use a device address such as 192.168.4.1 or http://192.168.4.1:80")
    return value.rstrip("/")


@dataclasses.dataclass(frozen=True)
class PadState:
    """Unmapped incoming controls. All fields are sent every time."""

    buttons: int = 0
    dpad: int = 0
    lx: int = 0
    ly: int = 0
    rx: int = 0
    ry: int = 0
    lt: int = 0
    rt: int = 0

    def __post_init__(self) -> None:
        bounds = {"buttons": (0, 4095), "dpad": (0, 15),
                  "lx": (-32768, 32767), "ly": (-32768, 32767),
                  "rx": (-32768, 32767), "ry": (-32768, 32767),
                  "lt": (0, 255), "rt": (0, 255)}
        for name, (low, high) in bounds.items():
            value = getattr(self, name)
            if type(value) is not int or not low <= value <= high:
                raise ValueError(f"{name} must be an integer from {low} to {high}")
        if self.dpad & 3 == 3 or self.dpad & 12 == 12:
            raise ValueError("Opposite D-pad directions cannot be pressed together")

    @classmethod
    def pressed(cls, *names: str, **axes: int) -> PadState:
        values = dict(axes)
        buttons = values.pop("buttons", 0)
        dpad = values.pop("dpad", 0)
        for raw in names:
            name = raw.lower()
            if name in BUTTONS:
                buttons |= BUTTONS[name]
            elif name in DPAD:
                dpad |= DPAD[name]
            elif name in ("lt", "rt"):
                values[name] = 255
            else:
                raise ValueError(f"Unknown button: {raw}")
        return cls(buttons=buttons, dpad=dpad, **values)


class RemoteClient:
    """Async client. Use ``async with`` to guarantee best-effort release.

    ``send`` sends one state. ``hold`` refreshes a state for a duration. An
    application that uses ``send`` directly must refresh its snapshot regularly
    (normally 60 Hz, or 30 Hz on the ESP8285 fallback UART rate); the adapter
    neutralizes stale input after its timeout. send() also paces calls to the
    board's advertised maximum so a tight application loop cannot flood it.
    New firmware also reserves ownership with a background heartbeat, without
    refreshing any held button. After send() stops, output still goes neutral
    after 250 ms; a later fresh send can resume without reacquiring the token.
    Set ``reconnect=True`` only for live producers: on Wi-Fi-only boards a
    broken connection gets up to eight neutral reconnect attempts. ``send``
    drops snapshots while reconnecting and requires a fresh resting state
    before enabling input again. Scripts/sequences keep this option off by
    default. Release always cancels recovery.
    """

    def __init__(self, host: str = DEFAULT_HOST, *, timeout: float = 4.0, reconnect: bool = False):
        self.url = device_url(host)
        self.timeout = timeout
        self.session: aiohttp.ClientSession | None = None
        self.socket: aiohttp.ClientWebSocketResponse | None = None
        self.token: int | None = None
        self.timeout_ms = 250
        self.owner_timeout_ms = 0
        self.input_hz = DEFAULT_RATE
        self.sequence = 0
        self._last_send_at = 0.0
        self._last_owner_send_at = 0.0
        self._last_ack_at = 0.0
        self._send_lock = asyncio.Lock()
        self.last_ack: dict[str, Any] | None = None
        self._reader: asyncio.Task | None = None
        self._heartbeat: asyncio.Task | None = None
        self._connection_error: str | None = None
        self._closing = False
        self._request_lock = asyncio.Lock()
        self._reconnect_enabled = reconnect
        self._control_requested = False
        self._intent_generation = 0
        self._acquiring = False
        self._reconnect_task: asyncio.Task | None = None
        self._wifi_only = False
        self._input_mode = "wifi"
        self._needs_neutral = False
        self._client_name = "python"
        self._ping_supported = False
        self._last_ping_at = 0.0

    async def __aenter__(self) -> RemoteClient:
        self.session = aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=self.timeout), trust_env=False)
        return self

    async def __aexit__(self, *_exc) -> None:
        await self.close()

    def _session(self) -> aiohttp.ClientSession:
        if self.session is None or self.session.closed:
            raise RemoteError("Open the client with 'async with RemoteClient(...)'")
        return self.session

    @property
    def control_requested(self) -> bool:
        return self._control_requested

    @property
    def reconnecting(self) -> bool:
        return self._reconnect_task is not None and not self._reconnect_task.done()

    @property
    def needs_neutral(self) -> bool:
        return self._needs_neutral

    async def request(self, path: str, body: dict | None = None) -> dict:
        if path not in API_PATHS:
            raise ValueError("Unsupported API endpoint")
        # One HTTP operation at a time; input and health use the independent
        # WebSocket. Repeat only an explicit pre-accept service_busy rejection.
        async with self._request_lock:
            for attempt in range(4):
                try:
                    async with self._session().request(
                        "GET" if body is None else "POST", self.url + path, json=body
                    ) as response:
                        try:
                            data = await response.json(content_type=None)
                        except (ValueError, aiohttp.ContentTypeError):
                            raise RemoteError(f"Unreadable response from {path}") from None
                        if not isinstance(data, dict):
                            raise RemoteError(f"Unexpected response from {path}")
                        if response.status >= 400 or data.get("ok") is False:
                            message = ("Enter Configuration mode first: POST /api/control/mode with mode=configure, "
                                       "then wait until mode_pending is false." if data.get("error") == "configuration_mode_required"
                                       else "Select Wi-Fi mode first: POST /api/control/mode with mode=wifi, then wait until mode_pending is false."
                                       if data.get("error") == "wifi_mode_required"
                                       else str(data.get("error") or data.get("message") or f"Request failed ({response.status})"))
                            error = RemoteError(message,
                                                code=data.get("error"), status=response.status)
                            if response.status == 503 and data.get("error") == "service_busy" and attempt < 3:
                                await asyncio.sleep(.1 * (attempt + 1))
                                continue
                            raise error
                        return data
                except (aiohttp.ClientError, asyncio.TimeoutError) as error:
                    raise RemoteError(f"Cannot reach {self.url}: {error or 'request timed out'}") from error
        raise RemoteError("The adapter remained busy")

    async def status(self) -> dict:
        status = await self.request("/api/status")
        network = status.get("network", {})
        rate = network.get("input_hz") if isinstance(network, dict) else None
        self._wifi_only = isinstance(network, dict) and network.get("bluetooth_available") is False
        self._input_mode = status.get("control", {}).get("mode", "wifi")
        if type(rate) is int and 10 <= rate <= 125:
            self.input_hz = rate
        return status

    async def profile(self) -> dict:
        return await self.request("/api/profile")

    async def apply_profile(self, profile: dict, *, revision: int | None = None) -> dict:
        if revision is None:
            revision = (await self.profile())["revision"]
        if type(revision) is not int or revision < 0:
            raise ValueError("A valid profile revision is required")
        return await self.request("/api/profile", {"profile": profile, "revision": revision})

    async def save_profile(self, revision: int) -> dict:
        if type(revision) is not int or revision < 0:
            raise ValueError("A valid profile revision is required")
        return await self.request("/api/profile/save", {"revision": revision})

    async def acquire(self, name: str = "python", *, _resume: bool = False) -> dict:
        if self.token is not None or self._acquiring:
            raise RemoteError("This client already has control or an acquisition pending")
        if self.reconnecting and not _resume:
            raise RemoteError("A reconnect is already pending; release it before acquiring again")
        self._acquiring = True
        if not _resume:
            self._intent_generation += 1
            self._control_requested = True
        generation = self._intent_generation
        acquire_sent = False
        granted_token = None

        def check_intent():
            if generation != self._intent_generation or not self._control_requested:
                raise RemoteError("Control request cancelled", code="acquire_cancelled")

        try:
            check_intent()
            await self._close_socket()
            check_intent()
            self._closing = False
            self._connection_error = None
            self._client_name = name
            # Each await may allow Release to cancel this acquisition. Never
            # open or retain a late stream/lease after that user intent changed.
            status = await self.status()
            check_intent()
            if _resume and (not self._wifi_only or self._input_mode != "wifi"
                            or status.get("control", {}).get("mode_pending")
                            or status.get("control", {}).get("active")):
                raise RemoteError("Automatic reconnect stopped: input mode or owner changed", code="reconnect_stopped")
            self.socket = await self._session().ws_connect(
                self.url + "/ws", max_msg_size=8192, compress=0,
                autoping=True, heartbeat=None,
                timeout=aiohttp.ClientWSTimeout(ws_close=1))
            check_intent()
            self._reader = asyncio.create_task(self._read_replies())
            acquire_sent = True
            result = await self.request("/api/control/acquire", {"client": name})
            token = result.get("token")
            if type(token) is not int or not 0 < token <= 0xFFFFFFFF:
                raise RemoteError("The adapter did not grant a valid control token")
            # Capture a confirmed late grant before the cancellation check so
            # cleanup can release exactly this token, even if Release just ran.
            granted_token = token
            self.token = token
            check_intent()
            self.timeout_ms = int(result.get("timeout_ms", 250))
            advertised_owner = result.get("owner_timeout_ms")
            self.owner_timeout_ms = (advertised_owner if type(advertised_owner) is int
                                     and 1000 <= advertised_owner <= 30000 else 0)
            self._ping_supported = result.get("ping_supported") is True
            self._last_ping_at = 0.0
            self._needs_neutral = _resume
            self.sequence = 0
            self._last_send_at = self._last_owner_send_at = 0.0
            self._last_ack_at = time.monotonic()
            # Initial neutral is part of acquisition, not a discardable live
            # producer snapshot. A write failure must reach cleanup/recovery.
            await self._send_state(PadState(), strict=True)
            check_intent()
            if self._connection_error:
                raise RemoteError(self._connection_error, code="control_connection_failed")
            if self.owner_timeout_ms or self._ping_supported:
                self._heartbeat = asyncio.create_task(self._keep_owner())
            return result
        except Exception as error:
            known_neutral_failure = (_resume and granted_token is not None and
                                     isinstance(error, RemoteError) and error.code == "control_connection_failed")
            explicit_setup_rejection = isinstance(error, RemoteError) and error.code == "network_initializing"
            # A lost acquire reply is ambiguous. A confirmed grant followed by
            # a failed neutral is different: release it before bounded recovery.
            if generation == self._intent_generation and (
                    not _resume or (acquire_sent and not known_neutral_failure and not explicit_setup_rejection)):
                self._control_requested = False
            if granted_token is not None:
                with contextlib.suppress(Exception):
                    await self.request("/api/control/release", {"token": granted_token})
            self.token = None
            await self._close_socket()
            raise
        finally:
            self._acquiring = False

    def _lost_connection(self, message: str, *, recoverable: bool = True) -> None:
        self._connection_error = message
        if (recoverable and not self._closing and not self._acquiring and self._reconnect_enabled and
                self._control_requested and self._wifi_only and self._input_mode == "wifi"):
            if not self.reconnecting:
                self._reconnect_task = asyncio.create_task(self._recover())
        elif not recoverable:
            self._control_requested = False

    async def _recover(self) -> None:
        old_token = self.token
        self.token = None
        self._needs_neutral = True
        await self._close_socket()
        if old_token is not None:
            with contextlib.suppress(RemoteError, OSError):
                await self.request("/api/control/release", {"token": old_token})
        for delay in RECONNECT_DELAYS:
            if not self._control_requested:
                return
            await asyncio.sleep(delay)
            if not self._control_requested:
                return
            try:
                await self.acquire(self._client_name, _resume=True)
                return
            except RemoteError as error:
                self._connection_error = str(error)
                if (not self._control_requested or error.status == 409 or
                        error.code in ("reconnect_stopped", "not_control_owner", "wifi_mode_required")):
                    break
            except (aiohttp.ClientError, asyncio.TimeoutError, OSError) as error:
                self._connection_error = str(error)
        self._control_requested = False

    async def _read_replies(self) -> None:
        socket = self.socket
        assert socket is not None
        try:
            async for reply in socket:
                if reply.type == aiohttp.WSMsgType.TEXT:
                    try:
                        data = json.loads(reply.data)
                    except ValueError:
                        self._lost_connection("Unreadable control reply", recoverable=False)
                        break
                    if not isinstance(data, dict):
                        self._lost_connection("Unexpected control reply", recoverable=False)
                        break
                    self.last_ack = data
                    self._last_ack_at = time.monotonic()
                    if data.get("ok") is False:
                        if data.get("error") == "service_busy":
                            continue
                        self._lost_connection(str(data.get("error") or "Control rejected"), recoverable=False)
                        break
                elif reply.type == aiohttp.WSMsgType.ERROR:
                    self._lost_connection("Control connection failed")
                    break
        except (aiohttp.ClientError, OSError) as error:
            self._lost_connection(str(error))
        finally:
            if not self._closing and self._connection_error is None:
                self._lost_connection("Control connection closed")

    async def _keep_owner(self) -> None:
        # Ping checks the transport without refreshing ownership or input.
        # A heartbeat extends only the owner lease, never a held input sample.
        interval = min(.5, self.owner_timeout_ms / 3000) if self.owner_timeout_ms else .5
        try:
            while True:
                await asyncio.sleep(interval)
                if self._connection_error or self.token is None or self.socket is None or self.socket.closed:
                    return
                now = time.monotonic()
                if now - self._last_ack_at > max(6, self.owner_timeout_ms / 1000 + 1):
                    self._lost_connection("The control connection stopped responding")
                    return
                async with self._send_lock:
                    if self.token is None or self.socket is None or self.socket.closed:
                        return
                    if self._ping_supported and now - self._last_ping_at >= 1:
                        await asyncio.wait_for(self.socket.send_json({"type": "ping", "id": int(now * 1000) & 0xFFFFFFFF}), .2)
                        self._last_ping_at = now
                    if self.owner_timeout_ms and now - self._last_owner_send_at >= interval:
                        await asyncio.wait_for(self.socket.send_str(json.dumps(
                            {"type": "heartbeat", "token": self.token}, separators=(",", ":"))), .2)
                        self._last_owner_send_at = time.monotonic()
        except (aiohttp.ClientError, asyncio.TimeoutError, ConnectionError) as error:
            self._lost_connection(f"Control heartbeat failed: {error or 'send timed out'}")

    async def send(self, state: PadState | None = None, **fields: int) -> None:
        if state is not None and fields:
            raise ValueError("Supply a PadState or keyword fields, not both")
        state = state if state is not None else PadState(**fields)
        if not isinstance(state, PadState):
            raise TypeError("state must be a PadState")
        if self.reconnecting:
            # Drop rather than queue producer snapshots during an outage.
            return
        if self._connection_error:
            raise RemoteError(self._connection_error)
        if self._needs_neutral:
            if (state.buttons or state.dpad or state.lt > 8 or state.rt > 8 or
                    any(abs(getattr(state, name)) > 4096 for name in ("lx", "ly", "rx", "ry"))):
                await self._send_state(PadState())
                return
            self._needs_neutral = False
            state = PadState()
        await self._send_state(state)

    async def _send_state(self, state: PadState, *, strict: bool = False) -> None:
        if self.token is None or self.socket is None or self.socket.closed:
            raise RemoteError("Acquire control before sending inputs", code="control_connection_failed")
        async with self._send_lock:
            delay = 1 / self.input_hz - (time.monotonic() - self._last_send_at)
            if delay > 0:
                await asyncio.sleep(delay)
            if self.token is None or self.socket is None or self.socket.closed:
                raise RemoteError("Control was released while waiting to send", code="control_connection_failed")
            payload = {"type": "input", "token": self.token, "seq": self.sequence,
                       **dataclasses.asdict(state)}
            self.sequence = (self.sequence + 1) & 0xFFFFFFFF
            try:
                await asyncio.wait_for(self.socket.send_str(json.dumps(payload, separators=(",", ":"))), .2)
                self._last_send_at = self._last_owner_send_at = time.monotonic()
            except (aiohttp.ClientError, asyncio.TimeoutError, ConnectionError) as error:
                self._lost_connection("The control connection stopped accepting input")
                if strict or not self.reconnecting:
                    raise RemoteError("The control connection stopped accepting input", code="control_connection_failed") from error

    async def hold(self, state: PadState, seconds: float, *, rate: int = DEFAULT_RATE) -> None:
        if not math.isfinite(seconds) or seconds < 0:
            raise ValueError("Duration must be finite and nonnegative")
        if not 10 <= rate <= 125:
            raise ValueError("Update rate must be from 10 to 125 Hz")
        rate = min(rate, self.input_hz)
        deadline = time.monotonic() + seconds
        while True:
            await self.send(state)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            await asyncio.sleep(min(1 / rate, remaining))

    async def tap(self, *buttons: str, seconds: float = 0.1) -> None:
        try:
            await self.hold(PadState.pressed(*buttons), seconds)
        finally:
            if self.token is not None:
                with contextlib.suppress(RemoteError):
                    await self.send(PadState())

    async def play(self, steps: list[dict]) -> None:
        """Run complete-state steps, each with ``duration_ms`` and controls.

        A step can use ``buttons: ["a", "start"]`` or an integer button mask.
        Omitted controls are neutral. The entire sequence is checked first.
        """
        parsed = parse_sequence(steps)
        try:
            for state, seconds in parsed:
                await self.hold(state, seconds)
        finally:
            if self.token is not None:
                with contextlib.suppress(RemoteError):
                    await self.send(PadState())

    async def release(self) -> None:
        self._intent_generation += 1
        self._control_requested = False
        if self.reconnecting:
            self._reconnect_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._reconnect_task
        self._reconnect_task = None
        token = self.token
        if token is not None:
            with contextlib.suppress(RemoteError, OSError):
                await self.send(PadState())
            self.token = None
            with contextlib.suppress(RemoteError, OSError):
                await self.request("/api/control/release", {"token": token})
        await self._close_socket()

    async def _close_socket(self) -> None:
        self._closing = True
        if self._heartbeat is not None:
            self._heartbeat.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._heartbeat
            self._heartbeat = None
        if self._reader is not None:
            self._reader.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._reader
            self._reader = None
        if self.socket is not None:
            with contextlib.suppress(aiohttp.ClientError, OSError):
                await self.socket.close()
            self.socket = None

    async def close(self) -> None:
        await self.release()
        if self.session is not None:
            await self.session.close()
            self.session = None


def parse_sequence(steps: list[dict]) -> list[tuple[PadState, float]]:
    if not isinstance(steps, list) or not 1 <= len(steps) <= 10000:
        raise ValueError("A sequence must contain 1–10,000 steps")
    parsed = []
    for number, step in enumerate(steps, 1):
        if not isinstance(step, dict):
            raise ValueError(f"Step {number} must be an object")
        fields = dict(step)
        duration = fields.pop("duration_ms", None)
        if (isinstance(duration, bool) or not isinstance(duration, (int, float))
                or not math.isfinite(duration) or not 0 <= duration <= 3600000):
            raise ValueError(f"Step {number}: duration_ms must be from 0 to 3,600,000")
        buttons = fields.pop("buttons", 0)
        try:
            state = (PadState.pressed(*buttons, **fields) if isinstance(buttons, list)
                     else PadState(buttons=buttons, **fields))
        except (ValueError, TypeError) as error:
            raise ValueError(f"Step {number}: {error}") from error
        parsed.append((state, duration / 1000))
    return parsed


def web_source() -> Path:
    return Path(__file__).resolve().parents[2] / "src/NetworkService/web/index.html"


def make_gateway(host: str, *, html_path: Path | None = None) -> web.Application:
    """Same-origin localhost UI/API/WebSocket proxy; binds on loopback in CLI."""
    upstream = device_url(host)
    app = web.Application(client_max_size=16384)
    session_key = web.AppKey("upstream_session", aiohttp.ClientSession)
    sockets_key = web.AppKey("browser_sockets", set)
    app[sockets_key] = set()

    async def lifetime(application):
        async with aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=5), trust_env=False
        ) as session:
            application[session_key] = session
            yield

    app.cleanup_ctx.append(lifetime)

    async def shutdown(application):
        await asyncio.gather(*(socket.close(code=1001, message=b"Companion closing")
                               for socket in tuple(application[sockets_key])),
                             return_exceptions=True)

    app.on_shutdown.append(shutdown)

    def same_origin(request):
        origin = request.headers.get("Origin")
        if origin and origin != f"{request.scheme}://{request.host}":
            raise web.HTTPForbidden(text="Open the companion's local page to control the adapter.")

    async def index(request):
        source = html_path or web_source()
        if source.is_file():
            return web.Response(body=source.read_bytes(), content_type="text/html",
                                headers={"Cache-Control": "no-store"})
        try:
            async with request.app[session_key].get(upstream + "/") as response:
                if response.status != 200:
                    raise web.HTTPBadGateway(text="The adapter's page is unavailable.")
                return web.Response(body=await response.read(), content_type="text/html",
                                    headers={"Cache-Control": "no-store"})
        except (aiohttp.ClientError, asyncio.TimeoutError):
            raise web.HTTPBadGateway(text="Connect to the adapter Wi-Fi and reload.") from None

    async def proxy_api(request):
        if request.path not in API_PATHS:
            raise web.HTTPNotFound()
        same_origin(request)
        if request.method not in ("GET", "POST"):
            raise web.HTTPMethodNotAllowed(request.method, ["GET", "POST"])
        body = await request.read() if request.method == "POST" else None
        try:
            async with request.app[session_key].request(
                request.method, upstream + request.path, data=body,
                headers={"Content-Type": "application/json"} if body is not None else {}
            ) as response:
                data = await response.read()
                return web.Response(body=data, status=response.status,
                                    content_type="application/json",
                                    headers={"Cache-Control": "no-store"})
        except (aiohttp.ClientError, asyncio.TimeoutError) as error:
            return web.json_response({"ok": False, "error": f"Adapter unavailable: {error or 'timeout'}"}, status=502)

    async def proxy_socket(request):
        same_origin(request)
        session = request.app[session_key]
        try:
            remote = await session.ws_connect(upstream + "/ws", compress=0,
                                              max_msg_size=8192, heartbeat=None,
                                              timeout=aiohttp.ClientWSTimeout(ws_close=1))
        except (aiohttp.ClientError, asyncio.TimeoutError):
            raise web.HTTPBadGateway(text="Cannot open the adapter control connection.") from None
        browser = web.WebSocketResponse(compress=False, max_msg_size=8192, timeout=1)
        await browser.prepare(request)
        app[sockets_key].add(browser)
        token: int | None = None
        candidate_token: int | None = None

        async def to_device():
            nonlocal candidate_token
            async for packet in browser:
                if packet.type == aiohttp.WSMsgType.TEXT:
                    try:
                        data = json.loads(packet.data)
                        if (candidate_token is None and isinstance(data, dict)
                                and type(data.get("token")) is int and 0 < data["token"] <= 0xFFFFFFFF):
                            candidate_token = data["token"]
                    except ValueError:
                        pass
                    await remote.send_str(packet.data)
                elif packet.type in (aiohttp.WSMsgType.ERROR, aiohttp.WSMsgType.CLOSE):
                    break

        async def to_browser():
            nonlocal token
            async for packet in remote:
                if packet.type == aiohttp.WSMsgType.TEXT:
                    try:
                        data = json.loads(packet.data)
                        if (token is None and isinstance(data, dict) and data.get("ok") is True
                                and data.get("type") != "pong"):
                            accepted = data.get("token", candidate_token)
                            if type(accepted) is int and 0 < accepted <= 0xFFFFFFFF:
                                token = accepted
                    except ValueError:
                        pass
                    await browser.send_str(packet.data)
                elif packet.type in (aiohttp.WSMsgType.ERROR, aiohttp.WSMsgType.CLOSE):
                    break

        tasks = [asyncio.create_task(to_device()), asyncio.create_task(to_browser())]
        try:
            await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            await remote.close()
            await browser.close()
            app[sockets_key].discard(browser)
            if token is not None:
                with contextlib.suppress(aiohttp.ClientError, asyncio.TimeoutError):
                    async with session.post(upstream + "/api/control/release", json={"token": token}) as result:
                        await result.read()
        return browser

    app.router.add_get("/", index)
    app.router.add_get("/ws", proxy_socket)
    app.router.add_route("*", "/api/{tail:.*}", proxy_api)
    return app


async def start_gateway(host: str, port: int = 8765) -> web.AppRunner:
    runner = web.AppRunner(make_gateway(host))
    await runner.setup()
    try:
        await web.TCPSite(runner, "127.0.0.1", port).start()
    except Exception:
        await runner.cleanup()
        raise
    return runner


def clamp_axis(value: int) -> int:
    return max(-32768, min(32767, value))


def sdl_state(controller, pg) -> PadState:
    buttons = 0
    names = {"A": "a", "B": "b", "X": "x", "Y": "y", "LEFTSTICK": "l3",
             "RIGHTSTICK": "r3", "BACK": "back", "START": "start",
             "LEFTSHOULDER": "lb", "RIGHTSHOULDER": "rb", "GUIDE": "guide"}
    for constant, name in names.items():
        if controller.get_button(getattr(pg, "CONTROLLER_BUTTON_" + constant)):
            buttons |= BUTTONS[name]
    dpad = 0
    for name, bit in DPAD.items():
        if controller.get_button(getattr(pg, "CONTROLLER_BUTTON_DPAD_" + name.upper())):
            dpad |= bit
    if dpad & 3 == 3:
        dpad &= ~3
    if dpad & 12 == 12:
        dpad &= ~12
    axes = {}
    for key, constant, sign in (("lx", "LEFTX", 1), ("ly", "LEFTY", -1),
                                ("rx", "RIGHTX", 1), ("ry", "RIGHTY", -1)):
        axes[key] = clamp_axis(sign * controller.get_axis(getattr(pg, "CONTROLLER_AXIS_" + constant)))
    for key, constant in (("lt", "TRIGGERLEFT"), ("rt", "TRIGGERRIGHT")):
        value = controller.get_axis(getattr(pg, "CONTROLLER_AXIS_" + constant))
        axes[key] = max(0, min(255, round(value * 255 / 32767)))
    return PadState(buttons=buttons, dpad=dpad, **axes)


def keyboard_state(pressed, pg) -> PadState:
    buttons = [name for key, name in ((pg.K_SPACE, "a"), (pg.K_e, "b"), (pg.K_q, "x"),
               (pg.K_r, "y"), (pg.K_f, "l3"), (pg.K_g, "r3"), (pg.K_BACKSPACE, "back"),
               (pg.K_RETURN, "start"), (pg.K_z, "lb"), (pg.K_c, "rb"), (pg.K_1, "lt"),
               (pg.K_2, "rt")) if pressed[key]]
    dpad = 0
    for positive, negative, positive_bit, negative_bit in (
        (pg.K_UP, pg.K_DOWN, 1, 2), (pg.K_LEFT, pg.K_RIGHT, 4, 8)
    ):
        if pressed[positive] and not pressed[negative]:
            dpad |= positive_bit
        elif pressed[negative] and not pressed[positive]:
            dpad |= negative_bit

    def axis(positive, negative):
        return clamp_axis((int(bool(pressed[positive])) - int(bool(pressed[negative]))) * 32767)

    return PadState.pressed(*buttons, dpad=dpad, lx=axis(pg.K_d, pg.K_a),
                            ly=axis(pg.K_w, pg.K_s), rx=axis(pg.K_l, pg.K_j),
                            ry=axis(pg.K_i, pg.K_k))


async def desktop(host: str, *, port: int = 8765, rate: int = DEFAULT_RATE) -> None:
    os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
    try:
        import pygame as pg
        from pygame._sdl2 import controller as pads
    except ImportError:
        raise RemoteError("The controller window needs pygame. Install this directory's requirements.txt first.") from None
    pg.display.init()
    pg.font.init()
    pads.init()
    window = pg.display.set_mode((900, 670))
    pg.display.set_caption("dc-ogx-w · Controller workbench")
    font = pg.font.SysFont("Segoe UI", 18)
    small = pg.font.SysFont("Segoe UI", 14)
    heading = pg.font.SysFont("Segoe UI", 31, bold=True)
    colors = {"bg": (11, 18, 16), "surface": (18, 31, 25), "line": (44, 66, 53),
              "text": (231, 245, 235), "muted": (157, 183, 166), "green": (167, 237, 99),
              "teal": (84, 212, 186), "red": (255, 155, 151)}
    buttons = {"acquire": pg.Rect(40, 140, 180, 44), "release": pg.Rect(230, 140, 115, 44),
               "source": pg.Rect(365, 140, 210, 44), "browser": pg.Rect(595, 140, 150, 44),
               "next": pg.Rect(755, 140, 105, 44)}
    current = None
    current_index = -1
    source = "keyboard"
    state = PadState()
    status_text = "Ready. Take control to send keyboard input."
    error = False
    connected_name = "No supported PC gamepad detected"
    gateway = None
    running = True
    focused = True
    last_scan = 0.0

    def scan(next_controller=False):
        nonlocal current, current_index, connected_name
        available = [i for i in range(pads.get_count()) if pads.is_controller(i)]
        if not next_controller and current is not None and current.attached():
            return
        if current is not None:
            current.quit()
            current = None
        if available:
            index = available[0]
            if next_controller and current_index in available:
                index = available[(available.index(current_index) + 1) % len(available)]
            current_index = index
            current = pads.Controller(index)
            connected_name = current.name
        else:
            current_index = -1
            connected_name = "No supported PC gamepad detected"

    def text(value, x, y, color="text", use_font=None):
        window.blit((use_font or font).render(str(value), True, colors[color]), (x, y))

    def wrap(value, x, y, max_width, color="muted"):
        line = ""
        for word in value.split():
            candidate = line + (" " if line else "") + word
            if small.size(candidate)[0] > max_width and line:
                text(line, x, y, color, small)
                y += 21
                line = word
            else:
                line = candidate
        text(line, x, y, color, small)

    async with RemoteClient(host, reconnect=True) as client:
        try:
            while running:
                start = time.monotonic()
                if start - last_scan > 1:
                    scan()
                    last_scan = start
                for event in pg.event.get():
                    action = None
                    if event.type == pg.QUIT:
                        running = False
                    elif event.type == pg.WINDOWFOCUSLOST:
                        focused = False
                        if client.control_requested or client.token is not None:
                            await client.release()
                            state = PadState()
                            status_text = "Control released while the window is out of focus."
                    elif event.type == pg.WINDOWFOCUSGAINED:
                        focused = True
                    elif event.type == pg.KEYDOWN:
                        if event.key == pg.K_ESCAPE:
                            action = "release"
                        elif event.key == pg.K_F8:
                            action = "release" if client.token is not None or client.control_requested else "acquire"
                    elif event.type == pg.MOUSEBUTTONUP and event.button == 1:
                        action = next((key for key, rect in buttons.items() if rect.collidepoint(event.pos)), None)
                    if action:
                        try:
                            if action == "acquire" and client.token is None:
                                if source == "gamepad" and current is None:
                                    raise RemoteError("Connect a supported controller, or choose keyboard input.")
                                await client.acquire("python-desktop")
                                status_text = "You have control. Esc releases; F8 toggles control."
                            elif action == "release":
                                await client.release()
                                state = PadState()
                                status_text = "Control released."
                            elif action == "source":
                                await client.release()
                                state = PadState()
                                source = "gamepad" if source == "keyboard" else "keyboard"
                                status_text = "Input source changed. Take control when ready."
                            elif action == "next":
                                await client.release()
                                state = PadState()
                                scan(True)
                                status_text = "PC controller selected. Take control when ready."
                            elif action == "browser":
                                await client.release()
                                state = PadState()
                                if gateway is None:
                                    gateway = await start_gateway(host, port)
                                webbrowser.open(f"http://127.0.0.1:{port}/")
                                status_text = "Browser controls opened. Keep this companion running."
                            error = False
                        except (RemoteError, aiohttp.ClientError, OSError, ValueError) as exc:
                            status_text = str(exc)
                            error = True
                if client.reconnecting:
                    state = PadState()
                    status_text, error = "Reconnecting Wi-Fi control. Input stays neutral; release all controls before resuming.", False
                elif client.token is not None:
                    try:
                        if not focused:
                            await client.release()
                            state = PadState()
                        elif source == "gamepad":
                            if current is None or not current.attached():
                                raise RemoteError("PC controller disconnected. Control released.")
                            state = sdl_state(current, pg)
                            await client.send(state)
                        else:
                            state = keyboard_state(pg.key.get_pressed(), pg)
                            await client.send(state)
                    except (RemoteError, pg.error, ValueError) as exc:
                        await client.release()
                        state = PadState()
                        status_text, error = str(exc), True

                if client.needs_neutral and client.token is not None:
                    status_text = "Connected. Release buttons and centre the sticks to resume input."
                elif client.token is not None and status_text.startswith(("Reconnecting", "Connected. Release")):
                    status_text = "You have control. Esc releases; F8 toggles control."
                elif client.token is None and not client.reconnecting and status_text.startswith("Reconnecting"):
                    status_text, error = (client._connection_error or "Connection recovery stopped") + ". Take control again when ready.", True
                window.fill(colors["bg"])
                text("dc-ogx-w", 40, 25, use_font=heading)
                text("CONTROLLER WORKBENCH", 42, 67, "teal", small)
                text(device_url(host), 40, 100, "muted", small)
                text("RECONNECTING" if client.reconnecting else "REMOTE ACTIVE" if client.token is not None else "RELEASED", 690, 43,
                     "green" if client.token is not None else "muted", small)
                labels = {"acquire": "Take control · F8", "release": "Release", "source": "Input: " + source.title(),
                          "browser": "Open browser", "next": "Next pad"}
                for key, rect in buttons.items():
                    primary = key == "acquire" and client.token is None
                    pg.draw.rect(window, colors["green"] if primary else colors["surface"], rect, border_radius=9)
                    pg.draw.rect(window, colors["line"], rect, 1, border_radius=9)
                    label = small.render(labels[key], True, colors["bg"] if primary else colors["text"])
                    window.blit(label, label.get_rect(center=rect.center))
                wrap(status_text, 40, 200, 815, "red" if error else "green" if client.token is not None else "muted")
                pg.draw.rect(window, colors["surface"], (40, 250, 820, 190), border_radius=15)
                for x, key in ((160, "l"), (370, "r")):
                    center = (x, 334)
                    pg.draw.circle(window, colors["line"], center, 56, 1)
                    pg.draw.line(window, colors["line"], (x - 56, 334), (x + 56, 334))
                    pg.draw.line(window, colors["line"], (x, 278), (x, 390))
                    point = (x + round(getattr(state, key + "x") / 32768 * 45),
                             334 - round(getattr(state, key + "y") / 32768 * 45))
                    pg.draw.circle(window, colors["green"], point, 10)
                    text(("Left" if key == "l" else "Right") + " stick", x - 36, 399, "muted", small)
                text(f"LT  {state.lt:3d}       RT  {state.rt:3d}", 535, 282, "teal")
                active_buttons = [name.upper() for name, bit in BUTTONS.items() if state.buttons & bit]
                active_buttons += [name.title() for name, bit in DPAD.items() if state.dpad & bit]
                wrap(" · ".join(active_buttons) or "No buttons pressed", 535, 326, 285)
                wrap(connected_name, 535, 382, 285)
                text("Keyboard", 40, 460)
                rows = ["W A S D  Left stick        I J K L  Right stick        Arrow keys  D-pad",
                        "Space / E / Q / R  A / B / X / Y        Z / C  LB / RB        1 / 2  LT / RT",
                        "Enter  Start        Backspace  Back        F / G  Stick clicks        Esc  Release"]
                for i, row in enumerate(rows):
                    text(row, 40, 494 + i * 26, "muted", small)
                text("Input stops on focus loss. The browser page also includes profiles and button mapping.",
                     40, 605, "muted", small)
                pg.display.flip()
                await asyncio.sleep(max(0, 1 / min(rate, client.input_hz) - (time.monotonic() - start)))
        finally:
            if gateway is not None:
                await gateway.cleanup()
            if current is not None:
                current.quit()
            pads.quit()
            pg.quit()


def json_file(path: str) -> Any:
    source = Path(path)
    if source.stat().st_size > 2 * 1024 * 1024:
        raise ValueError("The JSON file is too large")
    return json.loads(source.read_text(encoding="utf-8-sig"))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=DEFAULT_HOST, help="adapter address (default: %(default)s)")
    parser.add_argument("--prompt-host", action="store_true", help="ask for the adapter IP/name before starting")
    commands = parser.add_subparsers(dest="command")
    gui = commands.add_parser("desktop", help="PySide6 keyboard and PC-controller window (default)")
    gui.add_argument("--port", type=int, default=8765, help="localhost browser port")
    gui.add_argument("--rate", type=int, choices=range(10, 126), default=60, metavar="10..125",
                     help="requested rate, capped to the board's advertised input rate (default: 60)")
    sdl = commands.add_parser("desktop-sdl", help="legacy pygame controller window")
    sdl.add_argument("--port", type=int, default=8765)
    sdl.add_argument("--rate", type=int, default=60)
    browser = commands.add_parser("web", help="open the browser UI on localhost, enabling Gamepad API")
    browser.add_argument("--port", type=int, default=8765)
    browser.add_argument("--no-open", action="store_true")
    commands.add_parser("status", help="print adapter status as JSON")
    tap = commands.add_parser("tap", help="press one or more buttons, then release")
    tap.add_argument("buttons", nargs="+", choices=list(BUTTONS) + list(DPAD) + ["lt", "rt"])
    tap.add_argument("--duration", type=float, default=.1, help="seconds (default: %(default)s)")
    send = commands.add_parser("send", help="hold a complete controller state for a duration")
    send.add_argument("--button", action="append", default=[], choices=list(BUTTONS) + list(DPAD))
    for name in ("lx", "ly", "rx", "ry", "lt", "rt"):
        send.add_argument("--" + name, type=int, default=0)
    send.add_argument("--duration", type=float, default=1)
    run = commands.add_parser("run", help="run a JSON sequence of timed controller states")
    run.add_argument("file")
    profile = commands.add_parser("profile", help="read, apply, or persist the active profile")
    actions = profile.add_subparsers(dest="profile_action", required=True)
    get = actions.add_parser("get")
    get.add_argument("--output", "-o")
    apply = actions.add_parser("apply")
    apply.add_argument("file")
    save = actions.add_parser("save", help="persist an applied revision and restart the adapter")
    save.add_argument("--revision", required=True, type=int)
    return parser


async def async_main(args) -> None:
    if args.prompt_host:
        entered = input(f"Adapter IP or local name [{args.host}]: ").strip()
        args.host = device_url(entered or args.host)
    command = args.command or "desktop"
    if command == "desktop-sdl":
        await desktop(args.host, port=getattr(args, "port", 8765), rate=getattr(args, "rate", 60))
        return
    if command == "web":
        runner = await start_gateway(args.host, args.port)
        print(f"dc-ogx-w: http://127.0.0.1:{args.port}/  →  {device_url(args.host)}")
        print("In the local page choose PC gamepad, press a controller button, then Take control.")
        print("Keep this window open. Press Ctrl+C to stop.")
        try:
            if not args.no_open:
                webbrowser.open(f"http://127.0.0.1:{args.port}/")
            await asyncio.Event().wait()
        finally:
            await runner.cleanup()
        return
    # Check action files and state before acquiring the controller.
    steps = None
    state = None
    if command == "run":
        document = json_file(args.file)
        steps = document.get("steps") if isinstance(document, dict) else document
        parse_sequence(steps)
    elif command == "send":
        state = PadState.pressed(*args.button, **{name: getattr(args, name) for name in ("lx", "ly", "rx", "ry", "lt", "rt")})
    async with RemoteClient(args.host) as client:
        if command == "status":
            print(json.dumps(await client.status(), indent=2))
        elif command in ("tap", "send", "run"):
            await client.acquire("python-cli")
            if command == "tap":
                await client.tap(*args.buttons, seconds=args.duration)
            elif command == "send":
                await client.hold(state, args.duration)
            else:
                await client.play(steps)
        elif command == "profile":
            if args.profile_action == "get":
                data = await client.profile()
                output = json.dumps(data, indent=2) + "\n"
                if args.output:
                    Path(args.output).write_text(output, encoding="utf-8")
                    print(f"Saved {args.output}")
                else:
                    print(output, end="")
            elif args.profile_action == "apply":
                document = json_file(args.file)
                if not isinstance(document, dict):
                    raise ValueError("The profile must be a JSON object")
                print(json.dumps(await client.apply_profile(document.get("profile", document),
                                                           revision=document.get("revision")), indent=2))
            else:
                print(json.dumps(await client.save_profile(args.revision), indent=2))
                print("Profile saved; the adapter will restart.")


def main() -> int:
    args = build_parser().parse_args()
    try:
        if args.prompt_host:
            entered = input(f"Adapter IP or local name [{args.host}]: ").strip()
            args.host = entered or args.host
            args.prompt_host = False
        if args.command in (None, "desktop"):
            from ogx_desktop_qt import run_gui
            return run_gui(args.host, rate=getattr(args, "rate", 60))
        asyncio.run(async_main(args))
        return 0
    except KeyboardInterrupt:
        return 130
    except (RemoteError, aiohttp.ClientError, ValueError, OSError) as error:
        print(f"dc-ogx-w: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
