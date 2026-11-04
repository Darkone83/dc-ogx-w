# Build and software validation — 0.2.4

The integrated source was compiled and linked with **Pico SDK 2.3.0**, **Arm GNU
14.2.Rel1 (GCC 14.2.1)**, CMake 3.31.6 and Ninja. Both board Release targets and
the clone Debug target produced ELF, UF2 and board-specific `.ogxw` packages.
Existing dependency checkouts and build directories were reused. Delivery is
individual source files; generated binaries and the SDK are not part of it.

| Target | Build | Result | Flash image span | Static main SRAM | Heap address span |
|---|---|---|---:|---:|---:|
| ESP8285 `pico_w_2023` | Release | Pass | 376,952 B | 88,924 B | 156,836 B |
| Official `pico_w` | Release | Pass | 996,576 B | 154,820 B | 90,940 B |
| ESP8285 `pico_w_2023` | Debug | Pass | 661,976 B | 95,348 B | 150,412 B |

Values come from ELF symbols: flash span is `__flash_binary_end - 0x10000000`,
static main SRAM is `__bss_end__ - 0x20000000`, and heap span is
`__HeapLimit - __end__`. Heap span is allocator address space, **not measured
free heap after startup**. Physical stack high-water usage is not measured.

All three actual compilation databases retain `OGXM_STATUS_LED_PIN=8` and
firmware version `0.2.4`. The generated UI gzip in each build was decompressed
and compared byte for byte with the delivered HTML: **34,449 compressed bytes**.
The official firmware has 27,424 bytes of remaining primary-image capacity;
additional features must continue to respect that bound.
A subsequent clone Release build returned `ninja: no work to do.`

## Layout and integration

All three images retain 8 KiB stacks at `0x2003c000`–`0x2003e000` (Core1) and
`0x2003e000`–`0x20040000` (Core0). The heap limit is `0x2003c000`. The primary
image is bounded to 1,024,000 bytes (1000 KiB). The staged image starts at flash
offset `0x100000` and ends before `0x1fa000`; the last 24 KiB of the conservative
2 MiB layout retain Bluetooth/settings storage. The last 16 KiB is NVS.

The normal package generator validated each SDK-produced image and generated
its board/layout identity and CRC32. The unchanged SRAM installer was audited
in the 0.2.0 linked ELF; this iteration does not change its flash-copy routine.
Compilation and package generation do not establish power-fail-safe flashing.

The final ELF symbol checks found no TinyUSB host (`tuh_`, `hcd_`) or PIO-USB
implementation. Clone Release and Debug omit CYW43, Bluepad32 and BTstack. The
official target includes lwIP mDNS and Bluetooth. The built-in USB connector
remains the device output. Clone Debug keeps UART1 logging on GP4/GP5 separate
from the ESP UART0 on GP0/GP1. The official build retains Bluepad32 deprecation
warnings for its existing connection-control API; there were no link errors.

## Checks performed for this version

| Area | Result | Coverage |
|---|---|---|
| Shared HTTP/WebSocket service | 28 scenarios, normal + ASan/UBSan | Application pings independent of controller mailbox; bounded/coalesced pong replies and close priority; preaccept setup gate; response-completion mDNS retry; configuration-mode mutation guards; retained busy/age/scan/save/update cases |
| ESP AT engine | 3,570 checks, normal + ASan/UBSan | Missing SEND terminal with second socket preserved; no ambiguous payload replay; identifiable GMR synchronization; late/busy command handling; initial setup gate; independent mDNS retries, failure diagnostics, address changes and timer wraparound |
| Controller runtime | Both board policies passed; official also ASan/UBSan | Explicit source transitions; neutral handoff; no physical fallback on lease loss; maintenance and profile application; real capture/range math, persistence and simulated reboot; stale/mixed-controller rejection; remote input unaffected by Bluetooth calibration; Steam remapping; concurrency and rollover |
| Actual embedded UI in Chromium | 325 checks | Ordinary HTTP and localhost gamepad API; neutral recovery; Escape during backoff; owner conflict; no BT automatic reacquisition; calibration/remapping requests; general configuration gates; mDNS diagnostics; retained scanner, settings, LED and update behavior |
| Python companion | 26 tests passed | Ownership, health probes, neutral recovery, cancellation, conflict handling, and real localhost HTTP/WebSocket proxy to a fake adapter; scripts keep recovery opt-in |

