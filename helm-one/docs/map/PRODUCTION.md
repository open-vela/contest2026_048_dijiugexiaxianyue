# 本地图制作方式

从 OpenStreetMap 到设备可读的 `map/` 目录，整条链路在 PC 完成；设备只读打包结果。

**当前设备包**是 3 km 全国网格、每格 `lon*/lat*/x*_y*.vpk`（无 `map.idx`，门户
在 vpk 尾部 VPOR）。城市合包步骤见 [../osm/CITIES_MAP.md](../osm/CITIES_MAP.md)。
下文仍保留 Web / 旧 `rNNN.vpk` + `map.idx` 说明，便于对照历史产物。

```
全国/区域 OSM (.osm / .osm.pbf)
    ↓  osmium extract（按 bbox 裁剪，可选）
    ↓  build_vmap.py（按 slippy 瓦片切片 → .vt）
    ↓  build_vgraph.py（可选，highway 有向图 → graph.vgrf）
    ↓  pack_map.py --region-km 3 --grid-global
map/
    └── lon<ix/4>/lat<iy/4>/x<ix>_y<iy>.vpk   # VTIL + VGRF + VPOR
```

工具位于 `vendor/HelmOne/docs/osm/`；Web 封装在 `docs/osm/web/`。

---

## 1. 数据源

| 来源 | 路径 / 方式 | 说明 |
|------|-------------|------|
| 全国 PBF | `/home/jinsc/SDK/vela/osm_data/china-latest.osm.pbf` | Web 生成器默认源 |
| Overpass XML | `docs/osm/osm/*.osm` | `make_map.sh fetch` 或手下载 |
| 裁剪子集 | `web/out/<job>/osm/extract.osm.pbf` | `osmium extract` 按框选 bbox |

坐标系约定：

- **OSM、GPS、设备内部、瓦片编号**：WGS84 + Web Mercator（标准 XYZ 瓦片）。
- **用户框选中心（高德/腾讯）**：GCJ02；`build_vmap.py --center-gcj` 会换算后再切片。
- **Web 框选矩形**：直接传 WGS84 `--bbox W,S,E,N`。

---

## 2. build_vmap.py — OSM → 矢量瓦片 `.vt`

将 OSM 几何按标准 slippy map 切成 `z/x/y.vt`，格式 **VTIL**（见 `vmap_format.h`）。

### 基本用法

```bash
cd vendor/HelmOne/docs/osm
.venv/bin/python build_vmap.py \
  --osm osm/chuzhou.osm \
  --center-gcj 118.3175,32.3025 \
  --zoom 14 \
  --radius-deg 0.12 \
  --out vmap
```

### 任意矩形（Web / 脚本推荐）

```bash
.venv/bin/python build_vmap.py \
  --osm extract.osm.pbf \
  --bbox 118.17,31.76,119.10,32.45 \
  --zoom 14 \
  --out vmap
```

`--bbox` 优先于 `--center-*` / `--radius-deg`。

### 输出结构

```
vmap/
├── 14/<tileX>/<tileY>.vt
└── 15/<tileX>/<tileY>.vt   # 可选；设备 demo 常用 z14 + 连续缩放
```

每个 `.vt` 内含分层几何（水域、林地、水系线、道路、标签），瓦片内坐标量化为
`extent=4096`。道路标签**按瓦片**生成锚点，保证可见瓦片内能显示路名（见
[../osm/DESIGN.md](../osm/DESIGN.md)）。

### 设备常用 zoom

| zoom | 用途 |
|------|------|
| **14** | 底图主层级（`VMAP_DEMO_ZOOM`） |
| 15 | 可选备用；当前固件用 `scale` 连续缩放，不切换 z15 层 |

---

## 3. build_vgraph.py — 离线路网（可选）

```bash
.venv/bin/python build_vgraph.py --osm osm/chuzhou.osm --out graph.vgrf
```

- 输出 **VGRF v2**（含 200 m SNAP 网格；默认再采样 DEM 写入 ELEV）。
- OSM 端点 **12 m 缝合**仅在此阶段完成。
- 路权与固件 `vmap_route.c` 对齐。
- 高度：`--dem-dir` 指向 SRTM `.hgt`，否则下载 Terrarium z12 PNG 到 `docs/osm/cache/dem/`。`VMAP_NO_DEM=1` 可关掉。

不建 graph 则只有底图，无离线路线规划。

---

## 4. pack_map.py — 打成设备目录

```bash
python3 vendor/HelmOne/boards/sf32lb52/helmone/ui/bicycle/tools/pack_map.py \
  -i vendor/HelmOne/docs/osm/vmap \
  --graph vendor/HelmOne/docs/osm/graph.vgrf \
  -o /path/to/map \
  --region-km 6 \
  --grid-global \
  --subdir \
  --max-zoom 0
```

