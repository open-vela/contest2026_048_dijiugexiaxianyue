# 本地图加载与渲染原理

设备端在 **LiveMap** 页面通过 `vmap_view` 显示矢量底图：按当前中心经纬度算出
3 km 网格 cell，从 LittleFS 读该 cell 的 `.vpk`、缓存在 PSRAM、软件光栅化到
RGB565 画布。

配置入口：`CONFIG_VMAP_DIR_PATH` → `/mnt/lfs/map`（`Config.h`）。

LiveMap 当前方法、图层顺序与不可破坏的实现约束统一见
[LIVEMAP.md](./LIVEMAP.md)；开机 splash 首帧见 [LIVEMAP.md §4.4](./LIVEMAP.md)
与 bicycle [`doc/splash.md`](../../boards/sf32lb52/helmone/ui/bicycle/doc/splash.md)。
本文保留加载流水线和调试细节。

当前打包布局与城市合包见 [../osm/CITIES_MAP.md](../osm/CITIES_MAP.md)。

---

## 1. 总体数据流

```
/mnt/lfs/map/lon*/lat*/x*_y*.vpk     # 无 map.idx；路径由经纬度算出
    ↓  GPS / 视图中心 (lon, lat)
vmap_grid_lonlat_to_cell()
    ↓  cell (ix, iy) → 文件存在则打开

跟车（|pan| < 边距）：
    vmap_view_set_center() → 只平移 RGB565 画布（底图/导航/标签/REC 已盖在一起）
    不读盘、不解码、不光栅

越出边距 / 首帧 / 换缩放：
    vmap_view_render()                # 只 begin：scroll 或清画布、算瓦片范围
        ↓  map_page render_timer 20 ms
    vmap_view_render_pump(max=2, 8ms) # 解算与画都在这里分片
        ↓  vmap_tile_cache_get()
    PSRAM arena 命中？ → 解析 VTIL
        ↓  未命中
    load_from_flash() → 对应 cell .vpk 读 blob   ← 一次约 50–150 ms
        ↓
    vmap_render_draw_tile()（可 clip 到脏条）+ 收集路名
        ↓  瓦片循环结束
    render_finish()：导航 → 标签 → REC 依次烤进画布 → 箭头置顶
```

路网导航在底图之上由 `vmap_route` / `vmap_track` 绘制折线（见
[../osm/ROUTING.md](../osm/ROUTING.md)）。跨区接点读每个 vpk 尾部的
**VPOR**，不再依赖顶层 `route.port`。

---

## 2. 启动：目录与 cell 寻址

### `vmap_tile_cache_set_dir("/mnt/lfs/map")`

1. 关闭已打开的 pack 句柄，清空区域槽位。
2. `vmap_map_catalog_load(dir)`：若根目录没有旧版 `map.idx`（VIDX），进入
   **网格模式**（当前推荐）。不读区域表进 RAM。

### 网格模式（无 `map.idx`）

`vmap_grid.c` 与 `pack_map.py` 使用同一套 3 km 全国网格（原点 `(0°,0°)`，
锚纬 35°N）：

```
cell_lat = 3 / 111.32
cell_lon = 3 / (111.32 × cos(35°))
ix = floor((lon - 0) / cell_lon)
iy = floor((lat - 0) / cell_lat)
path = lon<ix/4>/lat<iy/4>/x<ix>_y<iy>.vpk
```

`vmap_map_catalog_find_region(lon, lat, &id)`：算出 `(ix, iy)`，文件存在则
`id = (iy << 16) | ix`。活动 cell 用于导航图与 arena key，**不等于**
「只有活动 cell 的瓦片才能显示」（见 §5）。

### VIDX 旧格式

根目录有 `map.idx` 且 magic 为 `VIDX` 时仍走旧分片：`vmap_tile_index` +
`p*.vpk`。新包不要再写 `map.idx`。

---

## 3. 坐标与可视瓦片范围

### 经纬度 → 像素

