#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
subset_font.py — build smaller .ttf subsets for the bicycle firmware.

Canonical location: vendor/my_vendor/docs/font/
(vela layer: vendor/openvela/docs/font -> symlink here)

Keeps glyphs for firmware (~2 MB target per PuHuiTi face):

  preset osm (default):
    nationwide OSM names (t2s) + vmap + i18n + ASCII
  preset common2m:
    GB2312 + OSM names + scan
  preset scan:
    vmap + i18n + ASCII only

Usage:
  ../osm/.venv/bin/python ../osm/extract_osm_chars.py   # china-latest.osm.pbf
  ./subset_font.py                                      # osm foundation
  ./subset_font.py --preset common2m                    # also full GB2312
  ./subset_font.py --preset scan                        # minimal
  ./subset_font.py --sync-myvendor      # also copy Regular -> my_vendor LFS fonts/
  ./subset_font.py --list-chars
  ./subset_font.py --write-chars-file   # also write .subset.chars.txt

Requires: fonttools (see requirements.txt)
"""

from __future__ import annotations

import argparse
import glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------------------
# Defaults (paths relative to openvela tree)
# ---------------------------------------------------------------------------
HERE = os.path.dirname(os.path.abspath(__file__))
OPENVELA_ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
SDK_ROOT = os.path.normpath(os.path.join(OPENVELA_ROOT, ".."))

FONT_TOOL_DIR = HERE
DEFAULT_SRC_DIR = os.path.join(
    OPENVELA_ROOT,
    "vendor/openvela/boards/vela/bicycle/etc/font",
)
DEFAULT_OUT_DIR = os.path.join(DEFAULT_SRC_DIR, "subset")
DEFAULT_MYVENDOR_FONT_DIR = os.path.join(
    OPENVELA_ROOT,
    "vendor/my_vendor/boards/sf32lb52/my_vendor/mkfs/fat/fonts",
)
DEFAULT_VMAP_DIR = os.path.join(HERE, "..", "osm", "vmap")
DEFAULT_OSM_CHARS = os.path.join(DEFAULT_VMAP_DIR, "osm_chars.txt")
DEFAULT_CHINA_OSM_CHARS = os.path.join(HERE, "..", "osm", "data", "china_osm_chars.txt")
DEFAULT_I18N = os.path.join(
    OPENVELA_ROOT,
    "vendor/openvela/boards/vela/bicycle/src/App/Service/i18n/lv_i18n.c",
)

REGENERATE_HINT = (
    "cd vendor/my_vendor/docs/font && "
    ".venv/bin/python subset_font.py"
)

PUHUITI_FACES = [
    "Alibaba_PuHuiTi_2.0_55_Regular",
    "Alibaba_PuHuiTi_2.0_65_Medium",
    "Alibaba_PuHuiTi_2.0_115_Black",
]

FA_SOLID = "Font Awesome 6 Free-Solid-900"

BASE_ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
EXTRA_PUNCT = (
    "，。！？、；：""''（）【】《》…—·"
    "℃°×÷±"
    "←→↑↓"
)
# Demo / known map names that must stay in the subset even if vmap scan misses them.
EXTRA_PLACE_CHARS = "南谯区琅琊滁州譙區"


def gb2312_chars(which: str = "all") -> set[str]:
    rows_l1 = list(range(1, 10)) + list(range(16, 56))
    rows_l2 = list(range(10, 16)) + list(range(56, 88))
    if which == "l1":
        rows = rows_l1
    elif which == "l2":
        rows = rows_l2
    else:
        rows = list(range(1, 95))
    out: set[str] = set()
    for row in rows:
        for col in range(1, 95):
            try:
                out.add(bytes([0xA0 + row, 0xA0 + col]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return out


def _label_chars_from_vtil(b: bytes) -> set[str]:
    chars: set[str] = set()
    if len(b) < 20 or b[:4] != b"VTIL":
        return chars
    nl = struct.unpack("<H", b[16:18])[0]
    off = 20
    for _ in range(nl):
        if off + 8 > len(b):
            break
        lid, _r, fc, blob = struct.unpack("<BBHI", b[off : off + 8])
        off += 8
        if lid == 4:
            o = off
            for _ in range(fc):
                if o + 8 > len(b):
                    break
                _k, _p, _x, _y, ln, _rr = struct.unpack("<BBHHBB", b[o : o + 8])
                o += 8
                if o + ln <= len(b):
                    chars.update(b[o : o + ln].decode("utf-8", "replace"))
                o += ln
        off += blob
    return chars


def collect_from_vmap(vmap_dir: str) -> set[str]:
    chars: set[str] = set()
    if not os.path.isdir(vmap_dir):
        return chars
    try:
        tools = os.path.join(
            OPENVELA_ROOT,
            "vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/tools",
        )
        if tools not in sys.path:
            sys.path.insert(0, tools)
        from pack_vmap import collect_tiles
        from pathlib import Path

        for tile in collect_tiles(Path(vmap_dir)):
            chars.update(_label_chars_from_vtil(tile.data))
        if chars:
            return chars
    except Exception:
        pass
    for z in ("14", "15"):
        pattern = os.path.join(vmap_dir, z, "*", "*.vt")
        for path in glob.glob(pattern):
            try:
                with open(path, "rb") as fp:
                    chars.update(_label_chars_from_vtil(fp.read()))
            except OSError:
                continue
    return chars


def collect_from_text_file(path: str) -> set[str]:
    chars: set[str] = set()
    if not os.path.isfile(path):
        return chars
    with open(path, "r", encoding="utf-8", errors="replace") as fp:
        for line in fp:
            if line.lstrip().startswith("#"):
                continue
            for c in line:
                if c.isprintable() and c not in "\r\n":
                    chars.add(c)
    return chars


def collect_from_paths(extra_paths: list[str]) -> set[str]:
    chars: set[str] = set()
    for p in extra_paths:
        if os.path.isfile(p):
            chars.update(collect_from_text_file(p))
        elif os.path.isdir(p):
            for root, _dirs, files in os.walk(p):
                for fn in files:
                    if fn.endswith((".c", ".cpp", ".h", ".json", ".txt", ".md", ".yml")):
                        chars.update(
                            collect_from_text_file(os.path.join(root, fn))
                        )
    return chars


def china_osm_chars() -> set[str]:
    return collect_from_text_file(DEFAULT_CHINA_OSM_CHARS)


def build_charset(
    vmap_dir: str,
    i18n_path: str,
    extra_paths: list[str],
    preset: str = "osm",
) -> tuple[str, dict]:
    base = set(BASE_ASCII + EXTRA_PUNCT + EXTRA_PLACE_CHARS)
    vmap = collect_from_vmap(vmap_dir)
    i18n = collect_from_text_file(i18n_path)
    i18n |= collect_from_paths(extra_paths)
    scan = base | vmap | i18n
    osm = china_osm_chars()
    common: set[str] = set()
    if preset == "osm":
        common = osm
        if not common:
            print(
                "[warn] missing nationwide OSM charset %s "
                "(run docs/osm/extract_osm_chars.py)" % DEFAULT_CHINA_OSM_CHARS,
                file=sys.stderr,
            )
    elif preset == "common2m":
        common = gb2312_chars("all") | osm
    elif preset != "scan":
        raise ValueError("unknown preset: %s" % preset)
    all_chars = scan | common
    meta = {
        "preset": preset,
        "scan_count": len(scan),
        "common_count": len(common),
        "osm_count": len(osm),
        "gb2312_cjk": sum(1 for c in common if "\u4e00" <= c <= "\u9fff")
        if preset == "common2m"
        else 0,
    }
    return "".join(sorted(all_chars, key=lambda c: ord(c))), meta


def run_pyftsubset(
    src: str,
    dst: str,
    text: str,
    *,
    awesome_pua: bool = False,
) -> None:
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", suffix=".txt", delete=False
    ) as tf:
        tf.write(text)
        text_file = tf.name

    cmd = [
        sys.executable,
        "-m",
        "fontTools.subset",
        src,
        f"--output-file={dst}",
        f"--text-file={text_file}",
        "--layout-features=*",
        "--recommended-glyphs",
        "--no-hinting",
    ]
    if awesome_pua:
        cmd.append("--unicodes=U+F000-U+F8FF")
    print(
        "[subset] %s -> %s (%d chars)"
        % (os.path.basename(src), os.path.basename(dst), len(text)),
        file=sys.stderr,
    )
    try:
        subprocess.check_call(cmd)
    finally:
        os.unlink(text_file)


def human_size(n: int) -> str:
    if n < 1024:
        return "%d B" % n
    if n < 1024 * 1024:
        return "%.1f KB" % (n / 1024)
    return "%.2f MB" % (n / 1024 / 1024)


def charset_breakdown(vmap_dir: str, i18n_path: str, extra_paths: list[str]):
    base = set(BASE_ASCII + EXTRA_PUNCT + EXTRA_PLACE_CHARS)
    vmap = collect_from_vmap(vmap_dir)
    i18n = collect_from_text_file(i18n_path)
    i18n |= collect_from_paths(extra_paths)
    all_chars = base | vmap | i18n
    return base, vmap, i18n, all_chars


def _sorted_str(chars: set[str]) -> str:
    return "".join(sorted(chars, key=lambda c: ord(c)))


def write_regular_subset_md(
    out_dir: str,
    charset: str,
    base: set[str],
    vmap: set[str],
    i18n: set[str],
    ttf_name: str,
    meta: dict,
) -> None:
    ttf_path = os.path.join(out_dir, ttf_name)
    ttf_size = human_size(os.path.getsize(ttf_path)) if os.path.isfile(ttf_path) else "?"
    cjk = sum(1 for c in charset if "\u4e00" <= c <= "\u9fff")
    vmap_only = vmap - base - i18n
    base_only = base - vmap - i18n
    common = gb2312_chars("all") if meta.get("preset") == "common2m" else set()

    md_path = os.path.join(out_dir, "Alibaba_PuHuiTi_2.0_55_Regular.subset.md")
    preset = meta.get("preset", "?")
    lines = [
        "# Alibaba_PuHuiTi_2.0_55_Regular.subset.ttf 说明",
        "",
        "本文件是 **阿里巴巴普惠体 2.0 Regular** 的子集字体，供固件 TinyTTF / FreeType 加载。",
        "字符集与同级 `Medium.subset.ttf` / `Black.subset.ttf` **完全相同**，仅字重不同。",
        "",
        "## 基本信息",
        "",
        "| 项 | 值 |",
        "|---|---|",
        "| 子集文件 | `%s` |" % ttf_name,
        "| 文件大小 | %s |" % ttf_size,
        "| 预设 | `%s` |" % preset,
        "| 全量源字体 | `../Alibaba_PuHuiTi_2.0_55_Regular.ttf`（8.1 MB，未删除） |",
        "| 固件路径 (vela ROMFS) | `/etc/font/subset/%s` |" % ttf_name,
        "| 固件路径 (my_vendor FAT) | `/mnt/fat/fonts/%s` |" % ttf_name,
        "| 用途 | 地图路名、Dashboard、主题 UI（`regular` 字重） |",
        "",
        "## 字符集构成",
        "",
    ]
    if preset == "common2m":
        lines += [
            "目标体积 **~2 MB**；实际约 **%s**（PuHuiTi Regular）。" % ttf_size,
            "",
            "| 来源 | 数量 | 说明 |",
            "|---|---:|---|",
            "| **合计** | **%d** | 全部码点 |" % len(charset),
            "| 汉字 (CJK) | %d | |" % cjk,
            "| GB2312 常用字表 | %d | 一级+二级，覆盖几千常用汉字 |"
            % meta.get("common_count", 0),
            "| GB2312 内汉字 | %d | |" % meta.get("gb2312_cjk", 0),
            "| vmap/i18n 扫描 | %d | 地图 UTF-8 标签 + 界面文案 |"
            % meta.get("scan_count", 0),
            "| 地图标签独有 | %d | 不在 GB2312 表内的额外路名用字 |"
            % len(vmap_only - common),
            "| 基础 ASCII+标点 | %d | |" % len(base_only),
            "",
            "> 地图 `.vt` 标签仍是 **UTF-8** 存储；GB2312 仅用于扩充字体字形库，避免缺字。",
            "> 完整字表见同级 `Alibaba_PuHuiTi_2.0_55_Regular.subset.chars.txt`",
            "",
        ]

    lines += [
        "## 再生",
        "",
        "```bash",
        "cd vendor/my_vendor/docs/font",
        "python3 -m venv .venv && .venv/bin/pip install -r requirements.txt",
        ".venv/bin/python subset_font.py                    # common2m ~2MB",
        ".venv/bin/python subset_font.py --sync-myvendor    # 同步 Regular 到 SF32 LFS",
        ".venv/bin/python subset_font.py --preset scan      # 最小 ~100KB",
        ".venv/bin/python subset_font.py --write-chars-file # 导出完整字表 txt",
        "```",
        "",
    ]
    with open(md_path, "w", encoding="utf-8") as fp:
        fp.write("\n".join(lines))
    print("[write] %s" % md_path, file=sys.stderr)


def write_chars_file(out_dir: str, charset: str, ttf_name: str) -> None:
    base = ttf_name.replace(".ttf", ".chars.txt")
    path = os.path.join(out_dir, base)
    with open(path, "w", encoding="utf-8") as fp:
        fp.write("# %d codepoints, sorted by Unicode\n" % len(charset))
        fp.write(charset)
        fp.write("\n")
    print("[write] %s (%d chars)" % (path, len(charset)), file=sys.stderr)


def sync_myvendor_regular(out_dir: str, myvendor_dir: str) -> None:
    src = os.path.join(out_dir, "Alibaba_PuHuiTi_2.0_55_Regular.subset.ttf")
    if not os.path.isfile(src):
        print("[warn] missing Regular subset, skip my_vendor sync: %s" % src, file=sys.stderr)
        return
    os.makedirs(myvendor_dir, exist_ok=True)
    dst = os.path.join(myvendor_dir, os.path.basename(src))
    shutil.copy2(src, dst)
    print(
        "[sync] %s -> %s (%s)"
        % (src, dst, human_size(os.path.getsize(dst))),
        file=sys.stderr,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="Build firmware font subsets (~2 MB).")
    ap.add_argument(
        "--src-dir",
        default=DEFAULT_SRC_DIR,
        help="directory with full .ttf (default: vela bicycle/etc/font)",
    )
    ap.add_argument(
        "--out-dir",
        default=DEFAULT_OUT_DIR,
        help="output directory (default: .../font/subset)",
    )
    ap.add_argument(
        "--myvendor-font-dir",
        default=DEFAULT_MYVENDOR_FONT_DIR,
        help="my_vendor LFS staging fonts/ directory",
    )
    ap.add_argument(
        "--vmap-dir",
        default=DEFAULT_VMAP_DIR,
        help="vmap root with z14/z15 tiles (default: ../osm/vmap)",
    )
    ap.add_argument("--i18n", default=DEFAULT_I18N, help="lv_i18n.c path")
    ap.add_argument(
        "--extra",
        action="append",
        default=[],
        help="extra UTF-8 file/dir to scan for text",
    )
    ap.add_argument(
        "--preset",
        choices=("osm", "common2m", "scan"),
        default="osm",
        help="osm: nationwide OSM names; common2m: GB2312+OSM; scan: vmap+i18n only",
    )
    ap.add_argument(
        "--write-chars-file",
        action="store_true",
        help="write full charset to .subset.chars.txt",
    )
    ap.add_argument("--list-chars", action="store_true", help="print charset and exit")
    ap.add_argument(
        "--faces",
        nargs="*",
        default=PUHUITI_FACES,
        help="PuHuiTi base names to subset",
    )
    ap.add_argument(
        "--awesome",
        action="store_true",
        default=True,
        help="also subset Font Awesome solid (default: on)",
    )
    ap.add_argument("--no-awesome", action="store_true", help="skip Font Awesome subset")
    ap.add_argument(
        "--sync-myvendor",
        action="store_true",
        help="copy Regular subset to my_vendor mkfs/fonts/",
    )
    args = ap.parse_args()

    extra_paths = list(args.extra)
    if os.path.isfile(DEFAULT_OSM_CHARS):
        extra_paths.append(DEFAULT_OSM_CHARS)

    charset, meta = build_charset(
        args.vmap_dir,
        args.i18n,
        extra_paths,
        preset=args.preset,
    )
    cjk = sum(1 for c in charset if "\u4e00" <= c <= "\u9fff")
    print(
        "[charset] preset=%s total=%d CJK=%d common=%d scan=%d"
        % (
            args.preset,
            len(charset),
            cjk,
            meta.get("common_count", 0),
            meta.get("scan_count", 0),
        ),
        file=sys.stderr,
    )

    if args.list_chars:
        print(charset)
        return 0

    if not os.path.isdir(args.src_dir):
        print("[error] src-dir not found: %s" % args.src_dir, file=sys.stderr)
        return 1

    os.makedirs(args.out_dir, exist_ok=True)
    base, vmap, i18n, _all = charset_breakdown(args.vmap_dir, args.i18n, extra_paths)

    manifest_lines = [
        "# Font subsets generated by subset_font.py",
        "# preset=%s  chars=%d" % (args.preset, len(charset)),
        "# Regenerate: %s" % REGENERATE_HINT,
        "",
    ]

    for face in args.faces:
        src = os.path.join(args.src_dir, face + ".ttf")
        dst = os.path.join(args.out_dir, face + ".subset.ttf")
        if not os.path.isfile(src):
            print("[warn] missing source: %s" % src, file=sys.stderr)
            continue
        run_pyftsubset(src, dst, charset)
        sz = os.path.getsize(dst)
        manifest_lines.append("%-45s %s" % (os.path.basename(dst), human_size(sz)))
        print("[write] %s  %s" % (dst, human_size(sz)), file=sys.stderr)

    if args.awesome and not args.no_awesome:
        src = os.path.join(args.src_dir, FA_SOLID + ".ttf")
        dst = os.path.join(args.out_dir, FA_SOLID + ".subset.ttf")
        if os.path.isfile(src):
            cmd = [
                sys.executable,
                "-m",
                "fontTools.subset",
                src,
                f"--output-file={dst}",
                "--unicodes=F000-F8FF,0020-007E",
                "--layout-features=*",
                "--recommended-glyphs",
                "--no-hinting",
            ]
            print(
                "[subset] %s -> %s (PUA icons)"
                % (os.path.basename(src), os.path.basename(dst)),
                file=sys.stderr,
            )
            subprocess.check_call(cmd)
            sz = os.path.getsize(dst)
            manifest_lines.append("%-45s %s" % (os.path.basename(dst), human_size(sz)))
            print("[write] %s  %s" % (dst, human_size(sz)), file=sys.stderr)
        else:
            print("[warn] missing: %s" % src, file=sys.stderr)

    manifest = os.path.join(args.out_dir, "MANIFEST.txt")
    with open(manifest, "w", encoding="utf-8") as fp:
        fp.write("\n".join(manifest_lines) + "\n")
    print("[done] manifest -> %s" % manifest, file=sys.stderr)

    write_regular_subset_md(
        args.out_dir,
        charset,
        base,
        vmap,
        i18n,
        "Alibaba_PuHuiTi_2.0_55_Regular.subset.ttf",
        meta,
    )
    if args.write_chars_file or args.preset == "common2m":
        write_chars_file(
            args.out_dir, charset, "Alibaba_PuHuiTi_2.0_55_Regular.subset.ttf")

    if args.sync_myvendor:
        sync_myvendor_regular(args.out_dir, args.myvendor_font_dir)

    print("", file=sys.stderr)
    print("Tool dir: %s" % FONT_TOOL_DIR, file=sys.stderr)
    print("Output:   %s" % args.out_dir, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
