DC-OGX-W Windows Companion - Portable Edition v0.1.3
====================================================
Run DC-OGX-W.exe from a writable folder on Windows 10/11 x64.
Microsoft Edge WebView2 Runtime is required; it is usually already installed.
No Python, Node.js or Rust installation is required to run the finished EXE.

The app creates an app-data folder next to its executable for local UI preferences
and WebView2 storage. Keep this folder if you move the EXE to another location.

Device discovery uses UDP port 38473 on official Pico W and ESP8285
Pico W-2023 boards when their firmware includes UDP discovery. Manual IP entry
remains available as a fallback. Network mode must be Wi-Fi remote
before taking control. The XInput path supports 4 controller indices.

The desktop v0.1.3 is an initial test build: controller sessions, saved-profile
selection, OLED/RGB settings, and Wi-Fi configuration are supported. Full
profile editing, calibration, and firmware uploads remain in the firmware WebUI.
