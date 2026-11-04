# Release checklist — DC-OGX-W

This is a **documentation-aligned source snapshot**, not a hardware-qualified release. The firmware project version in this ZIP is `0.2.4`. The supplied `firmware/` images and `apps/Windows/bin/` executable are preserved without rebuilding and may represent earlier local revisions.

## Source integration

- [ ] Merge the latest maintainer-approved network/UDP/Core0 firmware patches into `src/src/` as intended.
- [ ] Merge the successful PIO-USB CMake integration into `src/CMakeLists.txt`, `src/cmake/` and `src/CMakePresets.json` where appropriate; keep non-PIO presets for rollback.
- [ ] Confirm `src/tools/firmware/make_update.py` exists locally and generates checked `.ogxw` images.
- [ ] Verify all targets compile using VS Code **Compile** without an extra build workaround and with the intended SDK/toolchain versions.
- [ ] Check both board target outputs fit their linker flash limits and name/version labels match the board firmware image.

## Physical hardware validation

- [ ] ESP8285 clone boots with its existing legacy AT firmware; SoftAP and station remain stable during extended use.
- [ ] HTTP and WebSocket clients retain an active controller session; no unexplained `503 service_busy`, red recovery flashing or stale pressed buttons.
- [ ] UDP discovery (if present in merged firmware) works from Windows without depending on newer modem firmware features.
- [ ] Official Pico W Wi-Fi/Bluetooth input works and changes owner cleanly.
- [ ] Native USB device output works and controller mode/profile mapping is correct.
- [ ] Optional PIO-USB host on GP9/GP10 safely enumerates a low-power controller with protected 5 V VBUS; verify host disconnect and latency.
- [ ] OLED brightness, rotation, fade, idle dim/off and operation notices work on the physical 0.91-inch 128×32 display.
- [ ] RGB LED status reliably reflects genuine failures, not only delayed HTTP responses.
- [ ] Firmware updater validates board identity and can install a checked `.ogxw` with BOOTSEL recovery available.

## Packaging and documentation

- [ ] Keep both board `.uf2`/`.ogxw` files in the `firmware/` folder and verify checksums/provenance before replacing older files.
- [ ] Check the Windows binary against Windows companion source and confirm Python automatic dependency installation on supported platforms.
- [ ] Confirm the public README matches the **actual merged** CMake configuration, no stale links remain and the required updater utility is included.
- [ ] Retain root `LICENSE`, credits and third-party notices for every linked dependency, including optional USB host.
- [ ] Do not ship developer test harnesses or stale build output in the release archive; keep tests in a separate development checkout if required.

Compiling successfully is necessary but does not prove Wi-Fi stability, USB host electrical safety or a two-hour play session.
