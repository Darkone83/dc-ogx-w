"""PySide6 desktop remote for DC-OGX-W (Linux, macOS and Windows).

Qt only owns the UI; an isolated worker thread owns aiohttp and RemoteClient.
This avoids installing qasync or running the network loop inside Qt's GUI loop.
"""
from __future__ import annotations

import asyncio
import contextlib
import json
import os
from pathlib import Path
import queue
import sys
import threading
import time
from urllib.parse import urlsplit

from PySide6.QtCore import Qt, QThread, QTimer, Signal, QEvent, QUrl
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QFrame, QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QLineEdit, QComboBox, QStackedWidget, QTextEdit,
    QScrollArea, QDialog, QGridLayout, QSizePolicy, QMessageBox,
)

from ogx_input import DEFAULT_BINDINGS, check_bindings, keyboard_snapshot, rebind
from ogx_remote import PadState, RemoteClient, RemoteError, device_url, sdl_state


def pref_file() -> Path:
    if sys.platform == "darwin":
        root = Path.home() / "Library" / "Application Support"
    elif os.name == "nt":
        root = Path(os.environ.get("APPDATA", str(Path.home())))
    else:
        root = Path(os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config")))
    return root / "dc-ogx-w" / "python-remote.json"


def load_prefs() -> dict:
    try:
        doc = json.loads(pref_file().read_text(encoding="utf-8"))
        return doc if isinstance(doc, dict) else {}
    except (OSError, ValueError):
        return {}


def save_prefs(prefs: dict) -> None:
    path = pref_file()
    path.parent.mkdir(parents=True, exist_ok=True)
    scratch = path.with_suffix(".tmp")
    scratch.write_text(json.dumps(prefs, indent=2) + "\n", encoding="utf-8")
    scratch.replace(path)


class RemoteWorker(QThread):
    message = Signal(str)
    device = Signal(object)
    control = Signal(str)
    last_error = Signal(str)
    profile = Signal(object)

    def __init__(self, host: str, rate: int, parent=None):
        super().__init__(parent)
        self.host = host
        self.rate = max(10, min(125, rate))
        self.commands: queue.Queue[tuple[str, str]] = queue.Queue(maxsize=32)
        self.stop_event = threading.Event()
        self._state_lock = threading.Lock()
        self._state = PadState()
        self._focused = True

    def command(self, name: str, payload: str = "") -> None:
        try:
            self.commands.put_nowait((name, payload))
        except queue.Full:
            self.last_error.emit("Too many pending actions; try again when the adapter responds.")

    def snapshot(self, state: PadState, focused: bool) -> None:
        with self._state_lock:
            # New state replaces the previous one, never queues it for replay.
            self._state = state if focused else PadState()
            self._focused = focused

    def get_state(self) -> tuple[PadState, bool]:
        with self._state_lock:
            return self._state, self._focused

    async def _main(self) -> None:
        client = RemoteClient(self.host, reconnect=True)
        await client.__aenter__()
        last_status = 0.0
        last_control = ""
        last_error = ""
        try:
            while not self.stop_event.is_set():
                for _ in range(3):
                    try:
                        command, argument = self.commands.get_nowait()
                    except queue.Empty:
                        break
                    try:
                        if command == "connect":
                            new_url = device_url(argument)
                            await client.close()
                            client = RemoteClient(new_url, reconnect=True)
                            await client.__aenter__()
                            self.host = new_url
                            self.control.emit("idle")
                            self.message.emit("Connecting to " + new_url)
                            self.device.emit(await client.status())
                        elif command == "take":
                            await client.acquire("python-pyside6")
                            self.control.emit("active")
                            self.message.emit("Controller session acquired. F8 or Esc releases control.")
                        elif command == "release":
                            await client.release()
                            self.control.emit("idle")
                            self.message.emit("Control released. Console input is neutral.")
                        elif command == "refresh":
                            if client.control_requested:
                                self.message.emit("Release control before refreshing diagnostics.")
                                continue
                            self.device.emit(await client.status())
                            last_status = time.monotonic()
                        elif command == "profiles":
                            if client.control_requested:
                                self.message.emit("Release control before loading profiles.")
                                continue
                            self.profile.emit(await client.request("/api/profiles"))
                        else:
                            self.message.emit("Ignored unknown command")
                    except (RemoteError, OSError, ValueError, asyncio.TimeoutError) as exc:
                        self.last_error.emit(str(exc))
                state, focused = self.get_state()
                if not focused and client.control_requested:
                    with contextlib.suppress(RemoteError, OSError):
                        await client.release()
                    self.control.emit("idle")
                    self.message.emit("Control released because the window lost focus.")
                if client.token is not None and focused and not client.reconnecting:
                    try:
                        await client.send(state)
                    except (RemoteError, OSError, asyncio.TimeoutError) as exc:
                        self.last_error.emit(str(exc))
                now = time.monotonic()
                current = ("reconnecting" if client.reconnecting else
                           "active" if client.token is not None else
                           "failed" if client.control_requested else "idle")
                if current != last_control:
                    self.control.emit(current)
                    last_control = current
                if client._connection_error and client._connection_error != last_error:
                    self.last_error.emit(client._connection_error)
                    last_error = client._connection_error
                # Status is read only when idle. Polling HTTP while playing can
                # block the same old ESP8285 AT pipe used by controller frames.
                if current == "idle" and now - last_status > 4.0:
                    last_status = now
                    try:
                        self.device.emit(await client.status())
                    except (RemoteError, OSError, asyncio.TimeoutError) as exc:
                        self.last_error.emit(str(exc))
                await asyncio.sleep(0.010 if current == "active" else 0.040)
        finally:
            with contextlib.suppress(Exception):
                await client.close()

    def run(self) -> None:
        try:
            asyncio.run(self._main())
        except Exception as exc:
            self.last_error.emit(f"Network worker stopped: {exc}")
        self.control.emit("idle")

    def stop(self) -> None:
        self.stop_event.set()


class KeyCapture(QPushButton):
    selected = Signal(str)
    def __init__(self, label: str, parent=None):
        super().__init__(label, parent)
        self.waiting = False
        self.clicked.connect(self.arm)

    def arm(self):
        self.waiting = True
        self.setText("Press a key…")
        self.setFocus()

    def keyPressEvent(self, event):
        if not self.waiting:
            super().keyPressEvent(event)
            return
        name = key_name(event.key())
        self.waiting = False
        if name and name not in {"Escape", "F8"}:
            self.selected.emit(name)
        else:
            self.selected.emit("")


def key_name(key: int) -> str:
    keys = {int(Qt.Key.Key_Space): "Space", int(Qt.Key.Key_Return): "Return",
            int(Qt.Key.Key_Enter): "Return", int(Qt.Key.Key_Backspace): "Backspace",
            int(Qt.Key.Key_Left): "Left", int(Qt.Key.Key_Right): "Right",
            int(Qt.Key.Key_Up): "Up", int(Qt.Key.Key_Down): "Down",
            int(Qt.Key.Key_Home): "Home", int(Qt.Key.Key_Escape): "Escape",
            int(Qt.Key.Key_F8): "F8", int(Qt.Key.Key_Tab): "Tab",
            int(Qt.Key.Key_Shift): "Shift", int(Qt.Key.Key_Control): "Control",
            int(Qt.Key.Key_Alt): "Alt"}
    if key in keys:
        return keys[key]
    if 0x20 < key <= 0x7e:
        return chr(key).upper()
    return ""


class BindingsDialog(QDialog):
    def __init__(self, bindings: dict[str, str], parent=None):
        super().__init__(parent)
        self.setWindowTitle("Keyboard mappings")
        self.resize(430, 620)
        self.bindings = bindings.copy()
        self.capture: dict[str, KeyCapture] = {}
        root = QVBoxLayout(self)
        root.addWidget(QLabel("Select an assignment, then press its new key. Escape and F8 remain reserved."))
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        panel = QWidget()
        grid = QGridLayout(panel)
        for row, (action, key) in enumerate(self.bindings.items()):
            name = {"white":"White (LB)", "black":"Black (RB)"}.get(action, action.replace("_", " ").upper())
            grid.addWidget(QLabel(name), row, 0)
            button = KeyCapture(key)
            button.selected.connect(lambda value, action=action: self.remap(action, value))
            self.capture[action] = button
            grid.addWidget(button, row, 1)
        scroll.setWidget(panel)
        root.addWidget(scroll)
        footer = QHBoxLayout()
        defaults = QPushButton("Restore defaults")
        defaults.clicked.connect(self.restore)
        footer.addWidget(defaults)
        footer.addStretch()
        done = QPushButton("Done")
        done.clicked.connect(self.accept)
        footer.addWidget(done)
        root.addLayout(footer)

    def restore(self):
        self.bindings = DEFAULT_BINDINGS.copy()
        self.update_buttons()

    def remap(self, action: str, value: str):
        if value:
            self.bindings = rebind(self.bindings, action, value)
        self.update_buttons()

    def update_buttons(self):
        for action, button in self.capture.items():
            button.setText(self.bindings[action])


class ControllerPanel(QFrame):
    def __init__(self, changed, parent=None):
        super().__init__(parent)
        self.changed = changed
        self.buttons: dict[str, QPushButton] = {}
        self.setObjectName("surface")
        container = QVBoxLayout(self)
        title = QLabel("OG XBOX CONTROLLER")
        title.setObjectName("eyebrow")
        container.addWidget(title)
        grid = QGridLayout()
        self.add(grid, "white", "WHITE", 0, 0)
        self.add(grid, "black", "BLACK", 0, 4)
        self.add(grid, "lt", "LT", 1, 0)
        self.add(grid, "rt", "RT", 1, 4)
        self.add(grid, "up", "▲", 2, 0)
        self.add(grid, "y", "Y", 2, 3)
        self.add(grid, "left", "◀", 3, 0)
        self.add(grid, "right", "▶", 3, 1)
        self.add(grid, "x", "X", 3, 2)
        self.add(grid, "b", "B", 3, 3)
        self.add(grid, "a", "A", 3, 4)
        self.add(grid, "down", "▼", 4, 0)
        self.add(grid, "back", "BACK", 5, 0)
        self.add(grid, "guide", "GUIDE", 5, 2)
        self.add(grid, "start", "START", 5, 4)
        self.add(grid, "l3", "L3", 6, 0)
        self.add(grid, "r3", "R3", 6, 4)
        for c in range(5):
            grid.setColumnStretch(c, 1)
        container.addLayout(grid)
        self.state_label = QLabel("L  0 / 0       R  0 / 0       TRIGGERS  0 / 0")
        self.state_label.setObjectName("muted")
        container.addWidget(self.state_label)

    def add(self, grid, action: str, text: str, row: int, col: int):
        button = QPushButton(text)
        button.setObjectName("pad")
        button.setMinimumHeight(42)
        button.setProperty("active", False)
        button.pressed.connect(lambda a=action: self.changed(a, True))
        button.released.connect(lambda a=action: self.changed(a, False))
        self.buttons[action] = button
        grid.addWidget(button, row, col)

    def update_state(self, state: PadState):
        button_bits = {"a":1,"b":2,"x":4,"y":8,"l3":16,"r3":32,
                       "back":64,"start":128,"white":256,"black":512,"guide":1024}
        dpad_bits = {"up":1,"down":2,"left":4,"right":8}
        active = {name for name, bit in button_bits.items() if state.buttons & bit}
        active.update(name for name, bit in dpad_bits.items() if state.dpad & bit)
        if state.lt > 12: active.add("lt")
        if state.rt > 12: active.add("rt")
        for name, button in self.buttons.items():
            enabled = name in active
            if button.property("active") != enabled:
                button.setProperty("active", enabled)
                button.style().unpolish(button)
                button.style().polish(button)
        self.state_label.setText(f"L  {state.lx:+6d}/{state.ly:+6d}    R  {state.rx:+6d}/{state.ry:+6d}"
                                 f"    LT {state.lt:3d}  RT {state.rt:3d}")


class GamepadSource:
    """SDL gamepad polling on Qt's main thread; gracefully degrades to keyboard."""
    def __init__(self):
        self.pg = None
        self.pads = None
        self.controller = None
        self.index = -1
        self.error = ""
        try:
            os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
            import pygame as pg
            from pygame._sdl2 import controller as pads
            pg.display.init()
            # Pumping SDL events is needed to read current joystick positions.
            # The SDL window is hidden; Qt remains the only visible window.
            pg.display.set_mode((1, 1), pg.HIDDEN)
            pads.init()
            self.pg, self.pads = pg, pads
        except Exception as exc:
            self.error = str(exc)
            self.quit()

    def refresh(self, next_device=False):
        if self.pads is None:
            return "Gamepad unavailable: " + self.error
        try:
            available = [i for i in range(self.pads.get_count()) if self.pads.is_controller(i)]
            if self.controller is not None and self.controller.attached() and not next_device:
                return self.controller.name
            if self.controller:
                self.controller.quit()
                self.controller = None
            if available:
                self.index = available[(available.index(self.index) + 1) % len(available)] if next_device and self.index in available else available[0]
                self.controller = self.pads.Controller(self.index)
                return self.controller.name
            self.index = -1
            return "No supported gamepad detected"
        except Exception as exc:
            self.error = str(exc)
            return "Gamepad unavailable: " + self.error

    def state(self) -> PadState:
        if not self.pg or self.controller is None or not self.controller.attached():
            return PadState()
        try:
            self.pg.event.pump()
            return sdl_state(self.controller, self.pg)
        except Exception as exc:
            self.error = str(exc)
            return PadState()

    def quit(self):
        with contextlib.suppress(Exception):
            if self.controller: self.controller.quit()
        with contextlib.suppress(Exception):
            if self.pads: self.pads.quit()
        with contextlib.suppress(Exception):
            if self.pg: self.pg.quit()


class RemoteWindow(QMainWindow):
    def __init__(self, host: str, rate: int):
        super().__init__()
        self.setWindowTitle("DC-OGX-W | PC Remote")
        self.resize(990, 710)
        self.setMinimumSize(740, 600)
        self.prefs = load_prefs()
        self.bindings = check_bindings(self.prefs.get("bindings"))
        self.pressed: set[str] = set()
        self.held: set[str] = set()
        self.active = False
        self.focused = True
        self.gamepad = GamepadSource()
        chosen_host = self.prefs.get("host", host) if host == "192.168.4.1" else host
        self.worker = RemoteWorker(chosen_host, rate, self)
        self.worker.message.connect(self.message)
        self.worker.device.connect(self.show_device)
        self.worker.control.connect(self.show_control)
        self.worker.profile.connect(self.show_profile)
        self.worker.last_error.connect(self.error)
        self.init_ui()
        self.worker.start()
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.poll_controls)
        self.timer.start(32)
        self.scan_timer = QTimer(self)
        self.scan_timer.timeout.connect(self.update_controller_name)
        self.scan_timer.start(1500)
        self.update_controller_name()
        QApplication.instance().installEventFilter(self)

    def init_ui(self):
        shell = QWidget()
        root = QHBoxLayout(shell)
        root.setContentsMargins(12, 12, 12, 12)
        root.setSpacing(12)
        nav = QFrame()
        nav.setObjectName("sidebar")
        nav.setFixedWidth(180)
        column = QVBoxLayout(nav)
        brand = QLabel("DC  /  OGX-W")
        brand.setObjectName("brand")
        column.addWidget(brand)
        desc = QLabel("REMOTE COMPANION")
        desc.setObjectName("eyebrow")
        column.addWidget(desc)
        column.addSpacing(20)
        self.pages = QStackedWidget()
        for i, name in enumerate(("Controller", "Device", "Profiles", "Help & logs")):
            b = QPushButton(name)
            b.setObjectName("nav")
            b.clicked.connect(lambda _=False, i=i: self.pages.setCurrentIndex(i))
            column.addWidget(b)
        column.addStretch()
        self.connection = QLabel("●  Searching / offline")
        self.connection.setWordWrap(True)
        column.addWidget(self.connection)
        root.addWidget(nav)
        self.pages.addWidget(self.controller_page())
        self.pages.addWidget(self.device_page())
        self.pages.addWidget(self.profiles_page())
        self.pages.addWidget(self.help_page())
        root.addWidget(self.pages, 1)
        self.setCentralWidget(shell)
        self.setStyleSheet('''
            QWidget { background:#101518; color:#E5F0EF; font-size:13px; font-family:Segoe UI,Arial; }
            QFrame#sidebar { background:#152025; border:1px solid #294047; border-radius:12px; }
            QFrame#surface { background:#18252B; border:1px solid #31434C; border-radius:12px; }
            QLabel#brand { font-size:23px; font-weight:800; color:#7DE0D7; }
            QLabel#eyebrow {font-size:11px; color:#91A5AA; font-weight:700; letter-spacing:1px;}
            QLabel#muted { color:#AAC0C4; }
            QLabel#heading { font-size:22px; font-weight:800; }
            QPushButton { background:#293B44; border:1px solid #42616B; border-radius:7px; padding:9px; }
            QPushButton:hover { background:#35505B; }
            QPushButton:pressed { background:#3C777C; }
            QPushButton#primary { background:#63D3C4; color:#0C2024; font-weight:800; }
            QPushButton#nav { text-align:left; border:0px; background:transparent; padding:12px; }
            QPushButton#nav:hover { background:#27414A; }
            QPushButton#pad {min-width:42px; font-weight:700;}
            QPushButton#pad[active="true"] { background:#7DE0D7; color:#091A1B; }
            QLineEdit, QComboBox, QTextEdit {background:#0D171C; border:1px solid #46636D; border-radius:7px; padding:8px;}
            QTextEdit { font-family:monospace; font-size:12px; }
            QScrollArea { border:0; }
        ''')

    def controller_page(self):
        area = QWidget()
        col = QVBoxLayout(area)
        col.setSpacing(12)
        col.addWidget(QLabel("Controller"), 0)
        controls = QFrame()
        controls.setObjectName("surface")
        c = QVBoxLayout(controls)
        row = QHBoxLayout()
        self.host_field = QLineEdit(self.worker.host)
        self.host_field.setPlaceholderText("Adapter IP or hostname")
        row.addWidget(QLabel("Adapter:"))
        row.addWidget(self.host_field, 1)
        connect = QPushButton("Connect")
        connect.clicked.connect(self.connect_device)
        row.addWidget(connect)
        c.addLayout(row)
        input_row = QHBoxLayout()
        self.source = QComboBox()
        self.source.addItems(("Keyboard", "PC gamepad"))
        self.source.currentIndexChanged.connect(self.change_source)
        input_row.addWidget(self.source, 1)
        self.take = QPushButton("Take control")
        self.take.setObjectName("primary")
        self.take.clicked.connect(self.toggle_control)
        input_row.addWidget(self.take)
        release = QPushButton("Release")
        release.clicked.connect(lambda: self.worker.command("release"))
        input_row.addWidget(release)
        c.addLayout(input_row)
        self.pad_name = QLabel("Gamepad: detecting...")
        self.pad_name.setObjectName("muted")
        c.addWidget(self.pad_name)
        self.status_text = QLabel("Ready. Connect, choose an input source, and take control.")
        self.status_text.setWordWrap(True)
        c.addWidget(self.status_text)
        col.addWidget(controls)
        self.pad = ControllerPanel(self.set_held)
        col.addWidget(self.pad, 1)
        row = QHBoxLayout()
        remap = QPushButton("Keyboard mappings")
        remap.clicked.connect(self.edit_mappings)
        row.addWidget(remap)
        next_pad = QPushButton("Next gamepad")
        next_pad.clicked.connect(lambda: self.update_controller_name(True))
        row.addWidget(next_pad)
        web = QPushButton("Open device WebUI")
        web.clicked.connect(self.open_web)
        row.addWidget(web)
        col.addLayout(row)
        hint = QLabel("F8: take/release   ·   Esc: release   ·   Z / C: White / Black   ·   Focus loss releases")
        hint.setObjectName("muted")
        col.addWidget(hint)
        return area

    def device_page(self):
        pane = QWidget()
        col = QVBoxLayout(pane)
        h = QLabel("Device status")
        h.setObjectName("heading")
        col.addWidget(h)
        row = QHBoxLayout()
        refresh = QPushButton("Refresh status")
        refresh.clicked.connect(lambda: self.worker.command("refresh"))
        row.addWidget(refresh)
        web = QPushButton("Advanced settings in WebUI")
        web.clicked.connect(self.open_web)
        row.addWidget(web)
        col.addLayout(row)
        self.device_info = QTextEdit()
        self.device_info.setReadOnly(True)
        self.device_info.setPlainText("Waiting for adapter...")
        col.addWidget(self.device_info, 1)
        return pane

    def profiles_page(self):
        pane = QWidget()
        col = QVBoxLayout(pane)
        h = QLabel("Controller profiles")
        h.setObjectName("heading")
        col.addWidget(h)
        text = QLabel("View profiles here. Use the device WebUI to edit, calibrate or save them.")
        text.setWordWrap(True)
        col.addWidget(text)
        row = QHBoxLayout()
        b = QPushButton("Load profiles")
        b.clicked.connect(lambda: self.worker.command("profiles"))
        row.addWidget(b)
        b = QPushButton("Open profile editor")
        b.clicked.connect(self.open_web)
        row.addWidget(b)
        col.addLayout(row)
        self.profiles_text = QTextEdit()
        self.profiles_text.setReadOnly(True)
        col.addWidget(self.profiles_text, 1)
        return pane

    def help_page(self):
        pane = QWidget()
        col = QVBoxLayout(pane)
        h = QLabel("Help & diagnostics")
        h.setObjectName("heading")
        col.addWidget(h)
        col.addWidget(QLabel("The app stops sending held inputs on focus loss. The firmware also neutralizes stale input."))
        col.addWidget(QLabel("The old ESP8285 shares one AT/UART channel. No concurrent HTTP status polling during play."))
        self.log = QTextEdit()
        self.log.setReadOnly(True)
        col.addWidget(self.log, 1)
        return pane

    def message(self, text):
        self.status_text.setText(text)
        self.log.append(time.strftime('%H:%M:%S') + "  " + text)

    def error(self, text):
        self.status_text.setText(text)
        self.log.append(time.strftime('%H:%M:%S') + "  ERROR  " + text)

    def show_device(self, doc):
        self.connection.setText("●  Connected" if isinstance(doc, dict) and doc.get("ok") is not False else "●  Check adapter")
        self.device_info.setPlainText(json.dumps(doc, indent=2))
        if isinstance(doc, dict):
            details = doc.get("network", {})
            if isinstance(details, dict):
                ip = details.get("station_ip") or details.get("ap_ip") or ""
                if ip: self.connection.setText("●  " + str(ip))

    def show_profile(self, doc):
        self.profiles_text.setPlainText(json.dumps(doc, indent=2))

    def show_control(self, state):
        self.active = state == "active"
        self.take.setText("Release control" if self.active else "Take control")
        if state == "reconnecting":
            self.connection.setText("●  Reconnecting (neutral)")
        elif state == "failed":
            self.connection.setText("●  Session unavailable")

    def set_held(self, action, down):
        if down: self.held.add(action)
        else: self.held.discard(action)

    def change_source(self, *_args):
        self.clear_buttons()
        if self.active:
            self.worker.command("release")

    def clear_buttons(self, *_args):
        self.pressed.clear()
        self.held.clear()
        self.worker.snapshot(PadState(), self.focused)

    def connect_device(self):
        try:
            host = device_url(self.host_field.text())
        except ValueError as exc:
            self.error(str(exc))
            return
        self.clear_buttons()
        self.prefs["host"] = host
        self.persist()
        self.worker.command("connect", host)

    def toggle_control(self):
        if self.active:
            self.worker.command("release")
        elif self.source.currentIndex() == 1 and self.gamepad.controller is None:
            self.error("No compatible PC gamepad is connected; choose keyboard or plug in a gamepad.")
        else:
            self.clear_buttons()
            self.worker.command("take")

    def update_controller_name(self, next_device=False):
        name = self.gamepad.refresh(next_device)
        self.pad_name.setText("PC gamepad: " + name)

    def poll_controls(self):
        if self.source.currentIndex() == 1:
            state = self.gamepad.state()
        else:
            snapshot = keyboard_snapshot(self.pressed, self.bindings, self.held)
            state = PadState(**snapshot)
        self.pad.update_state(state)
        # Only one most recent state crosses to the worker; no input backlog.
        self.worker.snapshot(state if self.active else PadState(), self.focused)

    def persist(self):
        self.prefs["bindings"] = self.bindings
        with contextlib.suppress(OSError):
            save_prefs(self.prefs)

    def edit_mappings(self):
        dialog = BindingsDialog(self.bindings, self)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            self.bindings = dialog.bindings
            self.clear_buttons()
            self.persist()

    def open_web(self):
        try:
            url = device_url(self.host_field.text())
            QDesktopServices.openUrl(QUrl(url))
        except ValueError as exc:
            self.error(str(exc))

    def eventFilter(self, obj, event):
        if event.type() == QEvent.Type.ApplicationDeactivate:
            self.focused = False
            self.clear_buttons()
            self.worker.command("release")
        elif event.type() == QEvent.Type.ApplicationActivate:
            self.focused = True
        if event.type() in (QEvent.Type.KeyPress, QEvent.Type.KeyRelease):
            key = key_name(event.key())
            focus = QApplication.focusWidget()
            if isinstance(focus, KeyCapture):
                return False
            if key == "F8" and event.type() == QEvent.Type.KeyPress and not event.isAutoRepeat():
                self.toggle_control()
                return True
            if key == "Escape" and event.type() == QEvent.Type.KeyPress:
                self.worker.command("release")
                self.clear_buttons()
                return True
            # Don't steal keystrokes from editing host/IP and keyboard mapping.
            if isinstance(focus, (QLineEdit, QComboBox, QTextEdit)):
                return False
            if key and not event.isAutoRepeat():
                if event.type() == QEvent.Type.KeyPress: self.pressed.add(key)
                else: self.pressed.discard(key)
        return super().eventFilter(obj, event)

    def closeEvent(self, event):
        self.clear_buttons()
        self.worker.stop()
        if not self.worker.wait(10000):
            self.message("The network worker is finishing a pending request. Close the window again after it stops.")
            event.ignore()
            return
        self.gamepad.quit()
        self.persist()
        super().closeEvent(event)


def run_gui(host: str, rate: int = 60) -> int:
    app = QApplication.instance() or QApplication(sys.argv)
    window = RemoteWindow(host, rate)
    window.show()
    return app.exec()
