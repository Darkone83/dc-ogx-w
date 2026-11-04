#!/usr/bin/env python3
"""Package this project's RP2040 .bin for its browser updater (no dependencies).

CMake runs this after pico_add_extra_outputs. Initial installation still uses
the usual .uf2; subsequent upgrades select the generated board-specific .ogxw.
The package is compatibility/integrity checked, not cryptographically signed.
"""
import argparse
from pathlib import Path
import struct
import zlib

MAGIC = b"DCOGXW1\0"
IDENTITY = b"DCOGXW-IMAGE-V1\0"
MAX_IMAGE = 0xFA000
STAGING_OFFSET = 0x100000
LAYOUT = 1
FLASH_BASE = 0x10000000
BOARDS = {"pico_w": 1, "pico_w_2023": 2}


def boot2_crc(data: bytes) -> int:
    state = 0xFFFFFFFF
    for value in data[:252]:
        state ^= value << 24
        for _ in range(8):
            state = ((state << 1) ^ (0x04C11DB7 if state & 0x80000000 else 0)) & 0xFFFFFFFF
    return state


def validate_image(image: bytes, board: str) -> None:
    if not 512 <= len(image) <= MAX_IMAGE:
        raise ValueError(f"Image is {len(image)} bytes; OTA image limit is {MAX_IMAGE} bytes")
    if boot2_crc(image) != struct.unpack_from("<I", image, 252)[0]:
        raise ValueError("Invalid RP2040 boot2 checksum; input must be the SDK .bin")
    stack, reset = struct.unpack_from("<II", image, 256)
    if stack & 7 or not 0x20000000 < stack <= 0x20042000:
        raise ValueError("Invalid initial RP2040 stack pointer")
    if not reset & 1 or not FLASH_BASE + 264 <= reset & ~1 < FLASH_BASE + len(image):
        raise ValueError("Invalid RP2040 reset vector")
    identity = IDENTITY + struct.pack("<4I", BOARDS[board], LAYOUT, MAX_IMAGE, STAGING_OFFSET)
    if identity not in image[264:]:
        raise ValueError(f"Firmware identity is missing or does not match {board}")


def package(image: bytes, board: str) -> bytes:
    validate_image(image, board)
    header = bytearray(64)
    header[:8] = MAGIC
    struct.pack_into("<7I", header, 8, 1, BOARDS[board], LAYOUT, len(image),
                     zlib.crc32(image), FLASH_BASE, 0)
    struct.pack_into("<I", header, 32, zlib.crc32(header))
    return bytes(header) + image


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", choices=BOARDS, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = package(args.input.read_bytes(), args.board)
    except (ValueError, OSError) as exc:
        parser.exit(1, f"Cannot create update package: {exc}\n")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(result)
    print(f"{args.output.name}: {len(result) - 64} firmware bytes; board {args.board}; layout {LAYOUT}")


if __name__ == "__main__":
    main()
