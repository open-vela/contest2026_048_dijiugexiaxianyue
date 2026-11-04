# 矢量字体工具（板卡层）

Canonical 目录：`vendor/HelmOne/docs/font/`  
vela 层入口：`vendor/openvela/docs/font` → 软链接到本目录

## 工具

| 脚本 | 用途 |
|------|------|
| `subset_font.py` | 从全量 TTF + vmap/i18n/全国 OSM 字表生成子集 `.subset.ttf` |
| `embed_ttf.py` | 将子集 TTF 嵌入为 C 数组（medium/bold/awesome Flash 常驻） |

字库底表、繁简、全国抽字见 [FONT_CHARSET.md](FONT_CHARSET.md)。

## 环境

```bash
cd vendor/HelmOne/docs/font
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

## 生成子集

默认 `--preset osm`：全国 OSM 名称底表 + 当前 vmap + i18n。

源字体：`vendor/openvela/boards/vela/bicycle/etc/font/*.ttf`  
输出：`vendor/openvela/boards/vela/bicycle/etc/font/subset/`

```bash
.venv/bin/python subset_font.py                 # osm 底表
.venv/bin/python subset_font.py --preset scan        # 最小字表 ~100KB
.venv/bin/python subset_font.py --preset common2m    # 再并 GB2312
.venv/bin/python subset_font.py --write-chars-file
```

## 同步到 SF32 LFS staging

```bash
.venv/bin/python subset_font.py --sync-myvendor
```

复制 `Regular.subset.ttf` 到  
`vendor/HelmOne/boards/sf32lb52/helmone/mkfs/fonts/`  
之后执行 `build-fs` / `flash-fs`。

## 嵌入 Flash 字体（CMake 构建时）

`embed_ttf.py` 由 vela bicycle `CMakeLists.txt` 在开启  
`CONFIG_BICYCLE_FONT_REGULAR_PSRAM` 时调用，无需手工运行。

```bash
.venv/bin/python embed_ttf.py awesome \
  vendor/openvela/boards/vela/bicycle/etc/font/subset/Font\ Awesome\ 6\ Free-Solid-900.subset.ttf \
  /tmp/font_embedded_awesome.c
```

## 依赖数据

> 详细文档：[../osm/README.md](../osm/README.md)（OSM 转换）、[../map/README.md](../map/README.md)（打包烧录）。

- 全国名称底表：`../osm/data/china_osm_chars.txt`（见 [FONT_CHARSET.md](FONT_CHARSET.md)）
- 地图标签：`../osm/vmap/`（loose 瓦片，供扫字；packed 见 `mkfs/fat/map/`）
- i18n 文案：`vendor/openvela/boards/vela/bicycle/src/App/Service/i18n/lv_i18n.c`
