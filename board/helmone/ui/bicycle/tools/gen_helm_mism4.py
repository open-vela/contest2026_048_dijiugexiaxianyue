#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate production MiSans Medium 4bpp bitmaps for menu/title/val."""

from __future__ import annotations

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BICYCLE_DIR = os.path.normpath(os.path.join(HERE, ".."))
OUT_DIR = os.path.join(BICYCLE_DIR, "src", "lvgl_page", "fonts")
SYM_PATH = os.path.join(OUT_DIR, "helm_mism4_symbols.txt")
MISANS = "/usr/share/fonts/truetype/misans/MiSans-Medium.ttf"

SIZES = (12, 15, 22)


def main() -> int:
    if not os.path.isfile(MISANS):
        print(f"[error] missing {MISANS}", file=sys.stderr)
        return 1
    if not os.path.isfile(SYM_PATH):
        print(f"[error] missing {SYM_PATH}", file=sys.stderr)
        return 1

    symbols = open(SYM_PATH, encoding="utf-8").read().replace("\n", "")
    print(f"[charset] {len(symbols)}", file=sys.stderr)

    for size in SIZES:
        name = f"helm_mism4_{size}"
        out = os.path.join(OUT_DIR, f"{name}.c")
        cmd = [
            "npx", "--yes", "lv_font_conv",
            "--size", str(size),
            "--bpp", "4",
            "--format", "lvgl",
            "--no-compress",
            "--no-prefilter",
            "--no-kerning",
            "--lv-include", "lvgl/lvgl.h",
            "--lv-font-name", name,
            "-o", out,
            "--font", MISANS,
            "--autohint-strong",
            "-r", "0x20-0x7E",
            "--symbols", symbols,
        ]
        print("[lv_font_conv]", name, file=sys.stderr)
        subprocess.check_call(cmd)
        print("[wrote]", out, os.path.getsize(out), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
