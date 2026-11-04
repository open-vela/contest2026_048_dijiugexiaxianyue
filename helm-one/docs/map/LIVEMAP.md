# LiveMap 当前实现、方法与约束

本文是 Helm One 设备端 **LiveMap 的实现约定**。记录当前可工作的运行模型、
缓存生命周期、标签算法、REC/导航叠层顺序以及后续修改时不得破坏的约束。

若本文与历史设计文档不一致，以本文和当前 `ui/bicycle/src/vmap/` 源码为准。
地图文件制作、二进制格式和路网算法分别见：

- [PRODUCTION.md](./PRODUCTION.md)
- [RUNTIME.md](./RUNTIME.md)
- [perf/README.md](./perf/README.md) — 渲染性能专题（**动 `vmap_render.c` 前先读它**：
  按行裁剪并集、成对 32 位写、圆盘去 sqrt；含"像素逐位不变"的差分测试与实测数据）
- [../osm/DESIGN.md](../osm/DESIGN.md)
- [../osm/ROUTING.md](../osm/ROUTING.md)
- 开机 splash / 首帧：[bicycle/doc/splash.md](../../boards/sf32lb52/helmone/ui/bicycle/doc/splash.md)

目录：§4.1 Helm 翻页 · §4.2 cover · §4.3 MTP · **§4.4 开机首帧** · §5 缓存 · §6 rebase

---

## 1. 产品环境

- SoC：SF32LB52，Cortex-M33。
- 系统：openvela / NuttX。
- 屏幕：240×320，RGB565，无触摸。
- Helm 主内容高度：296；LiveMap 地图区高度：
  `HELM_MAP_BODY_H = 296 - 22 - 80 = 194`。
- 地图画布边距：nsh 配置 `CONFIG_MYVENDOR_VMAP_CANVAS_MARGIN=128`。
- retained RGB565 大画布：约 `496×450`，约 436 KiB。
- 地图目录：`CONFIG_VMAP_DIR_PATH`，当前设备为 `/mnt/lfs/map`。
- 推荐地图包：3 km 固定网格 `lon*/lat*/x*_y*.vpk`，无顶层 `map.idx`。

关键配置：

```text
CONFIG_MYVENDOR_VMAP_TILE_CACHE_KB=1536
CONFIG_MYVENDOR_VMAP_VPK_HEAP_KB=1024
CONFIG_MYVENDOR_VMAP_TRACK_ARENA_KB=512
CONFIG_MYVENDOR_VMAP_CANVAS_MARGIN=128
VMAP_RENDER_PUMP_MAX_TILES=2
VMAP_RENDER_PUMP_BUDGET_MS=8
VMAP_GNSS_UPDATE_MS=200
```

---

## 2. 总体方法

LiveMap 不是“进入页面时重新生成一张地图”，而是一个进程生命周期内持续存在的
**retained 大画布**：

1. 把若干 256×256 矢量小瓦片解算并拼进一张带 overscan 的 RGB565 大画布。
2. GNSS 小位移只移动 LVGL canvas 对象，不读取地图文件、不解算瓦片。
3. 位移越过画布边距后，把已有像素按地理位移 scroll，保留重叠区，只把新露出的
   一至两条边作为 dirty rect。
4. 20 ms render timer 在后台轮转加载/绘制 dirty rect 涉及的小瓦片。
5. 所有小瓦片处理完后，再一次性完成导航、标签、REC 的画布叠层。
6. 切到 Helm 数据页、转向页、爬升页时不销毁地图；后台低频维护地图中心。
7. 返回 LiveMap 直接显示 retained canvas，**页面进入不是瓦片加载条件**。

稳态调用链：

```text
GNSS 200 ms
  └─ on_gpx_timer()
      └─ vmap_view_set_center()
          ├─ 位移 <= 116 px：lv_obj_set_pos()，结束
          └─ 位移 > 116 px：vmap_view_render()
                                └─ render_begin()

render timer 20 ms（仅 busy 时运行）
  └─ vmap_view_render_pump(max=2)
      ├─ ensure region
      ├─ 未缓存瓦片：本拍只 warm
      ├─ 已缓存瓦片：本拍光栅化
      └─ render_finish()：统一叠层
```

---

## 3. 三组坐标状态与硬不变量

`vmap_view` 同时维护三类状态：

