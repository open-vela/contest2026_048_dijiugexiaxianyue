# AGENT_COORDINATION — vmap 效率（2026-10-27）

## 基线

`vendor/HelmOne` @ `711eeca1`（retire-eval），工作树干净。视口 240×216，
画布 496×472（margin 128），scale 1.69，泵 2 张/20 ms。

## 工作包

### WP-1 选片范围 ring=1 → 0（主对话执行）— PASS

- 文件：`src/vmap/vmap_view.c`（`render_begin` 里的 `tile_range` 调用）
- 依据：`ring` 是在画布半宽之外再加 `ring×256` 瓦片像素；ring=1 时每边多出一整
  行/列，近边不超过画布边 ⇒ 像素与标签都被裁掉，只付要素遍历与闪存读。
- 几何核算（脚本，视口 240×216 / margin 128 / scale 1.69）：所选瓦片 16~20 张，
  与画布相交 4~6 张。改后 4~6 张全部相交（边界恰好落在瓦片线上时至多多 1 张）。
- `pump_tx_min/max` 的使用点已核对：只有 `render_next_tile` / `pump_advance` /
  一条日志，无"故意画到画布外"的依赖。

### WP-2 道路图层分桶（执行子 Agent `agent_dca361bf…`）— PASS（复核后）

- 文件：`vmap_config.h` / `vmap_render.h` / `vmap_render.c` / `vmap_view.c`
  （create/destroy 与 bind）
- 方案：创建时分配 1536×4 B PSRAM 索引并绑定；`draw_road_layer` 每瓦片扫一次
  ROAD 层装记录起点，casing/fill 两个 pass 只在数组上按 `rec[0]` 过滤，命中才
  解码单条记录。未绑定 / cap<=0 / 溢出（>1536 条）→ 原 14 遍实现原样保留。
- 主对话复核：逐行读 diff；顺序、守卫、`raw` 合法化、颜色/宽度逐条对得上
  （见 `evidence/WP-2/R1/IMPLEMENTATION.md` §3）。
- **主对话补充修正**：子 Agent 列的残余风险 1（销毁旧视图会解绑新视图的索引，
  换主题重建 shell 时可能"先建新后毁旧"⇒ 静默退回 14 遍）已修：新增
  `vmap_render_road_index()`，destroy 只在"当前绑定是自己"时解绑。
- 等价性：宿主差分测试（真实 `vmap_render.c`+`vmap_tile.c`，bind 与不 bind 两条
  路径渲染到同一画布逐字节比较）14 个用例 **ALL EQUIVALENT**，ASan 干净；
  1400 条密集瓦片 `vmap_feature_iter_next` 调用 19618 → 4205（≈4.7×）。
  复现命令见 `evidence/WP-2/R1/IMPLEMENTATION.md` §4（已对新源码重跑通过）。

## 编译

- `ninja -C cmake_out/helmone_nsh`：exit 0。
- 两个被改文件均真实重编（`vmap_render.c.o` / `vmap_view.c.o` mtime 20:29:55-56）。
- 镜像里确认落地：`nm` 有 `vmap_render_bind_road_index` / `vmap_render_road_index`，
  `strings nuttx.bin` 有 `[vmap] road index unavailable`。
- 体积：flash 3158748 B（75.31%），sram 466100 B（88.90%）——索引在 PSRAM，
  未占内部 SRAM。
- 无新增编译警告（`vmap_view.c:1075` 的 `board_psram_canvas_arena` 隐式声明是
  改动前既有）。

## 证据

- `docs/agent/evidence/WP-2/R1/IMPLEMENTATION.md`（实现 + 自查 + 差分测试记录）
- 产物：`cmake_out/helmone_nsh/{nuttx,nuttx.bin,System.map}`（2026-10-27 20:29）

## 未做 / 留观察

- 设备 A/B 口径：`[vperf] frame … tickd= tiles= cyc_tile= cyc_tot=`；全量帧
  `tiles` 应 16~20 → 4~6。
- 既有问题（与本改无关）：`VMAP_RENDER_AA=0` 时 `box_may_write` 在
  `#if VMAP_RENDER_AA` 内导致 `draw_road_polyline` 编译失败。

## 上板结果（2026-10-27 20:38 起，`bicycle_nsh demo start linear`）
- 固件：工具链 `build`（ELF 存档 `my_vendor+nsh+20261027-203328`）→ `flash`
  （用户终端 `build flash monitor -p /dev/ttyACM0` 完成烧录）；板上行为即新代码。
- 演示：linear 118.329000,32.212000 → 118.647519,32.212000 @120 km/h。
- 全量帧（`dirty=0`）：`tiles=4` / `tiles=4 warms=1` / `tiles=6 warms=2`。
  改前按同一几何应选 16~20 张。`cyc_tile` 29.5M 周期（4 张，123 ms）、
  25.2M（6 张，105 ms）。
- 补边帧（`dirty=1..3`）：`tiles=2~5`、`warms=0`，`cyc_tile` 8~17M 周期
  （33~69 ms），`scr` 6.5M（27 ms，EPIC 滚动）—— 跟车路径未受影响。
- 无 `[vmap] road index unavailable`，无断言/栈/异常。
- **z=15 出图 0 张**：`warms=0` 且无报错 ⇒ 该 cell 的分片没有 15 级瓦片
  （数据覆盖缺口，与本改无关；z=14 正常）。
- **`[vperf] SLOW pump gap` 是误报**：泵定时器在帧结束时会 `lv_timer_pause`
  （`map_page.c:1816`），而基线只在"泵被调用且空闲"时清零 ⇒ 每次 rebase 的第一拍
  都会把"上一帧结束到现在"算成帧内间隔（实测 8~17 s，与 rebase 周期一致）。
- **`hz=` 字段小 10 倍**：`CONFIG_USEC_PER_TICK=1000`（1 ms tick），而
  `vmap_perf_frame_end` 按 `tickd*10 ms` 反解。`tickd` 本身就是毫秒。
  A/B 请比 `tiles=` / `cyc_tile` / `cyc_warm`，不要比 `hz`。


## WP-4 航向朝上（地图随骑行方向旋转，箭头恒指上）— 2026-10-27

### 设计

- **画布仍按正北向上渲染**（瓦片/导航线/轨迹/公里标都烘在同一块缓冲里），旋转只发生在
  显示合成那一步：LVGL 把 `draw_dsc->rotation` 直接喂给 EPIC 的 transform
  （`lv_draw_sifli_epic_img.c:128`），所以**不需要中间缓冲、也不需要重光栅化**。
- 屏幕位置补偿：`T = C - p - R(θ)·pan`（C 视口中心、p 画布中心、θ 画布旋转角），
  这样"画布中心 + pan"（骑手位置）永远落在视口中心；θ=0 时退化成原来的公式。
- 角度：`θ = -heading`（顺时针为正），圆周低通 α=0.35/拍，变化 ≥1.5° 才重设；
  首个航向无条件接受，之后速度 <4 km/h 保持上次朝向。
- **该模式不烘路名标签**（会转成斜字）⇒ [vperf] 里 `labels=0`、`cyc_label=0`。
- **跟车 rebase 阈值变紧**：旋转后视口四角离中心是半对角线 r=161 px，画布必须盖住它，
  否则 45° 附近四角露底 ⇒ `limit = margin - guard - (r - view_h/2)`（240x216、margin 128
  → 62 px，比正北模式的 116 px 更早 rebase）。
- 运行期开关：`bicycle_nsh rot on|off`（新 ctl op `MAP_ROT`），默认开。

### 改动文件

`vmap_config.h`（常量）、`vmap_view.h/.c`（模式/航向/位置/失效域/标签/箭头/limit）、
`map_page.h/.c`（默认开、每拍喂航向、`map_page_apply_rot_cmd`）、
`bicycle_ui_ctl.c`（op 分发）、`bicycle_nsh_ctl.c`（`rot` 命令）、
`include/myvendor_bicycle_ctl.h`（新 op）。

### 上板

- 固件 `my_vendor+nsh+20261027-210200`，`flash` 退出 0；`[map] heading-up on/off`
  日志成对出现；`labels=0` 证明渲染路径确实进了该模式；无断言/EPIC 报错。
- 60 s 窗口粗比：rot on 3 条 `SLOW pump gap` / 2 帧，rot off 1 / 1 —— 样本太小，
  不足以判定旋转增加 UI 停顿；LVGL 合成耗时不在 `[vperf]` 覆盖范围内。
- **待用户目视**：旋转是否平顺、箭头是否恒指上、画布边缘有无露底/缺条。

### 已知取舍 / 下一步

- 路名标签关闭（第一版）；轨迹公里数字仍烘在画布上 ⇒ 会跟着转。
- EPIC 旋转是**最近邻 + 亚像素**，旋转时图面会有轻微抖动。
- 若观感卡：先调 `VMAP_ROT_STEP_10`（1.5° → 3°）与 `VMAP_ROT_SMOOTH`，再考虑
  只在可见视口上做一次自有 EPIC 旋转到中间缓冲（多 104 KB + 一次搬运）。

## WP-5 帧率统计 + 旋转开销定位（2026-10-27 晚）

### 探针

- `vmap_view.c` 的 `vmap_fps_probe_cb`：挂 `LV_EVENT_RENDER_START / RENDER_READY`
  （`lv_refr.c:765/833`，`inv_p==0` 提前返回 ⇒ 只统计"真的画了"的刷新），
  每 2 s 打一条 `[mapfps] n=… fps=… avg=… max=… win=…`。
- **默认静默**（`LOG_DEBUG`）：`ctl log all` 打开，测完 `ctl log notice`。

### 实测（明湖环线 45 km/h，`[mapfps]`）

| 场景 | fps | 每次重绘 avg | max |
|---|---|---|---|
| 旋转关（正北） | 3.3~5.1 | **14~26 ms** | 26~33 ms |
| 旋转开（任意角） | 3.2~4.7 | **50~91 ms** | 105~128 ms |
| 旋转开（直线，恒定 90°） | 4.4~7.6 | **12~14 ms** | 16~34 ms |

### 结论

- **EPIC 的任意角旋转比 90° 整数倍贵 4~5 倍**（同样条带数、同样缓存维护 ⇒ 不是
  刷新域/清缓存的问题，是源地址不连续）。90° 那次和"旋转关"同价。
- 画布对象带旋转后，**任何一次失效（包括 1 px 平移）都会重跑一次完整 transform**
  ⇒ 跟车的每 1 px 重绘都付 ~47 ms。这才是 3~4 fps、每次 60~80 ms 的成因。
- 把角度量化从 1.5° 放到 3° **基本没用**（3.2~4.4 fps、avg 50~87 ms），因为
  主导项是平移重绘，不是角度步进。

### 可选下一步

- **A（便宜、但抖）**：航向朝上时把 `VMAP_PAN_MIN_STEP_PX` 1→3，重绘降到 ~1 次/s
  ⇒ ~10% UI 时间；代价是地图按 3 px 跳（低速时很顿）。
- **B（正确、但改动大 ~300 行）**：屏幕对齐的旋转缓存 —— 角度变化时才整幅重转，
  平移改成"整块滚动 + 只转新露出的窄条"（和正北模式同一套思路）。预计平移
  ~3 ms/次、角度步进 ~100 ms/次 ⇒ 总负载约现在的一半，且平移 1 px 也不抖。

### EPIC 实测（`[epicimg]` 探针，同一场景：直线 demo）

