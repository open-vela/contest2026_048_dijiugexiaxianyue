#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Fill rare OSM glyphs into the MiSans map font.

Keeps MiSans outlines for everything it already has. Missing codepoints are
either aliased to an equivalent MiSans glyph (radicals / compatibility /
punctuation) or imported from gothic/serif fallbacks (Noto CJK SC, Source Han,
HanaMin, SimSun-ExtB/G, BabelStone).

  .venv/bin/python patch_misans_map.py
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import unicodedata
from pathlib import Path

from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.filterPen import DecomposingFilterPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "osm"))
import zh_simplify  # noqa: E402

CHARSET_PATH = HERE.parent / "osm" / "data" / "china_osm_gb2312_chars.txt"
MISANS_SRC = Path("/usr/share/fonts/truetype/misans/MiSans-Medium.ttf")
SUBSET_DIR = HERE.parents[2] / "openvela/boards/vela/bicycle/etc/font/subset"
OUT_TTF = SUBSET_DIR / "MiSans-Medium.subset.ttf"
MKFS_DIR = HERE.parents[1] / "boards/sf32lb52/my_vendor/mkfs/fonts"

# Prefer gothic (Noto SC / Source Han) for CJK, then Ext B/G specialists.
FALLBACKS = [
    (Path("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"), 0),
    (Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"), 0),
    (Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc"), 2),  # SC
    (Path(
        "/home/jinsc/SDK/vela/openvela/vendor/my_vendor/boards/sf32lb52/"
        "my_vendor/ui/bicycle/fonts/SourceHanSansCN-Regular.ttf"
    ), 0),
    (Path("/usr/share/fonts/truetype/winfonts/BabelStoneHan.ttf"), 0),
    (Path("/usr/share/fonts/truetype/winfonts/HanaMinA.ttf"), 0),
    (Path("/usr/share/fonts/truetype/winfonts/HanaMinB.ttf"), 0),
    (Path("/usr/share/fonts/truetype/winfonts/simsunb.ttf"), 0),
    (Path("/usr/share/fonts/truetype/winfonts/SimsunExtG.ttf"), 0),
]

# Extra aliases when NFKC/t2s is not enough (Kangxi radicals, dashes, dots).
MANUAL_ALIAS = {
    0x2011: (0x2010, 0x002D),           # ‑ → ‐ / -
    0x2012: (0x2013, 0x2014, 0x002D),  # ‒ → – / —
    0x2027: (0x00B7, 0x2022),           # ‧ → ·
    0x2EC6: (0x89D2,),                  # ⻆ → 角
    0x2ED3: (0x957F,),                  # ⻓ → 长
    0x2EE9: (0x9EC4,),                  # ⻩ → 黄
    0x2FC8: (0x9EC4, 0x9EC3),           # ⿈ → 黄 / 黃
    0x301C: (0xFF5E, 0x007E),           # 〜 → ～
    0x30FB: (0x00B7,),                  # ・ → ·
    0xFF65: (0x30FB, 0x00B7),           # ･ → ・ / ·
}


def load_charset(path: Path) -> str:
    chars: list[str] = []
    seen: set[str] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith("#"):
            continue
        for c in line:
            if c.isprintable() and c not in "\r\n" and c not in seen:
                seen.add(c)
                chars.append(c)
    return "".join(sorted(chars, key=ord))


def font_cmap(path: Path, face: int) -> set[int]:
    font = TTFont(os.fspath(path), lazy=True, fontNumber=face)
    try:
        return set(font.getBestCmap() or {})
    finally:
        font.close()


def add_cmap(font: TTFont, cp: int, glyph_name: str) -> None:
    for table in font["cmap"].tables:
        if not table.isUnicode():
            continue
        if table.format == 4 and cp > 0xFFFF:
            continue
        table.cmap[cp] = glyph_name
    if cp > 0xFFFF and not any(
        t.isUnicode() and t.format == 12 for t in font["cmap"].tables
    ):
        sub = CmapSubtable.newSubtable(12)
        sub.platformID = 3
        sub.platEncID = 10
        sub.language = 0
        sub.cmap = dict(font.getBestCmap() or {})
        sub.cmap[cp] = glyph_name
        font["cmap"].tables.append(sub)


def alias_target(ch: str, present: dict[int, str]) -> str | None:
    """Return an existing glyph name to reuse, or None."""
    cp = ord(ch)
    for cand in MANUAL_ALIAS.get(cp, ()):
        if cand in present:
            return present[cand]
    nfkc = unicodedata.normalize("NFKC", ch)
    simp = zh_simplify.to_simplified(nfkc)
    for form in (simp, nfkc):
        if len(form) == 1 and ord(form) in present and form != ch:
            return present[ord(form)]
    return None


def import_glyph(src: TTFont, dst: TTFont, src_name: str, dst_name: str) -> None:
    for tag in ("vmtx", "vhea", "VORG"):
        if tag in src:
            try:
                del src[tag]
            except Exception:
                pass
    scale = dst["head"].unitsPerEm / src["head"].unitsPerEm
    gs = src.getGlyphSet()
    pen = TTGlyphPen({})
    out = pen
    if "CFF " in src or "CFF2" in src:
        out = Cu2QuPen(out, max_err=1.0, reverse_direction=True)
    if scale != 1.0:
        out = TransformPen(out, (scale, 0, 0, scale, 0, 0))
    gs[src_name].draw(DecomposingFilterPen(out, gs))
    glyph = pen.glyph()
    dst["glyf"][dst_name] = glyph
    if glyph.numberOfContours:
        glyph.recalcBounds(dst["glyf"])
        lsb = int(getattr(glyph, "xMin", 0))
    else:
        lsb = 0
    advance = int(round(gs[src_name].width * scale))
    dst["hmtx"][dst_name] = (max(advance, 0), lsb)
    order = list(dst.getGlyphOrder())
    if dst_name not in order:
        order.append(dst_name)
        dst.setGlyphOrder(order)
        dst["maxp"].numGlyphs = len(order)


def recalc_maxp(font: TTFont) -> None:
    glyf = font["glyf"]
    max_contours = 0
    max_points = 0
    for name in font.getGlyphOrder():
        g = glyf[name]
        if g.numberOfContours <= 0:
            continue
        coords, end_pts, _flags = g.getCoordinates(glyf)
        max_contours = max(max_contours, len(end_pts))
        max_points = max(max_points, len(coords))
    font["maxp"].numGlyphs = len(font.getGlyphOrder())
    font["maxp"].maxContours = max(font["maxp"].maxContours, max_contours)
    font["maxp"].maxPoints = max(font["maxp"].maxPoints, max_points)


def human(n: int) -> str:
    return "%.2f MB" % (n / 1024 / 1024) if n >= 1024 * 1024 else "%.1f KB" % (n / 1024)


def main() -> int:
    ap = argparse.ArgumentParser(description="Patch MiSans map subset with rare glyphs.")
    ap.add_argument("--charset", default=str(CHARSET_PATH))
    ap.add_argument("--src", default=str(OUT_TTF), help="MiSans subset to patch")
    ap.add_argument("--out", default=str(OUT_TTF))
    ap.add_argument("--no-sync", action="store_true")
    args = ap.parse_args()

    charset = load_charset(Path(args.charset))
    src_path = Path(args.src)
    if not src_path.is_file():
        if not MISANS_SRC.is_file():
            print("[error] no MiSans subset or source", file=sys.stderr)
            return 1
        print("[warn] subset missing, start from full MiSans", file=sys.stderr)
        src_path = MISANS_SRC

    font = TTFont(os.fspath(src_path), recalcBBoxes=True)
    if "DSIG" in font:
        del font["DSIG"]
    present = dict(font.getBestCmap() or {})
    missing = [c for c in charset if ord(c) not in present]
    print(
        "[patch] %s  charset=%d present=%d missing=%d"
        % (src_path.name, len(charset), len(present), len(missing)),
        flush=True,
    )

    aliased = 0
    imported = 0
    log: list[str] = []
    still: list[str] = []

    for ch in missing:
        gname = alias_target(ch, present)
        if gname:
            add_cmap(font, ord(ch), gname)
            present[ord(ch)] = gname
            aliased += 1
            log.append("alias U+%04X %s -> %s" % (ord(ch), ch, gname))
            continue
        still.append(ch)

    fallback_cmaps: list[tuple[Path, int, set[int]]] = []
    for path, face in FALLBACKS:
        if not path.is_file():
            print("[warn] skip missing fallback %s" % path, file=sys.stderr)
            continue
        fallback_cmaps.append((path, face, font_cmap(path, face)))

    open_fonts: dict[tuple[str, int], TTFont] = {}

    def src_font(path: Path, face: int) -> TTFont:
        key = (str(path), face)
        if key not in open_fonts:
            open_fonts[key] = TTFont(
                os.fspath(path), fontNumber=face, ignoreDecompileErrors=True
            )
        return open_fonts[key]

    remain: list[str] = []
    for ch in still:
        cp = ord(ch)
        picked = None
        for path, face, cmap in fallback_cmaps:
            if cp in cmap:
                picked = (path, face)
                break
        if not picked:
            remain.append(ch)
            continue
        path, face = picked
        src = src_font(path, face)
        src_name = (src.getBestCmap() or {})[cp]
        dst_name = "uni%04X" % cp if cp <= 0xFFFF else "u%05X" % cp
        if dst_name in font.getGlyphOrder():
            dst_name = "fb_%s" % dst_name
        import_glyph(src, font, src_name, dst_name)
        add_cmap(font, cp, dst_name)
        present[cp] = dst_name
        imported += 1
        log.append("import U+%04X %s <- %s" % (cp, ch, path.name))

    for f in open_fonts.values():
        f.close()

    recalc_maxp(font)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    font.save(os.fspath(out))
    font.close()

    chk = TTFont(os.fspath(out), lazy=True)
    got = set(chk.getBestCmap() or {})
    chk.close()
    miss_final = [c for c in charset if ord(c) not in got]
    miss_path = out.with_suffix(".missing.txt")
    miss_path.write_text(
        "# %s: %d missing of %d\n%s\n"
        % (out.name, len(miss_final), len(charset), "".join(miss_final)),
        encoding="utf-8",
    )
    log_path = out.with_name("MiSans-Medium.patch.log")
    log_path.write_text("\n".join(log) + "\n", encoding="utf-8")

    print(
        "[write] %s  %s  aliased=%d imported=%d still_missing=%d"
        % (out, human(out.stat().st_size), aliased, imported, len(miss_final)),
        flush=True,
    )
    if miss_final:
        print("[miss] %s" % "".join(miss_final), file=sys.stderr)

    if not args.no_sync:
        MKFS_DIR.mkdir(parents=True, exist_ok=True)
        dst = MKFS_DIR / out.name
        shutil.copy2(out, dst)
        print("[sync] %s (%s)" % (dst, human(dst.stat().st_size)), flush=True)
    return 0 if not miss_final else 0


if __name__ == "__main__":
    raise SystemExit(main())