`vmap_tile_system.c`（Web Mercator）：

- `vmap_latlong_to_pixel(lat, lon, zoom, &px, &py)`  
  得到 zoom 级世界像素坐标；每瓦片 256×256 世界像素。

### 中心与缩放

- `view->lon/lat`：逻辑中心（GPS 或手动平移）。
- `view->zoom`：瓦片层级（demo 固定 **14**）。
- `view->scale`：连续缩放 0.25×–8×，**不切换 z15 瓦片层**。

### 枚举要画的瓦片

`render_begin()` 里 `tile_range()`：

```
half_w = (canvas_w/2) / scale
half_h = (canvas_h/2) / scale
left   = center_x - half_w - ring*256
…
tx_min..tx_max, ty_min..ty_max
```

`ring=1` 多画一圈，减少平移时边缘露白。240×400、scale≈0.25 时大约 **4×4 =
16** 块 z14 瓦片。

### 瓦片落屏位置

对每个 `(tx, ty)`：

```
base_x = (tx*256 - center_x) * scale + screen_w/2
base_y = (ty*256 - center_y) * scale + screen_h/2
```

`vmap_render_draw_tile()` 把瓦片内 4096 坐标系几何按 `scale` 画到 canvas。

---

## 4. 瓦片加载流水线

### `vmap_tile_cache_get(cache, z, x, y, &tile)`

1. **zoom 变化**：`vmap_tile_cache_set_zoom`。
2. **PSRAM arena**（`vmap_cache_arena.c`）：
   - key：`(cell_id, z, x, y)`。
   - 命中 → 直接 `vmap_tile_parse_borrowed`。
   - 曾失败 → `MISSING` 标记，避免反复打盘。
3. **未命中** → `load_from_flash()` 读盘 → `vmap_cache_insert`。

### 从 cell `.vpk` 读 VTIL

- 路径由 `vmap_grid_vpk_path` 算出。
- 读 RIDX 瓦片表到 slot（每 cell 一次）。
- 按 `(z, y, x)` 二分查找 offset/size，`fseek` + `fread`。

### 活动 cell 切换

`render_begin()` 开头：

```c
vmap_tile_cache_ensure_region(cache, view->lon, view->lat);
```

内部 `find_region` + 打开该 cell 的 vpk：更新活动 cell、包内 VGRF 偏移
（供导航加载）。cell 变化时 `vmap_cache_flush_city` 清旧 arena 项。

---

## 5. 跨 cell 瓦片加载

### 问题背景

3 km cell 在 z14 大约 **2×2～3×3** 块瓦片。屏幕 240×400、scale 0.25× 时可视
范围约 **13 km**，常同时覆盖 **多个 cell**。

若只从**活动 cell** 的瓦片表查找，邻格瓦片会读失败 → 渲染 `continue` →
**条带空白**。

### 修复逻辑（`vmap_tile_cache.c`）

`load_from_flash()`：

1. 先在**活动 cell** slot 里查找 `(z,x,y)`。
2. 未命中 → 用 `tile_center_lonlat(z,x,y)` 算瓦片中心 WGS84。
3. `vmap_map_catalog_find_region` — 与 pack 时按瓦片中心分 cell 的规则一致。
4. `slot_load` 打开对应 `x*_y*.vpk`，再查找并读取。

### 多包驻留（LRU slots）

- 最多同时保持若干 cell 的瓦片表 + 文件句柄。
- 淘汰：优先空槽，否则淘汰 **LRU 且非活动 cell**。
- MTP 期间 `myvendor_mtp_lfs_quiesce()`：关闭文件句柄，保留瓦片表；恢复后按需重开。

因此：**显示**可跨多 cell；**导航图**仍按活动 cell 加载包内 VGRF。

---

## 6. 增量渲染（本次算法）

整帧 16 块瓦片同步光栅化约 **3 s**，会卡住 `lv_timer_handler`：按键死、看门狗
紧。所以 **`vmap_view_render()` 不再画完一帧**。