| 模式 | EPIC 调用 | 处理像素 | blend 均值 | blend 最坏 | 整帧 avg |
|---|---|---|---|---|---|
| 旋转关（rot=0.0） | 48~57 次/2 s | 419~473 k | **0 ms** | 2~4 ms | 11~14 ms |
| 旋转开（rot=270.3°） | 41~62 次/2 s | 317~475 k | **12~13 ms** | 52~54 ms | 50~67 ms |

- `st=0`（HAL_OK）⇒ 调用成功、确实走 EPIC；像素数相当 ⇒ **不是调用次数/面积问题**。
- 同一条调用带旋转后吞吐掉到 ~0.8 Mpx/s（≈350 周期/像素），不旋转时几乎免费
  ⇒ 慢在 EPIC 的 **transform 通路**（源地址不连续），不是 CPU 回退、不是缓存维护
  （后者两种模式都付，≤2 ms/次）。
- 每次重绘 ~6 次调用、合计 51.8 k 像素 = 整幅视口。**平移 1 px 却要重转整幅**，
  所以跟车的每一次 1 px 位移都付 ~55 ms —— 这才是 3~4 fps 的根因。
- 早前"90° 快（12~14 ms）"是测量假象：那时地图静止，那些刷新是箭头/chrome 的。

### 旋转是否解码

不解码。画布是 `LV_IMAGE_SRC_VARIABLE`（`lv_image_dsc_t` + `draw_buf`），解码器只校验
`data != NULL` 就把缓冲指针交出去，没有 PNG/JPEG 解码、没有拷贝。每次调用的固定开销
只有两笔整块 cache 维护（`lv_draw_buf_flush_cache(decoded, NULL)` +
`lv_epic_flush_layers_cache`，各按 468 KB 全画布算），实测 ≤2 ms。

**临时探针**：`apps/graphics/lvgl/lvgl/src/draw/sifli/epic/lv_draw_sifli_epic_img.c`
里的 `epic_img_probe_note()`（`[epicimg]`，LOG_DEBUG）。**提交前要删**。

## WP-6 视觉最优方案：屏幕对齐旋转缓冲（2026-10-27 深夜）

### 做法

- 新增 EPIC 旋转原语 `myvendor_epic_rotate()`（`services/myvendor_epic_blit.c`）：
  摆法照抄 LVGL 适配层（bg=目的整层，fg=源整层 + transform_cfg，`offset = dst_c - pivot`）。
  带开机自检 `myvendor_epic_rotate_selftest()`（坐标编码图案转 30°、抽查 10 点）。
  **实测 PASS**：`[vrot] selftest PASS: 10 probes, 0 fail`。
- vmap 不再让 LVGL 旋转画布，改为维护**屏幕对齐的旋转缓冲 R**（`VMAP_ROT_MARGIN=32`
  ⇒ 304×280，170 KB PSRAM）：
  - 角度变化 / 余量用完 / 整帧结束 → `myvendor_epic_rotate` 整幅重烘一次；
  - 平地跟车 → 只移动 R 的**对象**（`vmap_rot_track`），不再重转整幅；
  - 画布 C 仍正北烘焙，R 显示时隐藏 C。

### 实测（明湖环线 45 km/h）

| 指标 | 改前（LVGL 旋转画布） | 改后（旋转缓冲） |
|---|---|---|
| LVGL 合成/刷新 | 50~91 ms（max 105~128） | **14~26 ms**（max 23~41） |
| 刷新率 | 3.2~4.7 /s | 2.9~5.3 /s |
| 重烘（任意角 304×280） | — | **151~158 ms，约 1 次/s**（转弯时） |

- `[mapfps]` 覆盖不到重烘（它发生在 GNSS 更新路径），`[vrot] bakes` 才是重烘口径。
- 剩余观感风险：转弯时每秒一次的 155 ms 停顿 + 3° 的朝向跳变。
- 可调：`VMAP_ROT_STEP_10`（30=3°）调大 ⇒ 重烘更少、跳变更大；调小反之。
- 若要再压重烘：R 缩到视口大小（240×216）改走"滚动 + 只转新露出的窄条"，
  预计重烘 ~95 ms、平移 ~7 ms，但要再写一套条带旋转。

### 未做

- 条带旋转（当前整幅重烘）。
- 标签仍关闭（航向朝上模式）。
- 临时探针 `[epicimg]` / `[vrot] bakes` / `[mapfps]` **提交前删**。

## WP-7 航向朝上的正立文字（2026-10-28）

用户定：文字**不能烘进背景**（会随地图转成斜字），改成独立文本渲染。

### 做法

- 标签仍在 tile 渲染时收集（`vmap_label_collector`，画布坐标）；
- 航向朝上时**不**烘进 C（`stamp_overlays` 的烘焙只在正北模式）；
- 每次 `vmap_rot_full()` 之后调 `vmap_rot_labels_update()`：把候选点按
  `R_center + R(θ)·(画布点 - 骑手点)` 投到旋转后的屏幕坐标，摆进最多
  `VMAP_ROT_LABEL_MAX=8` 个 `lv_label` 对象（正立、按 kind 上色、同名去重、
  视口外裁剪）；
- 跟车平移时 `vmap_rot_place()` 把 R 和这些标签一起按 (sx,sy) 移动，y_bias 统一在
  那里加（导航横幅出现/消失时两者不脱节）。
- 开关：`bicycle_nsh rot label on|off`（默认 on）。

### A/B（明湖导航 + heading-up，同一固件，各 60 s）

| | fps | 每次刷新 avg | max |
|---|---|---|---|
| 有文字（正立对象） | 2.7~5.2（均值≈4.2） | 16~30 ms | 25~43 ms |
| 无文字 | 2.8~6.7（均值≈4.5） | 16~32 ms | 24~47 ms |

差异在噪声内 ⇒ **正立文字几乎不花钱**（8 个小对象，每次刷新几百像素）。
对照：如果把 20~30 条文字烘进画布，每帧要 3.5~35 ms（见 WP-4/上一轮测量），
那才会明显。

### 待确认（只有屏幕能判）

- 文字是否正立、位置是否落在对应道路/地物上、边缘是否有半截字。

### 1° 步进 + 重烘限频（2026-10-28，用户要求）

- `VMAP_ROT_STEP_10` 30 → **10（1°）**：朝向变化满 1° 才重烘。
- 新增 `VMAP_ROT_BAKE_MIN_MS`（默认 500 ms）：两次整幅重烘的最小间隔。
  1° 步进若不限频，3.6°/s 的弯道要 3~4 次/秒（≈50% UI）；限频后最多 2 次/秒，
  转弯时地图比真实航向慢半拍，常见弯道单步落在 1~2°。
- 明湖实测：bakes **0.8~1.2 次/秒**、每次 **147~152 ms**（≈15% UI）；
  合成 24~31 ms/次、fps 2.4~4.0。文字对象开/关差异仍在噪声内（WP-7）。
- 调法：想更慢更省 ⇒ 加大 `VMAP_ROT_BAKE_MIN_MS`；想更跟手 ⇒ 减小它。

## WP-8 bicycle_ui 74% → 51.5%（2026-10-28）

### 定位（新增探针）

- `[ui] gpx=…ms/n pump=…`：`on_gpx_timer` / `on_render_pump` 两个 UI 定时器的净耗时
  （包装函数，见 map_page.c）。
- `[gpx] total=… navupd=… gpxwin=… follow=…`：`on_gpx_timer` 内部分段（整拍 ≥60 ms 才打）。
- `[mapfps]` 增加 `refr=`/`ovh=`：整次刷新 vs 其中"画"的部分（差值 = 刷新管线开销）。

### 结论：最大头不是地图，是导航投影

- `vmap_route_nav_update()` 单次 **195 ms**，每 500 ms 一次 ⇒ 约 39% UI。
- 根因：`vmap_geo_point_to_seg_m()` **逐段**做 3 次 haversine + 2 次 bearing，
  约 15~20 个**双精度**超越函数；这颗核只有 FP32，FP64 三角函数是软件实现
  ⇒ 2000 段的路线投影 ≈ 195 ms。

### 修法

- `vmap_geo_point_to_seg_m()` 改成**局部等距圆柱平面**：cos(lat) 用四阶多项式
  （|lat|≤80° 误差 <1e-4），其余纯乘加 ⇒ **195 ms → 17 ms（11×）**。
  旧实现留在 `#if 0` 里作对照。所有调用点（导航/吸附/门户/GPX 最近点）一并受益。
- 重烘限频 500 → **900 ms**、R 余量 32 → **24**（每次 155 → 138 ms）。

### 实测（明湖，日志级别 notice）

| | 改前 | 改后 |
|---|---|---|
| `bicycle_ui` | **74.0%** | **51.5%** |
| `CPU0 IDLE` | 2.1% | **33.0%** |
| `diag` | 18.9% | 3.1%（那 19% 主要是 `ctl log all` 的串口日志） |
| `[ui] gpx` | 43~74% | 23~32% |
| 重烘 | 156 ms × 1.5/s | 138 ms × ~1/s |
| 合成 | 15~38 ms | 23~28 ms |

### 还能再压（未做）

- 重烘限频 900 → 1500 ms（+5%），代价是转弯更滞后。
- 平移量化 1 → 2 px（合成 −3%，代价是地图 2 px 一跳）。
- `diag` 空闲时仍有 3%，与地图无关。

### 试过但**回退**：把 EPIC 的忙等改成中断+信号量（2026-10-28）

- 动机：任意角重烘 138 ms 是 `EPIC_WaitDone()` 忙等，`ps` 里算进 bicycle_ui。
- 做法：`lv_epic_blend()` 的 NuttX 分支改走 `HAL_EPIC_BlendStartEx_IT` +
  完成中断 post 信号量（`nxsem_tickwait`，400 ms 超时兜底并永久退回忙等）。
- 实测（明湖，`CONFIG_SCHED_CPULOAD=200Hz` 窗口化，可比）：
  - bicycle_ui **51.5% → 72.2%**，IDLE 33% → 12%
  - 重烘 138 ms → **140~174 ms**
  - 无超时告警（中断路径本身是通的）
- 结论：**变差了**。每次 EPIC 调用都要付信号量排空 + 唤醒的固定开销，而"空转"本来
  也没人抢（IDLE 有 33%）。瓶颈是 **EPIC 引擎吞吐**，不是 CPU 有没有空。
- 已回退到忙等（`lv_sifli_epic_cfg.c` 里留了说明）。回退后复测 bicycle_ui 51.7%。
- **别再试这条路**。

## WP-9 航向朝上做成设置开关（2026-10-28）

用户定：做成可选项，**默认关 = 原样**（正北朝上、路名烘进地图图片，性能最好）；
打开 = 随航向旋转（箭头恒指上）、路名走独立正立文本、不烘进底图。

### 实现（沿用 devctl 的 KV 模式）

- `ctl/myvendor_devctl.h/.c`：新键 `persist.ui.map_heading_up`（默认 0），
  `myvendor_devctl_map_heading_up_get/set()`。
- `lvgl_page/helm_menu.c`：设置页「骑行」组新增「地图朝上」开关
  （`VAL_MAP_UP` / `ACT_TOGGLE_MAP_UP`，图标 `HELM_ICO_NAV`）；切换时写 KV 并
  post `MYVENDOR_BICYCLE_CTL_OP_MAP_ROT` 立即生效。
