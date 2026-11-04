"""Protocol/client/gateway tests against a local fake adapter; no hardware needed."""
import asyncio
import contextlib
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest import mock

import aiohttp
from aiohttp import web

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ogx_remote import (BUTTONS, PadState, RemoteClient, RemoteError, device_url,
                        make_gateway, parse_sequence, sdl_state)


async def start(app):
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]
    return runner, f"http://127.0.0.1:{port}"


class FakeAdapter:
    def __init__(self):
        self.token = 12345
        self.owned = False
        self.busy = False
        self.reject = False
        self.inputs = []
        self.input_times = []
        self.input_hz = None
        self.owner_timeout_ms = 5000
        self.heartbeats = []
        self.pings = []
        self.connections = []
        self.bluetooth = False
        self.mode = "wifi"
        self.ping_supported = False
        self.silent_controls = False
        self.pause_status = None
        self.pause_acquire = None
        self.requests = []
        self.releases = []
        self.profile = {"id": 1, "button_a": 1}
        self.revision = 1
        self.saved = None
        self.app = web.Application()
        self.app.router.add_get("/ws", self.socket)
        self.app.router.add_route("*", "/api/{path:.*}", self.api)

    async def api(self, request):
        self.requests.append(request.path)
        body = await request.json() if request.method == "POST" else {}
        if request.path == "/api/status":
            if self.pause_status is not None:
                entered, resume = self.pause_status
                entered.set()
                await resume.wait()
            result = {"ok": True, "board": "PI_PICOW", "output_mode": "Xbox OG",
                      "network": {"bluetooth_available": self.bluetooth},
                      "control": {"mode": self.mode, "active": self.owned}}
            if self.input_hz is not None:
                result["network"]["input_hz"] = self.input_hz
            return web.json_response(result)
        if request.path == "/api/control/mode":
            if body:
                self.mode = body["mode"]
            return web.json_response({"ok": True, "mode": self.mode, "mode_pending": False})
        if request.path == "/api/calibration":
            return web.json_response({"ok": True, "phase": "center" if body else "idle", "samples": 0})
        if request.path == "/api/network/mdns/retry":
            return web.json_response({"ok": True, "queued": True}, status=202)
        if request.path == "/api/network/scan":
            if request.method == "POST":
                return web.json_response({"ok": True, "scan_pending": True, "generation": 1}, status=202)
            return web.json_response({"ok": True, "state": "complete", "generation": 1,
                                      "networks": [{"ssid": "Home WiFi", "rssi": -48, "channel": 6,
                                                    "security": "wpa2", "supported": True}]})
        if request.path == "/api/control/acquire":
            if self.busy or self.owned:
                return web.json_response({"ok": False, "error": "Control is in use"}, status=409)
            self.owned = True
            if self.pause_acquire is not None:
                entered, resume = self.pause_acquire
                entered.set()
                await resume.wait()
            reply = {"ok": True, "token": self.token, "timeout_ms": 250}
            if self.owner_timeout_ms is not None:
                reply["owner_timeout_ms"] = self.owner_timeout_ms
            if self.ping_supported:
                reply["ping_supported"] = True
            return web.json_response(reply)
        if request.path == "/api/control/release":
            self.releases.append(body.get("token"))
            if body.get("token") == self.token:
                self.owned = False
            return web.json_response({"ok": True})
        if request.path == "/api/profile":
            if request.method == "POST":
                if body.get("revision") != self.revision:
                    return web.json_response({"ok": False, "error": "Profile revision changed"}, status=409)
                self.profile = body["profile"]
                self.revision += 1
            return web.json_response({"ok": True, "profile": self.profile, "revision": self.revision})
        if request.path == "/api/profile/save":
            if body.get("revision") != self.revision:
                return web.json_response({"ok": False, "error": "Profile revision changed"}, status=409)
            self.saved = self.profile.copy()
            return web.json_response({"ok": True, "restarting": True})
        raise web.HTTPNotFound()

    async def socket(self, request):
        connection = web.WebSocketResponse()
        await connection.prepare(request)
        self.connections.append(connection)
        async for message in connection:
            if message.type == aiohttp.WSMsgType.TEXT:
                packet = json.loads(message.data)
                if packet.get("type") == "ping":
                    self.pings.append(packet)
                    await connection.send_json({"ok": True, "type": "pong", "id": packet["id"]})
                elif self.reject:
                    await connection.send_json({"ok": False, "error": "Input rejected"})
                elif packet.get("type") == "heartbeat":
                    self.heartbeats.append(packet)
                    if not self.silent_controls:
                        await connection.send_json({"ok": True, "heartbeat": True, "token": self.token})
                else:
                    self.inputs.append(packet)
                    self.input_times.append(time.monotonic())
                    if not self.silent_controls:
                        await connection.send_json({"ok": True, "seq": packet["seq"], "token": self.token})
        return connection


