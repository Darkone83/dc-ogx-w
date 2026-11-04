# Reusable software foundations — dc-ogx-w

This pass deliberately separates **what can be shared now** from what must await new hardware. Its purpose is to make dc-ogx-w a stable reference firmware that can be forked for future OGX boards without treating other boards as active compile targets.

## Current boundaries

| Subsystem | State | Intended reusable contract |
|---|---|---|
| Device-side controller output | Existing USB device implementation | Neutral output on source disconnect; do not block USB loop |
| ESP8285 AT transport | Existing, stabilized path | Serializes HTTP/control commands; avoid extra polling |
| Official Pico W radio | Existing network/BT implementation | Different capabilities from ESP8285 clone |
| OLED display | Implemented 1-bit framebuffer, glyph atlas, activity priority | Publish status via atomics, render in service context, no I²C in USB loop |
| WebUI | Implemented responsive menu, themes, local gamepad preview | Keep tab API IDs stable, offline assets, color tokens, no extra HTTP requests |
| USB-A PIO host (GP9 D+/GP10 D−) | **Planned, not yet enabled** | Wired gamepad input, same profile processing & ownership rules |
| Windows UDP controller input | **Future separate Windows application** | Exclusive negotiated input ownership, sequenced whole-state packets |

## Why the browser preview is local

The original ESP8285 board has limited AT command concurrency. The Profiles editor now uses the *browser host's* Gamepad API as input for live stick/trigger comparison, with local preview shaping. This avoids introducing continuous API polling. It is ideal for tuning while the controller is attached to the PC, but does **not** represent controller input delivered directly to the RP2040. When native USB host support arrives, a device telemetry channel can be added behind an explicit capability flag; do not expose unsupported controls simply because an unrelated board supports Bluetooth.

## Input arbitration — next framework stage

One input source will own Xbox output at a time: browser WebSocket, Bluetooth controller (where supported), planned wired host, or eventual Windows UDP session. Acquisition must explicitly neutralize the previous owner's state and release stale buttons. A lost connection must neutralize in a bounded timeout. UI should always show the **actual owner**, and network failure must not imply USB is unavailable. The source-specific reader should translate input into a common normalized controller snapshot before profile shaping and USB output.

The initial version of this framework **does not replace the existing controller core**. It stages future host board pin definitions and UI capability directions without taking ownership away from proven live paths.

## Reuse recommendations

1. Keep `src/Display/OledGlyphs.h` and general OLED framebuffer/utilities reusable; keep product logo and controller-specific status in the product layer.
2. Keep WebUI appearance tokens separate from network calls; reuse the same offline sidebar/drawer shell and remove unsupported destinations according to device capabilities.
3. Expose input source capabilities explicitly (Bluetooth, wired USB host, remote WebSocket, UDP) instead of using Bluetooth capability as a synonym for physical controller presence.
4. Add real board/runtime integration tests before expanding the set of board targets.
5. Preserve upstream OGX-Mini, Pico SDK, TinyUSB and BTstack credits and applicable license notices.