- `map_page.c`：建页时按设置初始化 `vmap_view_set_heading_up()`；
  `map_page_apply_rot_cmd()` 里统一落盘（设置页与 `bicycle_nsh rot on|off` 共用）。
- 文字行为自动跟随：`heading_up` 关 ⇒ `stamp_overlays` 把路名烘进画布（`labels>0`）；
  开 ⇒ 烘焙跳过、改用 `lv_label` 正立对象（`labels=0`）。

### 上板验证

- 默认（KV 无文件）：`[vperf] ... labels=21` + 无 `heading-up` 日志 ⇒ 原样路径 ✓
- `bicycle_nsh rot on`：`[map] heading-up on`，随后帧 `labels=0` ✓
- 重启后：`build_date 202610280112` 且开机 8.0 s 打出 `[map] heading-up on`
  ⇒ **设置持久化生效** ✓
- 已把板上设置恢复为关（产品默认）。

---

# 2026-10-28 第二轮：审查 + 剩余热点 + 修复（WP-10..WP-13）

## WP-10 Codex 独立审查 `e66eb6ff`（模型 gpt-6.1-sol / high）

- 原始日志 `/tmp/codex-review-vmap-e66eb6ff.log`，最终消息
  `/tmp/codex-review-vmap-e66eb6ff.md`；**结论 REJECT**。
- OK：选片 ring=0 全角度覆盖与边界、道路索引等价性/绑定生命周期、旋转坐标数学、
  四角露底（`limit=62.56`，取整后最大 pan 63，源画布短边仍余 11.56 px）。
- **P1**：航向朝上时，帧外轨迹/导航增量只改 C，R 不更新 ⇒ 屏上内容可无限期停在旧版本
  （这是 **heading-up 模式的功能缺陷**，不是 e66eb6ff 之前就有的）。
- P2：`render_begin` 先改 C 后置 `pump_phase`（可能烘半成品）；标签尾部隐藏循环把
  `rot_label_n` 恒置 8；destroy 漏删 8 个 label；cos 实为 6 阶、80° 误差 3.5e-4
  与注释 `<1e-4` 不符；`110540 m/°` 比球面低 0.59% 故"<0.1%"不成立。
- 其它记录（不修，属产品边界）：跨 180° 经线/极点为已知不覆盖；
  `VMAP_NAV_MATCH_AHEAD_SEGS=64` 与 `follow_mode=false` 的箭头投影是潜在项；
  非跟随箭头路径实际不可达（`follow_mode` 恒 true）。

## WP-11 剩余热点测绘（只读，`evidence/WP-11/R1/ANALYSIS.md`）

**先纠正两个过期口径**：

- R 不是 304×280（那是 WP-6 的 margin=32），实际 **288×264 = 76032 px**；
  138 ms ÷ 76032 = **0.551 Mpx/s**；线性模型同时复现 WP-6 的 155 ms 与 WP-8 的 138 ms。
- `[mapfps] avg` 是**墙钟**（含 flush 的 `sem_timedwait` 睡眠），不能当 CPU%；
  `[ui] gpx=…` 是**窗口累计毫秒**不是百分比（分母用打印的 `win`）。
- WP-8 的 `bicycle_ui 51.5%` 是 **heading-up 开** 的数字；WP-9 后产品默认是关。

**排序（收益 / 改动面 / 风险）**：

| # | 项 | 收益 | 结论 |
|---|---|---|---|
| 1 | `VMAP_ROT_BAKE_MIN_MS` 900→1200 | 曲线 2.3–5.1 pp | 采纳 |
| 2 | R 专用 guard（不再共用 `VMAP_PAN_GUARD=12`） | 直道最多 3.0 pp，零滞后 | 采纳（guard=2） |
| 3 | heading-up 停止对 C-only 变化瞎作废 | 低速最多 3.5 pp，零视觉变化 | 采纳 |
| 4 | 平移量化 1→2 px | 1.8–2.6 pp | **未做**（观感 2 px 跳，需用户拍板） |
| 5 | 去掉设置切换时的重复重烘 | 一次性 138–276 ms | **未做**（去掉后无后续帧时 R 会停在旧内容） |
| 6 | 导航扫描 64→32 段 | 1.6 pp | **未做**（需真实 GPX 回放验证选段） |
| 7 | 条带旋转 + R 缩到视口 | 2.4–3.9 pp | **不做**（要新 EPIC 原语 + cache bbox 化，接缝风险） |
| 8 | 探针口径修正 | 0 CPU | 采纳 |

否决：R 固定 + memmove（R 152 KB memmove ≈10 ms/次，且上屏像素一个不少）；
全量帧拆脏条（稳态只有 0.3–0.5 pp）。

## WP-12 第一轮实现（`evidence/WP-12/R1/`）

1. **导航每拍全路线重扫**（真实热点，默认正北模式也吃）：
   `vmap_route_nav_update()` 在 `set_progress_match()` 之后又调
   `vmap_route_set_progress_pos()`，后者从 `progress_seg-1` 扫到路线末尾
   （最多 2047 段）重算最近段 ⇒ **17 ms/次 × 2/s ≈ 3.4 pp**。
   删掉该调用，改由同一次匹配结果同步进度；`set_progress_match()` 补
   `progress_pos_seen = true`。重规划的 3 个 `set_progress_pos` 调用点保持不变。
2. heading-up 的 C-only 作废改为直接 `return`（旧 R 本来就没变）。
3. 新增 `VMAP_ROT_TRACK_GUARD=2`，`vmap_rot_track()` 的 `lim` 不再共用
   `VMAP_PAN_GUARD`（该宏仍供标签/正北 rebase 使用）。直道重烘间隔
   2.3 s → 4.6 s（45 km/h）。
4. `VMAP_ROT_BAKE_MIN_MS` 900→1200。
5. 探针：`vmap_perf_frame_begin()` 清零 `s_pf_last_pump_cyc`（修 `SLOW pump gap`
   误报：帧末定时器 pause，下一帧第一拍会把 8–17 s 空闲算成帧内停顿）；
   `[vperf] frame` 新增 `cycms=cyc_tot/tickd`（`tickd` 本来就是 ms，
   `CONFIG_USEC_PER_TICK=1000`），可自校准 HCLK；`[mapfps]` 只在
   `RENDER_START` 之后统计刷新，`refr/ovh` 不再被空转节拍稀释。
6. 注释里的 100 ms/155 ms/0.8 Mpx/s/"给 500 ms"/"取 32 ⇒ 12 s"全部改成实测口径。

## WP-13 第二轮：修 Codex 缺陷（`evidence/WP-13/R1/`）

1. **[P1] 帧外增量能上屏**：`vmap_view_invalidate_canvas_area()` 的 heading-up
   分支改为按**与角度重烘共用**的 `VMAP_ROT_BAKE_MIN_MS` 补一次 `vmap_rot_full()`
   （帧在飞时它自己拒绝，帧末兜底）；`vmap_rot_full()` 成功路径写 `last_bake_ms`。
   形参仍是 `const`，只在实现内一次去 const。
2. **[P2] 开帧顺序**：`render_begin()` 入口即置 `pump_phase=1; pump_need_region=1;`
   （两条路径都覆盖），删掉末尾的重复赋值 ⇒ 期间的位置更新不会烘半成品。
3. **[P2] 标签**：尾部隐藏循环改用独立变量，`rot_label_n` 恢复真实条数；
   `vmap_view_destroy()` 删除 8 个 `lv_label` 并置 NULL。
4. **[P2] 地理精度**：`vmap_geo_cos_deg()` 补 8 阶项（|lat|≤80° 绝对误差 <1e-5）；
   注释改为"单段距离误差 ≤0.7%（22~45°N 实测 0.59~0.62%；79°N/400 m 段最大 0.91%）"。
   原料不变（`110540 m/°`），不改任何阈值。

## WP-14 第二轮审查后的收口（主对话执行，2026-10-28）

Codex 第二轮（`/tmp/codex-review-vmap-round2.md`）仍 REJECT，两条有效：

- **P1 残余**：被 1200 ms 限频跳过的增量没有重试 —— 若最后一次增量发生在
  窗口内、之后位置/航向/帧都不再变化（停车、`nav_review`），R 可能永远停在旧内容；
  `vmap_view_stamp_nav_then_track()` 还只 `lv_obj_invalidate(C)`（隐藏对象）。
- **P3**：`[mapfps]` 窗口打印时提前清 `rendered`，每窗口丢最后一个刷新样本；
  "≤0.6%" 实测出现 0.62%。

修法：

- 新增 `bool rot_pending` + `int32_t want_rot10` + `static vmap_rot_service()`：
  窗口内只记账；到期后**先把 `want_rot10` 应用成 `rot10` 再烘一次**，
  即"待应用角度 + 帧外增量合并到同一次重烘"。
- service 由 **200 ms 的 `vmap_view_set_heading()`** 调用（航向没变、低速、
  NaN 分支也都会走到）；`vmap_rot_full()` 成功时清 pending，
  `vmap_view_set_heading_up()` 切换时清 pending 并把 `want_rot10` 归零。
- `vmap_view_set_heading()` 不再自己做限频、不再直接写 `rot10`、
  不再用 `last_pan=INT32_MIN` 触发重烘（那段 dance 已由 service 取代）。
- `vmap_view_stamp_nav_then_track()` 与 `vmap_track.c` 的
  `track_invalidate_canvas()`（全量重绘 / dirty 无效回退）都改走
  `vmap_view_invalidate_canvas_area()` 的统一入口，不再对隐藏 C 直接作废。
- `[mapfps]` 窗口重置不再清 `rendered`（由随后到达的 `REFR_READY` 消费）。
- geo 注释改成"已测样本 22~45°N 为 0.59~0.62%，高纬更大（79°N/400 m 0.91%），
  没有全域上界"。

**为什么必须合并**：第一版把 service 放在角度更新之前，它会用**旧 `rot10`** 烘
增量并刷新 `last_bake_ms`，随后角度请求立刻被 1200 ms 窗口挡住 ⇒ 持续记录时
地图会长期保持旧朝向（Codex 第三轮抓到的回归）。合并后同一次烘既转到位又补增量。

**已知边界（不修，已记录）**：

- **无有效 fix**：最后一次增量在窗口内、随后定位失效 ⇒ `on_gpx_timer` 早退，
  `set_heading` 不再被调用，pending 要等定位恢复后的第一拍才消费。此时地图
  没有移动、也没有新轨迹点，C 不再变化 ⇒ 观感是"丢最后一点尾巴"，不会乱刷。
- `nav_review` / `!map_ui_visible` / `covered` 三个分支同样跳过 200 ms 拍。
- 帧末与余量重烘按设计**不受** 1200 ms 限制（正确性优先）。45 km/h 下
  余量 22 px ≈ 4.2 s 一次（≈0.24 次/s），不是 Codex 假设的 5 次/s
  （那需要 110 px/s ≈ 260 km/h）。
- 绘制侧的分割点仍独立窗口搜索（`vmap_route.c:2210+`），与 nav 匹配不同源；
  自交/回环路段是既有行为，未改。

## 验证（本轮）

- 编译：`ninja -C cmake_out/helmone_nsh` exit 0；flash 3166640 B（75.50%）、
  sram 472932 B（90.20%）；无新增警告（`vmap_route.c:1620` 的 const 警告已随
  签名恢复消除）。