- `lon/lat`：最新逻辑跟车中心。
- `render_lon/render_lat`：当前 RGB565 像素真正采用的 Web Mercator 原点。
- `pan_dx/pan_dy`、`last_pan_px/py`：逻辑中心相对 render 原点的画布平移量。

必须始终满足：

```text
逻辑中心世界像素 - render 原点世界像素
    == canvas pan / scale
```

软平移只改变 `lon/lat` 和 pan，不改变 `render_lon/render_lat`。

rebase 必须：

1. 用 `new render origin - old render origin` 重新计算真实 scroll 像素；
2. 不能直接使用离开地图前残留的 `last_pan_px` 代替地理位移；
3. scroll 旧 RGB565 像素；
4. 设置 `render_lon/render_lat` 为新原点；
5. 清 pan；
6. 只补 scroll 露出的 dirty edge。

显式 `set_view`、zoom/scale 改变、首帧或位移超过整张画布时不得复用旧投影像素，
必须完整重建。

此约束同时决定底图、导航、标签和 REC 的投影。破坏它会表现为道路/面错位、
三角形残片、REC 偏移或 nav 线与道路分离。

---

## 4. retained canvas 与页面生命周期

### 4.1 Helm 内部翻页

`map_page_set_map_ui_visible(false)` 只给 `map_area` 加 `HIDDEN`：

- 不销毁 `map_page_t`；
- 不销毁 `vmap_view_t`；
- 不释放 RGB565 大画布；
- 不清瓦片 LRU；
- 不关闭 region slots；
- REC 点仍进入 RAM；
- 导航状态仍更新。

隐藏期间 gpx timer 每 1 s 调一次 `vmap_view_set_center()`：

- **开机后还没进过地图页**（`map_ui_seen==false`）：不要跟车 rebase，保留
  清零且隐藏的 canvas，等待实际进入后开首帧；
- 未越过 `margin - VMAP_PAN_GUARD` 时只更新 canvas 位置；
- 越界才启动后台 rebase/pump；
- 因为位置被逐步维护，返回时不会形成一次超大跳转。

重新显示地图时：

- 立即 invalidate retained canvas；
- 箭头重新置顶；
- 下一 GNSS 拍补最新 nav/REC；
- `render_rev > 0` 时不因“进入页面”调用全图 render。

### 4.2 普通 cover/menu

cover 会暂停 render timer，但保留画布和缓存。resume 时：

- `render_rev == 0` 才请求首帧；
- 已有画布只 invalidate；
- 半帧 pump 尚未完成则继续 pump；
- 禁止直接把隐藏期间 REC 尾巴画进错误坐标系。

### 4.3 MTP

MTP 改写 LittleFS 地图期间：

- `myvendor_mtp_lfs_quiesce()` 禁止地图读盘；
- 关闭 `.vpk` 文件句柄，但保留画布、瓦片表和可保留的 PSRAM 数据；
- MTP 真正修改地图文件后才允许 storage reload；
- 普通页面切换不得走 storage reload。

### 4.4 开机首帧

Startup splash 盖在 LiveMap 空壳上。vmap 在白屏「地图加载」事项里创建，
splash 仍盖着时只走 `map_page_boot_prepare()`：加载 catalog 和启动坐标对应
region pack，不解码或绘制瓦片。

约束：

- 画布分配后清零；canvas 隐藏到第一帧 `render_finish`。
- Startup 文件准备完成即可 pop，不等待 `render_rev`。
- LiveMap `will_appear` 只做可见性和按键绑定；淡入完成后的 `dis_appear`
  生命周期才启动第一帧 render/pump。
- 主题 apply 发生在 `boot_deferred` 时不要 `vmap_view_render()`。
- 默认 Helm 是待机页，不是地图。在用户第一次翻到地图之前，隐藏页 GNSS
  **不得** `set_center` 清画布。
- 屏幕是 LCDC QSPI，不是 SPI LCD。

事项文案与关机对称动画见
[bicycle/doc/splash.md](../../boards/sf32lb52/helmone/ui/bicycle/doc/splash.md)。

---

## 5. 瓦片与 PSRAM 缓存

当前不是多张全屏 framebuffer 轮换，而是“一张 retained 大画布 + 多级小瓦片缓存”。
不要增加 GPU page flip 或第二张完整地图快照。

### 5.1 已解析瓦片 LRU

`vmap_cache_arena.c`：

