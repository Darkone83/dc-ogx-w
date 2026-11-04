# Credits and third-party notices

This project is developed by **Darkone83 / DarkoneCustoms**. Thanks to the original OGX-Mini maintainers and the wider Xbox homebrew/modding community for the foundations on which this project builds.

## Core controller foundation

- **[wiredopposite/OGX-Mini](https://github.com/wiredopposite/OGX-Mini)** — original RP2040 controller-adapter project; original copyright notice is preserved in `LICENSE` and upstream source.
- **[MegaCadeDev/OGX-Mini-2026](https://github.com/MegaCadeDev/OGX-Mini-2026)** — fork/continuation used as the source foundation, described by the pre-branding project notes at commit `2c5b41096e14e2ff7b0b0f84c1b1cce8e403e34b`.
- **OpenStickCommunity / GP2040-CE**, **Jason Skuby**, **Ryan Wendland** — contributors/copyright holders identified in the existing project LICENSE and original source notices. Their contributions remain acknowledged there.

## Libraries and toolchains

- **Pico-PIO-USB** — optional wired USB-host implementation when enabled and linked; preserve its upstream notices and review the exact pinned revision.
- **Tauri / Rust** — optional Windows companion application; its dependencies have independent licenses.
- **PySide6, aiohttp, pygame** — Python desktop/network companion dependencies with independent redistribution obligations.
- **Raspberry Pi Pico SDK** — RP2040 C/C++ firmware toolchain and hardware access.
- **TinyUSB** — USB device stack (pinned fork in `cmake/dependencies.cmake`).
- **Bluepad32**, **BTstack** — Bluetooth controller support on official Pico W builds; licensing differs from the Wi-Fi-only build.
- **libfixmath**, **libxsm3**, **ExCrypt** — fixed-point and console-controller compatibility components. See their bundled notices and the root `LICENSE` for details.
- **DejaVu Sans Condensed Bold** — rasterized uppercase font glyphs embedded in the small OLED interface, derived from a DejaVu family distributed under the DejaVu/Bitstream Vera font license. The project distributes raster data, not the TTF font files.
- **faithvoid** — original host-side rumble-handling attribution carried over in the root `LICENSE`.

## Visual identity

The **DXC / DC-OGX-W** emblem, wordmark and boot-splash compositions were created for DarkoneCustoms in this project. They are supplied in `assets/branding/`, including the original concept board and the split, derived project assets. The OLED version is a separate pixel-optimized monochrome interpretation.

## License considerations

**Do not assume that all optional configurations are MIT-only.** The original `LICENSE` distinguishes between the core project, Bluetooth library combinations, and libxsm3's LGPL and ExCrypt notices. Before distributing a binary, check the dependencies actually linked into that build and retain all applicable third-party notices. Refer to any dependency's own license for its authoritative terms. This file is attribution, not a replacement for those licenses.