- 几何定向回归（宿主，`/tmp/geo_check.c`，6480 例 × 2 组）：
  多项式 vs 精确 cos 最大相对距离误差 **3.5e-5**（79°N）；局部平面 vs 球面
  haversine 在产品纬度带 **0.59~0.62%**（79°N/400 m 段 0.91%）；
  退化端点返回到 A 的距离、`t` 钳制正常。
- **宿主像素 harness 不适用**：它只覆盖 `vmap_render.c` / `vmap_blit.c`，
  这两个文件本次一行未改（`git diff --stat` 可证）。
- 独立审查（Codex `gpt-6.1-sol`，逐轮原始日志 `/tmp/codex-review-vmap-*`）：

| 轮 | 对象 | 结论 | 要点 |
|---|---|---|---|
| 1 | 提交 `e66eb6ff` | REJECT | 选片/分桶/坐标数学 OK；**P1 航向朝上帧外增量不进 R**；P2 开帧顺序、标签计数/泄漏、cos 精度 |
| 2 | 工作树（WP-12/13） | REJECT | 被限频跳过的增量无重试；nav 盖章只作废隐藏 C；`[mapfps]` 丢样本 |
| 3 | 工作树（+pending） | REJECT | 抓到**航向饥饿回归**（service 用旧角刷新窗口）；track 全量重绘绕过记账 |
| 4 | 工作树（want 合并 + track 统一入口） | **ACCEPT** | 仅剩 P2：旋转失败会丢角请求 |
| 5 | 单点（成功后提交 `rot10`） | **ACCEPT** | 失败保留 want、下一拍重试；其余调用点副作用不变 |

- 审查期间工作树被本次修改推进过，因此每轮都重新取哈希；第 5 轮的 ACCEPT
  覆盖最终工作树。

## 未做 / 需上板

- 设备 A/B（串口在用户手上）：`[vrot] bakes n/avg`、`[vperf] frame` 的
  `cyc_tile/cyc_nav` 段和、`ps` 的 `bicycle_ui`/`IDLE`；两侧日志级别必须相同。
- 默认（正北）模式没有与 51.5% 同口径的基线，需补一条。
- 直道重烘是"角度主导还是余量主导"未实测 ⇒ guard 收益需 `[vrot]` 验证。
- 平移量化、AHEAD 64→32、条带旋转三项按上表保留。

## 2026-10-28 05:00 定时巡检（无新证据）

- **配额**：五轮 Codex 全部正常退出（第 4/5 轮 ACCEPT）；日志里出现的
  "usage limit" 字样是 brief 里引用的技能原文，**不是**真实告警。
  尾 40 行无配额错误、无中断。
- **设备**：`/home/jinsc/SDK/vela/zcode` 采集目录**整个不存在**
  （没有 `console.log`、没有 `state/last_build.txt`）⇒ 无法判断是否烧录过；
  本轮没有任何设备日志可分析。若需要恢复采集，得先重建该目录/采集链路。
- **代码**：`vendor/HelmOne` 工作树停在 2026-10-28 03:00（源码）/03:03（文档），
  无新改动、无新提交。
- **产出**：按计划写 `docs/agent/evidence/WP-15/R1/DESIGN.md` —— 下一项优化
  "航向朝上子矩形旋转（只转脏区）"的设计与三道验证方案，**未改固件源码**。
  要点：HAL 输出层裁剪语义已从镜像里实际编译的
  `vendor/HelmOne/chips/drivers/hal/bf0_hal_epic.c` 核对（`offset` 公式
  `:1877`、反投影 `:794-968`），只缩 `layers[0]` 的 x/y/width/height、
  保持 data/stride 与源层参数不变即可保持采样相位；最大未知量是
  `lv_epic_blend` 内部按整层 flush 可能吃掉收益，必须先实测。
- 待用户：烧录 + A/B；平移量化与 AHEAD 64→32 仍待拍板。

---

# 2026-10-28 第三轮（长任务）：航向朝上子矩形旋转 + 导航扫描窗口

档位：长任务；验证范围：冒烟（本轮）+ 定向回归（收尾，板卡 A）；收尾 Codex 审查：使用。

## 基线

`vendor/HelmOne` @ `a32f3168`（retire-eval），工作树干净；`ninja -C cmake_out/helmone_nsh`
本轮起手 exit 0（无改动可编）。

## 为什么做这两项（证据链）

- WP-11 §2.2/§4：航向朝上重烘 **138 ms/次、0.8~1.2 次/s ⇒ 约 11~17 pp**，是单笔最大
  CPU 项；WP-15 的成本模型（0.551 Mpx/s，线性于目的像素）预测典型脏区 2~9 ms。
- WP-11 §4 第 6 项：`VMAP_NAV_MATCH_AHEAD_SEGS=64`，17 ms × 2/s = 3.4 pp；减半约 −1.6 pp。
- 两项都**不改变正北模式行为**（不引入正北回归面）；产品默认仍是正北。

## WP-16 子矩形旋转（航向朝上）

### 16a 板级原语（主对话）

- `services/myvendor_epic_blit.c`：新增
  `myvendor_epic_rotate_area(src…, dst…, angle10, pivot_x, pivot_y, dst_cx, dst_cy,
   clip_x, clip_y, clip_w, clip_h)`。
- 语义：**clip 内的像素与 `myvendor_epic_rotate()` 逐位一致**；clip 外不动。
  clip 空 ⇒ 返回 true（无事可做）；参数/硬件失败 ⇒ false（调用方回退整幅）。
- 做法（已核对 `chips/drivers/hal/bf0_hal_epic.c`）：
  - 输出层（layers[0]）只改 `x_offset/y_offset/width/height` = clip，`data` 仍为**缓冲基址**、
    `total_width` 不变 ⇒ HAL 的 `AreaFromLayer(out) + AreaMove(-input->x_offset,-input->y_offset)`
    反投影出的源矩形随之收窄，采样相位不变（`bf0_hal_epic.c:790-800, 900-968`）。
  - `data_size` 只被 ezip 分支与适配层 flush 使用（`bf0_hal_epic.c:3254`、
    `lv_sifli_epic_cfg.c:614-646`）⇒ 本路径把它设成 1，**自己做**精确 cache 维护：
    源按反投影 bbox(+2 px) 的**行区间** clean（`(y1-y0+1)*stride`），目的按 clip 行 invalidate。
  - 探针：`[vrot] area px= clip= ms=`（LOG_DEBUG）。

### 16b vmap_view 记账（子 Agent）

- `struct vmap_view` 增加 `rot_dirty`（画布坐标 AABB，空 = x0>x1）。
- `vmap_view_invalidate_canvas_area()` heading-up 分支：并入 `rot_dirty`，仍置
  `rot_pending` 并调 service（不再只置 pending）。
- `vmap_rot_service()`：有角度待应用 ⇒ 整幅（现状）；**只有** `rot_dirty` ⇒ 投到 R
  坐标调 16a；成功清 `rot_dirty`，**不刷新 `last_bake_ms`**（否则拖住角度重烘）。
- `vmap_rot_full()` 成功清 `rot_dirty`（整幅已覆盖）。
- 屏上只用 `lv_obj_invalidate_area` 作废 clip 对应的那一块（含 y_bias/对象位置换算）。

### 16c 验证（子 Agent + 主对话）

- 宿主寻址差分：独立 CPU 参考实现，"先整幅再裁" vs "只算子矩形"，1e4 组随机逐像素相同。
- 开机自检：坐标编码图案，整幅裁剪 vs 子矩形比对；不过 ⇒ **运行时整幅回退**。

## WP-17 导航扫描窗口（子 Agent）

- `vmap_route_nav.c:16-17` 的 AHEAD 64 → 32，条件是**等价性有证据**：用真实轨迹
  （`mkfs/lfs/mtp/record/TRK_2026090*.gpx` + 板内 20260529 轨迹）逐点回放，比较
  32/64 窗口选出的段号。任何一点不同 ⇒ 不改，只落报告。

## WP-18 探针清理（主对话）

- 删 `lv_draw_sifli_epic_img.c` 的 `[epicimg]`（WP-6 明确"提交前删"；LVGL 已不再旋转
  画布，该探针只剩噪声）。`[vrot] bakes` 保留（A/B 要用），新增 `[vrot] area`。

## WP-19 收口（主对话）

- 编译 + 镜像符号核对；写 A/B 口径；Codex 只读审查；更新本文件。

## 结果（2026-10-28 19:50）

### WP-16a 板级原语 — DONE（主对话）

- 新增 `myvendor_epic_rotate_area()` + `epic_rotate_area_selftest()`。
- **修正了 WP-15 设计稿的一处错误**：输出层的 `data` 必须指向 clip 左上角像素
  （`bf0_hal_epic.h:297-300` 的层约定），只缩 x/y/width/height 会把像素写到缓冲左上角。
- cache：子矩形路径按反投影的**源行区间** clean（±2 px、两种 sin 符号取并集），
  用 `data_size = 1` 掐掉适配层的整层 flush；整幅路径保持原样（`data_size = 0`）。
- 开机自检：整幅留底 → 子矩形覆盖 → clip 内逐像素相同、clip 外一个像素不动；
  失败则原语永久返回 false（上层回退整幅）。
- 证据：`evidence/WP-16/R1/IMPLEMENTATION-A.md`。

### WP-16c 源行区间穷举 — PASS（主对话补做；原子 Agent 因网络失败无产出）

- `docs/map/perf/verify/area_diff.c`，1e4 组随机（含整数角/近 0°/近 90°/贴边/越界/1 px clip），
  6,948,739 个采样像素。
- 外扩 0 px **FAIL**（最大越界 1 px，正好是 HAL 自己的 AABB ±1）；1/2/3 px 均 PASS。
  ⇒ 实现取 2 px 有一行余量。
- 证据：`evidence/WP-16/R1/AREA-DIFF.md`。

### WP-16b vmap 脏区记账 — DONE（子 Agent）

- `rot_dirty` AABB + `rot_full_pending`；service 分流：角度/整幅待补 → 整幅（原限频），
  否则 `vmap_rot_area()`（用 **r_pan/r_rot10** 的烘焙时基准投影，AABB+2 px，clamp 到 R）。
- 成功后清脏区、不刷 `last_bake_ms`、只 `lv_obj_invalidate_area` 那一块；
  原语失败 → 回退整幅。
- 主对话补：**脏区 > R 面积 3/5 时直接走整幅**（整幅顺带刷新正立路名与限频窗口，
  且像素数相当）；同 tag 的两条 `[vrot] area` 探针合并成一条（保留 vmap 侧，带 px）。
- 证据：`evidence/WP-16/R1/IMPLEMENTATION-B.md`。

### WP-17 导航扫描窗口 — **不改**（子 Agent）

- 用真实固件源码编两版（AHEAD 64/32）回放真实 GPX：6 个真实场景 6948 点全一致。
- 但压力样本给出 2 个反例：3 m 顶点路线 + 单拍 143 m 跳变 ⇒ 32 段选段偏 47.7 m；
  300 m 缺口 ⇒ 滞后 103 m 且越过 2×50 m 重规划阈值。**窗口 32 段在常见顶点间距下
  比代码自己的 200 m 容差还窄**，判不等价 ⇒ 保持 64。
- 证据：`evidence/WP-17/R1/REPORT.md`（含复现命令与 `DIFF_POINTS.csv`）。

### WP-18 探针清理 — DONE

- 删掉 `lv_draw_sifli_epic_img.c` 里的 `[epicimg]` 探针与 `syslog.h` 引用
  （LVGL 已不再旋转画布，该探针只剩噪声；WP-6 本来要求"提交前删"）。

