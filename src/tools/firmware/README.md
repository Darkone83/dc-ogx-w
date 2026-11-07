# Browser firmware updates

The normal build emits both `dc-ogx-w.uf2` and a board-specific update package:

* `dc-ogx-w-pico_w_2023.ogxw` for the ESP8285 clone.
* `dc-ogx-w-pico_w.ogxw` for an official Raspberry Pi Pico W.
* `dc-ogx-w-pico2_w.ogxw` for an official Raspberry Pi Pico 2 W (Arm).

The `pico2-w` CMake configure/build preset builds the current firmware feature
set with the same carrier pins: USB host GP9/GP10, OLED GP14/GP15 and LED GP8.
It uses Pico SDK 2.3.0 or newer and a separate `build/pico2-w` directory.
Initial installation on Pico 2 W requires its own generated `.uf2`; Pico W
firmware and update packages cannot be used on Pico 2 W, or vice versa.

Install this updater-enabled firmware once using the ordinary BOOTSEL `.uf2`
process. Subsequent firmware builds can be uploaded from the WebUI's firmware
panel. Select the `.ogxw` package matching the physical board. The WebUI releases
remote control for the update and transfers one bounded chunk at a time. After
the entire staged image validates, explicitly choose **Install and reboot**.

The installed controller firmware is updated. This does **not** reflash the separate
ESP8285 radio, and it does not turn a clone into an official Pico W.

## Flash layout and recovery

All three builds conservatively use 2 MiB of flash. The running image is limited to
1000 KiB; a separate 1000 KiB staging area starts at offset `0x100000`. The final
24 KiB remain reserved for Bluetooth and user settings. Both the linker and
package generator reject images too large for this layout. Wi-Fi credentials,
controller profiles and Bluetooth storage are not erased by the installer.

An incomplete upload, disconnected browser, invalid header, wrong board, wrong
layout or checksum mismatch never overwrites the running firmware. Sessions
expire after two minutes without an accepted operation. There is no persistent
"install on next boot" flag, so an interrupted upload is discarded on reboot.

**Keep the board powered while installing.** After you confirm installation,
the installer stops USB and the radio core, checks the complete staged image,
and copies it from an SRAM routine using the selected chip's ROM flash functions. It reads
back each destination sector, then reboots. A failed readback enters ROM USB
recovery. This is staged replacement, not an A/B bootloader: power loss during
the final copy can require BOOTSEL and the normal `.uf2` to recover. No rollback
or uninterrupted controller operation is promised during an update.

Update packages contain board/layout markers, boot vectors and CRC32 integrity
checks. RP2040 images also require the boot2 checksum; RP2350 images require
the SDK's Arm-secure PICOBIN boot block. They are **not cryptographically signed**;
use firmware you built or trust. The updater is intended for the local WebUI.

## Protocol for Python or other clients

All mutation requests use JSON. The package's first 64 bytes are its manifest;
the remaining bytes are the image. Keep the token returned by `begin` private
to the uploading client. Only one session may exist at a time.

| Request | Body / result |
| --- | --- |
| `GET /api/update` | State, processing flag, received/total bytes, board and limits. |
| `POST /api/update/begin` | `{"header":"128 hex characters"}`; returns a random token. |
| `POST /api/update/chunk` | `{"token":123,"offset":0,"data":"hex bytes"}`; at most 1024 image bytes. |
| `POST /api/update/finish` | `{"token":123}`; validates the complete staged upload. |
| `POST /api/update/install` | `{"token":123}`; allowed only after status becomes `ready`. |
| `POST /api/update/cancel` | `{"token":123}`; releases update mode without altering the running image. |

Queued jobs return HTTP 202. Poll status until `processing` is false and check
`error` before issuing another operation. All chunks except the last must be a
multiple of 256 bytes; 1024-byte chunks are recommended. `offset` counts image
bytes, excluding the package header. A byte-identical retry of the immediately
previous chunk is idempotent. Out-of-order or conflicting chunks are rejected.
The status response intentionally does not expose the session token.

Flash erase/program operations run only from Core0, through SDK flash-safe
execution that pauses Core1. Network callbacks only copy a bounded command into
a mailbox. The integration must inhibit controller output while `active()` is
true and initialize flash-safe execution on both cores.

Run portable validation with `python tests/firmware_update/run.py`. The tests
cover wrong-board and malformed manifests, boot vectors, checksums, incomplete
and out-of-order transfers, duplicate chunks, flash failures, cancellation and
timeout rollover. These checks do not replace a physical interrupted-transfer
and successful-install test on each board.
