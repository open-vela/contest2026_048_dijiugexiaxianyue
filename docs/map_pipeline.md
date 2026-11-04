# 离线矢量地图流水线（单独说明）

> 这一篇是给评审的**入口**：离线全国地图是本作品最重的一块工程，而它的**数据体积远超
> 任何代码仓能承载的量级**。所以仓里放的是「工具链 + 样本 + 文档」，全量数据在开发机上
> 生成、按需下发到 SD 卡。本篇说清楚：**有什么、没有什么、怎么补齐、许可是什么**。
>
> 全量操作手册在 [`helm-one/docs/osm/README.md`](../helm-one/docs/osm/README.md)，
> 渲染与加载算法在 [`helm-one/docs/map/RUNTIME.md`](../helm-one/docs/map/RUNTIME.md)。

---

## 一、为什么单独说明

| 事实 | 数字 |
| --- | --- |
| 全国 OSM 原始数据（`china-latest.osm.pbf`） | **1.55 GB** |
| 三城设备包（滁州 / 南京 / 广州，3 km 网格） | **5 925 个 `.vpk` 分片 / 约 314 MB** |
| 仓内附带的样本包 | **64 个 `.vpk` / 7.1 MB**（`mkfs/fat/map/lon883~885`） |
| 运行时依赖 | 只读 FAT 卷 `/mnt/fat/map/`，按视野**按需加载**，PSRAM 缓存 |

把 314 MB 二进制塞进 git 会毁掉仓库（且每次改动都翻倍），把 1.55 GB PBF 放进去更不可能。
因此本仓的策略是：**代码与工具 100% 开源可复现，数据按脚本现生**。

## 二、数据分层

```text
OSM PBF（1.55 GB，自行下载）
   │  build_vmap.py --bbox / make_cities_map.py
   ▼
.vt 瓦片（矢量要素，未压缩）
   │  pack_vmap.py（z14/z15 打包、路名表、索引）
   ▼
.vpk 分片（3 km 网格，一格一文件）
   │  pack_map.py（合并多城、写 tiles.idx）
   ▼
SD FAT 卷 /mnt/fat/map/lon<LON>/lat<LAT>/x<x>_y<y>.vpk
```

路由侧独立一条线：

```text
OSM PBF ──build_vgraph.py──▶ graph.vgrf（VGRF v2：节点/边/等级/封路 + portal + 海拔）
                                  └─▶ 设备上的堆式 Dijkstra + 跨网格 portal 拼接
```

## 三、仓里有什么

| 路径 | 内容 |
| --- | --- |
| [`helm-one/docs/osm/`](../helm-one/docs/osm/README.md) | 完整操作手册 + 全部 Python 工具（抓取 / 转瓦片 / 打包 / 建路由图 / 繁简转换 / 字表抽取） |
| `helm-one/docs/osm/build_vmap.py` | OSM XML/PBF → `.vt` 瓦片（支持任意 `--bbox`） |
| `helm-one/docs/osm/build_vgraph.py` | OSM → `graph.vgrf`（路由图 + portal + 海拔） |
| `helm-one/docs/osm/make_cities_map.sh` | **当前设备包**的一键脚本（三城合包进 3 km 网格） |
| `helm-one/docs/osm/make_china_map.sh` | 全国一次性生成 |
| `helm-one/docs/osm/dem.py` | SRTM `.hgt` / Terrarium PNG 取海拔（爬坡页与坡度） |
| `helm-one/docs/osm/web/` | 网页版框选生成器 + 样式试验台（一次 ≤3°） |
| `helm-one/boards/sf32lb52/helmone/ui/bicycle/tools/pack_map.py` | `.vpk` 合包（固件侧构建链） |
| `helm-one/boards/sf32lb52/helmone/mkfs/fat/map/` | **64 片样本**，开箱即可在设备上看图 |
| `helm-one/docs/map/` | 渲染规范 `STYLE.md`、运行时 `RUNTIME.md`、性能实测 `perf/` |

样本包覆盖 `lon883~885 / lat369~371`（安徽滁州一带），足够验证：加载、按需换页、
路名排版、缩放、路由与偏航重规划。

## 四、仓里没有什么（以及怎么补）

| 缺的 | 为什么 | 怎么补 |
| --- | --- | --- |
| `china-latest.osm.pbf`（1.55 GB） | 体积；且 ODbL 数据应由使用者自行获取 | Geofabrik / BBBike 下载中国区 PBF |
| `osm_data/china_1deg/`（按经度切片的中间产物） | 生成物 | `make_china_map.sh` 自动切 |
| `cities/<city>/`（每城 `.vpk` 树 + `graph.vgrf`） | 生成物 | `make_cities_map.sh` |
| 三城全量 `map/`（314 MB / 5 925 片） | 生成物 + 体积 | 同上，产物直接写 `osm_data/map/` |
| DEM 瓦片缓存（`docs/osm/cache/`） | 可重新下载 | `dem.py` 按需拉 Terrarium |

补齐后按 3 步进设备：

```bash
# 1) 生成（在 openvela 工作区根目录）
bash vendor/HelmOne/docs/osm/make_cities_map.sh

# 2) 合包 + 打进 SD 整盘镜像（产物落到 mkfs/fat/map）
python3 vendor/HelmOne/boards/sf32lb52/helmone/ui/bicycle/tools/pack_map.py \
  -i /path/to/osm_data/map --graph /path/to/graph.vgrf \
  -o vendor/HelmOne/boards/sf32lb52/helmone/mkfs/fat/map --clean --pack-size 512000

# 3) 出镜像写卡
bash vendor/HelmOne/docs/tools/make_sd_8g.sh
```

## 五、开发机上的路径（兼容入口）

开发机上为了方便，SDK 根目录放了一个**软链接**：

```text
/home/jinsc/SDK/vela/osm  →  openvela/vendor/HelmOne/docs/osm
```

它只是路径别名，**不是第二份代码**；仓内 canonical 路径永远是
[`helm-one/docs/osm/`](../helm-one/docs/osm/README.md)。文档里出现 `docs/osm` 即指此处。

外部数据（不在任何仓库里、也不随仓发布）在 `~/SDK/vela/osm_data/`：
`china-latest.osm.pbf`、`china_1deg/`、`chuzhou/`、`cities/`、`map/`。

## 六、许可与署名

- **地图数据**来自 OpenStreetMap，遵循 **ODbL 1.0**：产品界面与本仓文档均需署名
  「© OpenStreetMap contributors」；派生数据（`.vpk`）如对外分发同样受 ODbL 约束。
- **DEM 海拔**来自 SRTM（公有领域）/ Terrarium（Mapzen，公开托管）。
- 仓内附带的 64 片样本与 `helm-one/docs/osm/osm/chuzhou.osm` 均属上述来源。

## 七、常见问题

| 现象 | 原因 | 处置 |
| --- | --- | --- |
| 设备上图空白 | FAT 卷里没有 `lon*` 目录 | 先跑 `make_sd_*.sh`（内含 pack_map） |
| 地图能显示但路由规划失败 | 缺 `graph.vgrf` 或区域不覆盖 | 重跑 `build_vgraph.py`，确认 bbox 与显示区一致 |
| 打包后体积远超预期 | `pack-size` 太小导致碎片 | 调大 `--pack-size`（默认 512000） |
| 中文路名乱码 | 字表与字库不匹配 | `extract_osm_chars.py` → `zh_simplify.py` → 字库子集重生成 |
| 爬坡页没有坡度 | 缺 DEM | `dem.py` 拉对应瓦片后重生成 `.vpk` |