### 6.1 状态机

`view->pump_phase`：

| 值 | 含义 |
|----|------|
| `0` | 空闲。画布上已有上一帧（`render_rev` 可能 > 0） |
| `1` | 正在画：`pump_tx/ty` 从右下往左上扫 |

```
vmap_view_render()          render_begin()
        │                   能 scroll 则平移像素并记脏条，否则清屏
        │                   reset 标签收集、tile_range、pump_phase = 1
        ▼
render_timer 20 ms ──► vmap_view_render_pump(max=2, budget=8ms)
        │                   仅当 pump_phase != 0 才工作
        │                   未缓存：本拍只 warm（读盘+入 arena），下一拍再画
        │                   已缓存且落在脏区：光栅到 canvas
        │                   不在脏区：只收集路名，不重画几何
        ▼
瓦片扫完 ──► render_finish()
            导航线 / 路名 / REC 按固定顺序烤进 canvas
            render_rev++
            pump_phase = 0
            invalidate + 箭头置顶
```

### 6.2 API 约定

| 函数 | 行为 |
|------|------|
| `vmap_view_render()` | 只 `render_begin()`。MTP quiesce 则直接返回。 |
| `vmap_view_render_pump(n)` | 最多画 `n` 块。**`pump_phase==0` 时返回 true，不开新帧。** |
| `vmap_view_render_busy()` | `pump_phase != 0` |
| `vmap_view_render_revision()` | 已完成帧计数；`0` 表示从未画完过 |

`pump` 禁止自动 `render_begin`：调用方必须显式开帧，避免一帧未完成时又清屏。

### 6.3 LiveMap 调度

`map_page.c`：

- `render_timer` 周期 **20 ms**，回调 `on_render_pump`：cover 期间直接 return；
  不 busy 则 return；`pump(VMAP_RENDER_PUMP_MAX_TILES=2)`，单次再受
  `VMAP_RENDER_PUMP_BUDGET_MS=8` 限制。完成后 `map_page_sync_arrow` + 抬 chrome。
  一次 `warm()` 若读盘超过预算，这一拍仍会做完（无法中途切开 fread），下一拍再画。
  > ⚠ **别把它调小**（试过 10 ms，上板实测无效，已回退）：每拍实际只画**一个**瓦片
  > （预算 8 ms 拦不住一个实测 15~38 ms 的瓦片），所以一帧的节奏是**工作受限**而不是
  > 周期受限；再叠加系统节拍本来就是 10 ms（`CONFIG_USEC_PER_TICK=10000`、非
  > tickless）、UI 循环 `BICYCLE_LOOP_SLEEP_MS=10u` 且 `lv_timer_handler()` 每循环只
  > 调一次 —— 砍周期既拿不到等待，还多付每拍的重复开销。数据见
  > [perf/MEASUREMENTS.md](./perf/MEASUREMENTS.md) §10 与 [perf/PUMP_PERIOD.md](./perf/PUMP_PERIOD.md)。
- cover / boot 暂停该 timer；`map_will_appear` 不恢复，页面淡入完成后的
  `map_did_appear` 才 resume。
- `map_refresh_pending`：只在 **`rev==0`**（从未画完）时置位，由 gpx_timer
  调 `vmap_view_render()` 开帧。隐藏地图 UI **不再**每个 GNSS 拍都 pending。
- 切到其他 Helm 页面后，大画布与 4 个 region slot / 解析瓦片 LRU 均保留；
  gpx_timer 每 **1 s** 低频更新中心。位移在 margin 内只移动 canvas，越过
  margin 才由 render_timer 补边。重新进入 LiveMap 本身不是加载条件。
- `map_page_boot_prepare()`：Startup 内只设置启动中心、加载 catalog/region
  文件；不调用 `render()` 或 `pump()`，完成后即可退出启动页。
- `vmap_view_create`：画布 `memset` 0，canvas HIDDEN，直到第一帧
  `render_finish` 才显示。
