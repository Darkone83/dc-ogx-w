# dc-ogx-w — OLED and UDP discovery foundation

This source update builds on dc-ogx-w 0.2.4 plus the previously verified HTTP/WebSocket stability patch. It is an application-source change, not an ESP8285 firmware update.

## Optional OLED

- Supported first-pass display: SSD1306 128×64 I2C at address `0x3C` (128×32 and `0x3D` are selectable in CMake).
- SDA: RP2040 GP14 (I2C1), SCL: GP15 (I2C1). VCC: 3.3 V. GND: common.
- Existing WS2812 status pixel remains on RP2040 GP8. ESP8285 UART remains GP0 TX / GP1 RX.
- The screen shows a compact `DC/OGX-W` text identity, AP or STA IP, applied profile, USB output state, remote input state, and Wi-Fi status.
- Missing display: one bounded I2C probe and automatic retries every 10 seconds; device keeps operating. No I2C from the USB core.
- Rendering: a small 1,024-byte framebuffer for 128×64, with 16-byte I2C bursts paced across the network core. OLED refresh rate is intentionally slow to preserve control responsiveness.
- Build options: `-DDC_OGX_OLED_ENABLED=OFF`, `-DDC_OGX_OLED_ADDRESS=0x3d`, `-DDC_OGX_OLED_HEIGHT=32`. Use a separate build directory per board.
- SSD1309, SH1106 and other SSD13xx variants are **not** confirmed compatible with these initialization commands.

## UDP discovery foundation

- Probe datagram: ASCII `DC-OGX-W|DISCOVER|1`, sent to UDP port **38473**.
- Announcement: `DC-OGX-W|1|id=<ID>|board=<BOARD>|ip=<IPv4>|http=80\n`.
- The official Pico W's native CYW43/lwIP backend now answers probes and, while not actively remotely controlled, sends small directed-subnet broadcasts every five seconds. It uses the existing network core and never transmits passwords, Bluetooth identities or profiles.
- **ESP8285 Pico-W-2023:** the UDP AT-command transport is **not enabled in this pass**. Adding a UDP socket/periodic AT commands to the same busy UART could recreate the browser/controller timeouts. The shared packet format is defined and tested, and the OLED can display the station IP until an AT-compatible discovery backend is validated on hardware.
- For a read-only probe on Windows: `py tools\discover.py --broadcast 192.168.1.255` (replace with your LAN broadcast address).
- Broadcast is not mDNS; it lets an application find the device but does not provide automatic `dc-ogx-w.local` DNS resolution.
- Windows app implementation is deferred and will live in a separate project.

## Planned Windows input protocol (not implemented in this update)

Discovery and control use distinct sockets. UDP discovery is on 38473; a future UDP unicast control channel can use a separately assigned port after testing on both network backends.

Session negotiation: discover → choose device → request exclusive control → adapter neutralizes USB output and, on Bluetooth-capable boards, disconnects/inhibits Bluetooth controller input → grant a session ID → stream complete sequenced controller snapshots → release/expiry → neutral output and optional Bluetooth resume.

Packets will contain a version, session ID, monotonic sequence number, button mask, both analog sticks, trigger values and a client timestamp. Duplicate or old sequences are discarded, not queued. A missing input refresh releases all buttons/sticks in 250ms while a separate lease controls reconnection. Feedback can carry rumble, last accepted sequence and status. Private LAN use does not eliminate the need for explicit ownership and session validation.

The existing WebUI and WebSocket API remain the configuration, calibration, update and proof-of-concept interface. The Windows client will use its own low-latency transport, not a browser WebSocket loop.

## Local validation

```
python tests/display/run.py
python tests/discovery/run.py
python tests/network/run.py --sanitize
python tests/esp_at/run.py
```

These tests use host mocks/sanitizers; full Pico SDK cross-compilation and physical radio/OLED tests must be performed in VS Code on your toolchain. We deliberately leave the existing ESP AT command engine and the HTML source unchanged.
