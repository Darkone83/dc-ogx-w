# Wired PIO-USB host — GP9 / GP10

DC-OGX-W implements external controller USB **host input** using RP2040 PIO on **GP9 (D+) / GP10 (D−)**. The native RP2040 USB connector remains **USB device output** toward the console/host; this is controller translation, not a transparent electrical passthrough.

## Software status and source locations

The repository includes `src/src/USBHost/WiredHost.cpp`, TinyUSB host callbacks, controller drivers, and the required host build integration. The PIO-enabled source has compiled successfully, but **controller enumeration, input mapping, hot-plug behavior and long-run stability still require physical testing.**

**Build configuration:** `DC_OGX_USB_HOST_ENABLED` is `ON` by default in `src/cmake/options.cmake`, and `src/CMakeLists.txt` links the PIO USB host targets. The standard `pico-w-2023` and `pico-w` presets therefore include host support. The dedicated `pico-w-2023-pio` and `pico-w-pio` presets also enable it explicitly and use separate build directories for bring-up. Select the correct preset in the Pico VS Code extension and click **Compile**; no separate build script is needed.

## Electrical and timing considerations

- Confirm D+/D− physical polarity on GP9/GP10, host-side data pull-downs and proper 5 V VBUS supply/switching/current protection before plugging in a controller.
- Verify the external port does not backfeed the RP2040 or the console. Host is not supplied by GPIO pins.
- Confirm PIO state-machine allocation with the WS2812 status LED, interrupt priorities, 1 kHz USB frame timing and controller latency under network load.
- Test wired-controller disconnect, stale input neutralization, source arbitration, wired/Bluetooth takeover where supported and native USB output continuity.
- Start with a low-power wired controller; avoid hubs/high-draw accessories until the power path is tested.

## Suggested bring-up order

1. Confirm the PIO-enabled target links and produces `.uf2` and matching `.ogxw` without violating the OTA image size limit.
2. Flash by USB/BOOTSEL and check no regression in WebUI, OLED, LED and output device enumeration.
3. Connect a known low-power controller; verify TinyUSB host mount/unmount and normalized button/axis reports.
4. Confirm profile selection, Xbox White/Black, triggers, controller handoff and neutral output when the cable is unplugged.
5. Repeat alongside Wi-Fi input and extended sessions on both board variants.

Do not declare PIO-USB production-ready based solely on a compiler success message.
