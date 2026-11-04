# 本地矢量地图（vmap）

bicycle 码表使用的**离线矢量底图**从 OSM 在 PC 上生成，经 MTP 或烧录放到设备
LittleFS，由固件 `vmap_*` 模块按需加载并软件光栅化到 `lv_canvas`。

本文档目录说明**制作**与**加载**两条主线；二进制细节与导航算法见
[../osm/DESIGN.md](../osm/DESIGN.md)、[../osm/ROUTING.md](../osm/ROUTING.md)。

---

## 文档索引

| 文档 | 内容 |
|------|------|
| [LIVEMAP.md](./LIVEMAP.md) | **固件约定**：retained 大画布、缓存生命周期、标签算法、REC/nav 图层顺序、禁止回退项、验收清单 |
| [PRODUCTION.md](./PRODUCTION.md) | **制作方式**：OSM → `.vt` → 打包；Web 框选；旧 `rNNN.vpk` 说明 |
| [RUNTIME.md](./RUNTIME.md) | **加载与渲染**：3 km cell、跟车平移、rebase 脏条、pump 解算、降负载 |
| [perf/README.md](./perf/README.md) | **性能专题**：已落地的光栅化优化（按行裁剪并集 / 成对 32 位写 / 圆盘去 sqrt / 多边形去逐行软除）、"像素逐位不变"的差分测试、改前改后实测数据 |
| [perf/PUMP_PERIOD.md](./perf/PUMP_PERIOD.md) | **一次改动的完整记录（否掉）**：渲染泵周期 20→10 ms 为什么拿不到收益（配对帧数据 + 系统节拍 10 ms、每拍工作 15~38 ms ⇒ 工作受限） |
| [perf/POLYGON_DDA.md](./perf/POLYGON_DDA.md) | **一次改动的完整记录（方案 + 测试结果）**：多边形填充去掉逐行 64 位软除 —— 先量的 `[vperf] prim` 拆分表、floor 形式的推导与两个坑、宿主差分矩阵（30 万例）、上板定点 A/B（−7.6%）、收益上限为什么被 PSRAM 回写吃掉、复跑命令 |
| [GPU_ACCEL.md](./GPU_ACCEL.md) | **GPU/搬运专题**：已落地的 EPIC 搬运分层、**航向朝上（地图旋转）**的方案与 §5 的失败回退记录、跨 MCU 移植用的厂商中立 API 面与契约 |
| [perf/TESTS.md](./perf/TESTS.md) | **测试与复核**：三层验证（宿主差分 / 设备自检 / 端到端）各管什么、套件清单与命令、**基线分水岭（搬运接入）**、已知缺口 |
| [../osm/CITIES_MAP.md](../osm/CITIES_MAP.md) | **滁州/南京/广州合包**（当前设备包，`lon*/lat*/x*_y*.vpk` + VPOR） |
| [../osm/README.md](../osm/README.md) | OSM 工具链操作指南（`build_vmap.py`、`make_cities_map.sh`） |
| [../osm/web/README.md](../osm/web/README.md) | 网页框选生成器；**打包结果预览** `view_map.sh` |
| [../osm/ROUTING.md](../osm/ROUTING.md) | VGRF 路网、Dijkstra、每 cell **VPOR** |

---

## 两种打包模式

| | **坐标网格（推荐）** | **城市分片（旧 demo）** |
|---|-------------------|------------------------|
| `map.idx` | **无**（路径由 lon/lat 算出） | `VIDX` |
| 瓦片容器 | `lon*/lat*/x*_y*.vpk`（3 km 全国网格） | `pNNN.vpk`（按体积切分） |
| 门户 | 每个 vpk 尾部 **VPOR** | 无跨区 portal |
| 典型用途 | 城市合包、MTP 上传 | 旧滁州 demo |
| 生成 | `make_cities_map.sh` / `pack_map.py --grid-global` | `pack_map.py --legacy-shards` |

**新区域、MTP、多城增量**一律用 **坐标网格 cell 包**。旧 VIDX 仍可读。历史
`dNNN/rNNN.vpk` + `map.idx`(VREG) 固件不再作为制作目标。

---

## 路径总览