- 固定 PSRAM arena，nsh 为 1536 KiB；
- key 为 `(cell_id, z, x, y)`；
- 缓存 VTIL blob；
- 命中后零拷贝解析；
- LRU 淘汰；
- 缺失瓦片记录 `MISSING`，避免反复读盘。

### 5.2 region pack slots

`vmap_tile_cache.c`：

- `VMAP_REG_SLOTS=4`；
- 每槽保存一个 3 km cell 的 RIDX、文件句柄或 VPK RAM pin；
- 活动 cell 不优先淘汰；
- 其他槽按 LRU 轮换；
- 屏幕跨 cell 时按瓦片中心反查所属 cell。

### 5.3 whole-VPK heap

`vmap_vpk_heap.c`：

- 固定 1024 KiB PSRAM；
- 能容纳时把整个 cell `.vpk` pin 在 RAM；
- 不够时退回文件 `fseek/fread`；
- 与解析瓦片 LRU 是不同层次。

### 5.4 加载约束

- GNSS/set-center 快路径禁止同步 `warm()`。
- 未缓存瓦片只能由 render pump 加载。
- 单次 pump 最多处理 2 个步骤，预算 8 ms。
- 一次 `fread` 不能被预算中断，冷瓦片仍可能单拍 50–150 ms。
- 未命中瓦片的一拍只 warm，下一拍才 draw。
- 不在 dirty rect 的已缓存瓦片只收集标签候选，不重画几何。

---

## 6. rebase 与 dirty edge

nsh 当前：

```text
margin = 128
VMAP_PAN_GUARD = 12
soft-pan limit = 116 px
VMAP_RENDER_DIRTY_PAD = 24 px
```

位移未超过 116 px：

- 只执行 `lv_obj_set_pos(canvas, -margin-pan_x, -margin-pan_y)`；
- 不读盘；
- 不重画；
- 地图、nav、标签、REC 因位于同一 RGB565 canvas 而一起移动。

位移超过 116 px：

- `vmap_render_scroll()` 平移已有像素；
- 最多生成水平、垂直两条 dirty rect；
- dirty rect 外扩 24 px，避免道路/面/文字接缝；
- dirty 外的道路像素保留；
- dirty 外的小瓦片仅参与标签候选收集；
- render finish 后清除 dirty 状态。

若 scale/zoom 不等于上一完成帧，禁止 scroll-reuse。

---

## 7. 标签方法

### 7.1 目标

- 标签不能在 256×256 小瓦片边界被截断；
- 相邻瓦片中的同一路名不能被当成完全无关的文字重复密集显示；
- 长道路进入屏幕时应持续有合理间隔的路名；
- 标签可以先烘焙在当前屏幕外的大画布区域，随后随 canvas 平移进入屏幕；
- 标签必须位于 nav 之上、REC 之下。

### 7.2 两阶段处理

小瓦片阶段只做候选收集：

1. 遍历道路几何；
2. 按道路方向和视口尺寸计算弧长间隔；
3. 产生文字锚点、方向、名称和优先级；
4. 地名、水域名、POI 收集其点锚；
5. 不在小瓦片自己的 256×256 边界内直接画字。

所有需要的小瓦片处理完成后，`render_finish()` 才在大画布上统一：

1. 量出完整 UTF-8 文本框；
2. 判断文本框是否完整落入 retained canvas 安全边界；
3. 做同名间距与不同标签碰撞；
4. 完整绘制文字；
5. 再由后续 REC 层覆盖。

因此，tile seam 只影响几何来源，不是文字裁剪边界。

增量 rebase 的 dirty edge 只限制底图几何重画，**不限制新标签只能出现在
dirty edge**。非脏瓦片收集到的新道路候选，只要完整落入大画布且不与 PSRAM
retained 占位冲突，也可以在大画布内部补烤。否则道路已经平移进屏幕后，第二个
同名标签可能永远没有绘制机会。

本轮新增标签会合并成一个包围框；REC 补描 clip 同时覆盖原 tile dirty 和该
包围框，确保新增文字仍位于 REC 下方。

### 7.3 同名间隔与道路覆盖

道路候选间隔：

```text
horizontal spacing = max(view_w × 0.42, 80)
vertical spacing   = max(view_h × 0.42, 64)
same-name gap      ≈ (horizontal + vertical) / 2
```

当前 240×194 视口下，横向候选约 101 px、竖向候选约 81 px，同名去重间隔
约 91 px。横排文字框更宽，因此横向候选有意采得更密。

