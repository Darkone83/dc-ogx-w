# Contributing to DC-OGX-W

Controller safety, radio stability and repeatable builds take priority over adding features. Please submit focused changes and state what was verified on actual hardware rather than relying solely on simulations.

## Project locations

- Firmware and Pico SDK/CMake project: **`src/`**; C++ implementation under `src/src/`.
- Embedded WebUI: `src/src/NetworkService/web/index.html`.
- Desktop applications: `apps/Windows/src/` (Tauri/Rust) and `apps/Python/` (PySide6).
- PCB design and case: `PCB/` and `STL/`.

## Building

With Pico SDK 2.3.0, ARM toolchain, Git, Python, CMake and Ninja available, open `src/` in the Pico VS Code extension or run from that directory:

```powershell
cmake --preset pico-w-2023
cmake --build --preset pico-w-2023 --parallel
```

Use `pico-w` for official Pico W. The firmware packaging script is included at `src/tools/firmware/make_update.py` and runs automatically after linking. Wired PIO-USB host integration is present and enabled by default; the `-pio` presets provide separate build directories for hardware testing. USB-host functionality still requires physical validation. See the [build guide](docs/BUILD_AND_FLASH.md).

## Engineering expectations

- Preserve native USB device output and the input neutralization/deadman safety behavior; never replay ambiguous or stale remote control packets.
- ESP8285 support must respect the installed legacy AT firmware and limited serialized UART command channel. Avoid blocking Core0 for HTTP, UDP, OLED or flash activity.
- Do not move or reuse GP0/GP1 (ESP clone UART), GP8 (RGB), GP9/GP10 (PIO host) or GP14/GP15 (OLED) without an explicit board revision.
- Verify USB host power switching/current limiting and pull-downs before connecting a physical controller.
- Keep embedded WebUI resources self-contained; avoid extra HTTP requests for theme assets. Maintain mobile accessibility and full original Xbox button mapping.
- Preserve third-party source notices and licenses. Do not include passwords, personal IP information or console identifiers in bug reports.

When contributing, run applicable tests available in your development environment and report real-board behavior: board variant, firmware/source version, modem firmware if ESP8285, exact reproduction steps, network topology, logs and whether controller output was neutralized.

See [security](SECURITY.md) and [credits](CREDITS.md).