Controller responses were measured using the real 4096-byte capacity: status
900–907 bytes before network diagnostics, profile 1557–1558 bytes, calibration
794–800 bytes. The status regression reserves a further 1800 bytes for the
network object. The 190-byte UserProfile layout remains unchanged. New
Bluetooth calibration occupies an independent versioned 30-byte record per
profile and is loaded/saved through the existing settings lifecycle.

The Steam driver now consumes the selected Gamepad instead of a physical input
side channel. Native mapping tests cover canonical output bits; mode transitions
also release queued Steam mouse buttons. Switch and Steam output drivers no
longer reinterpret already-mapped bits through mutable profile mappings.

## What the timeout and mDNS checks establish

The ESP test reproduces a complete CIPSEND body whose terminal response never
arrives. The corrected engine closes that handle, synchronizes the command
parser and preserves an unrelated live socket. It does not resend the old body.
Missing prompts, incomplete bodies or failed synchronization still require the
existing conservative recovery. A command timeout is now visible even when a
full modem restart was avoided.

The first configured station/address/mDNS attempt receives a bounded setup
window before a new remote lease is granted. Monitor connections remain open.
Handled optional-command failures release that gate. Later mDNS attempts have
an independent backoff and defer during real ownership rather than treating
Wi-Fi source selection itself as permanent radio ownership. Changed DHCP
addresses queue responder restart. Explicit retries wait for HTTP response
completion; abandoned requests cannot start them.

The browser's former HTTPS-only gamepad check was removed. Tests supply a
standard-mapped gamepad while `isSecureContext` is false, exercising the actual
input loop and outgoing WebSocket states. Actual API absence/access denial is
handled separately. The localhost bridge is an optional compatibility route.
No browser-security override is used.

The Python regressions additionally cover a failed first neutral send during
reacquisition, exhaustion after exactly eight failed recovery attempts, Release
while initial status is pending, and Release while an acquire reply is pending.
A known late token is released without sending input. Ambiguous acquire replies
stop automatic retry; the client never assumes an unobserved grant failed.

The 0.2.3 exact owner-deadline, nonblocking mailbox retry, input-age preservation,
and updater idle-lock corrections remain in the source. The shared-service
and runtime suites continue to exercise the affected control/maintenance paths.
The standalone unchanged updater suite (107 image/session + 82 HTTP checks),
scan-cache suite (133), LED suite (322), protocol suite (107), and saved-network
suite (73) were validated in earlier iterations and were not rerun solely to
increase test counts here.

## Re-run locally

From the project root, with native C/C++ compilers installed:

```sh
python tests/remote_control/run_runtime.py --libfixmath build/pico-w-2023/_deps/libfixmath-src
python tests/remote_control/run_runtime.py --board pico_w_2023 --libfixmath build/pico-w-2023/_deps/libfixmath-src
python tests/remote_control/run_runtime.py --libfixmath build/pico-w-2023/_deps/libfixmath-src --sanitize
python tests/network/run.py --sanitize
python tests/esp_at/run.py --sanitize
python -m unittest discover -s tools/pc/tests -v
```

Adjust the libfixmath path if CMake uses an existing checkout. The C++ runners
also work without `--sanitize`. If a host sandbox prevents LeakSanitizer from
inspecting processes, `ASAN_OPTIONS=detect_leaks=0` disables that separate check
while retaining AddressSanitizer/UBSan.

The browser test requires Node and Playwright/Chromium; setup is documented at
the top of `tests/web/test_device_ui.cjs`. Run `node tests/web/test_device_ui.cjs`;
`OGX_TEST_CHROMIUM` may select an existing Chromium executable. The tests use
local intercepted requests and a mocked standard gamepad, not a board.

## Physical validation still needed

The user previously confirmed that the clone broadcasts its AP and serves
remote controls. This version's actual RF latency/recovery, LAN multicast name
resolution, real-controller capture and Bluetooth handoff have **not** been
exercised on hardware here. ESP `mdns_state:active` means the modem accepted
responder startup; CYW43 uses its probe result. Neither proves that a remote
computer can resolve the name through its network. The diagnostic state/error
and modem-version fields distinguish those conditions for the hardware check.

Physical GPIO8 output, console USB acceptance and real OTA installation also
remain hardware checks. Interrupted-upload handling does not make final flash
installation power-fail-safe; BOOTSEL remains the recovery path.
