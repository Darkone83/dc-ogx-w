# Desktop companion applications

The desktop tools are independent of the RP2040 firmware source and communicate through the adapter's existing HTTP/WebSocket interfaces. The adapter must already be on the same reachable network as the computer.

## Windows — portable Tauri/Rust

- **Binary in this archive:** `apps/Windows/bin/dc-ogx-w.exe`.
- **Source project:** `apps/Windows/src/`; version in the supplied source manifests is **0.1.4**.
- Windows 10/11 requires the installed WebView2 runtime (normally present); Node.js, Rust and C++ Build Tools are **developer** dependencies only.
- Build from `apps/Windows/src/` using its existing `build-portable.ps1` (or `npm.cmd install` followed by `npm.cmd run build`). The source build emits a native executable that can be copied without a setup installer.
- Features include network discovery (requires compatible firmware), manual address entry, keyboard/XInput control, OG Xbox White/Black buttons, selected profile management and basic device settings. The original firmware WebUI remains the tool for the full advanced editor and updater.
- The binary is preserved from the uploaded archive, not rebuilt during the documentation cleanup. Verify that the binary and source version match before a public release.

## Linux/macOS (and Windows) — Python/PySide6

The complete source and launchers live in `apps/Python/`. Run `start-remote.sh` on Linux, `start-remote.command` on macOS, or `start-remote.cmd` on Windows. At first launch the bootstrap creates a local `.venv-ogx` and installs missing `aiohttp`, `PySide6` and `pygame`. An internet connection is needed for that first dependency download; users don't need to run `pip` manually.

`ogx_remote.py` also offers a CLI for status/control testing and a browser bridge. The PySide6 UI is the default desktop mode. The desktop interface is a companion, not a replacement for the complete on-device WebUI.

**Control-session safety:** an active client must maintain ownership and send fresh controller states; on focus loss or disconnect it should send/reach neutral and should not replay old held buttons. Long-duration connectivity remains a hardware validation requirement on legacy ESP8285 boards.

Details are in [Windows source README](../apps/Windows/src/README.md) and [Python README](../apps/Python/README.md).