布局顺序：

1. 候选按离当前焦点从近到远排序；
2. 先为每个不同道路名称尝试一个候选；
3. 再为长道路放重复名称；
4. 同名、过近候选合并；
5. tile 顺序不能决定最终保留哪个锚点；
6. 同名冲突时保留离焦点更近的锚点。

这避免一条长主路先吃完标签预算，导致屏幕里的其他道路完全没有名称。

### 7.4 固定 PSRAM 标签占位

`VMAP_LABEL_RETAIN_CAP=256`（约 23 KiB 固定 PSRAM）：

每项记录：

- 完整文本框；
- canvas 锚点；
- kind；
- UTF-8 名称。

scroll-rebase 时：

1. 占位框按与 RGB565 像素相同的方向和距离移动；
2. 已完全移出大画布的占位删除（像素已随 scroll 离开）；
3. 文本框不再完整落在大画布内，或与 dirty 相交：把该框仍在画布上的像素
   并入 dirty，**整框擦除后再删占位**。禁止只丢掉占位却留下半截墨水；
4. 只有完整留在非脏 interior 的旧文字继续占位；
5. 新烤的标签必须写入占位表；表满则不再烘焙（禁止无记录墨水）。

全画布清屏或 keep-pan 全重画时清空占位表，由完整候选集重新布局。

### 7.5 字体与绘制

- UTF-8 名称来自 VTIL；
- 使用系统 TTF / FreeType；
- 字号不随地图 scale 缩放；
- 道路近竖直时按 UTF-8 字符竖排，不旋转整张 canvas；
- 文字无背景色块；
- 1 px 白色半透明 outline 保证道路上的可读性；
- 新地图区域出现新汉字时必须重新生成字体子集并 `build-fs`。

---

## 8. 图层顺序

### 8.1 RGB565 大画布内部：底 → 顶

顺序是硬约束：

```text
1. 地图瓦片
2. 导航线 + 剩余航点点位
3. 道路名 / 地名 / 水域名 / POI
4. REC 轨迹线
5. REC 公里圆标
6. REC 公里数字
```

实现位置：

- 地图瓦片：`vmap_render_draw_tile()`；
- 导航线：`vmap_route_paint_to_canvas()`；
- 剩余航点点位（多站行程）：`vmap_route_set_trip_pins()` + `route_paint_trip_pins()`，
  与导航线同一个函数、在其之前落笔。**不看 `vmap_route_is_active()`**：每段规划时
  `vmap_route_apply()` 会先清零再填，`active` 有短暂空窗，点位跟着闪没没道理；
- 标签：`vmap_label_paint_to_canvas()`；
- REC：`vmap_track_paint_to_canvas()`。

`render_finish()` 的完整叠层顺序：

```text
tiles already in buffer
  → route/nav（含剩余航点点位）
  → labels
  → REC polyline
  → REC km discs
  → REC km numbers
```

### 8.2 LVGL 对象层：底 → 顶

```text
RGB565 地图大画布
  → GNSS 导航箭头 lv_img
  → Helm subbar / nav banner / rotbar / pause/arrival chrome
```

箭头和 Helm chrome 不属于 RGB565 内部标签碰撞系统。

### 8.3 进度增量刷新

导航进度变化但瓦片没有变化时：

```text
vmap_view_stamp_nav_then_track()
  → nav
  → REC
```

此路径不重画标签，避免同一字体反复烤入导致笔画变粗。完整标签只在 tile frame
结束或明确的全画布 restamp 时处理。

---

## 9. REC 轨迹约定

- REC 点按最小地理距离进入 PSRAM 环形点表；
- 地图隐藏时仍记录，不丢点；
- render pump 进行中不直接画尾巴；
- 返回地图或 pump 完成后再按当前 render 原点投影；
- 局部 dirty stamp 不能把 `last_paint_count` 假装推进到全部点；
- 只有真正全画布 stamp 才能声明全部轨迹已绘制；
- 增量尾巴只画未绘制段；
- 公里数字必须最后绘制；
- 轨迹环回不得每个新点触发 full repaint。

轨迹可见范围搜索从最新点向后，不能每个 GNSS tick 扫描整趟所有点。

---

## 10. 导航约定