- `boot_deferred` 时主题回调**不要**开第一帧（`set_view` 会作废那一帧）。
- LiveMap 淡入完成后才 `render_begin` 并恢复 20 ms pump，避免同步 VPK I/O
  与转场动画竞争。
- 开机后还没进过地图页（`map_ui_seen==false`）：隐藏页禁止 `set_center`
  全图 rebase，否则用户马上翻进来会看到半帧黑噪点。

详见 [LIVEMAP.md §4.4](./LIVEMAP.md) 与 bicycle [`doc/splash.md`](../../boards/sf32lb52/helmone/ui/bicycle/doc/splash.md)。

瓦片扫描顺序：从 `(tx_max, ty_max)` 递减 `ty`，一列结束再 `tx--`。

### 6.4 主题回调不要整帧重画

关菜单 / 回主页会 `lv_pm_theme_apply` → `map_page_on_pm_theme`。旧逻辑无条件
`vmap_view_render()`，fade 动画结束瞬间再堵 3 s，日志表现为：

```
theme apply … → vmap_render request tiles tx=…  →  约 3 s 无 lv_timer_handler
→ btn timer gap 3391ms
```

现规则（cover 除外）：

```
rev > 0  且  !busy  且  (theme_id 未变或尚未记录)
    → 只换 chrome / 箭头图，skip rasterize
否则
    → vmap_view_render() + resume render_timer
```

cover 期间只设 `map_refresh_pending`，不在回调里开帧。

### 6.5 定位箭头

箭头是 `map_area` 上的 `lv_img`，新建默认坐标 **(0,0) = 左上角**。跟随模式
必须 `lv_obj_align(CENTER)`。

`map_page_create` 时 `lv_pm_theme_apply` 往往发生在 `user_data` / `s_live_map`
尚未挂上，主题回调空返回。随后真正 `set_arrow_image` 若再碰上 **skip
rasterize**，旧代码不会 `update_arrow`，箭头就钉在左上。无 GNSS 时 gpx_timer
也不会摆它。

算法：

1. `vmap_view_set_arrow_image`：若 `follow_mode`，创建后立刻 `ALIGN_CENTER`。
2. `map_page_sync_arrow`：有有效 GNSS 用 fix，否则用视图中心；`update_arrow` +
   bring front。
3. 主题回调 **无论是否 skip 栅格化** 都 `sync_arrow`。
4. `map_page_create` 在 `bind_arrow_layer` 后立刻 `set_arrow_image`。
5. `render_finish` / `on_render_pump` 完成时再 `sync_arrow`。

跟随模式 `update_arrow` 只改角度、始终居中；非跟随才按经纬度换算屏幕坐标。

### 6.6 Cover / 回主页

`pause_for_cover`：`covered=true`，停 render_timer，`release_storage` **只关
LFS 句柄**，画布与 tile arena 仍在。

`resume_after_cover`：

- `rev==0`：才 `map_refresh_pending`（开机未画完）。
- `rev>0`：只 `invalidate` + 箭头置顶，**不开新帧**。
- 若仍 `busy`：resume `render_timer` 把未画完的帧泵完。

### 6.7 看门狗

IWDT 约 15 s。UI 心跳只在 `bicycle_c_main` 循环里、`lv_timer_handler()`
**之前** `myvendor_watchdog_ui_beat()`。

`myvendor_watchdog_busy_pump()`：每满 1 s 才 `usleep(10 ms)`，**不伪造** ui/work
beat。加载时假心跳会掩盖真卡死。splash 分块加载字体/地图文件，以及 LiveMap
出现后的首帧 `pump` 会调用它；只为让出 CPU，不是喂狗。

### 6.8 跟车平移与 rebase（当前解算时机）

画布比屏幕大一圈边距（`CONFIG_MYVENDOR_VMAP_CANVAS_MARGIN`，nsh 为 **128 px**）。
可视窗口是画布中心那块 240×(320−顶栏−底栏)。跟车把含底图、导航、标签和
REC 的整张画布按 GNSS 平移，露出原先画在边距里的像素。

