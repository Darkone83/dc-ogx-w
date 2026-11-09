# DC-OGX-W Windows Companion 0.1.5

Portable Windows 10/11 x64 companion, built with Tauri 2 and Rust. This is the desktop app source; adapter firmware is a separate project.

## Build

Install Node.js LTS, Rust stable MSVC, Visual Studio C++ Build Tools with the Windows SDK, and Microsoft Edge WebView2 Runtime. From this folder in PowerShell:

```powershell
.\build-portable.ps1
```

The script installs locked dependencies and produces `dist-portable/DC-OGX-W.exe` without an installer. End users need WebView2 but do not need Node or Rust. Run from a writable folder; preferences and WebView2 data live beside the EXE in `app-data`. Keep that folder when updating the EXE.

## Controller compatibility

- **Keyboard + on-screen:** editable key bindings and the existing controller layout.
- **Native XInput:** low overhead Windows XInput capture, four controller slots. Keep this option for Xbox controllers and devices already exposing XInput.
- **Standard gamepad:** WebView2 Gamepad API fallback for controllers Windows exposes with a standard mapping. Press a button to expose the device, then choose its slot. Availability and layout depend on the Windows driver and WebView2; this is not a native HID driver or a guarantee for every USB/Bluetooth controller. Unmapped devices produce neutral input with an explanatory message. Real controller testing remains necessary.

White/LB remains bit `0x0100` (Z), Black/RB remains `0x0200` (C). Gamepad input uses +Y up, signed 16-bit sticks and independent 8-bit triggers. Full snapshots are sent at up to 60 Hz, respecting the adapter's advertised input rate. No application deadzone or response curve is added. A centre tolerance is used only to arm input after a pause or reconnect, so normal stick drift cannot prevent resuming.

Input neutralizes on focus loss, stale standard-gamepad samples, disconnect or release. Ownership is never automatically reacquired after a drop. Centre controls before resuming. Esc, F8 or Release sends neutral, releases the exact ownership token, closes the stream, and returns to **Physical (USB / Bluetooth)** input when supported. Disconnect and closing perform the same handback and wait for bounded cleanup. If the adapter is unreachable, its own input timeout and lease expiry still apply.

## Adapter compatibility

Remote API v2 is required. The app uses existing HTTP/WebSocket/UDP protocols and reads board capabilities, supporting compatible Pico W, Pico 2 W and ESP8285 clone firmware. Manual IP/hostname entry remains available if UDP discovery or mDNS is blocked. Allow the app on the Windows private network; UDP discovery uses port 38473.

The app preserves the current firmware operating modes: **Physical** covers wired USB and available Bluetooth input, **Wi-Fi remote** accepts desktop/browser control, and **Configuration** permits settings/calibration while console output is neutral. **Take control automatically requests Wi-Fi mode, waits for the firmware handoff to complete, then acquires ownership.** You do not need to select the mode first. Handoffs poll the small `/api/control/mode` reply; full status is not requested when streaming begins. If a mode response is lost, the app reads back state instead of repeating the mutation. A closed stream now reports its close code/reason when provided. A failed handback is reported; another client's active ownership is not cleared by a mode change. Simply connecting or releasing an idle app does not change modes. A clone without Bluetooth still has Physical USB support when advertised by its firmware. Switch to Configuration before changing profiles if the firmware requires it.

Features include discovery, live status, saved profile selection, OLED/RGB settings, Wi-Fi configuration, eight existing themes and portable preferences. The firmware WebUI remains the place for full profile editing, calibration and firmware upload. Linux/macOS continue to use the separate Python companion.

## Checks

```powershell
npm.cmd ci
npm.cmd run check:ui
npm.cmd run check:core
```

The core tests compile the actual transport/control modules without desktop dependencies and exercise real loopback HTTP/WebSocket sessions, exact-token release, acquisition cancellation, safe busy retries and neutral behavior. UI tests cover controller mapping, coalescing, hotplug, clone mode availability and stale status rejection. See `BUILD_WINDOWS.md` for Windows build requirements. These checks do not replace testing the packaged EXE and controllers on Windows.