### 默认：区域包（VREG）

未加 `--legacy-shards` 且 `--region-km > 0` 时走 **regional** 模式（当前推荐）。

#### 地理网格

1. 对每个 `.vt` 算瓦片中心 `(lon, lat)`（Web Mercator 反算，与设备一致）。
2. 按 `region_km` 换算 cell 宽高（度）：
   - `cell_lat = region_km / 111.32`
   - `cell_lon = region_km / (111.32 × cos(lat_ref))`
3. 瓦片归属 cell：`ix = floor((lon - origin_lon) / cell_lon)`，纬度同理。
4. 每个有瓦片的 cell 打一个 **`r<id>.vpk`**，`id` 为本次导出内从 0 递增的序号。

#### `--grid-global`（全国固定网格）

- 固定锚纬 **35°**、原点 **(0°, 0°)** 计算 `cell_lon/cell_lat`。
- 不同时间、不同 bbox 分批生成时，**同一地理格子的 `ix/iy` 一致**，便于拼全国索引。
- 未加此选项时，原点与 cell 尺寸由**当前批瓦片中心**推算（仅适合单次小区域）。

#### `--merge` / `--keep-bbox` / `--no-portals`

全国一键脚本按 1° 多次调用 `pack_map`，写入**同一个** `map/`：

- `--merge`：已有 `map.idx` 时按 `(ix, iy)` 对齐格子，新格分配新 `id`，同格覆盖。
- `--keep-bbox W,S,E,N`：只写入中心落在该 1° 内的格子，避免接缝被邻格覆盖。
- `--no-portals`：合并过程中不重算 `route.port`；全部结束后一次 `--rebuild-portals`。
- `--skip-empty-graph`：没有路网的格子（海域）不占 `uint16` 区域号。

#### `--subdir`（MTP 友好）

- `bucket = region_id / 20`
- 路径：`<map>/d<bucket>/r<id>.vpk`
- 单文件夹最多 **20** 个 `r*.vpk`；顶层仅 `map.idx`（+ `route.port`）。

#### `--max-zoom`

- `0` = 不丢弃任何已生成 zoom（Web 管道默认）。
- `14` = 只打进 z≤14 的瓦片（减小包体）。

### 产物布局示例

```
map/
├── map.idx                 # VREG：区域目录 + 网格元数据
├── route.port              # 跨区 portal（v2 索引 + 按需读块）
├── d000/
│   ├── r000.vpk
│   └── …
└── d001/
    └── r020.vpk
```

### 单文件 `rNNN.vpk` 内部

```
VPKG 头 (16 B)
→ RIDX 头 (52 B)：tileCount, graphOff, graphSz
→ tileCount × 14 B 瓦片表 (z, x, y, offset, size)，按 (z, y, x) 排序
→ 拼接的 VTIL blob
→ （可选）裁剪到该区域 bbox **外扩 `GRAPH_CLIP_MARGIN_M`（默认 600m）** 的 VGRF 片段
```

> **重叠边界（关键）**：路由图按 cell bbox 外扩 `GRAPH_CLIP_MARGIN_M`（`pack_map.py`
> 顶部常量，默认 **600m**）裁剪，使相邻区域的图在边界处重叠。设备端据此在
> "终点刚跨过区界"时走**单区规划**（`VMAP_ROUTE_REGION_REACH_M`=300m），避免慢且
> 易出残桩的 corridor。改大此值可扩大单区可达带并减少边界断链，但会增大每个
> `rNNN.vpk` 的图体积。**改动后需重新打包。**

### `map.idx`（VREG）

```
magic "VREG" | u8 version | u16 regionCount
f64 origin_lon, origin_lat, cell_lon, cell_lat
regionCount × RegionRec (48 B):
  u16 id, grid_ix, grid_iy, neighbors
  f64 west, south, east, north
```

`neighbors` 为四邻接位掩码，供跨区路径规划选邻区。

### `route.port`（有导航图时）

`portal_match.py` 在相邻区域边界匹配路网节点，写出跨区接点表。

| 版本 | 设备行为 |
|------|----------|
| v1 | 全文件载入 RAM，查询线性扫描 |
| **v2** | RAM 只保留 `(region_a, region_b)` 对索引；查询时按对读一块记录（推荐） |

---

## 5. 三条打包规则

面向 SF32 LittleFS + **MTP 上传**约束：

| 规则 | 实现 |
|------|------|
| ① 单文件约 500–700 KB | `--region-km` 控制每格瓦片数量；默认 6 km。结果面板/Web 日志会标最大分片；偏大则减小 `region_km` 或只打 z14 |
| ② 单文件夹文件数可控 | `--subdir`：每 `dNNN/` ≤ 20 个 `r*.vpk` |
| ③ 全国可增量拼接 | `--grid-global`：固定全国网格，分批生成边界对齐 |

