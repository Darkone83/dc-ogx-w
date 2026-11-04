# DC-OGX-W software architecture

DC-OGX-W separates the RP2040 controller/output runtime from network/desktop input, status display and configuration. The source is under **`src/src/`**; the firmware CMake project is **`src/`**.

| Component | Source | Role |
|---|---|---|
| Native USB device and console modes | `src/src/USBDevice/` | Emit console-compatible USB output; preserve neutral output on disconnect |
| Remote control & profiles | `src/src/RemoteControl/`, `src/src/UserSettings/` | Exclusive ownership, input shaping, calibration and stored preferences |
| Network service | `src/src/NetworkService/` | HTTP/WebSocket API, configuration, ESP8285 AT or native CYW43 transport |
| WebUI | `src/src/NetworkService/web/index.html` | Embedded offline interface; browser-local themes and keyboard bindings |
| OLED / LED | `src/src/Display/`, `src/src/Board/` | Status and diagnostics without blocking controller processing |
| Firmware updater | `src/src/FirmwareUpdate/` | Board-specific validated staged update; protected flash layout |
| Wired controller host (experimental) | `src/src/USBHost/` | PIO-USB input on GP9/GP10, separately enabled via CMake integration |
| Desktop clients | `apps/Windows/src/`, `apps/Python/` | Tauri/Rust and PySide6 consumers of the same network protocol |

## Input and protocol boundaries

The ESP8285 clone uses legacy AT firmware over UART0. Its command channel is serialized and has limited concurrent socket support, so noncritical station maintenance, UDP discovery and display activity must never hold controller work indefinitely. The official Pico W uses CYW43439 and can additionally support Bluetooth. Features must be detected per board rather than inferred from the other variant.

USB device output and network input have different timing requirements. Stale controller snapshots must be neutralized and not replayed. A remote lease must be released or expire reliably, even if the browser/desktop client disappears. Flash saving and firmware installation are deferred until response/maintenance safeguards are satisfied; the updater is **not** power-failure-proof A/B.

## Source and migration status

The uploaded snapshot contains core host runtime but not its final CMake wiring. The maintainer has separately verified that a newer PIO-USB CMake build compiles and will integrate it manually. Similarly, network/UDP/Core0 changes made after this ZIP should not be assumed present until merged. Keep board-specific builds and verify software behavior on real hardware before publishing images.

## Reuse rules

- Keep the self-contained WebUI and OLED glyphs reusable, but preserve stable API paths and hardware-specific capability checks.
- Avoid extra UI polling and blocking or infinite retries on the legacy ESP AT connection.
- Keep wired input, native Bluetooth and network input separate at the transport boundary; normalize controller state before profile shaping.
- Do not remove upstream OGX-Mini, Pico SDK, TinyUSB, BTstack, PIO-USB or other third-party notices when redistributing linked code.
