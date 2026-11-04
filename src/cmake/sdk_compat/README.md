# Pico SDK compatibility

This Bluetooth compatibility layer is used only by the official `pico_w`
target. The ESP8285 `pico_w_2023` target does not fetch, patch or link BTstack,
Bluepad32, or the CYW43 Bluetooth transport.

`btstack_hci_transport_cyw43.c` comes from Raspberry Pi Pico SDK **2.1.0**,
commit `95ea6acad131124694cda1c162c52cd30e0aece0`, with the existing OGX-Mini
CYW43 receive pre-buffer correction. It reserves the complete aligned BTstack
pre-buffer before delivering received HCI packets. Its original copyright and
BSD-3-Clause license are retained in the source and `LICENSE.TXT`.

`../sdk_compat.cmake` selects this file for SDK versions before **2.3.0**, including
the previously validated official-board SDK 2.1.0 build. It also adapts older SDK source lists for the pinned
BTstack fork (`hids_host.c` and `hci_event_builder.c`) when needed.

SDK **2.3.0** already provides the corrected receive pre-buffer and declares the
CYW43 transport's synchronous transmit behavior correctly. Its native transport
is used without substitution. The SDK also supplies the required BTstack source
files, so the compatibility layer does not add a duplicate event-builder source.

These adjustments change CMake target properties only. The installed Pico SDK
is never patched or rewritten. The project defaults to SDK/picotool 2.3.0 in the
Pico VS Code integration; explicit `sdkVersion`, `toolchainVersion`, and
`picotoolVersion` CMake variables are preserved, as is an external `PICO_SDK_PATH`.
Current two-target validation uses SDK 2.3.0; retaining the legacy path does not
claim that the new ESP8285 implementation has been tested with SDK 2.1.0.