loose 的 `vmap/<z>/<x>/<y>.vt` **不上传设备**，仅用于检查、字体扫字、缓存复用。

---

## Web 框选生成器

路径：`vendor/HelmOne/docs/osm/web/`。

```bash
./run.sh    # http://127.0.0.1:5000
```

流程（`vmap_pipeline.py`）：

1. 根据框选 bbox 计算所需 slippy 瓦片集合。
2. 查 `web/cache/tiles/`：**PBF 未变且瓦片已存在则跳过切片**。
3. `osmium extract` 裁剪 bbox（有缺失瓦片时必跑，约数秒扫全国 PBF）。
4. 对缺失 zoom 跑 `build_vmap.py --bbox …`。
5. 可选 `build_vgraph.py` + `pack_map.py --grid-global --subdir --max-zoom 0`。
6. 输出 `out/<name>/map/`。

面板橙色虚线预览 **全国固定网格** 与 bbox 交集，便于估计 `rNNN.vpk` 数量。

环境变量见 [../osm/web/README.md](../osm/web/README.md)。

Web **不会**一次生成全国：`VMAP_MAX_BBOX_DEG` 默认 3°。全国包用下一节脚本。

---

## 5b. 全国一次性生成

`build_vmap.py` 会把 OSM 要素载入内存，不能直接解析整份 1.5 GB `china-latest.osm.pbf`。对每个 15 km 格子单独 `osmium extract` 又会把整份 PBF 扫约十万次。实际做法是 **1° 切块 + 合并进固定网格**：

```bash
cd vendor/HelmOne/docs/osm
./make_china_map.sh --dry-run
./make_china_map.sh
```

| 步骤 | 做什么 |
|------|--------|
| 1° 切块 | `osmium extract -c` 每批约 80 个 bbox，约 20 余次扫 PBF |
| 每 1° | `build_vmap.py --bbox`（z14 packed）→ `build_vgraph.py`（Terrarium 路网点）→ `pack_map.py --merge --grid-global --subdir --no-portals --keep-bbox` |
| 收尾 | `pack_map.py --rebuild-portals` |

路径：

| | 默认 |
|--|------|
| 源 PBF | `/home/jinsc/SDK/vela/osm_data/china-latest.osm.pbf` |
| 1° 中间文件 | `/home/jinsc/SDK/vela/osm_data/china_1deg/` |
| 设备包 | `/home/jinsc/SDK/vela/osm_data/map/` |

整次运行约数小时，可用 `*.osm.pbf.done` 续跑。高度仍是路网点采样，不下载全国 DEM。

---

## 6. 旧模式：VIDX 城市分片（`p*.vpk`）

滁州 demo / `make_map.sh` 仍可使用按**体积**切分的旧格式：

```bash
pack_map.py -i vmap --graph graph.vgrf -o mkfs/fat/map \
  --legacy-shards --pack-size 512000
```

```
map.idx (VIDX) + p000.vpk + p001.vpk + …
```

- 瓦片表在顶层 `map.idx`，按 `(z, y, x)` 二分查找。
- 大图 `graph.vgrf` 可跨多个 `p*.vpk` 分片。
- **无**地理网格、**无** `route.port` 区域接点（单城图内导航）。

新区域请勿再用此模式，除非维护旧 demo。

---

## 7. 制作检查清单

- [ ] bbox 覆盖骑行/GPX 起点与活动范围（可用 [README 验证脚本](./README.md) 或 Web 预览网格）
- [ ] z14 瓦片已生成并打进包
- [ ] 需要导航：`graph.vgrf` + `route.port` 存在
- [ ] 最大 `r*.vpk` 体积可接受（≈700 KB 内较理想）
- [ ] 新汉字已跑 `subset_font.py --sync-myvendor`（若改 mkfs 烧录）
- [ ] MTP：`map/` 整目录上传；或 `build-fs` + `flash-fs`

---

## 8. 常见问题

**Q: 框选很大但只想 z14**  
A: Web 上只勾 z14；或 `pack_map.py --max-zoom 14`。

**Q: 分片仍 > 1 MB**  
A: 减小 `region_km`（如 4–5），或缩小 bbox / 不建 graph。

**Q: 重叠区域重复生成很慢**  
A: 使用 Web 生成器瓦片缓存；完全包含的旧区域可秒出（仍可能跑 osmium extract）。

**Q: 设备显示城市对但位置偏**  
A: 多为地图中心/GPX 起点不在预期点，不是切片错误；用 WGS84 核对 `map.idx` 里 region bbox。

**Q: 屏幕下方一条空白**  
A: 多为视野跨两个 `rNNN.vpk` 而固件未加载邻区瓦片；需固件侧跨区加载（见 [RUNTIME.md](./RUNTIME.md)）。
