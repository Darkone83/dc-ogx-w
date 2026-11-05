# Build and flash DC-OGX-W

## Open the correct project

The **repository root is not the CMake firmware root**. Use `src/` (the folder containing `CMakeLists.txt` and `CMakePresets.json`) with the Raspberry Pi Pico VS Code extension. Pico SDK version is set to **2.3.0** and ARM toolchain to **15_2_Rel1** in `src/CMakeLists.txt`.

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

The script is included in this repository. The normal build produces `dc-ogx-w-pico_w_2023.ogxw` or `dc-ogx-w-pico_w.ogxw` from the `.bin` and validates package compatibility. Keep the packaging step enabled; it is part of the standard Compile/build workflow.

## Artifacts and flashing

- Standard presets: `src/build/pico-w-2023/` and `src/build/pico-w/`. PIO-USB host is **enabled by default** in this source tree.
- Dedicated hardware-test presets: `pico-w-2023-pio` and `pico-w-pio`, with separate output directories. Both include the current PIO-USB integration; see [USB host](USB_HOST.md).
- Initial install: RP2040 BOOTSEL and matching `.uf2` file.
- Subsequent updates: matching `.ogxw` through the device WebUI.
- The updater cannot update the separate legacy ESP8285 AT modem firmware.
- Never interrupt the final flash-copy operation. BOOTSEL is the recovery route after a broken installation; there is no A/B fallback.

## Dependencies and build issues

The Pico SDK uses an ARM cross-compiler for firmware and a **native Windows compiler or prebuilt `pioasm`** for the PIO assembler. Install the Pico VS Code extension's toolchain/components, including prebuilt native tools as appropriate. This repository already includes the PIO-USB host source list, `GameSirCyclone2Trace.cpp` linkage and the Pico-PIO-USB dependency. If dependency downloads or linking fail, inspect the toolchain and CMake output rather than disabling USB-host support.

If `pioasm` reports a missing native compiler, repair the Pico extension/toolchain installation rather than adding manual build scripts. Build/configuration versions may be cached per preset. Retain the known-good firmware for recovery during PIO testing.

## Packaging and hardware validation

The `firmware/` directory contains bundled prebuilt images; they may not match newer local source changes. Rebuild and verify board identity/version, image size under the **1,000 KiB OTA image limit**, and update package before publishing.

On real hardware test SoftAP and LAN, station-mode `dcogxw.local`, WebSocket ownership/release, Bluetooth where available, OLED/LED, update flow, and PIO host enumeration with safe 5 V USB power. Compilation alone is not a functional acceptance test.
