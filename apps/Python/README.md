# DC-OGX-W Python Remote — PySide6 edition

Python 3.10+ companion for Windows, Linux and macOS. **The ESP8285 modem/firmware is not changed.** This is a separate PC-only package. A portable native Windows Tauri/Rust app is maintained separately.

## Quick start — automatic dependencies

1. Copy this entire folder to your computer. Connect the computer to the internet for the first launch (before joining the adapter SoftAP).
2. Windows: double-click `start-remote.cmd`; Linux: `./start-remote.sh`; macOS: double-click `start-remote.command` (or run it from Terminal). You can also run `python3 ogx_remote.py` directly.
3. The launcher checks installed modules. If aiohttp, PySide6 or pygame are missing, it creates **`.venv-ogx/` in this folder**, installs them into that private environment using pip, and relaunches the app. There is no system-wide pip installation. Later launches reuse the installed environment. An active internet connection is required for the first dependency download.
4. Enter the adapter IP/hostname and click Connect. Choose Keyboard or PC gamepad, then Take control. **F8** toggles and **Esc** releases. Focus loss releases the lease and sends neutral input. White = Z and Black = C by default; use Keyboard mappings to change them.

On Linux, `python3-venv` and your distribution's Qt/XCB/Wayland system libraries may be required. On macOS, use a current Python.org or Homebrew Python with pip/venv. `PySide6` wheels are installed for your platform by pip. The SDL gamepad backend uses a hidden pygame window; if SDL fails to initialize, keyboard input remains available. If you only need HTTP scripts or the local WebUI bridge, those modes install only aiohttp.

## Supported commands

```sh
python3 ogx_remote.py                          # PySide6 desktop, default
python3 ogx_remote.py --host 192.168.1.40 desktop
python3 ogx_remote.py --host 192.168.1.40 web
python3 ogx_remote.py --host 192.168.1.40 status
python3 ogx_remote.py --host 192.168.1.40 tap a --duration 0.15
python3 ogx_remote.py --host 192.168.1.40 desktop-sdl  # older pygame window
```

`RemoteClient`, `PadState`, CLI input scripts and the localhost WebUI bridge are preserved. Historical protocol tests are not bundled in this cleaned release. The PySide6 desktop has a keyboard/controller visualization, remapping, gamepad input, status, device and profile views, and opens the firmware WebUI for advanced configuration. The GUI **does not clone the complete WebUI profile editor**.

The app keeps all HTTP and WebSocket networking off the Qt GUI thread. It sends complete fresh snapshots, uses existing firmware ownership/heartbeat semantics, and does not queue or replay stale input across disconnects. Idle device status requests are performed only while not playing, to minimize AT command contention on the legacy modem.

Preferences: Windows `%APPDATA%/dc-ogx-w/python-remote.json`, Linux `~/.config/dc-ogx-w/python-remote.json`, macOS `~/Library/Application Support/dc-ogx-w/python-remote.json`. CLI/API use the same original device endpoints.

## Development / troubleshooting

If dependency installation is blocked or the machine is offline, install requirements manually into an activated venv:

```sh
python3 -m venv .venv-ogx
.venv-ogx/bin/python -m pip install -r requirements.txt
.venv-ogx/bin/python ogx_remote.py
```

On Windows the venv interpreter is `.venv-ogx\\Scripts\\python.exe` instead. To force a clean reinstall, delete only the local `.venv-ogx` directory, not your profile data. The first launch was not tested here on actual Windows/macOS hardware, and this source ZIP is **not** a bundled native executable.
