# Release checklist — dc-ogx-w software framework foundation

This is a complete **source snapshot** with functional software improvements, not a hardware-qualified production firmware release. Version remains `0.2.4` until the maintainer assigns a new version. No prebuilt UF2 is supplied.

## Implemented and host-tested in this pass

- [x] Preserve prior OLED update/activity screens, Wi-Fi recovery and core Xbox USB output sources.
- [x] Responsive WebUI with desktop sidebar and mobile drawer; retain tab IDs and API handlers.
- [x] Eight distinct self-contained browser-local themes; no external resource fetches.
- [x] Live two-stick / two-trigger preview **within Profiles** using the browser Gamepad API.
- [x] Custom 16×16 OLED status glyph atlas and low-frequency activity animation.
- [x] Build-time OLED contrast, startup fade and opt-in idle dim settings.
- [x] Runtime Device OLED controls: brightness, dim/off timeouts, idle brightness, enable, invert, flip and fade.
- [x] OLED preferences persisted in a separate validated device NVS record with deferred HTTP response commit/cancel.
- [x] Browser-local reassignable keyboard buttons, D-pad, triggers and stick directions; conflict swap and reset.
- [x] Stage GP9 D+ / GP10 D− in CMake with USB host explicitly **OFF**.
- [x] Preserve user-facing README, credits, contributing notes and GitHub templates.
- [x] Add WebUI desktop/mobile browser regressions and native OLED/ESP-AT tests.

## Not implemented / awaiting validation

- [ ] Full Pico SDK + GCC ARM cross-build of **both** supported board targets.
- [ ] Physical tests on RP2040-Pico W-2023 clone (ESP8285 AT) and official Pico W.
- [ ] Confirm actual WebUI theme persistence across navigation and browser reload on real IP origin.
- [ ] Verify actual hardware USB controller output and input ownership during Wi-Fi loss.
- [ ] Confirm all OLED contrast/fade/dim behavior on MakerFocus 0.91-inch panel during network load.
- [ ] USB host hardware, 5V VBUS path and PIO-USB host implementation for GP9/GP10 **not yet done**.
- [ ] Device-side physical input telemetry and full firmware-equivalent profile shaping preview.
- [ ] Physical OLED runtime contrast/rotation/inversion settings and screen-off wake verified on target hardware.
- [ ] Windows UDP input app and direct UDP control ownership (separate future project).
- [ ] Recheck third-party license terms, credit details, release binary checksums and exact version before public release.

## Suggested smoke test

1. Build `cmake --preset pico-w-2023 && cmake --build --preset pico-w-2023 --parallel`.
2. Flash with RP2040 BOOTSEL; keep ESP8285 AT firmware intact.
3. Load WebUI from IP in desktop and phone browsers; confirm sidebar/mobile menu and each theme.
4. Tune deadzone with a controller connected to the **browser host** and verify raw/preview stick markers.
5. On **Device → OLED display**, test Apply now on brightness, invert, flip and both idle timeouts; leave screen idle, trigger an activity and confirm wake. Test Save and restart, reconnect and check persisted values.
6. On **Control → Keyboard mapping**, change A/B keys, stick directions and trigger keys; test swapped duplicate mappings and Reset keys, including a browser reload.
7. Check OLED startup, profile save and update-progress statuses under normal controller/network load.
8. Test fixed/changed browser source ownership and Wi-Fi reconnection; verify input always goes neutral after a lost lease.