- 导航路线与显示瓦片使用同一地理投影；
- 路线位于标签下方；
- REC 位于路线和标签上方；
- 导航匹配稳态只搜索 hint 前后有界窗口；
- hint 初次获取可粗采样后局部精修，禁止 UI GNSS tick 全路线重扫描；
- 导航重新规划/路线几何真正改变时才允许地图 restamp；
- 单纯页面进入不是路线或地图重建条件。

### 10.1 多站行程（常用点 / 坐标点导航）的「到达」语义

`vmap_route_nav_state_t::remain_m` 只算到**当前这一站**，到中间站时同样归零，
于是 `vmap_route_nav_is_arrived()` 在中间站就成立。以前 `map_page_nav_arrived()`
没有行程维度，直接把这一位透出去，带来两个后果：

1. 横幅误显示「已到达」；
2. 更严重的是 `helm_shell` 据此弹出「导航结束」dock，而那个 dock 上**任意一个
   按键**都会走 `helm_arrive_confirm() → map_page_nav_stop()`，整趟行程在第一站
   就被用户无意停掉。

现在的约定：

- `map_page_nav_arrived()` = **整趟行程**已到终点。中间站一律返回 false
  （判据是 `leg_idx + 1 < wp_count`，不能用 `vmap_route_trip_is_complete()`：
  跑到最后一站时 `leg_idx` 停在 `total-1` 不再 advance，complete 仍是 false）。
- tick 里 `nav_arrive_hold` 的置位也必须过同一道行程判定：中间站掉进
  GPX / 非行程那条 `should_auto_stop` 分支时**不能收尾**（到站瞬间若 ahead
  预规划在跑，推进分支会被 `!nav_trip_ahead_planning` 挡掉而掉进这里，
  一旦置位就永久短路，行程再也推不动）。
- 换站＝**拿到了新一段路线**才算数。`worker_busy()` 时不推进；`begin_leg` 或
  `rolling_begin` 交不出新路线就把 leg 退回上一站下一拍重试。否则 nav 会停在
  已经「到达」的旧路线上，下一拍到达判定再次成立 → 再推进一站，后面的站被
  一路跳光（同样的回退也补在 `nav_plan_done` 的 silent 失败分支里）。
- 中间站换站成功后给一次可见反馈（`已到第N站 · 还剩M站` + 到站提示音）：
  这一段的重新规划是 silent 的，不给反馈的话横幅直接跳到下一站，骑手容易
  以为导航在第一站就断了。

**表达层：「当前站」不能被看成「终点」。** 逐段滚动规划是硬约束（`region_path`
只存一段、`VMAP_ROUTE_MAX_PTS` 也装不下整趟），所以这条线天生止于当前站。
能做的只有把「后面还有站」讲清楚，三处：

- **地图点位**：`vmap_route_set_trip_pins()` 把还没到的航点画成白圈 + 导航色芯的
  小圆点（比回放的起终点标记小一号）。挂在 `vmap_route_t` 上只是借它已有的
  canvas/view；点位不属于任何一段几何，所以放在导航线之前画、且不看 `active`。
  同步点是 `map_page_nav_refresh_map()`（正好是 canvas 要重盖的时刻）+ 行程起步
  （此刻地图还盖在菜单下，refresh_map 会早退）。清空由 `vmap_route_clear()` 兜底。
- **横幅 / 状态栏**：`map_page_nav_status_text()` 在多站行程里输出
  `1/3 · 金陵路`，**角标放前面** —— 调用方缓冲只有 24 字节、站名又允许 23 字节，
  放后面会被长站名挤掉。
- **导航菜单**：`map_page_nav_trip_progress()` 供「途经点」行显示 `1/3`（原来写死
  「未到」）。总站数 < 2 视为单站导航，一律返回 false（没有「第几站」可言）。

**没有改的**：剩余里程 `N%dm` 与 ETA 仍是「到当前站」的口径。按整趟算只能拿
后续各站的直线距离凑，真值不准，不如不显示。

**「站就在脚下」＝那一站已经过掉。** 这条规矩贯穿三处，实现都在
`vmap_route_trip_skip_reached()` / `vmap_route_trip_first_unreached()`
（`VMAP_ROUTE_ARRIVE_M` 为判据）：

- **选点提交**（`map_page_nav_trip_plan()`）：先 `skip_reached()` 再 `begin_leg()`。
  站在第 1 个站在脚下时从第 2 站开始导，而不是拿一条 0 长度的 leg 去问规划器
  （`vmap_route_compute()` 只会回 `already at dest`）。返回 false（剩下的站全在
  脚下）才算「已在终点附近」。顺带修好了「无定位时把起点当成第 1 个站」那条
  兜底 —— 之前它实际会去规划一条起点＝终点的 leg，必然失败。