| 阶段 | 路径 | 说明 |
|------|------|------|
| OSM 源 | `/home/jinsc/SDK/vela/osm_data/china-latest.osm.pbf` | 全国 PBF（Web 生成器裁剪用） |
| PC 工具 | `docs/osm/` | `build_vmap.py`、`build_vgraph.py` |
| Web 生成 | `docs/osm/web/out/<name>/map/` | 框选产物，**直接 MTP 上传** |
| 烧录源 | `boards/.../mkfs/fat/map/` | `build-fs` 打进 `fat_root.bin`（FAT 卷的 `map/`） |
| 设备挂载 | `/mnt/fat/map/` | `CONFIG_VMAP_DIR_PATH`；产品只读，工厂读写 |
| 模拟器 | `bicycle/etc/vmap/` → `/etc/vmap` | loose `.vt`，不经 pack |

---

## 快速开始

### 方式 A：城市合包（当前设备数据）

```bash
cd vendor/HelmOne/docs/osm
./make_cities_map.sh --only guangzhou --reuse
./make_cities_map.sh --only chuzhou --reuse
./make_cities_map.sh --only nanjing --reuse
```

产物 `/home/jinsc/SDK/vela/osm_data/map/`，MTP 到 `/mnt/lfs/map/`。详见
[../osm/CITIES_MAP.md](../osm/CITIES_MAP.md)。

### 方式 B：Web 框选

```bash
cd vendor/HelmOne/docs/osm/web
./run.sh
# 浏览器 http://127.0.0.1:5000 — 框选区域 → 生成
# 上传 out/<name>/map/ 到设备 /mnt/lfs/map/
```

详见 [PRODUCTION.md § Web 生成器](./PRODUCTION.md#web-框选生成器) 与
[../osm/web/README.md](../osm/web/README.md)。

### 方式 C：命令行（旧滁州 demo）

```bash
cd vendor/HelmOne/docs/osm
./make_map.sh                    # fetch + z14/z15 + graph + pack → mkfs/fat/map
# 或手动 build_vmap.py + pack_map.py，见 PRODUCTION.md
```

### 方式 D：全国一次性生成

Web 单次框选不超过 3°。全国包：

```bash
cd vendor/HelmOne/docs/osm
./make_china_map.sh --dry-run
./make_china_map.sh              # 产物 /home/jinsc/SDK/vela/osm_data/map/
```

见 [PRODUCTION.md § 全国一次性生成](./PRODUCTION.md#5b-全国一次性生成)。

### 方式 E：浏览器预览打包结果

```bash
cd vendor/HelmOne/docs/osm/web
./view_map.sh                 # http://127.0.0.1:8766 ，默认打开 osm_data/map
```

页面会解析 `map.idx` / `r*.vpk` 里的 VTIL，按设备配色画出矢量图。

---

## 部署到设备

### MTP 上传（区域包常用）

把生成目录下的 **`map/`** 整体复制到设备 **`/mnt/lfs/map/`**：

```
map/
└── lon858/lat…/x…_y….vpk
```

路径公式与门户重建见 [../osm/CITIES_MAP.md](../osm/CITIES_MAP.md)。
`mkfs/fat/map` 不会自动跟上 `osm_data/map`。

### 烧录 LittleFS（整包固件）

```bash
# openvela 根目录
python3 vendor/HelmOne/build_board.py build-fs
python3 vendor/HelmOne/build_board.py flash-fs -p /dev/ttyUSB0
```

`flash-fs` 会擦除 FS 分区再写入，避免旧 LittleFS 元数据残留。

### 验证

```text
nsh> ls /mnt/lfs/map
     lon858/
     lon890/
     ...
```

进入 **LiveMap**，日志应有 vector map 加载成功；平移/缩放时底图应连续，**屏幕边缘跨网格时不应出现条带空白**（见 [RUNTIME.md § 跨区瓦片](./RUNTIME.md#跨区瓦片加载)）。

---

## 与字体的关系

路名/地名以 UTF-8 写入 `.vt`。新区域出现新汉字时，需跑字体子集化并重新 `build-fs`：

```bash
cd vendor/HelmOne/docs/font
.venv/bin/python subset_font.py --vmap-dir ../osm/vmap
.venv/bin/python subset_font.py --sync-myvendor
```

详见 [../font/README.md](../font/README.md)。

---

## 相关源码

| 模块 | 路径 |
|------|------|
| 打包 | `bicycle/tools/pack_map.py` |
| 格式常量 | `bicycle/src/vmap/vmap_format.h` |
| 区域目录 | `bicycle/src/vmap/vmap_map_catalog.c` |
| 瓦片加载 | `bicycle/src/vmap/vmap_tile_cache.c` |
| 渲染 | `bicycle/src/vmap/vmap_view.c`、`vmap_render.c` |
| 导航 | `bicycle/src/vmap/vmap_route*.c` |
