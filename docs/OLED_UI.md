# OLED interface — 128×32 SSD1306

## Electrical/runtime integration

Target panel: **0.91-inch, 128×32 pixels, 1-bit SSD1306**, I²C1 on GP14 SDA and GP15 SCL, address 0x3C by default (0x3D optional). The OLED is an optional accessory; absent-panel probing is rate-limited to every 10 seconds. The RP2040's controller/USB core **does not draw or make I²C calls**. A network/service-side task renders into a 512-byte framebuffer and sends bounded 16-byte I²C data chunks with pauses.

## Visual assets and reusable glyphs

- Startup DXC emblem: pixel-designed 64×24 monochrome mark, kept independent of desktop PNG complexity.
- Reusable atlas: [`src/Display/OledGlyphs.h`](../src/Display/OledGlyphs.h), eleven custom 16×16 icons for Wi-Fi, USB, Bluetooth, profile, controller, save, load, settings, update, success and warning. These are small row masks, not external font libraries.
- The existing rasterized DejaVu-based text tables remain for numerals/statuses and IP.
- The glyph atlas can be copied into future OGX projects; the event-specific screen rendering can remain project-specific.

## Screen hierarchy and behavior

Normal screen sequence: DXC splash on boot → IP (longest visible) → profile/output state → network state. The full IPv4 address remains one consistent font. Operation overlays take priority over rotation: accepted load/apply/save actions show an appropriate glyph and a slow four-dot animation, then the screen returns automatically to the IP display. Firmware RX/install/ready/error supersede ordinary notices and include a 5%-quantized progress display when applicable. The animation advances approximately every 420 ms; it never runs from an IRQ or USB polling function.

## Device → OLED display (runtime controls)

The WebUI **Device** tab now includes settings read from the firmware rather than browser-only placeholders. Controls:

| Setting | Supported values | Runtime behavior |
|---|---|---|
| Display enabled | On / Off | Uses SSD1306 display on/off; does not cut module power |
| Brightness | 0–255 | Writes hardware contrast using `0x81`; WebUI also shows 0–100% for convenience |
| Dim after inactivity | Never, 30 s, 60 s, 120 s, 300 s, 600 s, 900 s, 1800 s, 3600 s | Moves toward idle contrast after the selected time |
| Idle brightness | 0–255 | Contrast target after the dim timeout |
| Screen off after inactivity | Same timeouts | Sends `0xAE` display-off and wakes on subsequent meaningful activity |
| Invert pixels | On / Off | SSD1306 `0xA7`/`0xA6` |
| Rotate 180° | On / Off | SSD1306 segment remap and COM direction; no redraw on USB core |
| Fade in on startup | On / Off | Short incremental hardware-contrast transition |

Inactivity time is reset by accepted OLED/profile/network/LED activity notices, USB/profile/status changes, firmware progress, and applying OLED preferences. Ordinary API polling does **not** continuously wake the display. The display continues to be optional; the WebUI reports whether it was detected.

**Apply now** changes settings in RAM and updates the display without a reboot. **Save and restart** stages the complete settings record, transmits the HTTP reply, then commits a CRC-checked `oled-v1` NVS record from Core0 before restarting. On an abandoned response, staging is canceled to avoid unexpected flash writes. **Defaults** loads board-specific firmware build defaults into the form and requires Apply or Save to take effect. Flash changes are not issued from the I²C service or the USB polling path.

The original CMake settings still provide *first-boot defaults*, before any saved `oled-v1` record overrides them:

| CMake variable | Default | Meaning |
|---|---|---|
| `DC_OGX_OLED_CONTRAST` | 224 | Main SSD1306 contrast, 0–255 |
| `DC_OGX_OLED_FADE_ENABLED` | ON | Enable initial contrast fade |
| `DC_OGX_OLED_IDLE_DIM_MS` | 0 | Initial dim timeout; non-preset values round up to the next WebUI option |
| `DC_OGX_OLED_IDLE_CONTRAST` | 32 | Initial idle contrast |

```cmd
cmake --preset pico-w-2023 -DDC_OGX_OLED_CONTRAST=192 -DDC_OGX_OLED_IDLE_DIM_MS=300000 && cmake --build --preset pico-w-2023 --parallel
```

See [`remote-api.md`](remote-api.md) for GET `/api/oled`, POST `/api/oled/apply`, and POST `/api/oled/save`. Native host tests verify framebuffer scheduling, activity overlays, fade, idle-dim/off behavior, wake and NVS integrity; these are **not a substitute for a physical OLED test**.

## Acceptance checks before PCB release

1. Verify OLED probe/missing-device behavior and startup text on the exact 0.91-inch part.
2. Verify GP14/GP15 bus recovery after disconnect/reconnect.
3. Confirm rendering remains smooth under sustained ESP8285 networking and Xbox USB operation.
4. Verify updates and saves preempt normal display without falsely claiming success.
5. Test contrast at night/dark room for burn-in/eye comfort; test bright ambient visibility.
6. Verify dim wake behavior with connected/controlled input, OLED module power and profile operations.