- **换站**（tick 里 `advance_leg()` 之后）：两个站选得太近时，新 leg 的目标可能
  同样在脚下，再往后跳。跳不动了（剩下的站全在脚下）就直接收尾 —— 否则会退化成
  每拍「推进→规划失败→退回」的空转。提示里的「还剩几站」按跳站**之后**的 leg 算。
- **就近规划**（`map_page_nav_plan_nearest_trip()`）：最近的那个站常常就在脚下。
  先用纯判据 `first_unreached()` 判断后面还有没有值得去的站（**不动状态**：先挪
  leg 再判断的话，一旦决定不规划，leg 已经挪走，正在跑的导航就对不上了），
  有才 `set_leg()` + `skip_reached()`。

菜单里的「已在终点附近」判据也必须跟着改成整条行程（以前只看 `wps[0]`，
于是站在第 1 个**途经点**上就误报「已在终点附近」）。

**注意**：跳站是靠移动 `leg_idx` 实现的，**不裁剪 `wps[]`**。因为站点名按同一
下标存在 `nav_trip_names[]`（`set_names()` 单独调用），裁数组会让名字整体错位；
移动 `leg_idx` 则让名字、地图点位、`N/M` 角标全部自动对齐。

---

## 11. 性能与线程约束

### 必须保持

- LVGL 相关绘制仍在 bicycle UI 线程。
- render pump 分片执行。
- 冷瓦片 I/O 只在 pump。
- 地图隐藏时只低频跟车，不按 200 ms 全速栅格化。
- `helm_ui_cb` 只更新当前可见走势图。
- DEM 使用时间/距离缓存，不能在 UI tick 重复全图最近邻。
- SD/LFS 长 I/O 保持现有 watchdog busy/yield 约定。

### 禁止作为“优化”引入

- 每次进入 LiveMap 清画布或完整重载瓦片；
- GNSS callback 同步 `vmap_tile_cache_warm()`；
- 把地图、标签、REC 分别移动而不共享同一 pan；
- 把标签恢复成位于整个 RGB565 canvas 之上的 LVGL overlay；
- 让标签盖住 REC；
- GPU page flip、EPIC snapshot、`opa_layered`；
- 为掩盖卡顿降低 GNSS 1 Hz 数据质量；
- 在 UI handler 末尾额外加长 sleep；
- 从 idle 伪造 IWDT 喂狗；
- 用“增加线程”替代先消除 UI tick 的全表扫描。

---

## 12. 状态变化允许触发什么

正常 GNSS 小位移：

- 允许：canvas set-pos、箭头角度；
- 禁止：读瓦片、全重画。

GNSS 越过 116 px：

- 允许：scroll-rebase、dirty edge pump；
- 禁止：无条件 full clear。

Helm 页面切换：

- 允许：hide/show、invalidate、低频后台跟车，以及共享 Helm 内容根
  `8 px / 100 ms` 的轻微 Y 向入场；
- 禁止：storage reload、tile cache flush、fresh frame。

入场动画只能作用于 `map_page_root()` 的临时 style translate。禁止修改
`vmap_view` 内部 canvas 的位置、pan、render 原点、scale 或 opacity；动画结束后
translate 必须回到 0，因此不会参与地图地理坐标计算。

zoom/scale 改变：

- 允许：完整重建；
- 禁止：复用旧 scale 的 RGB565 像素。

主题真正变化：

- 允许：重建底图颜色；
- 主题未变时禁止因 resume 重建。

REC 开始/停止、圈结束：

- 允许：明确的底图 restamp 擦除旧 REC；
- 禁止：把新轨迹直接盖进旧地理原点。

MTP 改写地图：

- 允许：quiesce、关闭句柄、完成后 storage reload；
- 禁止：MTP 写入期间地图读 LFS。

---

## 13. 关键日志

正常软平移：

```text
[map] follow pan=...
```

越界重锚：

```text
[stall] vmap_set_center full dx=... dy=... limit=...
[map] rebase scroll=... dirty=...
```

后台泵：

```text
[stall] vmap_pump ... done=... n=... rev=... busy=...
```

页面切换不应仅因为“进入地图”出现完整 render request。若持续出现：

