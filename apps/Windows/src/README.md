# DC-OGX-W Windows Companion — v0.1.4

A separately packaged, portable **Tauri 2 + Rust** Windows application for DC-OGX-W, using the firmware WebUI's actual styling, DXC branding, original eight themes, and controller layout. This package does **not** contain or alter the RP2040 firmware.

## Build

This folder is **source**; the repository also ships a separately copied executable at `../bin/dc-ogx-w.exe`. Its exact match to the current source has not been reverified in this cleanup. On Windows 10/11 x64 with Node.js, Rust stable MSVC, C++ Build Tools and WebView2 installed, extract it and run:

```powershell
.\build-portable.ps1
```

The resulting portable EXE is `dist-portable/DC-OGX-W.exe`; its preferences live in the neighboring `app-data` directory. Node and Rust are only required for compiling, not by end users. See `BUILD_WINDOWS.md`.

## WebUI parity and White/Black buttons

The **original Xbox White and Black buttons** now have visible, functional controls in the desktop controller page, with correct mapping **White = LB = 0x0100 (Z)** and **Black = RB = 0x0200 (C)**. Keyboard assignments are editable and persist locally. Windows XInput LB/RB support was already correct. The app displays the buttons, D-pad, sticks, face buttons, menu buttons, and triggers using the same layout elements as the firmware's own WebUI.

The existing WebUI's stylesheet and DXC logo are included. Its eight original color themes are retained; there are **no fake White and Black appearance themes**. Navigation follows Control, Profiles, Wi-Fi, LED, Device, with separate desktop-specific Connection and About views.

## Features

UDP discovery (including ESP8285 with a discovery-capable firmware), manual IP connection, HTTP configuration, safe WebSocket control session, keyboard and native XInput capture, profile selection, OLED/LED settings, Wi-Fi configuration, and compact locally saved preferences.

**Advanced WebUI-only features not yet ported:** full profile editing, Bluetooth calibration, advanced button remapping and firmware upload. Use the firmware WebUI for those operations.

Linux/macOS Python companion and firmware networking code are unchanged. See `CONTROLLER_WEBUI_PARITY_v0.1.4.md` for mapping and testing details.
