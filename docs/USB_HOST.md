# Wired PIO-USB host — GP9 / GP10

DC-OGX-W has a planned external controller USB **host input** on RP2040 **GP9 (D+) / GP10 (D−)**. The native RP2040 USB connector remains **USB device output** toward the console/host; this is controller translation, not a transparent electrical passthrough.

## Software status and source locations

The uploaded repository contains the host runtime, including `src/src/USBHost/WiredHost.cpp`, its header, TinyUSB host callbacks, host drivers and board integration. A later CMake integration was proven to **compile successfully** on the maintainer's Pico W-2023 system. **Host-controller enumeration and long-run hardware stability have not yet been verified.**

**Migration required:** the ZIP used for this documentation pass still contains the old `src/cmake/options.cmake` with PIO host disabled and a deliberate CMake error if enabled. Its `src/CMakePresets.json` lacks the `-pio` presets. The maintainer will manually bring across the successful one-click CMake integration. The cleanup does **not** change the firmware build sources or claim that those new presets are already in the ZIP.

After migration, select the configured `pico-w-2023-pio` or `pico-w-pio` test preset using the Pico VS Code extension and click **Compile**; separate scripts are not required. Keep the normal board presets available for rollback. If the PIO preset name differs in your final merged tree, use the names actually defined in its `CMakePresets.json`.

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