1. 检查主题是否被误判为变化；
2. 检查 `map_refresh_pending` 是否被页面 visibility 设置；
3. 检查是否错误调用 storage reload；
4. 检查 `render_rev` 是否被清零。

性能分段：

```text
[stall] gpx dem ...
[stall] gpx nav ...
[stall] gpx follow ...
[stall] gpx track ...
[stall] gpx_timer ...
[stall] helm_ui_cb ... id=4
```

---

## 14. 验收清单

### 页面生命周期

- REC+导航时从地图切到其他 Helm 页面 1–5 分钟再返回；
- 返回立即有地图，不先清屏；
- 不因进入页面全量加载瓦片；
- 地图中心追上 GNSS；
- 道路、面、nav、REC 不错位。
- 开机：splash「地图加载」走完前不得露出未完成画布；
  默认 Helm 待机时 GNSS 不得 rebase 清掉开机首帧。

### rebase

- 连续骑行跨过多次 116 px 阈值；
- 只见 dirty edge pump；
- 不出现道路断层、三角形面残片和 tile seam；
- 按键无长时间失去响应。

### 标签

- 同一路跨两个小瓦片；
- 小瓦片接缝不能裁断文字；
- 同名标签不过近；
- 长道路平移进入屏幕时有连续路名；
- 不同道路优先各获得一个标签；
- 标签位于 nav 上、REC 下；
- REC 交叉处必须盖住路名。

### 轨迹

- REC 后离开地图，返回不缺线、不跳线；
- pump 期间新增点最终能补上；
- 公里圆标和数字跟随正确；
- 长时间记录后 gpx timer 不回到 100 ms 全表扫描。

### 多站行程

- 途经 3 站以上时，到第 1、2 站**不**出现「导航结束」dock、横幅不显示「已到达」；
- 每次到中间站有「已到第N站 · 还剩M站」提示，横幅随即切到下一站的转向与距离；
- 到中间站时按任意键不会结束整趟导航；
- 只有到最后一站才出现结束 dock，按键确认后 nav 才停；
- 到站瞬间恰好赶上后台重规划时，下一拍能自己续上，不出现停在原地不动；
- 地图上能看到后面几站的白色小圆点，**已到过的站不再画**；菜单「途经点」显示
  当前站/总站数，横幅显示 `1/3 · 站名`；停止导航后点位全部消失；
- 站在第 1 个（或前几个）站上时点「开始导航」：直接从那之后的第一个站开始导，
  **不**报「已在终点附近」；只有所有选中的站都站在上面时才报这句；
- 行程中两站挨得很近（新 leg 目标就在脚下）时能自动跳过，不出现「推进→失败→
  退回」的空转。

---

## 15. 源码索引

页面与调度：

```text
ui/bicycle/src/lvgl_page/live_map/map_page.c
ui/bicycle/src/lvgl_page/helm_shell.c
ui/bicycle/src/lvgl_page/helm_splash.c
ui/bicycle/src/lvgl_page/startup/startup_page.c
ui/bicycle/src/bicycle_c_main.c
```

视图、瓦片和光栅：

```text
ui/bicycle/src/vmap/vmap_view.c
ui/bicycle/src/vmap/vmap_render.c
ui/bicycle/src/vmap/vmap_tile_cache.c
ui/bicycle/src/vmap/vmap_cache_arena.c
ui/bicycle/src/vmap/vmap_vpk_heap.c
ui/bicycle/src/vmap/vmap_tile_system.c
```

标签与叠层：

```text
ui/bicycle/src/vmap/vmap_label.c
ui/bicycle/src/vmap/vmap_route.c
ui/bicycle/src/vmap/vmap_route_nav.c
ui/bicycle/src/vmap/vmap_track.c
```

配置：

```text
ui/bicycle/src/vmap/vmap_config.h
ui/bicycle/src/bicycle_config.h
configs/nsh/defconfig
```

---

## 16. 修改原则

修改 LiveMap 前先回答四个问题：

1. 这次变化是否改变了 RGB565 像素采用的地理原点？
2. retained interior 是否仍与 `render_lon/render_lat` 一致？
3. dirty edge 被重画时，标签占位、nav 和 REC 是否采用同一 clip？
4. 页面切换是否被错误地变成了地图加载事件？

如果任何答案不确定，不应直接在 GNSS/KEY callback 中追加 render、warm 或
overlay stamp。先把状态变化纳入 `render_begin → pump → render_finish`。
