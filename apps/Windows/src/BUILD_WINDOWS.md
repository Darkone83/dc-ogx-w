# Building the Windows companion 0.1.5

Required: Windows 10/11 x64, Node.js LTS, Rust stable for `x86_64-pc-windows-msvc`, Visual Studio Build Tools (Desktop development with C++, Windows SDK), and Microsoft Edge WebView2 Runtime.

From PowerShell in the source folder:

```powershell
.\build-portable.ps1
```

The script uses `npm.cmd ci` and the checked-in Cargo lockfile. Tauri/CLI versions are pinned; the portable directory override needs Tauri 2.12 or newer. HTML/CSS/JS are embedded in the executable. No installer is generated. The output is `dist-portable\DC-OGX-W.exe` with a short README.

For development or a manual build:

```powershell
npm.cmd ci
npm.cmd run dev
npm.cmd run build
```

For regression checks:

```powershell
npm.cmd run check:ui
npm.cmd run check:core
```

`check:core` needs Rust but not the desktop SDK. It includes actual HTTP/WebSocket transport tests on loopback. For the complete Windows project, also run `cargo test --manifest-path src-tauri/Cargo.toml --locked` with the Windows build tools installed.

Run the finished EXE from a writable folder, not Program Files. Its neighbouring `app-data` directory holds preferences and WebView2 storage. WebView2 is a separate runtime and is not bundled in the portable EXE. Existing portable preferences remain compatible.

Device smoke test: connect by IP and discovery; confirm board/version/profile; select Wi-Fi remote; try native XInput, keyboard White/Black and the Standard gamepad fallback; check both sticks and triggers; test focus loss, controller reconnect, Release, disconnect and app close. The fallback only accepts standard-mapped controllers exposed by Windows/WebView2. Configuration mode is a settings state; Physical includes USB and supported Bluetooth. Test Pico W, Pico 2 W and clone firmware as those boards become available.