```
limit = margin − VMAP_PAN_GUARD     # 128 − 12 = 116 px
```

| 阶段 | 谁调用 | 做什么 | 解算瓦片？ |
|------|--------|--------|------------|
| 跟车 | `gpx_timer` 200 ms → `vmap_view_set_center` | `\|dx\|,|dy\| ≤ limit`：`lv_obj_set_pos` 平移整幅 RGB565 画布 | **否** |
| 夹紧 | 正在 pump 或 MTP 占 LFS | 平移限制在 `±limit`，不新开帧 | **否** |
| rebase | `\|dx\|` 或 `\|dy\|` 超出 limit | `vmap_view_render()`：把画布像素滚回去、只补脏条 | **是，走 pump** |
| 首帧 / 换缩放 / 强制清屏 | `render_rev==0`、zoom 变、`force_clear` | 整张 canvas 填底色再泵全部瓦片 | **是** |

`render_next_tile()` 把「解算」和「画」拆开：

1. **warm**：`vmap_tile_cache_cached` 为假 → `vmap_tile_cache_warm()` =
   LittleFS `fread` + 写入 PSRAM arena。本拍 `return`，不光栅。
2. **draw**：已在 arena 且与脏条相交 → `vmap_render_draw_tile()`（canvas clip
   只写脏像素）+ 收集路名。
3. **label-only**：已缓存但不在脏条（scroll 后仍留在画布上的旧像素）→ 只
   `vmap_label_collector_add_tile`，不重画道路。

因此：**解码只发生在 pump 里，且每拍最多一张未命中的瓦片。** 跟车路径禁止
`vmap_tile_cache_warm()`。曾经在 `set_center` 里按平移比例预热下一屏，GNSS
每 200 ms 同步读盘约 150 ms，把 `bicycle_ui` 打到约 80%、空闲约 10%；已删除。

rebase 时不能直接相信 `last_pan_px`：地图隐藏期间逻辑位置可能移动几百米，
但物理画布仍停在离开前的十几像素 pan。当前实现由
`new_render_origin - old_render_origin` 重新计算精确滚动量，再按滚出的边生成
最多两条脏矩形（外扩 `VMAP_RENDER_DIRTY_PAD=24`）。滚动量超过画布、缩放改变
或显式 `set_view` 时必须全清，禁止把旧内部像素标记成新坐标系。

从其他 Helm 页面返回时直接显示 retained canvas。隐藏期间后台跟车已经把
位移拆成小步 rebase，因此不会出现一次远跳导致整画布清空；下一 GNSS 拍只补
导航/REC 进度。只有首次进入、缩放/主题/路线几何改变或 MTP 释放存储时允许
完整重建。

### 6.9 画布叠层（底 → 顶）

全部盖在同一张 RGB565 画布上，跟车只平移画布，各层不会错位：

1. 地图瓦片
2. 导航线
3. 道路/地名标签（`render_finish` 写入画布；retained 占位防止旧字重描，
   新候选可在大画布非脏区补入）
4. REC 轨迹线 + 公里圆标
5. REC 公里数字

LVGL 定位箭头在画布之上。进度刷新只重贴导航线再盖 REC，不重描路名，避免把字描胖。

标签有一块固定 PSRAM 占位表（`VMAP_LABEL_RETAIN_CAP=256`）：

- 同一路名跨 tile 以字符串 + 一个视口跨度去重，不使用 tile 内 feature 索引。
- 候选冲突时保留离视口焦点最近的锚点，避免接缝候选抢掉屏幕内路名。
- canvas scroll 时占位框同步平移。完全离开画布的占位删除；半截出界或与
  dirty 相交的占位必须先把整框并入 dirty 擦净，再删记录。禁止留下无占位残字。
