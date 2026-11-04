# Build and flash DC-OGX-W

## Open the correct project

The **repository root is not the CMake firmware root**. Use `src/` (the folder containing `CMakeLists.txt` and `CMakePresets.json`) with the Raspberry Pi Pico VS Code extension. Pico SDK version is set to **2.3.0** and ARM toolchain to **15_2_Rel1** in the uploaded `src/CMakeLists.txt`.

For command-line use, change into the firmware folder:

```powershell
cd src
cmake --preset pico-w-2023
cmake --build --preset pico-w-2023 --parallel
```

Substitute `pico-w` for the official CYW43439-based Pico W. Do not reuse a configured build directory for the other board. Normal builds should also run using the VS Code **Compile** button after initial setup, with no separate PowerShell workaround script.

## Required firmware packager

The post-build command in `src/cmake/generated.cmake` uses:

```text
<repository>/src/tools/firmware/make_update.py
```

It produces `dc-ogx-w-pico_w_2023.ogxw` or `dc-ogx-w-pico_w.ogxw` from the `.bin`. **The script is not part of the uploaded archive that this cleanup started from.** The maintainer is placing it in the local `src/` firmware project. Confirm the complete `tools/firmware/make_update.py` subpath before building/releasing. Do not disable the package generation just to silence a missing-script error; it also validates updater compatibility.

## Artifacts and flashing

- PIO-disabled baseline preset: `src/build/pico-w-2023/` or `src/build/pico-w/`.
- Optional PIO test presets only after you migrate the validated CMake patch; see [USB host](USB_HOST.md).
- Initial install: RP2040 BOOTSEL and matching `.uf2` file.
- Subsequent updates: matching `.ogxw` through the device WebUI.
- The updater cannot update the separate legacy ESP8285 AT modem firmware.
- Never interrupt the final flash-copy operation. BOOTSEL is the recovery route after a broken installation; there is no A/B fallback.

## Dependencies and build issues

The Pico SDK uses an ARM cross-compiler for firmware and a **native Windows compiler or prebuilt `pioasm`** for the PIO assembler. Install the Pico VS Code extension's toolchain/components, including prebuilt native tools as appropriate. The PIO-USB integration also needs its host source list and pico-pio-usb dependency, not merely `DC_OGX_USB_HOST_ENABLED=ON`. A missing `GameSirCyclone2Trace.cpp` can cause `undefined reference` at final link even when all compile steps succeed.

If the project still reports the original `CMAKE_CXX_COMPILER` missing for `pioasm`, repair the Pico extension/toolchain installation once; don't add local build scripts or disable PIO to hide it. Build/configuration versions may be cached per preset. Retain the known-good firmware for rollback during PIO testing.

## Packaging and hardware validation

The `firmware/` directory holds prebuilt images from the supplied archive; these may not include later local patches. Rebuild and verify board identity/version, image size under the **1,000 KiB OTA image limit**, and update package before publishing.

On real hardware test SoftAP and LAN, websocket ownership/release, Bluetooth where available, OLED/LED, update flow, and (when enabled) PIO host enumeration with safe 5 V USB power. Compilation alone is not a functional acceptance test.
