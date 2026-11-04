# Repository cleanup manifest

This archive was prepared from `DC-OGX-W-Repo.zip`. It is a **repository presentation and documentation cleanup**, not a firmware code release or a replacement for later patches.

## Removed from the distribution

| Original path | Reason |
|---|---|
| `apps/Python/tests/test_qt_input.py` | Development-only Python regression test |
| `apps/Python/tests/test_remote.py` | Development-only Python regression test |
| `apps/Windows/src/tests/check-ui.cjs` | Development-only desktop UI test; package manifest cleaned of dangling `check:ui` script |
| `docs/DEVELOPMENT_NOTES.md` | Long historic development/patch log, not current end-user instructions |
| `docs/oled-udp-foundation.md` | Obsolete feature proposal |
| `docs/wifi-led-correction.md` | Obsolete one-off troubleshooting notes |
| `docs/validation.md` | Stale build/test statistics for an older toolchain and earlier software state |
| `docs/USB_HOST_SOFTWARE_PLAN.md` | Superseded by current [USB host bring-up](USB_HOST.md) |

No C/C++ firmware implementation, PIO program, pinned dependency, USB host driver, executable, image, hardware file, or license was removed. The original ZIP is the rollback source for removed historical material.

## Updated or added

- Updated root `README.md`, `CONTRIBUTING.md`, `CREDITS.md`, feature docs, release checklist and both desktop-app READMEs.
- Added `docs/BUILD_AND_FLASH.md`, `docs/USB_HOST.md`, `docs/COMPANION_APPS.md` and this manifest.
- Removed the stale `check:ui` npm script from `apps/Windows/src/package.json`; its `dev` and `build` scripts remain unchanged.
- **Unchanged firmware source and CMake:** later PIO/network/Core0 patches must be migrated by the maintainer.

## Important locally supplied file

CMake references the updater packager at **`src/tools/firmware/make_update.py`** relative to repository root. The original uploaded archive did not include it; the maintainer is adding it locally. This cleaned copy intentionally does not invent or copy an obsolete version. Include the actual script before publishing the final source tree.

Prebuilt firmware and Windows binaries are copied unchanged; verify build provenance against the merged source before release.