- 每个小瓦片阶段只收集文字锚点；所有小瓦片候选齐备后，`render_finish`
  才在 retained 大画布上统一量字宽、碰撞和烘焙。文字框必须完整落在大画布
  安全边界内，可以暂时位于屏幕外，随后随 canvas 平移进入视口。烤上的字
  一定写入占位表，表满则停烤。
- 同名路名间隔约半个视口；先给每个不同路名尝试一个最近焦点的锚点，再放
  长道路的重复标签，避免“路已进入屏幕但所有名字都被另一条长路占满”。
- 全画布清屏 / keep-pan 重画时清空占位表，由完整候选集重新布局。

字模：系统 TTF 在 PSRAM，FreeType 位图缓存 **256 槽 / 同一份 TTF**。

---

## 7. MTP 与 LiveMap 暂停

MTP 写 LittleFS 时 `myvendor_mtp_lfs_quiesce()` 为 true：

- `load_from_flash` 失败；`vmap_view_render` / `pump` 早退。
- LiveMap **pause** 停渲染定时器、GNSS、不读 LFS；**不销毁**画布与 tile cache
  元数据。

普通 cover resume 保留并 invalidate 原画布；未完成的 pump 继续。MTP 改写地图
文件后才走 storage reload。

---

## 8. 离线路线导航（简要）

| 组件 | 作用 |
|------|------|
| `vmap_tile_cache_region_graph` | 当前活动 cell `.vpk` 内 VGRF 偏移/大小 |
| `vmap_route_graph` | 解析 VGRF v2/v3 + SNAP + 可选 ELEV |
| `vmap_route_portal` | 读 cell 包尾 **VPOR**（忽略顶层 `route.port`） |
| `vmap_map_catalog_region_path` | 网格四邻 BFS 找跨区路径 |
| `vmap_route` / `vmap_route_worker` | Dijkstra、偏航重规划、折线绘制 |

跨城/跨区：本区 Dijkstra + portal 拼接 + 邻区图；依赖制作时 `--graph` 与
`--rebuild-portals`。

### 8.1 近边界单区规划（避免频繁重规划）

区域路由图打包时带 `GRAPH_CLIP_MARGIN_M`（默认 600m）重叠：每个 cell 的 VGRF
会含出格 600m 的相邻道路。因此**终点只是刚跨过区界**时，它其实已在起点区域
的图里且连通良好。

- `vmap_route_corridor_needed()` 判定：起终点虽落在不同 grid cell，但终点在
  起点区域 bbox 外扩 `VMAP_ROUTE_REGION_REACH_M`（300m）以内 → **走单区
  Dijkstra**，不进 corridor。
- 好处：避开 corridor 的边界残桩（clip 出的孤立短链导致 `segment fail` /
  绕远），规划更快更稳，起点吸附回本区路网。
- 约束：`VMAP_ROUTE_REGION_REACH_M`（300m）必须显著小于打包 `GRAPH_CLIP_MARGIN_M`
  （600m），保证吸附到的路由带内的路都有连通几何支撑。

### 8.2 重规划节流（防抖）

`map_page` 对偏航重规划加冷却：距上次重规划 <4s **且**位移 <80m 时**吞掉**本次
触发，保留当前路线。用于骑行者卡在路网稀疏点（每次规划都吸附到 ~100m 外的
节点，立即又判偏航）时避免规划风暴。位移 ≥80m 视为真实偏航，立即放行。

---

## 9. 内存与性能要点

| 资源 | 规模 | 说明 |
|------|------|------|
| 网格目录 | 不算 `map.idx` | `(lon,lat)` 直接换路径 |
| VPOR | 跟 cell 走 | 打开该 vpk 时按需读尾部 |
| cell 瓦片表 | 每 slot 一份 | 与包内 tile 数成正比 |
| PSRAM arena | `CONFIG_MYVENDOR_VMAP_TILE_CACHE_KB`（nsh **1536**） | 已解析瓦片 blob LRU |
| 画布 | RGB565，边距 **128**，约 496×450 @ 当前地图区 | cover 期间保留；跟车吃边距 |
| pump | 最多 **2** 块 / 拍，预算 **8 ms** | 读盘一次仍可能 50–150 ms |
| 跟车 | GNSS **200 ms**，只 `set_pos` | 不解瓦片 |
| 路名 | 大画布烘焙 + PSRAM 占位 + FreeType 256 字形 | 位于 nav 上、REC 下 |

