# Contributing to DC-OGX-W

Thanks for helping improve the DarkoneCustoms adapter. The project is firmware for resource-constrained hardware, so **correctness, controller safety, and radio stability** take priority over feature count.

## Before opening an issue

Search existing issues. Include the board (**Pico W-2023 ESP8285** or **official Pico W**), the `dc-ogx-w` source version, the ESP-AT version if applicable, the WebUI access mode (SoftAP or station), and clear steps to reproduce. For a UI report, include the browser, selected Appearance theme and a screenshot. Do **not** post Wi-Fi passwords, private keys, or other credentials.

## Development environment

Use **Pico SDK 2.3.0**, CMake, Ninja, Git, Python and an Arm cross-compiler. Build each board in its **own preset build directory**:

```cmd
cmake --preset pico-w-2023 && cmake --build --preset pico-w-2023 --parallel
```

or

```cmd
cmake --preset pico-w && cmake --build --preset pico-w --parallel
```

Do not overwrite `boards/pico_w_2023.h` with an official Pico W definition. Keep the clone's **GP0/GP1 ESP UART**, **GP8 WS2812** and **GP14/GP15 OLED** allocations intact. **GP9/GP10 USB host** is only staged as a future hardware feature and must not be turned on without USB-power-path and PIO-USB scheduling validation.

## Code changes

- Keep controller-state updates bounded and nonblocking. Network work must not stall the USB output loop; partial controller packets must never leave input indefinitely pressed.
- Preserve the **neutral-input safety timeout** and exclusive ownership semantics. A dropped network connection must never replay stale controller inputs.
- Do not add an ESP8285 AT command while holding a WebSocket/HTTP/remote-control lock. Keep background polling and I²C updates bounded.
- The WebUI is one self-hosted, embedded HTML file at `src/NetworkService/web/index.html`. **No third-party CDN, external runtime font or Javascript dependency.** Theme selection must remain browser-local.
- OLED assets should be legible at **128×32**, not merely attractive when upscaled. Keep monochrome glyphs small and avoid full-screen redraws on every controller frame.
- Match upstream library and source notices; never remove another contributor's license or credits.

## Host testing

The native tests can be run without the Pico toolchain on a desktop with Python and `g++`:

```sh
python tests/display/run.py
python tests/esp_at/run.py --sanitize
python tests/network/run.py
python tests/discovery/run.py
python tests/firmware_update/run.py
```

For UI changes, run `python tests/web/test_branding.py` and `python tests/web/test_layout.py` (optional Playwright/Chromium dependency), and manually verify **all eight themes** on desktop and narrow/mobile layouts. The browser Gamepad API preview requires a gamepad connected to the browser host; it is not yet adapter-side telemetry. For OLED changes, test the actual 0.91-inch panel in addition to host framebuffer tests. For controller/network changes, test with a live console and both SoftAP and saved-network operation as appropriate.

## Pull requests

Describe the problem, the smallest fix, affected board(s), native-test commands, real-device observations, and any remaining known limitations. One focused change per PR is easier to review. Add or update README and credits when changes affect features, dependencies or installation.

The original software includes multiple licenses with different conditions. Review [LICENSE](LICENSE) and [CREDITS.md](CREDITS.md) before submitting code copied from another project or packaging releases.
