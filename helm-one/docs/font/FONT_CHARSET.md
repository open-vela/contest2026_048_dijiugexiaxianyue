# 地图矢量字库（底表与繁简）

记录 nationwide OSM 抽字、繁转简、以及和设备 TTF 的关系，方便后续继续改。

相关代码：

| 路径 | 作用 |
|------|------|
| [`extract_osm_chars.py`](../osm/extract_osm_chars.py) | 扫全国 PBF 名称，写出字表 |
| [`zh_simplify.py`](../osm/zh_simplify.py) | 繁转简（OpenCC `TSCharacters`）+ 设备字库过滤 |
| [`data/TSCharacters.txt`](../osm/data/TSCharacters.txt) | OpenCC 繁→简单字表 |
| [`data/china_osm_chars.txt`](../osm/data/china_osm_chars.txt) | **当前字库底表** |
| [`subset_font.py`](./subset_font.py) | 按底表切 TTF |
| [`build_vmap.py`](../osm/build_vmap.py) / [`build_vgraph.py`](../osm/build_vgraph.py) | 写路名时繁转简 |

设备当前加载：`/mnt/fat/fonts/MiSans-Medium.subset.ttf`  
（Kconfig `MYVENDOR_SYSTEM_FONT_FILE`，staging 在 `mkfs/fat/fonts/`）

---

## 背景

滁州底图 OSM 里同一路名同时有简体和繁体（`南谯区` / `南譙區`）。MiSans 子集有「谯」没有「譙」，路名方框缺字。

处理原则：

1. **瓦片/路网里的名字**：转换时繁→简，设备上不再依赖繁体字形。
2. **字库底表**：一次性抽出全国 OSM 所有名称用字（转简后），作为 `subset_font.py` 的默认字表，而不是 GB2312 全表。

---

## 当前底表（2026-09-01）

源：`/home/jinsc/SDK/vela/osm_data/china-latest.osm.pbf`（1.5 GB，2026-06-30）

| 项 | 值 |
|---|---|
| 扫描对象 | 251 478 704 |
| 名称标签 | 9 111 872 |
| 其中繁转简 | 658 344 |
| 过滤后码点 | **7582**（CJK **6784**） |
| CJK 扩展/生僻 | 约 183（GB2312 没有的地名用字） |
| 「谯」 | 有 |
| 「譙」 | 已丢掉（可转换繁体） |

过滤规则（`zh_simplify.is_device_font_char`）：只留拉丁、CJK（含 Ext A / SIP）、假名、注音、中日标点、全角。丢掉阿拉伯文、泰文、谚文、emoji 等。全脚本用 `--all-scripts`。

原始未过滤约 10551 码点。

---

## 再生

```bash
cd vendor/HelmOne/docs/osm
.venv/bin/python extract_osm_chars.py
# 默认写出 data/china_osm_chars.txt

# 只抽一份已有 .osm，不切片
.venv/bin/python build_vmap.py --osm osm/chuzhou.osm --chars-only
```

换全国 PBF：

```bash
VMAP_CHINA_PBF=/path/china-latest.osm.pbf .venv/bin/python extract_osm_chars.py
```

扫全国约半小时（tags-only，不建几何索引）。

切字体（默认 `--preset osm` = 全国底表 + 当前 vmap + i18n）：

```bash
cd vendor/HelmOne/docs/font
.venv/bin/python subset_font.py --preset osm
.venv/bin/python subset_font.py --preset common2m   # 再并上 GB2312 全表
```

运行时字体是 **MiSans**，不是普惠体。`subset_font.py` 目前默认切的是普惠体三份 face；MiSans 子集在 `mkfs/fat/fonts/MiSans-Medium.subset.ttf`，需要单独按底表重切后 `build-fs`。

---

## 转换 OSM 时的繁简

**默认就是简体**（`--zh hans`，可省略）。`pick_name()` 优先 `name:zh-Hans` / `name:zh-CN`，再对其余名称做繁→简。路名、水域名、`graph.vgrf` 边名都走同一套。

繁体是特殊转换：`--zh hant`（优先 `name:zh-Hant` / `name:zh-TW`，再简→繁）。

```bash
./make_chuzhou_map.sh                 # 默认简体
./make_chuzhou_map.sh --zh hant       # 繁体
./make_china_map.py --zh hant --only N32E118
```

**已切好的旧 `.vt` 仍可能是繁体。** 要屏上变成「南谯区」，需要按默认简体重切 `build_vmap` + `pack_map`。只改字库不够。

---

## 待优化

- [ ] 按 `china_osm_chars.txt` 重切 **MiSans-Medium.subset.ttf** 并烧 LFS
- [ ] 滁州（及全国）vmap 用新 `pick_name` 重切，去掉瓦片里的繁体
- [ ] TinyTTF / 源字体是否覆盖 Ext B、`𠀀` 一类生僻地名
- [ ] 底表是否再去掉日文假名、全角英文、兼容区汉字
- [ ] `subset_font.py` 增加 MiSans 输出路径（与 Kconfig 默认文件名对齐）
- [ ] 全国 PBF 更新后重跑 `extract_osm_chars.py` 的节奏
