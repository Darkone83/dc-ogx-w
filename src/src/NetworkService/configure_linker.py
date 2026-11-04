#!/usr/bin/env python3
"""Adapt the pinned Pico SDK RP2040 layout for two 8 KiB network stacks.

The SDK normally places each stack in a 4 KiB scratch bank. Keep the physical
scratch sections in their usual locations and reserve 16 KiB at the top of
main SRAM for the larger stacks. Fail if the upstream layout has changed.
"""
from pathlib import Path
import sys


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: configure_linker.py SDK_LINKER OUTPUT_LINKER")
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    replacements = {
        "RAM(rwx) : ORIGIN =  0x20000000, LENGTH = 256k":
            "RAM(rwx) : ORIGIN =  0x20000000, LENGTH = 240k\n"
            "    STACK1(rwx) : ORIGIN = 0x2003c000, LENGTH = 8k\n"
            "    STACK0(rwx) : ORIGIN = 0x2003e000, LENGTH = 8k",
        "*(.stack1*)\n    } > SCRATCH_X": "*(.stack1*)\n    } > STACK1",
        "KEEP(*(.stack*))\n    } > SCRATCH_Y": "KEEP(*(.stack*))\n    } > STACK0",
        "__StackOneTop = ORIGIN(SCRATCH_X) + LENGTH(SCRATCH_X);":
            "__StackOneTop = ORIGIN(STACK1) + LENGTH(STACK1);",
        "__StackTop = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y);":
            "__StackTop = ORIGIN(STACK0) + LENGTH(STACK0);",
        'ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed")':
            'ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed")\n'
            '    ASSERT(__flash_binary_end <= ORIGIN(FLASH) + 1024000,\n'
            '        "Firmware exceeds the 1000 KiB OTA image slot")',
    }
    for original, replacement in replacements.items():
        if source.count(original) != 1:
            raise SystemExit("Pinned RP2040 linker layout changed; review stack/storage placement")
        source = source.replace(original, replacement)
    output = Path(sys.argv[2])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("/* Generated from the pinned Raspberry Pi Pico SDK linker script. */\n" + source,
                      encoding="utf-8")


if __name__ == "__main__":
    main()
