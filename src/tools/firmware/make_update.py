#!/usr/bin/env python3
"""Package this project's board-specific .bin for its browser updater (no dependencies).

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
BOARDS = {"pico_w": 1, "pico_w_2023": 2, "pico2_w": 3}


def valid_rp2350_boot_block(image: bytes) -> bool:
    limit = min(len(image), 4096)
    u32 = lambda offset: struct.unpack_from("<I", image, offset)[0]
    for start in range(8, limit - 19, 4):
        if u32(start) != 0xFFFFDED3 or u32(start + 4) != 0x10210142:
            continue
        item = start + 4
        while item + 12 <= limit:
            word = u32(item)
            tag = word & 0xFF
            words = (word >> 8) & (0xFFFF if tag & 0x80 else 0xFF)
            if tag == 0xFF:
                if words != (item - start - 4) // 4 or u32(item + 8) != 0xAB123579:
                    break
                relative = struct.unpack_from("<i", image, item + 4)[0]
                next_block = start + relative
                return relative == 0 or (0 <= next_block <= len(image) - 16 and
                                         u32(next_block) == 0xFFFFDED3)
            if not words or words > (limit - item) // 4:
                break
            item += words * 4
    return False


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
    rp2350 = board == "pico2_w"
    if rp2350 and not valid_rp2350_boot_block(image):
        raise ValueError("Invalid RP2350 Arm-secure boot block; input must be the SDK .bin")
    if not rp2350 and boot2_crc(image) != struct.unpack_from("<I", image, 252)[0]:
        raise ValueError("Invalid RP2040 boot2 checksum; input must be the SDK .bin")
    vectors = 0 if rp2350 else 256
    stack, reset = struct.unpack_from("<II", image, vectors)
    if stack & 7 or not 0x20000000 < stack <= (0x20082000 if rp2350 else 0x20042000):
        raise ValueError("Invalid initial stack pointer")
    if not reset & 1 or not FLASH_BASE + vectors + 8 <= reset & ~1 < FLASH_BASE + len(image):
        raise ValueError("Invalid reset vector")
    identity = IDENTITY + struct.pack("<4I", BOARDS[board], LAYOUT, MAX_IMAGE, STAGING_OFFSET)
    if identity not in image[vectors + 8:]:
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