class StateTests(unittest.TestCase):
    def test_boundaries_and_types(self):
        PadState(lx=-32768, ly=32767, lt=255, buttons=4095)
        for bad in ({"lx": 32768}, {"ly": -32769}, {"lt": 256}, {"rt": -1},
                    {"buttons": 4096}, {"dpad": 3}, {"dpad": 12},
                    {"lt": True}, {"lx": 0.5}):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                PadState(**bad)

    def test_named_buttons_and_diagonal(self):
        state = PadState.pressed("a", "start", "up", "right", "lt", rx=123)
        self.assertEqual(state, PadState(buttons=129, dpad=9, lt=255, rx=123))

    def test_sequence_is_complete_state(self):
        steps = parse_sequence([{"duration_ms": 100, "buttons": ["a"]},
                                {"duration_ms": 20}])
        self.assertEqual(steps, [(PadState(buttons=1), .1), (PadState(), .02)])

    def test_sequence_validated_before_start(self):
        for bad in ([], [{"duration_ms": -1}], [{"duration_ms": float("inf")}],
                    [{"duration_ms": 1, "buttons": ["not-a-button"]}],
                    [{"duration_ms": 1, "unknown": 3}]):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                parse_sequence(bad)

    def test_device_url(self):
        self.assertEqual(device_url("192.168.4.1"), "http://192.168.4.1")
        self.assertEqual(device_url("http://localhost:9000/"), "http://localhost:9000")
        for value in ("ftp://device", "http://name:pass@device", "http://device/path", "http://device/?x=1"):
            with self.assertRaises(ValueError):
                device_url(value)

    def test_sdl_y_direction_and_trigger_range(self):
        class Constants:
            def __getattr__(self, key):
                return key

        class Controller:
            def get_button(self, key):
                return key in ("CONTROLLER_BUTTON_A", "CONTROLLER_BUTTON_DPAD_UP")

            def get_axis(self, key):
                return {"CONTROLLER_AXIS_LEFTY": -32768,
                        "CONTROLLER_AXIS_RIGHTY": 32767,
                        "CONTROLLER_AXIS_TRIGGERLEFT": 32767,
                        "CONTROLLER_AXIS_TRIGGERRIGHT": 0}.get(key, 0)

        state = sdl_state(Controller(), Constants())
        self.assertEqual(state, PadState(buttons=1, dpad=1, ly=32767, ry=-32767, lt=255))


class ClientTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.adapter = FakeAdapter()
        self.runner, self.url = await start(self.adapter.app)
        self.extra_runners = []

    async def asyncTearDown(self):
        for runner in self.extra_runners:
            await runner.cleanup()
        await self.runner.cleanup()

    async def test_acquire_heartbeat_tap_and_release(self):
        async with RemoteClient(self.url) as client:
            await client.acquire()
            await client.hold(PadState(lx=14000), .055)
            await client.tap("a", seconds=.035)
        packets = self.adapter.inputs
        self.assertGreaterEqual(len(packets), 6)
        keys = {"type", "token", "seq", "buttons", "dpad", "lx", "ly", "rx", "ry", "lt", "rt"}
        self.assertTrue(all(set(packet) == keys for packet in packets))
        self.assertEqual([p["seq"] for p in packets], list(range(len(packets))))
        self.assertTrue(all(p["token"] == 12345 for p in packets))
        self.assertEqual(packets[0]["buttons"], 0)
        self.assertTrue(any(p["lx"] == 14000 for p in packets))
        self.assertTrue(any(p["buttons"] == BUTTONS["a"] for p in packets))
        self.assertTrue(all(packets[-1][name] == 0 for name in PadState.__dataclass_fields__))
        self.assertEqual(self.adapter.releases, [12345])
        self.assertFalse(self.adapter.owned)

    async def test_owner_heartbeat_never_resends_held_input(self):
        async with RemoteClient(self.url) as client:
            await client.acquire()
            await client.send(PadState(buttons=1))
            await asyncio.sleep(.02)
            count = len(self.adapter.inputs)
            await asyncio.sleep(1.1)
            self.assertEqual(len(self.adapter.inputs), count)
            self.assertGreaterEqual(len(self.adapter.heartbeats), 1)
            self.assertEqual(self.adapter.heartbeats[-1], {"type": "heartbeat", "token": 12345})
            # No held state was refreshed during the idle interval. Firmware's
            # independently tested 250 ms deadman therefore goes neutral.
            self.assertGreater(time.monotonic() - self.adapter.input_times[-1], 1)
            await client.send(PadState())
            await asyncio.sleep(.01)
            self.assertEqual(self.adapter.inputs[-1]["buttons"], 0)
        self.assertIsNone(client._heartbeat)

    async def test_legacy_firmware_does_not_receive_new_heartbeat_type(self):
        self.adapter.owner_timeout_ms = None
        async with RemoteClient(self.url) as client:
            await client.acquire()
            self.assertEqual(client.owner_timeout_ms, 0)
            self.assertIsNone(client._heartbeat)

    async def wait_for(self, predicate, timeout=3):
        deadline = time.monotonic() + timeout
        while not predicate():
            if time.monotonic() >= deadline:
                self.fail("Timed out waiting for client state")
            await asyncio.sleep(.01)

    async def test_ping_health_is_independent_of_input_ack_and_never_replays(self):
        self.adapter.ping_supported = True
        self.adapter.silent_controls = True
        async with RemoteClient(self.url) as client:
            await client.acquire()
            await client.send(PadState(buttons=1))
            await asyncio.sleep(.02)
            count = len(self.adapter.inputs)
            client._last_ack_at = time.monotonic() - 4.5
            await asyncio.sleep(1.1)
            self.assertGreaterEqual(len(self.adapter.pings), 1)
            self.assertIsNone(client._connection_error)
            self.assertEqual(len(self.adapter.inputs), count)
            self.assertEqual(client.last_ack["type"], "pong")

    async def test_wifi_reconnect_starts_neutral_and_requires_producer_rest(self):
        async with RemoteClient(self.url, reconnect=True) as client:
            await client.acquire()
            await client.send(PadState(buttons=1, lx=16000))
            await asyncio.sleep(.02)
            before = len(self.adapter.inputs)
            await self.adapter.connections[-1].close()
            await self.wait_for(lambda: client.reconnecting)
            await client.send(PadState(buttons=1, lx=16000))
            await self.wait_for(lambda: client.token is not None and not client.reconnecting)
            await client.send(PadState(buttons=1, lx=16000))
            await asyncio.sleep(.02)
            after = self.adapter.inputs[before:]
            self.assertTrue(after)
            self.assertTrue(all(p["buttons"] == 0 and p["lx"] == 0 for p in after))
            self.assertTrue(client.needs_neutral)
            await client.send(PadState())
            self.assertFalse(client.needs_neutral)
            await client.send(PadState(buttons=2))
            await asyncio.sleep(.02)
            self.assertEqual(self.adapter.inputs[-1]["buttons"], 2)
            self.assertEqual(self.adapter.requests.count("/api/control/acquire"), 2)

    async def test_release_cancels_reconnect_backoff(self):
        async with RemoteClient(self.url, reconnect=True) as client:
            await client.acquire()
            await self.adapter.connections[-1].close()
            await self.wait_for(lambda: client.reconnecting)
            await client.release()
            await asyncio.sleep(.6)
            self.assertFalse(client.control_requested)
            self.assertFalse(client.reconnecting)
            self.assertIsNone(client.token)
            self.assertEqual(self.adapter.requests.count("/api/control/acquire"), 1)

    async def test_reacquire_failed_initial_neutral_is_cleaned_up_then_retried(self):
        original_send = aiohttp.ClientWebSocketResponse.send_str
        failures = 0

        async def fail_second_acquisition(socket, data, compress=None):
            nonlocal failures
            if (json.loads(data).get("type") == "input" and
                    self.adapter.requests.count("/api/control/acquire") == 2):
                failures += 1
                raise ConnectionError("Injected failure of initial reconnect neutral")
            return await original_send(socket, data, compress=compress)

        async with RemoteClient(self.url, reconnect=True) as client:
            await client.acquire()
            with mock.patch.object(aiohttp.ClientWebSocketResponse, "send_str", fail_second_acquisition), \
                    mock.patch("ogx_remote.RECONNECT_DELAYS", (.01,) * 8):
                await self.adapter.connections[-1].close()
                await self.wait_for(lambda: self.adapter.requests.count("/api/control/acquire") == 3 and not client.reconnecting)
            self.assertEqual(failures, 1)
            self.assertTrue(client.control_requested)
            self.assertEqual(client.token, self.adapter.token)
            self.assertIsNone(client._connection_error)
            self.assertIsNotNone(client._heartbeat)
            self.assertTrue(client.needs_neutral)
            # The failed grant was released before acquiring again.
            self.assertEqual(self.adapter.releases, [self.adapter.token, self.adapter.token])
            self.assertTrue(all(p["buttons"] == 0 for p in self.adapter.inputs))

    async def test_repeated_initial_neutral_failure_exhausts_only_eight_retries(self):
        original_send = aiohttp.ClientWebSocketResponse.send_str

        async def fail_reconnect_neutral(socket, data, compress=None):
            if json.loads(data).get("type") == "input":
                raise ConnectionError("Injected reconnect neutral failure")
            return await original_send(socket, data, compress=compress)

        async with RemoteClient(self.url, reconnect=True) as client:
            await client.acquire()
            with mock.patch.object(aiohttp.ClientWebSocketResponse, "send_str", fail_reconnect_neutral), \
                    mock.patch("ogx_remote.RECONNECT_DELAYS", (.01,) * 8):
                await self.adapter.connections[-1].close()
                await self.wait_for(lambda: not client.control_requested and not client.reconnecting)
            self.assertEqual(self.adapter.requests.count("/api/control/acquire"), 9)
            self.assertIsNone(client.token)
            self.assertIsNone(client.socket)
            self.assertIsNone(client._heartbeat)
            self.assertFalse(self.adapter.owned)

    async def test_release_during_initial_status_cancels_late_acquisition(self):
        entered, resume = asyncio.Event(), asyncio.Event()
        self.adapter.pause_status = (entered, resume)
        async with RemoteClient(self.url) as client:
            acquiring = asyncio.create_task(client.acquire())
            await asyncio.wait_for(entered.wait(), 1)
            await client.release()
            resume.set()
            with self.assertRaisesRegex(RemoteError, "cancelled"):
                await acquiring
            self.assertFalse(client.control_requested)
            self.assertIsNone(client.token)
            self.assertIsNone(client.socket)
            self.assertNotIn("/api/control/acquire", self.adapter.requests)

    async def test_release_during_pending_acquire_releases_known_late_token(self):
        entered, resume = asyncio.Event(), asyncio.Event()
        self.adapter.pause_acquire = (entered, resume)
        async with RemoteClient(self.url) as client:
            acquiring = asyncio.create_task(client.acquire())
            await asyncio.wait_for(entered.wait(), 1)
            await client.release()
            resume.set()
            with self.assertRaisesRegex(RemoteError, "cancelled"):
                await acquiring
            self.assertFalse(client.control_requested)
            self.assertIsNone(client.token)
            self.assertIsNone(client.socket)
            self.assertIsNone(client._heartbeat)
            self.assertFalse(self.adapter.owned)
            self.assertEqual(self.adapter.releases, [self.adapter.token])
            self.assertEqual(self.adapter.inputs, [])

    async def test_reconnect_never_overrides_conflicting_owner(self):
        async with RemoteClient(self.url, reconnect=True) as client:
            await client.acquire()
            self.adapter.busy = True
            await self.adapter.connections[-1].close()
            await self.wait_for(lambda: client.reconnecting)
            await self.wait_for(lambda: not client.reconnecting)
            self.assertFalse(client.control_requested)
            self.assertIsNone(client.token)
            self.assertEqual(self.adapter.requests.count("/api/control/acquire"), 2)

    async def test_bluetooth_mode_never_reconnects_automatically(self):
        self.adapter.bluetooth = True
        async with RemoteClient(self.url, reconnect=True) as client:
            await client.acquire()
            await self.adapter.connections[-1].close()
            await asyncio.sleep(.6)
            self.assertFalse(client.reconnecting)
            self.assertEqual(self.adapter.requests.count("/api/control/acquire"), 1)
            with self.assertRaises(RemoteError):
                await client.send(PadState(buttons=1))

    async def test_conflicting_owner_is_not_overridden(self):
        self.adapter.busy = True
        async with RemoteClient(self.url) as client:
            with self.assertRaisesRegex(RemoteError, "in use"):
                await client.acquire()
            self.assertIsNone(client.token)
            self.assertIsNone(client.socket)
        self.assertEqual(self.adapter.inputs, [])
        self.assertEqual(self.adapter.releases, [])

    async def test_profile_apply_and_revision_save(self):
        async with RemoteClient(self.url) as client:
            profile = (await client.profile())["profile"]
            profile["button_a"] = 2
            result = await client.apply_profile(profile)
            with self.assertRaisesRegex(RemoteError, "revision changed"):
                await client.apply_profile({"id": 1, "button_a": 4}, revision=result["revision"] - 1)
            with self.assertRaisesRegex(RemoteError, "revision changed"):
                await client.save_profile(result["revision"] - 1)
            await client.save_profile(result["revision"])
        self.assertEqual(self.adapter.saved["button_a"], 2)

    async def test_device_error_stops_input_stream(self):
        async with RemoteClient(self.url) as client:
            await client.acquire()
            self.adapter.reject = True
            await client.send(PadState(buttons=1))
            await asyncio.sleep(.02)
            with self.assertRaisesRegex(RemoteError, "rejected"):
                await client.send(PadState(buttons=2))
        self.assertFalse(self.adapter.owned)

    async def test_sequence_number_wrap(self):
        async with RemoteClient(self.url) as client:
            await client.acquire()
            client.sequence = 0xFFFFFFFF
            await client.send(PadState(buttons=1))
            await client.send(PadState())
        self.assertEqual([p["seq"] for p in self.adapter.inputs[:3]], [0, 0xFFFFFFFF, 0])

    async def test_no_send_without_acquire(self):
        async with RemoteClient(self.url) as client:
            with self.assertRaises(RemoteError):
                await client.send(PadState(buttons=1))

    async def test_fallback_uart_rate_paces_fast_application_calls(self):
        self.adapter.input_hz = 30
        async with RemoteClient(self.url) as client:
            await client.acquire()
            self.assertEqual(client.input_hz, 30)
            self.assertEqual(self.adapter.requests[:2], ["/api/status", "/api/control/acquire"])
            await client.send(PadState(buttons=1))
            await client.send(PadState())
        gaps = [b - a for a, b in zip(self.adapter.input_times, self.adapter.input_times[1:])]
        self.assertGreaterEqual(len(gaps), 3)
        self.assertTrue(all(gap >= .025 for gap in gaps), gaps)

    async def test_official_fast_uart_and_legacy_firmware_rates(self):
        for advertised in (60, None, True, 0, 10000, "30"):
            with self.subTest(advertised=advertised):
                self.adapter.input_hz = advertised
                async with RemoteClient(self.url) as client:
                    await client.acquire()
                    self.assertEqual(client.input_hz, 60)

    async def test_gateway_http_websocket_and_disconnect_release(self):
        with tempfile.TemporaryDirectory() as directory:
            html = Path(directory) / "index.html"
            html.write_text("<!doctype html><title>OGX Remote</title>")
            runner, gateway_url = await start(make_gateway(self.url, html_path=html))
            self.extra_runners.append(runner)
            async with aiohttp.ClientSession() as browser:
                async with browser.get(gateway_url + "/") as response:
                    self.assertIn("OGX Remote", await response.text())
                async with browser.get(gateway_url + "/api/status") as response:
                    self.assertEqual((await response.json())["board"], "PI_PICOW")
                async with browser.post(gateway_url + "/api/network/scan", json={}) as response:
                    self.assertEqual(response.status, 202)
                    self.assertEqual((await response.json())["generation"], 1)
                async with browser.get(gateway_url + "/api/network/scan") as response:
                    self.assertEqual((await response.json())["networks"][0]["ssid"], "Home WiFi")
                async with browser.post(gateway_url + "/api/control/mode", json={"mode": "configure"}) as response:
                    self.assertEqual((await response.json())["mode"], "configure")
                async with browser.post(gateway_url + "/api/calibration", json={"action": "begin", "mask": 63}) as response:
                    self.assertEqual((await response.json())["phase"], "center")
                async with browser.post(gateway_url + "/api/network/mdns/retry", json={}) as response:
                    self.assertEqual(response.status, 202)
                async with browser.post(gateway_url + "/api/control/acquire", json={"client": "browser"}) as response:
                    self.assertEqual(response.status, 200)
                async with browser.ws_connect(gateway_url + "/ws", origin=gateway_url) as socket:
                    await socket.send_json({"type": "input", "token": 12345, "seq": 0,
                                            "buttons": 1, "dpad": 0, "lx": 0, "ly": 0,
                                            "rx": 0, "ry": 0, "lt": 0, "rt": 0})
                    reply = await socket.receive_json(timeout=1)
                    self.assertTrue(reply["ok"])
                    await socket.send_json({"type": "ping", "id": 77})
                    self.assertEqual(await socket.receive_json(timeout=1), {"ok": True, "type": "pong", "id": 77})
                await asyncio.sleep(.025)
                self.assertIn(12345, self.adapter.releases)
                self.assertEqual(self.adapter.inputs[-1]["buttons"], 1)
                async with browser.post(gateway_url + "/api/control/acquire", json={},
                                        headers={"Origin": "https://different.example"}) as response:
                    self.assertEqual(response.status, 403)


if __name__ == "__main__":
    unittest.main()