### 编译（2026-10-28 19:46）

- `ninja -C cmake_out/helmone_nsh` exit 0；flash **3,169,008 B（75.56%）**、
  sram **473,700 B（90.35%）**（相对改前 3,166,640 / 472,932：+2,368 / +768 B，
  +768 是子矩形自检的 20×20 快照）。
- 镜像核对：`nm` 有 `myvendor_epic_rotate_area`；`strings` 有
  `[vrot] area n=%u … px=`、`[vrot] area selftest %s: in=%d out=%d …`；
  `[epicimg]` 已消失。
- 无新增警告（`vmap_view.c:1923` 的 `board_psram_canvas_arena` 隐式声明是改动前既有）。

## 上板 A/B（待用户执行；串口在用户手上）

固件：`cmake_out/helmone_nsh/nuttx.bin`（2026-10-28 20:1x，含 WP-16 + WP-18）。
烧录：`build flash monitor -p /dev/ttyACM0`。

```text
# 1) 打开航向朝上（设置 → 骑行 → 地图朝上，或）
bicycle_nsh rot on
# 2) 打开 DEBUG 探针（[vrot]/[mapfps] 都是 LOG_DEBUG）
ctl log all
# 3) 造负载（顺序不能反，否则导航规划会失败）
bicycle_nsh gnss sim /mnt/lfs/mtp/import/20260529户外骑行.gpx fwd 30
# 等十几秒
bicycle_nsh nav 118.3762928 32.2458731
```

先看**三条准入判据**（开机/进地图后 ~10 s 内）：

1. `[vrot] area selftest PASS: in=0 out=0 (clip 14,14 20x20)`
   —— 不过 ⇒ 子矩形永久关闭（自动回退整幅），本次优化的收益不会出现；
2. `[vrot] selftest PASS: 10 probes, 0 fail (area=on)` —— `area=on` 才说明原语被启用；
3. `[vperf] frame` / `[mapfps]` 正常出数。

60 s 记录（直道 + 弯道各一段），只看四个量：

| 口径 | 期望 |
|---|---|
| `[vrot] area n=… avg=…ms max=…ms px=…` | `n>0`，avg ≤15 ms（典型 2~9 ms），max ≪138 ms |
| `[vrot] bakes n=… avg=…ms` | 角度整幅；弯道 0.8~1.2 次/s、avg 130~150 ms（与 WP-14 同量级） |
| `ps` 的 `bicycle_ui` / `CPU0 IDLE` | 相对 WP-14 固件：ui 下降、IDLE 上升（同场景、同日志级别） |
| 目视 | 轨迹尾/导航灰线更新处无 1 px 接缝、无残影、朝向不滞后 |

回退手段（按代价从小到大）：

1. `bicycle_nsh rot off` —— 关掉航向朝上，完全绕开这条路径（正北模式一行未改）；
2. 若只想禁掉子矩形：让开机自检失败即可（不改代码也能用 `VMAP_ROT_BAKE_MIN_MS` 调回
   整幅节奏 —— 但注意子矩形路径不受该限频约束，真正要关得改代码）。

## 未做 / 待用户

- **设备 A/B 未跑**（串口归用户）：`[vrot] area selftest` 是否 PASS、`[vrot] area` 均值、
  `bicycle_ui` 降幅、目视接缝 —— 全部 NOT_RUN。
- `VMAP_NAV_MATCH_AHEAD_SEGS` 保持 64（WP-17 判不等价）。
- 平移量化 1→2 px、`VMAP_ROT_MARGIN`/条带旋转：仍按 WP-11 的结论不动。
- `myvendor_epic_rotate_selftest()` 的返回值现在同时包含子矩形结论（`area=on/off`）；
  唯一调用点仍 `(void)` 忽略，无行为变化。

## Codex 独立审查（收尾，使用）

模型 `gpt-6.1-sol`、`model_reasoning_effort=high`、只读沙箱；共 7 轮：

| 轮 | 结论 | 关键项 |
|---|---|---|
| 1 | REJECT | A1 端点溢出 / A5 EPIC 超时假成功 / A4 自检门禁 / B7 回退绕过限频 / B11 帧末整幅失败丢恢复 |
| 2 | REJECT | A2 我的措辞错误（已改文档）/ A4 返回值 / B6 回退整幅失败未升级 / 新2 `sy0` 初始化溢出 |
| 3 | REJECT | 跟车整幅失败未记录恢复 / 自检日志时机 / A3 Q12 是尺度误差 |
| 4 | REJECT | service 角度整幅失败仍需留恢复 / A3 需真实系数模型 |
| 5 | REJECT | A3：对带符号系数 floor ≠ HAL 取幅值右移（225°/90° 反例） |
| 6 | REJECT | A3：零角度时 HAL 关旋转（恒等映射）未建模 |
| 7 | **ACCEPT** | 零角度旁路已建模；新发现：无 |

- 最终源码哈希与消息全文、原始日志：`evidence/WP-16/R1/codex-review/`（见 `REVIEW.md`）。
- 审查期间每轮都重取哈希，第 7 轮 ACCEPT 覆盖当前工作树。
- 评审中发现的 8 处缺陷**全部改在固件里**（不是只改文档）：溢出、超时判定、自检门禁、
  两处"整幅失败后局部成功清账"的混合基准、回退限频、日志时机、`sy0` 初始化。

## 第三轮收尾（2026-10-28 21:05）：**WP-16 已上板验证并回退**

上板（串口 + 摄像头由用户开放）：固件 `202610282031` → `202610282053`（加原因探针）
→ 现在的 `202610282102`（回退后）。

### 结论：子矩形旋转在这颗板子上不成立

| 负载 | `[vrot] area` 命中 |
|---|---|
| `demo start linear`（120 km/h 直线，60 s） | **0** |
| `gnss sim TRK_20260831 fwd 45`（60 s） | **0** |
| `gnss sim TRK_20260831 fwd 8`（40 s） | **0** |
| `nav` + GPX 回放 | **0** |

整幅重烘原因（`[vrot] why`，demo 50 s）：`T`=17（跟车余量用完/角度）、`E`=6（帧末）、
`A`=1；`F`/`N`=0。服务侧（`[vrot] svc`，2 s）：`svc≈10`、`blk=0~3`、`area=0`、`rej=0`、
`inv=0~2`（`inv` 是 rebase 的瓦片边条，434 px 见方，四块一合并就是整画布）。

**根因**：WP-15 的成本模型（0.551 Mpx/s、按输出像素线性）没错，错在前提
"帧外增量是小区块"。实测帧外失效要么是全屏（rebase 边条、`stamp_overlays` 重描
整条路线+轨迹），要么根本不存在（不录轨迹时地图只在帧内变）；而 120 km/h 下
R 的 24 px 余量 1.6 s 就用完（`T`），这一项与脏区无关。

### 处置

- **回退** `vmap_view.c` / `myvendor_epic_blit.{c,h}` 的子矩形原语与脏区记账、
  以及 `fake/` 与 `stubs_host.c` 的对应桩（`git status` 里已看不到这些改动）。
- **保留**两处独立成果：
  1. 整幅旋转改走 `epic_result()` —— HAL 超时（只加计数仍返回 HAL_OK）之前会被当成功，
     R 可能停在半张旧图；这是真缺陷，与子矩形方案无关。
  2. 删 `[epicimg]` 探针（WP-6 要求）。
- **保留**宿主穷举 `docs/map/perf/verify/area_diff.c`（HAL Q15 查表+插值+幅值截断
  +零角度旁路的逐像素核对；结论：外扩 0 px 差 1 行、2 px 足够）。以后做任何 clip 化
  EPIC 原语可直接复用。
- 完整记录（含"若要重做的前置条件"）：`docs/map/perf/ROT_AREA.md`。
- 板上设置已恢复：`bicycle_nsh rot off`（`[map] heading-up off`，产品默认）。

### 回退后编译 / 上板

- `ninja -C cmake_out/helmone_nsh` exit 0；flash **3,166,392 B（75.49%）**、
  sram **472,900 B（90.20%）**（HEAD 3,166,640 / 472,932：−248 B 是删掉的探针）。
- 固件 `202610282102` 已烧录并启动：`[vblit] probe done`、`[map] heading-up off`、
  `[vrot] bakes` 正常，无断言/无 wdt。
- 旁证（与本轮无关，观测到的既有现象）：老固件 `202610280112` 在 `gnss sim` 起步时
  出现过一次 `[wdt] stop feed idle=2112055ms … ui=11260ms` 重启；同场多次
  `SD: CMD17 … FAILED (why=noerr_but_cmd_busy)` + re-identify 循环。新固件这次
  上电未见 `stop feed`。

## 第三轮最终状态（2026-10-28 21:50）

### 当前固件 / 板上

- 镜像：`cmake_out/helmone_nsh/nuttx.bin`，**build_date 202610282140**，已烧录并启动。
- flash **3,166,464 B（75.49%）**、sram **472,900 B（90.20%）**
  （HEAD 3,166,640 / 472,932 ⇒ 净 −176 / −32 B：删掉 `[epicimg]` 探针，加了几个字节的失败记账）。
- 启动自检：`[vrot] selftest PASS: 10 probes, 0 fail`、`[vblit] probe done`、无断言。
- 板上设置恢复正常：`[map] heading-up off`（产品默认）。
- 摄像头快照（21:46）：主界面、时钟/状态/地图卡正常刷新（`[mapfps] avg=2~3 ms、fps≈11`）；
  地图卡是"无定位/无瓦片"的空白态（室内无 GNSS、演示已停），不是渲染故障。

### 工作树里留下的改动（3 个固件文件）

1. `services/myvendor_epic_blit.c`：整幅旋转改走 `epic_result()`（HAL 超时不再被当成功）。
2. `ui/bicycle/src/vmap/vmap_view.c`：失败记账收口到 `vmap_rot_full()`
   （`rot_pending` + `rot_fail_valid/last_fail_ms` 重试限频 + `rot_broken` 冻结跟车平移），
   注释写明"单缓冲无法回滚、重试成功前可能有混合帧"这条边界。
3. `apps/graphics/lvgl/.../lv_draw_sifli_epic_img.c`：删 `[epicimg]` 探针。
（`include/myvendor_epic_blit.h` 同步了 `false` 的两种含义。）

### Codex 审查

- WP-16（子矩形旋转）本体：7 轮，第 7 轮 **ACCEPT**（记录在 `evidence/WP-16/R1/codex-review/`）。
- 回退后的最终小 diff：第 1 轮 REJECT（2×P2）、第 2 轮 REJECT（重试限频 + 显示有效性）、
  第 3 轮 REJECT（调用点覆盖 + 显示有效性）、第 4 轮 REJECT（tick=0 哨兵 + 冻结平移），
  上述**全部已改**；第 5 轮**配额耗尽（INCOMPLETE）**：
  `ERROR: You've hit your usage limit … try again at 11:25 PM`。
  ⇒ **最终 delta 还没拿到独立结论**，配额恢复后需续审（`codex exec resume <session-id>`）。
- 未跑（NOT_RUN）：EPIC 超时注入（无故障注入手段）、设备侧超时路径的观感验证。

### 旁证 / 待用户判断（与本轮改动无关）