冷启动或跨区首次进入会多读 LittleFS；跟车阶段 arena 不应被动。rebase 只解
脏条上缺的瓦片。

### 9.1 已经做了的降负载

| 手段 | 作用 |
|------|------|
| 跟车禁止 `warm()` | 避免每 200 ms 同步读盘把 UI 打满（曾 idle≈10%） |
| 大画布 + 软平移 | 边距内纯 memcpy 视野，不重栅格 |
| scroll rebase | 滚像素 + 脏条，代替整屏 `vmap_render_clear` |
| canvas clip | 只把新几何画进脏条，旧路网像素保留 |
| 分片 pump | 把解算摊到 20 ms 定时器，按键/看门狗能插空 |
| 未命中只 warm、命中再画 | 一拍最多一张冷瓦片的 LFS |
| 非脏瓦片只收路名 | rebase 不重画仍在画布上的道路 |
| PSRAM tile LRU | 同视野二次 rebase 走内存 |
| 路名 PSRAM 占位 | 补边保留区参与碰撞，接缝不重复烤同名 |
| 道路抽稀 | `VMAP_ROAD_MIN_SEG_PX` 等，少画折线 |
| 轨迹增量描尾 | 跟车不重画整条 polyline |
| 主题/cover skip | 关菜单、离开地图页不整帧重栅格 |
| 字形缓存 | 路名平移不重新 FreeType 光栅 |

### 9.2 还可以继续降的（未做）

这些是有意识留下的余地，不是当前 bug。

| 方向 | 预期 | 代价 |
|------|------|------|
| 提高 `VMAP_PAN_MIN_STEP_PX`（现 1） | 跟车少整屏 invalidate | 箭头下地图略顿 |
| rebase 时平移 label 而不隐藏 | 补图不再闪路名 | 要对齐 scroll 像素 |
| 把 `fread` 切成块 / 丢到低优先级线程 | 单瓦片 150 ms 不再卡住 handler | LFS 与 MTP 同盘，要互斥 |
| 加大 `VMAP_TILE_CACHE_KB` | rebase 更少打盘 | 占 PSRAM |
| 关掉路名 1 px outline | 刷新更便宜 | 浅色路上字对比下降 |
| 更早、更小条 rebase | 每次补边更短 | 路名层闪得更勤（若仍隐藏） |
| 可见视口外的 `lv_label` 不参与 refr | 跟车刷新更便宜 | 要确认 clip 是否已够 |

单块 `warm()` 无法被 8 ms 预算切开：读盘是一次 `fread`。所以 rebase 日志里
仍会出现单次 `vmap_pump 50–150ms`，这是正常的；跟车阶段不应再出现这种泵。

---

## 10. 调试与常见问题

`[stall]` 日志（syslog NOTICE）：

| 前缀 | 含义 |
|------|------|
| `lv_timer_handler Nms` | UI 循环一次超过 50 ms |
| `vmap_render request` | 开了一帧，后面应跟多次 `vmap_pump` |
| `vmap_pump Nms done=` | 单次泵耗时；`done=1` 本帧结束。跟车时不应刷屏出现 |
| `[map] follow pan=` | 软平移（≥2 s 节流）；不含解码 |
| `[map] rebase scroll=` | 画布滚像素 + 脏条，随后才 pump |
| `gpx_timer Nms vis=1` | 地图页 GNSS 拍；跟车应明显低于 40 ms |
| `gpx dem/nav/follow/track` | `gpx_timer` 分段；用来判断 100 ms 是 DEM 全图、导航匹配、跟车平移还是 REC 描线 |
| `helm_ui_cb Nms id=4` | Helm 500 ms 拍；id=4 是地图页。隐藏数据页走势不应再贡献这里 |
| `theme apply skip render` | 关菜单走了跳过路径 |
| `theme apply request render` | 主题变了或 rev=0，会开帧 |
| `map_resume` / `map_pause` | cover 进出 |
| `btn timer gap` | 按键轮询间隔过大（曾被同步渲染堵住） |

