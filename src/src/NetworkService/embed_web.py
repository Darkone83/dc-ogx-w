#!/usr/bin/env python3
"""Embed the offline WebUI as a deterministic gzip stream in flash."""
import gzip
from pathlib import Path
import sys


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: embed_web.py INPUT_HTML OUTPUT_HEADER")
    compressed = gzip.compress(Path(sys.argv[1]).read_bytes(), compresslevel=9, mtime=0)
    lines = [", ".join(f"0x{byte:02x}" for byte in compressed[i:i + 16])
             for i in range(0, len(compressed), 16)]
    header = ("// Generated from web/index.html; do not edit.\n#pragma once\n#include <cstddef>\n"
              "alignas(4) static const unsigned char ogxm_web_index_gzip[] = {\n    "
              + ",\n    ".join(lines)
              + "\n};\nstatic constexpr size_t ogxm_web_index_gzip_length = sizeof(ogxm_web_index_gzip);\n")
    output = Path(sys.argv[2])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(header, encoding="utf-8")


if __name__ == "__main__":
    main()