- 老固件 `202610280112` 上，`bicycle_nsh gnss sim …` 起步瞬后出现
  `[wdt] stop feed idle=2112055ms work=2043518ms ui=11260ms` 并重启；同场多次
  `SD: CMD17 … FAILED (why=noerr_but_cmd_busy)` + re-identify 循环（每 ~8 s）。
  新固件（2026102821xx）本轮上电与跑 demo/sim 期间**未见** `stop feed`。
  这条属于既有 SD 卡顿/看门狗族，需要单独一轮按 `diag` 落盘取证。

## 2026-10-28 22:15：合并到 master + 上板验证

- `master` 原本恰好是 `retire-eval` 的祖先 ⇒ `git merge --ff-only retire-eval`：
  **86 个提交快进**，无冲突、无合并提交；两分支现在同指 `43ab6054`（已推 origin）。
- 从合并后的 master 全量重编（checkout 触发了 2933 个目标重编）
  **需要注意**：`romfs_etc.c` 生成要用 `genromfs`，它不在默认 PATH ——
  需要 `export PATH=$PWD/prebuilts/build-tools/linux-x86_64/bin:$PATH`
  （本次第一次构建就卡在这里，报 `genromfs: not found`）。
- 编译 exit 0；flash **3,165,048 B（75.46%）**、sram **472,900 B（90.20%）**。
- 上板（`build_date 202610282209`）：
  - `[vrot] selftest PASS: 10 probes, 0 fail`、`[vblit] probe done`、无断言/无 wdt；
  - 主界面正常：地图有瓦片、路名（龙兴路/滁州大道）、状态栏、`开始骑行`；
  - `demo start linear` 30 s：`[mapfps]` fps 4.7~7.2、avg 11~17 ms，无崩溃；
  - `rot on` 22 s：`[vrot] bakes` 140~141 ms/次、`[mapfps]` avg 12~18 ms，无崩溃；
    测完已恢复 `rot off`。
- 观察到的既有错误族（与本轮改动无关，未处理）：
  - `SD: CMD17 … FAILED (why=noerr_but_cmd_busy)` → `re-identify FAILED ret=-5
    dead_run=1` → `DMA read fallback to PIO`（开机 21 s 处，卡片又哑了一次）；
  - `gnss_uart: ERROR rx ring overflow, dropped 25774 bytes`（95 s 处，解析可能失步）；
  - 开机 3.26 s 一次 `[lcd_flush] putarea_async failed -5`（随后显示正常）；
  - BLE `No valid legacy adv` / `Failed LE Set PHY (-5)`（老面孔）。

## 2026-10-28 22:45：R 余量（VMAP_ROT_MARGIN 24→40）上板 A/B —— **否掉，保持 24**

假设：单次整幅重烘 ∝ R 面积、触发频率 ∝ 1/可用余量 ⇒ 加大余量能摊薄每秒成本
（模型 m=24→40 直道 −28%）。上板实测**不成立**。

同一台板、同一段 `demo start linear`（120 km/h 直线），各自烧录后跑 60 s+，
只统计 `demo: mode linear` 之后的窗口：

| 版本 | 整幅重烘次数 | 频率 | 单次 | 每秒成本 |
|---|---:|---:|---:|---:|
| m=24（R 288×264） | 27 次 / 139 s | 0.19 /s | **136 ms** | **26 ms/s** |
| m=40（R 320×296） | 23 次 / 104 s | 0.22 /s | **173 ms** | **38 ms/s（+46%）** |

- 单次成本 +27%，与面积比 94720/76032 = 1.246 完全吻合 ⇒ 参数确实生效、模型对；
- **但频率没降**（0.19→0.22/s，按 lim 22→38 应该掉到 0.11/s）⇒ 这一档负载里整幅
  重烘不是"跟车余量"驱动的，而是**帧末**（画布 rebase 一帧一次）与角度。
  同帧 `[vperf]` 也指向别处：`cyc_tile` 209 ms、`cyc_warm` 255 ms（冷瓦片），
  旋转只占其中一部分。
- 结论：加大 R 只会让每次帧末重烘更贵 ⇒ **保持 24**，`vmap_config.h` 的注释已按
  实测改写（连同"要再压得先让帧末不必整幅/减少 rebase 帧"的方向）。

### 测量口径的两个坑（下次别再踩）

1. **日志里混着历史会话**：`monitor --follow` 的 replay 段 uptime 会回绕，直接按
   时间戳算窗口会得到负的时长；必须先 `rfind("build_date")` 切到本次启动，再用
   `demo: mode linear` 这类事件行当窗口起点。
2. **`gnss sim <gpx> fwd 45`（不带导航）在这一版上不会让地图跟车**：
   `[mapfps] avg=2 ms / pump=0`、无 `[vperf] frame`、无 rebase ⇒ 只能当"静止负载"，
   验证不了跟车相关的东西。今天唯一能让地图连续移动的负载是 `demo start linear`。

## 2026-10-28 23:30：R 余量 A/B **重做**（修正上一节的误判）

上一节（22:45）的"m=40 反而差 46%"是**测量误判**：两个窗口不等长（139 s vs 104 s）、
没有归因、且日志里混着 replay 的历史段。按下面的口径重做后结论变成"**持平**"。

### 修正后的口径

1. 先 `demo start linear`（它内部固定 120 km/h），**再用** `bicycle_nsh nav sim speed <kph>`
   改速 —— 顺序反了会被 `route_sim_start(..., 120)` 覆盖（踩过）。
2. 两段各 60 s，窗口起点用命令行回显之后的第一个时间戳定位；
   只用本次启动段（`rfind("build_date")`）。
3. 重烘原因用新增的 `[vrot] why <c>` 归因（A=角度 / N=有 pending 无脏区 / T=跟车余量或角度 /
   E=帧末 / S/L=开关切换），探针放在**限频检查之后**，否则会把被窗口挡掉的重试也记成一次。

| 版本 | @120 km/h | @45 km/h |
|---|---|---|
| m=24 | 0.31 次/s × 141 ms = **44 ms/s**（T=19, E=6） | 0.12 次/s × 141 ms = **16 ms/s**（T=5, E=3） |
| m=40 | 0.20 次/s × 175 ms = **35 ms/s**（T=12, E=6） | 0.08 次/s × 175 ms = **15 ms/s**（T=5, E=3） |

- 单次成本 141→175 ms（+24%），与面积比 1.246 吻合 ⇒ 参数确实生效、成本模型对；
- 次数确实降了（120 段 T 19→12），但每窗口只有 5~19 个事件，差异 ~1σ ⇒ **只能算持平**；
- 绝对量级：16~44 ms/s（约 1.5~4% CPU），**不是当前的大头**。

### 处置

- **保持 `VMAP_ROT_MARGIN=24`**：没有可测收益，不值得为它多付 37 KB PSRAM 与每次 +34 ms 停顿。
- `vmap_config.h` 的注释已改写（含"别引用旧的 +46%"这条）。
- **保留 `[vrot] why` 探针**（LOG_DEBUG，约 0.3 行/s）：这是第一次能按原因归因的观测点，
  后续动 rebase/角度都靠它，删了又得重写。

### 下一步（按实测排序）

1. `cyc_warm`：一帧 rebase 里冷瓦片 255 ms、`cyc_tile` 209 ms、`scr` 56 ms ⇒ 首帧/跳转的
   可见卡顿主要在这里，且正北/航向朝上都有份。预取+解码到缓存（空闲时做）是方向。
2. rebase 帧频：120 km/h 下每 8.3 s 一帧，每帧一次整幅重烘（E）。要降就动画布余量/守卫
   或让帧末不必整幅 —— 这两条都得先有 45 km/h 的稳定复现负载（现在有了）。

## 2026-10-29 00:00：收口（用户定：地图这条线结束，后续只做视觉优化）

- 性能现状与两条否证、明确不做的事、以及"UI 占用未测"这个开放项，统一写在
  **`docs/map/perf/STATUS.md`**（入口文档，别再往本文件里堆测量细节）。
- 40 km/h 基线原始数据：`docs/agent/evidence/WP-20/R1/MEASURE-40KPH.md`。
- 演示模式按用户要求改为 40 km/h（`BICYCLE_DEMO_LINEAR_KPH`）。
- 用户明确否掉"平移量化 1→2 px"（省 ~3% 但降观感），记为不做。

## 2026-10-29 01:5x：开机/进主界面的视觉过渡（WP-V1/V2/V3）

用户诉求（原话）：**"进入 main、开机资源加载完毕的状态，进入主界面，主界面的各个控件
无动画进入退出，所以生硬"**，以及"地图未出图时那一块是空白"。**视觉，不是性能。**

### WP-V1 splash 底色（`startup_page.c`，子 Agent）

- 旧代码在 `sync_boot()`/创建时**无条件写灰阶 0**（黑），bg 动画 0→255 又把首帧拉回黑；
  改为三处都取 `startup_theme_gray()`（日=白/夜=黑），bg 动画改成 STEPS→STEPS
  （360 ms 只当 settle），`theme_refresh()` 去掉 `!bg_started` 门。

### WP-V2 地图首帧占位 + 淡入（`map_page.c`，子 Agent）

- 首帧前地图卡里垫一个静态占位（SCR 底 + 「地图加载中…」），压在地图画布之下；
  两块画布创建即 `opa=TRANSP`；首帧 `render_finish()` 后一条 180 ms 一次性动画：
  画布 0→COVER、占位→0 并删除。正北 / 航向朝上同一条路径；工厂模式不建 LiveMap。
- 用户首轮拍照发现占位文字与**地图中心的箭头重叠**（读成"地图[箭头]中…"），
  已把提示上移到中心上方 38 px。

### WP-V3 主界面控件错峰入场（`helm_shell.c/.h`，子 Agent + 主对话收口）

- 三组：时钟块 `sb_clock` → 状态列 `sb_stat` → 操作条胶囊 `sb_cta`；每组 200 ms
  （opa 0→255 与 translate_y +12→0 同一条动画），组间错 80 ms，首组延迟 40 ms。
- 触发：`map_will_appear()`；主题重建末尾（重建会换控件）——两者用
  `s_theme_rebuild_pending` + `s_sb_in_visible` 保证**视觉上只播一轮**：
  待重建时页面出现这一轮保持透明，重建后播唯一一轮；兜底 500 ms 定时器落末态；
  已经亮过则重建不再补播。

### Codex 审查（`gpt-6.1-sol`，6 轮，记录在 `evidence/WP-VISUAL/R1/codex-review/`）

| 轮 | 结论 | 关键项 |
|---|---|---|
| 1 | REJECT | C1 主题重建截断入场、C4 `lv_anim_start` 失败留透明、C3 补调冗余 |
| 2 | REJECT | C1 变成"播两轮"、P3 注释过期 |
| 3 | REJECT | C1 仍会"先出现再消失再出现"（pending 分支落 COVER） |
| 4 | REJECT | C1 兜底条件写反（pending 为真时 break）、timer 失败不留末态 |
| 5 | REJECT | 兜底后重建仍补播 ⇒ 再次出现→消失→出现 |
| 6 | **ACCEPT** | 三种时序均只播一轮；菜单返回会播；无新发现 |

### 编译 / 上板 / 验证

- 编译 exit 0；flash **3,166,676 B（75.50%）**、sram 472,900 B（90.20%）。
- 上板 `build_date 202610290151`：`[vrot] selftest PASS`、`[vblit] probe done`，
  30 s 内无断言 / 无 HardFault / 无 `stop feed`。