| 现象 | 可能原因 | 处理 |
|------|----------|------|
| 关菜单后按键死约 3 s | 旧固件主题回调同步整帧渲染 | 升级含 pump + skip 的固件 |
| 箭头钉在左上 | skip 渲染后未 `sync_arrow` | 已修；确认 `set_arrow_image` 在 create 里 |
| 整屏空白 | `/mnt/lfs/map` 空、路径错、该点无 vpk | `ls /mnt/lfs/map/lon*`；MTP 城市包 |
| 有图但路名方框 | 字体子集缺字 | `subset_font.py` + `build-fs` |
| 中心不对 | 默认中心仍是广州 | 改 `VMAP_DEFAULT_LON/LAT` 或等 GNSS |
| 下方/边缘条带空白 | 旧固件只读活动 cell | 升级含跨区 slot 加载的固件 |
| 偏航后反复重规划 | 近边界走 corridor / 卡稀疏点 | 单区规划 + 重规划节流；600m margin 打包 |
| 缩放后外围空白 | bbox 外本无数据 | 扩大制作区域 |
| MTP 时地图不更新 | quiesce 设计如此 | 拔线后 resume |
| 导航失败 | 无 VGRF / 无 VPOR | `make_cities_map` 须 `--graph` 且 rebuild portals |
| 开机 IWDT | 首帧同步画完 / 假心跳掩盖卡死 | 用 pump；busy_pump 不要喂狗 |
| 跟车 idle 掉到约 10%，`bicycle_ui` 约 80% | 旧跟随预热在 GNSS 拍上 `warm()` | 已改为只 rebase 时 pump；确认无连续 `vmap_pump` |
| 路名挡 REC / 接缝残字 | 图层顺序错误或未维护 retained 占位 | 按 LIVEMAP.md：nav → 标签 → REC，并同步 scroll 占位 |

### 确认某点是否在已打包区域内

```
ix = floor(lon / cell_lon)
iy = floor(lat / cell_lat)
文件：lon<ix/4>/lat<iy/4>/x<ix>_y<iy>.vpk
```

广州约 `lon858–866`，滁州/南京约 `lon890–906`。瓦片级核对 z14 `tileX/tileY`
是否在该 vpk 的 RIDX 表内。

---

## 11. 模块对照表

| 文件 | 职责 |
|------|------|
| `vmap_grid.c` | lon/lat → cell → `lon*/lat*/x*_y*.vpk` |
| `vmap_map_catalog.c` | 网格模式查 cell、合成 bbox、邻区路径 |
| `vmap_tile_cache.c` | 目录设置、cell slot、跨区读瓦片、graph 位置 |
| `vmap_cache_arena.c` | PSRAM 瓦片 blob LRU |
| `vmap_tile.c` | VTIL 解析 |
| `vmap_render.c` | 线/面光栅化 |
| `vmap_label.c` | 路名收集与绘制 |
| `vmap_view.c` | 视图、`render_begin` / `pump` / 箭头 |
| `vmap_tile_system.c` | Web Mercator 像素换算 |
| `vmap_tile_index.c` | VIDX 旧格式索引 |
| `vmap_route_graph.c` | VGRF 加载 |
| `vmap_route_portal.c` | 每 cell VPOR |
| `map_page.c` | `render_timer`、主题 skip、cover、`sync_arrow` |
| `myvendor_watchdog.c` | UI beat；`busy_pump` 只 yield |

LiveMap 页面封装见 `lvgl_page/live_map/`（pause/resume、MTP 盖住地图不销毁实例）。