- **目视结论仍待用户**：占位位置、三组是否一档一档出来、splash 是否变白底。
- 已知未解（与本轮无关）：app 从 2.1 s 起的整屏 flush 返回成功，但屏幕到 ~15 s 才更新
  （显示的是 2SFBL 的 "loading"）；已排除面板 on/off —— app 侧 0 次调用。

## 2026-10-29：新任务 WP-LP 低功耗优化（长任务）

用户诉求（原话）："看看怎么进行低功耗优化"。

- 四选一：**长任务**；验证范围：**快速冒烟（本轮）+ 定向回归（收尾）**；
  收尾 **使用 codex-review** 独立审查。
- 串口当前归用户（`monitor-ctl`）⇒ 需要上板的判据只写步骤，由用户执行，不标 PASS。

### 阶段

1. **Phase 1（本轮）只读调查**，三路并行：
   - `WP-LP1` PM/睡眠基础设施（NuttX PM、tickless、PMU、板级睡眠入口）
   - `WP-LP2` 运行态耗电画像（谁周期性叫醒 CPU、谁常开、屏幕刷新模型）
   - `WP-LP3` 硬件能力与测量（电源轨、面板/GNSS/BLE 省电、电流怎么量）
2. **Phase 2**：汇总成 `docs/power/PLAN.md`（P0/P1/P2 + 收益/风险/验证），据此实施。
3. **收尾**：定向回归 + codex-review。

### 主对话已核实的事实

- nsh defconfig **没有** `CONFIG_PM*`、没有 `CONFIG_SCHED_TICKLESS`。
- `up_idle` = 纯 `wfi` + DWT 计数：`chips/sf32lb52/sf32lb52_idle.c:22`。
- DVFS 最低 72 MHz；48/24 MHz 属 D1/D0，会关 XT48/DLL2（`docs/dvfs.md` §2）。
- HAL 已有 PMU 原语：`HAL_PMU_EnterHibernate` / `HAL_PMU_EnterShutdown` /
  pin wakeup / RTC wakeup（`chips/drivers/hal/bf0_hal_pmu.c`）。
- LVGL 产品节拍为 1 ms tick / 16 ms 刷新（`myvendor-ui-perf-campaign` 记忆，
  待 WP-LP2 用源码核实）。

### 证据索引

- `docs/agent/evidence/WP-LP1/R1/ANALYSIS.md`
- `docs/agent/evidence/WP-LP2/R1/ANALYSIS.md`
- `docs/agent/evidence/WP-LP3/R2/ANALYSIS.md`

### 2026-10-29 Phase 1 结果（R1）

| 工作包 | 状态 | 关键结论 |
|---|---|---|
| WP-LP1 / R1 | PASS | NuttX PM 框架在，但 `arm_pminitialize()` 缺失；tickless 代码有但与 `ARCH_TIMER` 双时基冲突；**代码在 PSRAM ⇒ deep/standby 不可用**；只剩 light sleep + hibernate；72 MHz 以下被 HAL 硬拒绝；板级零睡眠入口 |
| WP-LP2 / R1 | PASS | 非 tickless 1 ms + 200 Hz cpuload；UI 主循环 10 ms；**约 1.5k 次/s WFI 退出**；面板带 GRAM、按需写、从不 SLEEP IN；背光默认 20% 恒定、无中间变暗；静止态只关背光+GNSS；主页提示环使刷新近似 50 fps |
| WP-LP3 / R1 | **FAIL** | provider 网络中断，未落证据；按 R2 重跑 |
| WP-LP3 / R2 | PASS | 可软件控制电源脚只有 6 个（PA43 GNSS / PA10 面板 / PA01 背光 / PA11 MPI2 / PA28 ETA_EN / PA29 锁存）；**无传感器/SD 电源脚**；GNSS 出厂 1 Hz 连续跟踪（无 CFG-RATE/PMS）；**板上无电流检测**；DCDC 常 FPWM |

### 产出

- **方案入口：`docs/power/PLAN.md`**（Phase 0 测量 → Phase 1 软件减负 →
  Phase 2 显示 → Phase 3 无线 → Phase 4 PM/tickless）。
- 核心判断：**先测预算，再动架构**；light sleep 可行性实验（M3）是 PM 线的立项闸门。
- 明确不做：48/24 MHz、deep/standby、单独 tickless、hero 动画（当前隐藏不耗电）。

### 当前状态

`PARTIAL` —— 等待用户提供：①是否有外部电流表；②电芯容量；③背光/提示环/BLE 间隔/
GNSS 策略的产品决策；④是否允许临时改码做 M2/M4 对照。Phase 2 代码未动。

### 2026-10-29 范围澄清（用户）

- 用户原话："我是说运行时功耗降低"。
- ⇒ **只做运行中（亮屏/记录/导航/GNSS+BLE 开）的平均功耗**；
  light sleep / PM / tickless / hibernate 全部移出本轮（方案 §2、Phase 4 保留作背景）。
- 方案 `docs/power/PLAN.md` 新增 §0.5 运行时排序：
  背光 > 显示/渲染 > GNSS/BLE > CPU 微优化（估算仅 1~3%）。
- 新增判据 §0.6：骑行场景 `test idle 30`，idle 低 ⇒ CPU/渲染；idle 高但电流高 ⇒ 板级。
- 纠错：提示环动画"放慢时长"不降帧率（transform 属性每次都无条件 invalidate，
  `lv_obj_style.c:362-364`）；只能停或改成每 500 ms 移 1 px。

### 2026-10-29 收口判断（用户）

- 用户原话："感觉没什么好优化的，毕竟一直运行"。
- 结论：**运行时软件侧已接近地板，不立项 CPU/唤醒源微优化。**
  - 已经在做的：脏区刷新、LVGL 刷新定时器自暂停、面板 GRAM 按需写、WFI、
    DVFS 144 地板、GPX 批量写、日志 2 s 合并、GNSS 1 Hz（立项目标按 10 Hz 算）。
  - "一直运行"的恒定负载是背光、GNSS、BLE、面板自刷新、DCDC 静态电流
    —— 这些是产品/硬件口径，不是代码问题。
  - 唯一近乎零成本的实验：白天 `ctl bl 0`，验证半透半反屏是否不需要背光。
- 任务状态：`REVIEW_READY`（无代码改动）；若后续实测续航不达标，再按
  `docs/power/PLAN.md` §0.5 的顺序重开。

---

# AGENT_COORDINATION — UI 在 lvgl_sim 上完整启动（2026-10-29 起）

## 任务与模式

- 目标：把 `ui/bicycle/src` 的真实 UI 在 `/home/jinsc/SDK/vela/lvgl_sim` 上
  以 **240×320 竖屏**跑起来，再继续做 UI 开发。
  （2026-10-29 用户纠正：先前"320×240 横屏"的口径作废。）
- 模式：**长任务**（分阶段 + 执行子 Agent）；验证范围：**快速冒烟（本轮）+
  定向回归（收尾）**；收尾**使用** Codex 独立审查。
- 基线：`vendor/HelmOne` @ `752c6c0f`（master），工作树未提交：
  `docs/osm/web/*`（地图样式）、`docs/agent/*`。

## 关键事实（主对话核实）

- 屏幕 **240×320 竖屏**（`HELM_STATUS_H 24` + `HELM_PAGE_H 296` = 320），
  与板端完全一致 ⇒ 启动阶段不需要任何布局改动；`PAGE_HOR_RES=240`、`PAGE_VER_RES=320`。
- 板级 MMIO 陷阱：`vmap_view.c:69` 的 `VMAP_PERF_PROBE 1` 无 `#ifndef` 保护，
  读写 DWT `0xE000EDFC/0xE0001004` ⇒ x86 必 SIGSEGV，须用构建期副本关掉（不改 vendor）。
- 源码清单已在 `ui/bicycle/CMakeLists.txt` 里列全（约 120 个 .c）：
  lv_pm 框架 21 + 页面 30 + 字体 11 + vmap 24 + gpx 7 + lv_port_buttons。
- 依赖：`lvgl` + `board` 库；额外 include：`NUTTX_BOARD_ABS_DIR/{include,ctl,domain/gpx,services/mtp_simple}`。
- 模拟器侧：LVGL 9.5 直接用本仓库那份；PC 无 EPIC（`src/draw/sifli` 已排除），
  所以**性能数字不作数**。

## 工作包

### WP-UI1 依赖摸底（执行子 Agent `agent_84512c97…`）— 进行中

- 只读；产出启动链路、依赖三档分类、分辨率假设、最小可编译子集、可复用 PC 工具。
- 证据：`evidence/WP-UI1/R1/audit.md`

### WP-UI2 让 UI 在模拟器里链接并启动（执行子 Agent `agent_d30e1da9…`）— 进行中

- 范围：只改 `lvgl_sim/**`，板端代码一行不动。
- 主对话已实测的前置结论（交给子 Agent 直接用）：
  - 128 个 UI .c 里 **123 个已能编过**（加 compat 头 + `-Wno-error=implicit-function-declaration`）；
    只有 `bicycle_c_main.c`（NuttX 入口）、`bicycle_c_debug.c`（功能关闭）、
    `bicycle_nsh*.c`、`factory/*` 需要排除。
  - compat 层只需 4 个头（nuttx/config、clock、compiler、sched/arch/lcd）+ 1 个 prelude，
    已在 `lvgl_sim/ui_port/` 落地。
  - 合并 .o 后除 libc 外缺 465 个符号：255 个 `lv_*`（liblvgl.a 提供），
    约 110 个是 `myvendor_*`/`board_*`/`BSP_LCD_BL_*`/`companion_*` 服务桩。
  - 启动链路 = `bicycle_c_main.c` 的 main 去掉 LCD/nuttx 显示初始化，改调
    `lvgl_page_init()` + `lvgl_page_open_boot_sequence()`。
- 证据：`evidence/WP-UI2/R1/{build.log,boot.png,symbols.md,NOTES.md}`

### WP-UI2 结果 — PASS（主对话独立复验）

- 子 Agent 交付：`ui_port/` 下 compat 层 + 145 个服务桩 + `ui_boot.c`；`ui` 场景注册；
  DWT/MMIO 用构建期副本绕开（`VMAP_PERF_PROBE` 关不掉那个分支本身编不过，改为把
  `*(volatile uint32_t*)0xE00...` 换成 `sim_ui_mmio_dummy`，vendor 一行未改）。
- **主对话复验**（不只看报告）：
  - `./build.sh` exit 0；`--headless --scene ui --frames 60` → 开机 splash；
    `--frames 600` → 主界面（时钟/日期/9 颗卫星/已定位/开始骑行）。
  - `git status -- boards/ apps/` 为空 ⇒ 板端源码零改动。
- 关键坑（子 Agent 未遇到、主对话补的）：**地图空白**。
  真因 = `vmap_map_catalog.c` 的 `char map_dir[64]`，PC 上 90+ 字符的绝对路径被截断
  ⇒ 探测不到瓦片。修法 = 运行时用 `/tmp/vmap` 短路径 + 启动时 symlink 到
  `mkfs/fat/map`（设备真正烧的 7.8 GB 网格包，580k 格）；`CONFIG_VMAP_DIR_PATH` 与
  `SIM_VMAP_REAL_DIR` 都由 CMake 传入。**注意 `set(... CACHE)` 改默认值不生效，必须
  `./build.sh --clean`。**
- 现状截图：开机 splash、主界面（待机页）、地图页（**旧**配色：饱和橙路 + 亮绿地块）。

### WP-UI3 全页巡页 + 问题清单（执行子 Agent `agent_95f5e115…`）— 进行中

- 加 `--ui-tour`：逐页 `lv_pm_open_page_msg(BICYCLE_PM_ID_*)` + 每页截图 + 主题三连拍。
- 产出 `ISSUES.md`（布局/对比度/地图观感/空数据），只取证不改板端代码。

### WP-UI3 结果 — PASS（7 张截图 + 问题清单）

- 新增 `--ui-tour DIR`：逐页 `lv_pm_open_page_msg()` + 截图 + 三主题连拍；
  USB 页需临时置 MTP `PLUG_ENUM`（否则被状态机立刻 pop）；STARTUP 每项停 1.3 s，上限 60 帧。
- **高严重度 2 条**（与 HTML 侧独立得出同样结论）：
  1. classic 底图道路/地块/水域亮度几乎相同（1.00–1.52:1），只靠色相区分 ⇒ 阳光下路网糊；
  2. 主 CTA「开始骑行」纸白字 on 橙 `#FF6E12` = **2.66:1**（与既有记忆里的实测值一致）。
- 中：比例尺被待机卡盖住、outdoor 次级文字 3.06:1、USB「请勿断电」3.77:1。
- 说明项：地图无路名 = 模拟器没编矢量字体（非 UI bug）。

### 矢量字体（用户指出 + 主对话落地）

- 码表 TTF = `mkfs/fat/fonts/MiSans-Medium.subset.ttf`；主文本用的是**由它生成的 4bpp 点阵**
  （`helm_mism4_*` 生成命令里就是 `--font MiSans-Medium.ttf`），TTF 只做点阵缺字兜底。
- 设备 `CONFIG_MYVENDOR_SYSTEM_VECTOR_FONT=1` ⇒ 模拟器改为编**真实**
  `apps/graphics/lvgl/myvendor_system_font.c`（不再用 stub），字体目录指到仓库那份。
- 结果：地图路名恢复（「合肥植物园 / 植物园南路 / 社岗路」），字形与板端同源。

### WP-UI4 新样式落地（执行子 Agent `agent_229b189c…`）— 进行中

- 只允许改 `vmap_style.h/.c`、`vmap_render.c`、`vmap_label.c` 四个文件。
- 依据 `docs/map/STYLE.md`：per-class 壳色、低→高画序（壳芯成对）、path 虚线、
  标签字号/同名去重、商务/工业 z15 前不画。
- 验收：模拟器 `--ui-tour` 出图对比 + `git diff --stat` 只有这四个文件。

### WP-UI4 结果 — PASS（主对话复验：diff 只 4 个文件、构建 0 error、巡页可复现）

- 改动：`vmap_style.h`（per-class 壳色 `cr/cg/cb`、宽度改 1/10 px、可见性阈值、`lbl_halo`）、
  `vmap_style.c`（三盘按 STYLE.md §2 重填）、`vmap_render.c`（画序 cls7→1 壳芯成对、
  `sqrt(scale)` 线宽、path 弧长 5/4 虚线、land 2/3 门）、`vmap_label.c`（字号表、
  优先级 +90/+40/+0、同名一屏一个、halo 跟主题）。共 659+/364-。
- 主对话复验：`./build.sh` 0 error；`--ui-tour` 7 张图可复现；`git diff --stat` 只有这四个文件。
- **偏差（子 Agent 已写明，主对话接受）**：
  1. `outdoor` 填的是**夜间盘**不是 terrain —— 板端 `bicycle_pm_theme.c` 把 outdoor+night
     都映射到 `VMAP_STYLE_OUTDOOR`，塞浅色 terrain 会让"夜间"变浅色地图。terrain 盘要另开工单。
  2. 整数线宽在默认 scale 1.69 下把 7 级压成 5/5/4/3/3/2/2（motorway=primary 同宽）——
     ribbon API 只收整数宽；**等级改由颜色/壳区分**，宽度只保证不崩。
  3. 字号只能 12/14/16（字体缓存 6 槽、UI 已占 4）；水名斜体拿不到。
  4. 路名不做任意角旋转（`lv_draw_label` rotation 是逐字形），保留横排/逐字竖排。
  5. 水面岸线未画（结构无字段）。
  6. path 虚线随线宽倍率放大（默认 6/5），STYLE.md §3 的 5/4 是 1× 值。
- **待观察**：新盘在 1× 下偏淡（路芯 `#FEFDF9` vs 底 `#F4F1E8` ≈1.05:1，靠壳 `#B7B0A1`
  的 ~1.9:1 边缘撑住）。这是"好看"与"阳光下可读"的取舍点，**由用户在板上定**。

### WP-UI4 收尾：Codex 独立审查 REJECT → 三条已修（主对话执行）

审查（`gpt-6.1-sol`，session `01a1888a…`，报告在 `evidence/WP-UI4/R1/REVIEW.md`）：
总判定 **REJECT**，三条具体缺陷，主对话逐条修复并复验：

| # | 位置 | 问题 | 修法 |
|---|---|---|---|
| 1 | `vmap_render.c:1921` | `ws10 > 170` 应为 `17`（单位 1/10）⇒ `min(1.7,ws)` 永不生效，ws=2.4 时壳宽多 ~1.3px | 改 `17` |
| 2 | `vmap_label.c:1172` | 同名只留最近锚点并**删掉其余**，该锚点撞地名/出界 ⇒ 整条路本轮无名 | 去掉"删同名"，改成按等级排序；放置成功后才记账，用 `label_name_selected()` 判重，后面的同名锚点可顶上 |
| 3 | `vmap_label.c:158` | 用画布尺寸判"已出过"，画布带 margin ⇒ 屏外 retained 压制屏内同名 | 改按**视口** `vis_x0..vis_y1` 判，两处调用同步 |

- ACCEPT 项：画序改动未破坏道路索引路径（绑定/回退像素等价）。
- WARN 保留：性能遍历 14N→2KN（默认持平，板端耗时 NOT_RUN）；outdoor 未用 terrain
  盘、`road_case_*` 成死字段。
- 修复后：`./build.sh` 0 error；`--ui-tour` 7 张图正常；diff 仍只有那 4 个文件（687+/367-）。
- **NOT_RUN**：整机构建（`cmake_out/helmone_nsh`）、烧录、板端帧耗时。

### WP-RD1 道路描边几何重写（用户：地图道路特别丑、不干净）— 进行中

- 主对话定位（6× 放大 + 读码）：
  1. `draw_road_polyline()` 在丝带后**逐顶点**补 `fill_disc(pts[i], (w+1)/2)`：
     半径比半宽大 0.5px ⇒ 每个折点鼓一圈；且圆盘与丝带是两次填充，AA 边缘重叠
     ⇒ 接缝二次混合，显脏。
  2. `draw_feature_line()`（水系等）走 `vmap_render_draw_thick_line()`，该函数每段
     两端也画 `(w+1)/2` 圆盘 ⇒ 逐段珠子。
- 修法（已派子 Agent）：丝带改成**单多边形**（左偏 → 端半圆帽 → 右偏倒序 → 起端半圆帽），
  被夹住的 miter（`1/dot > 2.5`）外侧补一段圆弧并进同一多边形；删掉逐顶点圆盘；
  水系改走同一套；虚线每个 dash 一次丝带。
- 验收：同裁切 6× 放大前后对比，折点无圆斑、拐角无缺口；颜色/线宽不动。

### WP-RD1 几何重写 — PASS（主对话复验）

- 单多边形丝带（左偏 → 端半圆帽 → 右偏倒序 → 起端半圆帽），被夹 miter 外侧补圆弧；
  删掉逐顶点圆盘；水系/虚线走同一套。`vmap_render.c` 754 行改动。
- 主对话复验：`./build.sh` 0 error；6×/12× 裁切对比折点圆斑消失；
  子 Agent 实测填充次数 2724 → 662（−76%）。
- 副作用修得对：旧代码靠圆盘遮盖"内侧 miter 夹短切出的洞"，改真实交点后 holes=0。

### WP-RD2 支路去壳 — PASS（主对话执行）

- 像素列实测证明：支路芯 2~3px + 每边 1px 半透明壳 = "白线上的米色影子"，
  2× 放大就是用户说的"脏"。
- 处置：壳只在 `core_w > 3`（干道）时画；支路单线。
- 否掉：壳加厚到 2.6×ws（更差，米色带白线）；关 AA（差别很小）。
- 证据：`evidence/WP-RD2/R1/{NOTES.md,after.png,with_casing_zoom.png,no_casing_zoom.png}`
- **NOT_RUN**：本包未跑 Codex（只改一个早退条件 + 注释）；整机构建/上板未验。

### WP-DIALGRID R1 · P02 剖面条按距离分格（用户 2026-11-04）

- 用户："P02，表盘页海拔折线图，最好做成一个一个竖向的线隔开，每段代表一个距离。"
- 口径：**每格 = 固定里程**，间隔从 100 m / 200 m / 250 m / 500 m / 1 km /
  2 km / 5 km 里挑"格数 ≤ 6"的最小值 ⇒ 导航态 6 km 窗口 = **1 km/格（6 格）**，
  记录态按最近 10 分钟骑过的里程换档（2.95 km → 500 m/格）。窗口 <200 m 不分格。
- 画法（三轮用户口径后的终态）：**2px** `MUTE` @ `OPA_50` 竖线，**只从基线升到
  折线**（每根高度随该列海拔变，不伸出折线之上），层级在填充之上、2px 脊线之下；
  x 用整数比例一次算出（不逐格累加），两端不画。左下角「X/格」（与右侧
  「还剩 X km / 近 N 分钟」同行，左对齐 24）。
- 位置标记：**老剖面页同款倒三角**（`HELM_COLOR_CLIMB_MARK` 橙红，尖端朝下贴
  折线、整个三角在折线之上 ⇒ 随海拔上下走），这里缩到 9×6；**没有竖线、没有
  遮盖**（v1 1px 线 → v2 2px 全高线 → v3 左侧灰块遮盖 → 终态，前三版均被否）。
  骑过段（三角左侧）**填充退灰 + 脊线退灰**，右侧按坡度绿/黄/红。
- **连带修的真 bug**：记录态步长原来用 `session_distance_m/(wn-1)`，缓冲只有
  10 分钟 ⇒ 骑满后每点距离越算越大，坡度分区失真（分格若沿用会写出"5 km/格而
  窗口只有 2.5 km"）。新增平行环 `s_rec_dist[]`（采样瞬间里程，240 B），窗口里程
  = `dist[n-1] − dist[0]`。
- 验证：模拟器新剧本 `SIM_UI_TOUR_SCRIPT=dialgrid`（导航态真实 GPX，游标用
  `helm_shell_demo_nav_cursor(95)` 钉位 —— 模拟器里沿路线位置推不动：GPX 回放按
  真实时间步进，实测 25 s 只走 6.9 m；记录态用 `helm_shell_demo_elev_seed(60, 50)`
  灌满 60 点）⇒ 「1 km/格」/「500 m/格」各 5 根竖线 + 倒三角；板端
  `ninja -C cmake_out/helmone_nsh` 成功，flash 72.74% / sram 92.08%。
- 证据：`evidence/WP-DIALGRID/R1/{NOTES.md,shots/*.png}`；docs/ui 换 P02 两张 +
  新增 `P02_骑行_表盘页_记录态.png`。
- **NOT_RUN**：板端定向回归（未烧录，需用户单独确认）；未跑 Codex。
