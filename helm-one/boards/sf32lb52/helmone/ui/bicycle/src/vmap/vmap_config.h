/**
 * @file vmap_config.h
 * @brief vmap config 模块。
 */

#ifndef VMAP_CONFIG_H
#define VMAP_CONFIG_H

#ifndef PAGE_HOR_RES
#define PAGE_HOR_RES 240
#endif
#ifndef PAGE_VER_RES
#define PAGE_VER_RES 320
#endif

#ifndef CONFIG_RESOURCE_DIR_PATH
#define CONFIG_RESOURCE_DIR_PATH "/mnt/lfs"
#endif

#ifndef CONFIG_VMAP_DIR_PATH
#define VMAP_TILE_DIR "/mnt/fat/map"
#else
#define VMAP_TILE_DIR CONFIG_VMAP_DIR_PATH
#endif

#ifndef CONFIG_MYVENDOR_VMAP_CANVAS_MARGIN
#define VMAP_CANVAS_MARGIN 192
#else
#define VMAP_CANVAS_MARGIN CONFIG_MYVENDOR_VMAP_CANVAS_MARGIN
#endif

#ifndef CONFIG_MYVENDOR_VMAP_RENDER_AA
#define VMAP_RENDER_AA 1
#else
#define VMAP_RENDER_AA CONFIG_MYVENDOR_VMAP_RENDER_AA
#endif

#ifndef CONFIG_MYVENDOR_VMAP_ROAD_SIMPLIFY
#define VMAP_ROAD_SIMPLIFY 1
#else
#define VMAP_ROAD_SIMPLIFY CONFIG_MYVENDOR_VMAP_ROAD_SIMPLIFY
#endif
#ifndef CONFIG_MYVENDOR_VMAP_ROAD_MIN_SEG_PX
#define VMAP_ROAD_MIN_SEG_PX 6.0f
#else
#define VMAP_ROAD_MIN_SEG_PX CONFIG_MYVENDOR_VMAP_ROAD_MIN_SEG_PX
#endif
#ifndef CONFIG_MYVENDOR_VMAP_ROAD_MAX_DEV_PX
#define VMAP_ROAD_MAX_DEV_PX 2.8f
#else
#define VMAP_ROAD_MAX_DEV_PX CONFIG_MYVENDOR_VMAP_ROAD_MAX_DEV_PX
#endif
#ifndef CONFIG_MYVENDOR_VMAP_ROAD_JOIN_DEG
#define VMAP_ROAD_JOIN_DEG 32.0f
#else
#define VMAP_ROAD_JOIN_DEG CONFIG_MYVENDOR_VMAP_ROAD_JOIN_DEG
#endif

#ifndef CONFIG_MYVENDOR_VMAP_TRACK_SIMPLIFY
#define VMAP_TRACK_SIMPLIFY 1
#else
#define VMAP_TRACK_SIMPLIFY CONFIG_MYVENDOR_VMAP_TRACK_SIMPLIFY
#endif
#ifndef CONFIG_MYVENDOR_VMAP_TRACK_MIN_SEG_PX
#define VMAP_TRACK_MIN_SEG_PX 4.0f
#else
#define VMAP_TRACK_MIN_SEG_PX CONFIG_MYVENDOR_VMAP_TRACK_MIN_SEG_PX
#endif
#ifndef CONFIG_MYVENDOR_VMAP_TRACK_MAX_DEV_PX
#define VMAP_TRACK_MAX_DEV_PX 1.8f
#else
#define VMAP_TRACK_MAX_DEV_PX CONFIG_MYVENDOR_VMAP_TRACK_MAX_DEV_PX
#endif

#ifndef CONFIG_MYVENDOR_VMAP_TRACK_RIBBON_MAX
#define VMAP_TRACK_RIBBON_MAX 384
#else
#define VMAP_TRACK_RIBBON_MAX CONFIG_MYVENDOR_VMAP_TRACK_RIBBON_MAX
#endif

#ifndef VMAP_RIBBON_CHUNK_MAX
#define VMAP_RIBBON_CHUNK_MAX 64
#endif

#define VMAP_STATUSBAR_H   24
#define VMAP_PANEL_H       64
#define VMAP_MAP_W        PAGE_HOR_RES
#define VMAP_MAP_H        (PAGE_VER_RES - VMAP_STATUSBAR_H - VMAP_PANEL_H)

#define VMAP_DEFAULT_LON   116.397455  /* 北京天安门（无定位 / 无上次骑行） */
#define VMAP_DEFAULT_LAT   39.909187
/* 全国图覆盖：上次坐标超出则当异常，开机回天安门。 */
#define VMAP_POS_LON_MIN   72.0
#define VMAP_POS_LON_MAX   136.0
#define VMAP_POS_LAT_MIN   17.0
#define VMAP_POS_LAT_MAX   54.0
#define VMAP_DEFAULT_ZOOM  14
#define VMAP_DEFAULT_SCALE 1.69

#define VMAP_ZOOM_MIN      11
#define VMAP_ZOOM_MAX      15
/** @brief 缩放 in/out 的乘性步长（比例与 NSH "in"/"out"）。 */
#define VMAP_SCALE_STEP    1.25

#define VMAP_SCRATCH_CAP   4096
/** @brief Initial VTIL read buffer. Grows up to VMAP_TILE_BYTES_MAX if needed. */
#define VMAP_STAGING_CAP   16384

/*
 * 单张瓦片道路要素索引容量（见 vmap_render.c:draw_road_layer）。
 * 实测：上海密集瓦片道路要素 p99≈1390；滁州 max=111。
 * 超出即回退逐 class 扫描（行为不变，只是慢）。
 */
#ifndef VMAP_ROAD_INDEX_CAP
#define VMAP_ROAD_INDEX_CAP 1536
#endif

#ifndef CONFIG_MYVENDOR_VMAP_VPK_HEAP_KB
#define CONFIG_MYVENDOR_VMAP_VPK_HEAP_KB 1024
#endif

#define VMAP_PAN_GUARD     12

/* ===== 航向朝上（地图跟随骑行方向旋转，箭头恒指上） =====
 *
 * 画布**仍然按正北向上渲染**（瓦片/导航线/轨迹都烘在同一块存里），旋转只发生在
 * 显示合成那一步：LVGL 把 `draw_dsc->rotation` 直接交给 EPIC 的 transform 通路
 * （`lv_draw_sifli_epic_img.c`），因此不需要中间缓冲、也不需要重光栅化。
 *
 * 代价与约束：
 *   1. 旋转后视口的四角离中心是**半对角线**（240x216 → 161 px），画布必须盖住它，
 *      否则四角露底 ⇒ 跟车 rebase 的阈值要比正北模式更早（见 set_center 里的 limit）。
 *   2. 烘在画布里的路名会跟着地图转成斜字 ⇒ 该模式下不画标签（第一版取舍）。
 *   3. 航向来自 GNSS course，低速时是噪声 ⇒ 低于 VMAP_ROT_MIN_KPH 不更新，
 *      并对角度做圆周低通；变化不足 VMAP_ROT_STEP_10 时不重排。
 */

/** @brief 低于此速度（km/h）不跟随航向，保持上一次朝向。 */
#ifndef VMAP_ROT_MIN_KPH
#define VMAP_ROT_MIN_KPH 4.0f
#endif
/** @brief 航向低通系数（每 GNSS 拍 5 Hz；0.35 ≈ 0.5 s 时间常数）。 */
#ifndef VMAP_ROT_SMOOTH
#define VMAP_ROT_SMOOTH 0.35
#endif
/** @brief 航向变化超过此值（0.1°）才重设画布旋转，避免每拍都重排。
 *
 * 实测（2026-10-27，`[mapfps]`）：EPIC 的**任意角**旋转每次重绘 ~55-91 ms，
 * 而 90° 整数倍只要 ~13 ms ⇒ 代价在源地址不连续，重绘次数就是成本。
 * 2026-10-28 用户定：步进放到 **1°**，靠下面的最小间隔控制重烘频率。 */
#ifndef VMAP_ROT_STEP_10
#define VMAP_ROT_STEP_10 10
#endif

/** @brief 两次整幅重烘之间的最小间隔（ms）。
 *
 * 每烘一次 ~138 ms（R 288×264，EPIC 任意角 ~0.55 Mpx/s）。1° 步进时若不限频，
 * 3.6°/s 的弯道就是 3~4 次/秒（≈50% UI 被占）。实际取 1200 ms ⇒ 最多 0.83 次/秒，
 * 转弯时地图比真实航向**慢半拍**（这正是"旋转慢一些"的代价），单步可能到 ~2°。
 */
#ifndef VMAP_ROT_BAKE_MIN_MS
#define VMAP_ROT_BAKE_MIN_MS 1200u
#endif

/** @brief 航向朝上时"屏幕对齐旋转缓冲"的四周余量（像素）。
 *
 * 旋转视图（R）按视口 + 本余量分配；平地跟车只**移动对象**（便宜），
 * 位移超过余量才重新烘一次整幅。
 *
 * 单次成本 ∝ R 面积（1.82 µs/px，实测标定）、触发频率 ∝ 1/可用余量，所以理论上
 * "加大余量"能摊薄每秒成本。**2026-10-28 上板 A/B 的结论是"没有可测收益"**：
 *   同负载（`demo start linear`，先起 demo 再用 `nav sim speed` 改速）、同窗口 60 s、
 *   用 `[vrot] why` 归因、各烧录一版：
 *     @120 km/h：m=24 = 0.31 次/s × 141 ms = 44 ms/s；m=40 = 0.20 次/s × 175 ms = 35 ms/s
 *     @45  km/h：m=24 = 0.12 次/s × 141 ms = 16 ms/s；m=40 = 0.08 次/s × 175 ms = 15 ms/s
 *   单次成本 +24%（与面积比 1.246 吻合，参数确实生效）；次数虽降，但每窗口只有 5~19
 *   个事件，差异在 1σ 量级 ⇒ 只能说"持平"。**收益不足以为它多付 37 KB PSRAM 和每次
 *   +34 ms 的停顿** ⇒ 保持 24。
 *   ⚠ 之前的"m=40 反而差 46%"是**测量误判**（两个窗口不等长、没做归因）—— 别引用它。
 *   要真正压这块钱，得减少重烘次数（rebase 帧/角度步进），而不是调余量。 */
#ifndef VMAP_ROT_MARGIN
#define VMAP_ROT_MARGIN 24
#endif

/** @brief 航向朝上时，R 跟车可用的余量要扣掉的安全边（像素）。
 *
 *  R 物理上能移到 ±VMAP_ROT_MARGIN 仍完整覆盖视口，但共用 `VMAP_PAN_GUARD`(12)
 *  会把一半余量锁死：45 km/h 下从 4.6 s/次 变成 2.3 s/次。这里单独留 2 px 给
 *  取整/圆角，其余全部给跟车用。 */
#ifndef VMAP_ROT_TRACK_GUARD
#define VMAP_ROT_TRACK_GUARD 2
#endif

/** @brief 航向朝上时，正立路名用多少个独立文本对象（不烘进画布）。
 *  取 12：正北烘图一屏能出 9~10 条，正立层原来只有 8 个槽 ⇒ 用户 2026-11-03
 *  "方向朝上还是少显示很多标签"。正立层自己会做 4 px 避让，多的槽位不会重叠；
 *  每条只是一个 lv_label 对象，重排走 1200 ms 的烘焙窗口，成本可控。 */
#ifndef VMAP_ROT_LABEL_MAX
#define VMAP_ROT_LABEL_MAX 12
#endif

/** @brief 每次定时器最多画/预热的瓦片数（轮转填充，避免一次占满 UI）。 */
#ifndef VMAP_RENDER_PUMP_MAX_TILES
#define VMAP_RENDER_PUMP_MAX_TILES  2
#endif
/** @brief 单次 pump 时间预算（ms）；超时则留到下次定时器。 */
#ifndef VMAP_RENDER_PUMP_BUDGET_MS
#define VMAP_RENDER_PUMP_BUDGET_MS  8u
#endif
/** @brief 平移补边时，脏区向外扩几像素，避免接缝露底。 */
#ifndef VMAP_RENDER_DIRTY_PAD
#define VMAP_RENDER_DIRTY_PAD       24
#endif
/** @brief scroll 脏条 + 被整框擦除的标签占位。必须能放下 trailing 残字。 */
#ifndef VMAP_RENDER_DIRTY_MAX
#define VMAP_RENDER_DIRTY_MAX       8
#endif
#define VMAP_GPX_PATH        CONFIG_RESOURCE_DIR_PATH "/Track/test.gpx"
#define VMAP_GNSS_UPDATE_MS  200
#define VMAP_GNSS_SIM_SPEED_KPH     15.0f
#define VMAP_GNSS_SIM_SPEED_MAX_KPH 40.0f
#define VMAP_GNSS_SIM_RAMP_SEC      120u
/** @brief NSH `nav sim`：默认沿路线速度（km/h）；可用 nav sim speed 覆盖。 */
#ifndef VMAP_NAV_SIM_SPEED_KPH
#define VMAP_NAV_SIM_SPEED_KPH      45.0f
#endif

#ifndef VMAP_TRACK_ENABLE
#define VMAP_TRACK_ENABLE 1
#endif

#ifndef VMAP_LAP_CLOSE_RADIUS_M
#define VMAP_LAP_CLOSE_RADIUS_M   50.0
#endif
#ifndef VMAP_LAP_MIN_DISTANCE_M
#define VMAP_LAP_MIN_DISTANCE_M  2000.0
#endif
#ifndef VMAP_LAP_MAX_COURSE_DIFF_DEG
#define VMAP_LAP_MAX_COURSE_DIFF_DEG 90.0
#endif

#ifndef VMAP_ROUTE_ENABLE
#define VMAP_ROUTE_ENABLE 1
#endif

#define VMAP_ROUTE_MAX_PTS 2048
#define VMAP_ROUTE_SNAP_M  150.0
#define VMAP_ROUTE_SNAP_K    8
/** @brief 路由前收集此数量候选（避免孤立最近节点）。 */
#define VMAP_ROUTE_SNAP_POOL_M 800.0
/** @brief 离路 GPS 回退吸附的硬上限（超出=地图外）。 */
#define VMAP_ROUTE_SNAP_MAX_M  4000.0
#define VMAP_ROUTE_OFF_M   35.0
/** @brief 剩余距离低于等于此值视为到达目的地。 */
#define VMAP_ROUTE_ARRIVE_M 45.0
/* 规划失败/绕不过去时，"人已经在终点附近"的判定半径（米）：目的地常常吸在
 * 100~200 m 外的路网节点上（那一带有路但节点稀/缺），于是任何重规划都"无路可走"，
 * 还会被判成逆行 —— 在这个半径内直接按到达收尾。 */
#ifndef VMAP_NAV_ARRIVE_NEAR_M
#define VMAP_NAV_ARRIVE_NEAR_M 300.0
#endif
/** @brief 到达且速度仍高于此值（km/h）时自动退出导航。 */
#define VMAP_ROUTE_END_MIN_SPEED_KPH 3.0f
#define VMAP_ROUTE_TURN_DEG 35.0f
#define VMAP_ROUTE_TURN_LOOKAHEAD_M 80.0

/** @brief A* 可采纳启发式缩放（最小自行车边权）。 */
#define VMAP_ROUTE_ASTAR_MIN_W 90u
#define VMAP_ROUTE_MAX_MANEUVERS 64

/** @brief 跨 VPK 走廊规划（分段，一次只加载一个 VGRF）。 */
#ifndef VMAP_ROUTE_CORRIDOR_MAX_REGIONS
#define VMAP_ROUTE_CORRIDOR_MAX_REGIONS  4
#endif
/** @brief 单次走廊规划的最大边界（区域过渡）数。 */
#ifndef VMAP_ROUTE_CORRIDOR_MAX_BORDERS
#define VMAP_ROUTE_CORRIDOR_MAX_BORDERS    16
#endif
/** @brief 多区域搜索时每条边界尝试的门户候选数。 */
#ifndef VMAP_ROUTE_CORRIDOR_PORTAL_TRIALS
#define VMAP_ROUTE_CORRIDOR_PORTAL_TRIALS  4
#endif
/** @brief 单段行程存储的最大区域 ID 数（全国级目录路径）。 */
#ifndef VMAP_ROUTE_REGION_PATH_MAX
#define VMAP_ROUTE_REGION_PATH_MAX       256
#endif
/** @brief 每批滚动细化物化的区域数。 */
#ifndef VMAP_ROUTE_REFINE_BATCH
#define VMAP_ROUTE_REFINE_BATCH          4
#endif
/** @brief 剩余距离低于此值（米）时触发下一次细化。 */
#ifndef VMAP_ROUTE_REFINE_REMAIN_M
#define VMAP_ROUTE_REFINE_REMAIN_M       8000.0
#endif
#ifndef VMAP_ROUTE_TRIP_MAX_WAYPOINTS
#define VMAP_ROUTE_TRIP_MAX_WAYPOINTS    32
#endif
/** @brief GPX 导航一次物化的前方窗口（米）；接近末端再加载下一段。 */
#ifndef VMAP_NAV_GPX_WINDOW_M
#define VMAP_NAV_GPX_WINDOW_M            8000.0
#endif
/** @brief 窗口剩余低于此值（米）时动态加载下一段 GPX。 */
#ifndef VMAP_NAV_GPX_RELOAD_M
#define VMAP_NAV_GPX_RELOAD_M            2500.0
#endif
/** @brief 滑动窗口向后重叠（米），便于偏航后重新吸附。 */
#ifndef VMAP_NAV_GPX_OVERLAP_M
#define VMAP_NAV_GPX_OVERLAP_M           400.0
#endif
/** @brief 定位点距 GPX 不超过此值时，从最近点开窗而不是从文件起点。 */
#ifndef VMAP_NAV_GPX_JOIN_MAX_M
#define VMAP_NAV_GPX_JOIN_MAX_M          2000.0
#endif
/** @brief 开始 GPX 时：信号较好且距最近点不超过此值才从当前位置规划接入。 */
#ifndef VMAP_NAV_GPX_START_JOIN_MAX_M
#define VMAP_NAV_GPX_START_JOIN_MAX_M    1000.0
#endif
/** 偏航重规划的最小间隔（毫秒）。
 *
 * 图几何稀疏时偏航判定会**每帧都成立**：实测骑行者离路线 junction 289 m、阈值 50 m，
 * `off-route reroute: junction off=289m >= 2x threshold 50m` 一秒刷 2-3 次，规划器被
 * 反复打断（用户看到的是"导航一直在重算、轨迹线老在变/断"）。这里给 need_reroute
 * 加最小间隔；抑制时按秒限频打一条日志，不会被静默吞掉。 */
#ifndef VMAP_NAV_REROUTE_MIN_INTERVAL_MS
#define VMAP_NAV_REROUTE_MIN_INTERVAL_MS 8000u
#endif

#ifndef VMAP_ROUTE_BORDER_PORTAL_SAMPLES
#define VMAP_ROUTE_BORDER_PORTAL_SAMPLES 7
#endif
/** 每条边界**查询**来的门户候选上限。打包数据（`tools/pack_map.py` 的
 *  `PORTAL_MAX_PER_BORDER`）每边界最多写 64 条，而且它们是按**匹配质量** `cross_m`
 *  排序的 —— 跟"顺不顺路"无关。以前这个上限只有 7（同一个 SAMPLES 常量兼任三件事），
 *  于是跨区规划只看得到匹配最紧的那几个口：2026-09-19 实测 醉翁东路 的跨界口明明在
 *  数据里（`x3596_y1196.vpk` 的 VPOR 里有 118.3359/32.2453），却因为排在第 8 名开外
 *  而被挡在候选之外，只能绕 龙蟠大道 一圈（3576 m，实际 1594 m）。 */
#ifndef VMAP_ROUTE_BORDER_PORTAL_QUERY_MAX
#define VMAP_ROUTE_BORDER_PORTAL_QUERY_MAX 64
#endif
/** 每条边界真正**试算**的门户数：从查询结果里按"起点→门户 + 门户→终点"的大圆距离和
 *  （该条路线的长度下界）预筛出前几个。每次试算 = 两个区域内各一次 Dijkstra，所以要有
 *  上限；12 已远好于原来"把 7 个匹配最好的全试一遍"。 */
#ifndef VMAP_ROUTE_PORTAL_TRIAL_MAX
#define VMAP_ROUTE_PORTAL_TRIAL_MAX 12
#endif
/** @brief 边界两侧吸附节点间的最大 haversine 间距（米）。 */
#ifndef VMAP_ROUTE_PORTAL_MATCH_M
#define VMAP_ROUTE_PORTAL_MATCH_M        120.0
#endif
/** @brief 打包门户缺失时运行时边界扫描的宽松匹配。 */
#ifndef VMAP_ROUTE_PORTAL_FALLBACK_MATCH_M
#define VMAP_ROUTE_PORTAL_FALLBACK_MATCH_M 150.0
#endif
/** @brief 扫描图边界门户时使用的裁剪边距带（pack_map）。 */
#ifndef VMAP_ROUTE_PORTAL_BORDER_M
#define VMAP_ROUTE_PORTAL_BORDER_M       600.0
#endif
/** @brief 段重叠在此距离（米）内时跳过重复缝合点。 */
#ifndef VMAP_ROUTE_STITCH_DEDUP_M
#define VMAP_ROUTE_STITCH_DEDUP_M         5.0
#endif
/** @brief 路径方位变化至少此角度时发出转向指令。 */
#define VMAP_ROUTE_MANEUVER_DEG 25.0f
/** @brief 三路及以上交叉口允许更低的角度阈值。 */
#define VMAP_ROUTE_MANEUVER_JUNCTION_DEG 12.0f
/** @brief 在此距离（米）内预告下一交叉口。 */
#define VMAP_ROUTE_MANEUVER_ANNOUNCE_M 50.0

/** @brief 导航路线叠加描边（RGB565，蓝色）。 */
#define VMAP_ROUTE_LINE_RGB565 ((uint16_t)0x13DFu)
/** @brief 领航线像素宽度（线稿 stroke-width 4，屏上再细一档）。 */
#define VMAP_ROUTE_LINE_W 3
/** @brief 折线抽稀：相邻绘制点至少相距这么多像素。 */
#define VMAP_ROUTE_DRAW_MIN_PX 4
/** @brief 重路由时与已骑路径重叠的段。 */
#define VMAP_ROUTE_RIDDEN_RGB565 ((uint16_t)0xF800u)
/** 已骑**历史**线（跨重规划保留）的颜色：灰。走过去的路线不再参与导航几何，
 *  用历史点单独画一条灰线，看起来就是"已经走过"。 */
/* 已走过那一段的颜色：深灰（红色是可见性测试用的，验证通过后换回灰）。
 * 中灰 0x8410 在这块半透半反屏上跟导航蓝太接近，所以用深灰 0x39E7。 */
#define VMAP_ROUTE_RIDDEN_GREY_RGB565 ((uint16_t)0x39E7u)
/** @brief 测试新路线与骑行历史的匹配容差（米）。 */
#define VMAP_ROUTE_RIDDEN_MATCH_M 25.0

#define VMAP_ROUTE_DEMO_DEST_LON 118.261790
#define VMAP_ROUTE_DEMO_DEST_LAT 32.256260

/** @brief 跨 VREG 演示：GPX 起点 (r004) → 西北 (r002)，约 20 km 走廊。 */
#define VMAP_ROUTE_CROSS_DEMO_FROM_LON 118.334564
#define VMAP_ROUTE_CROSS_DEMO_FROM_LAT 32.237495
#define VMAP_ROUTE_CROSS_DEMO_TO_LON   118.220000
#define VMAP_ROUTE_CROSS_DEMO_TO_LAT   32.380000

/** @brief 滁州 GPX → 向东至南京（约 48 km）；需 `make_map.sh nanjing` 地图包。 */
#define VMAP_ROUTE_NANJING_DEMO_FROM_LON 118.334564
#define VMAP_ROUTE_NANJING_DEMO_FROM_LAT 32.237495
#define VMAP_ROUTE_NANJING_DEMO_TO_LON   118.650000
#define VMAP_ROUTE_NANJING_DEMO_TO_LAT   32.165000

/** @brief 较短南京方向段（约 18 km 东南），部分东扩后适用。 */
#define VMAP_ROUTE_NANJING_SHORT_TO_LON 118.520000
#define VMAP_ROUTE_NANJING_SHORT_TO_LAT 32.200000

/** @brief 默认 sim / GPX 起点（test.gpx，WGS84）。 */
#define VMAP_ROUTE_GPX_START_LON VMAP_DEFAULT_LON
#define VMAP_ROUTE_GPX_START_LAT VMAP_DEFAULT_LAT

/** @brief 近东 VREG 边界（约 3 km 东）；终点跨纬网格线约 26 m → 单区域重叠。 */
#define VMAP_ROUTE_BORDER_DEMO_TO_LON 118.3604443
#define VMAP_ROUTE_BORDER_DEMO_TO_LAT 32.2316347

/** @brief 向东一个 6 km VREG 格（约 8 km）；走廊 2 区域。 */
#define VMAP_ROUTE_CROSS_EAST_DEMO_TO_LON 118.410000
#define VMAP_ROUTE_CROSS_EAST_DEMO_TO_LAT 32.235000

/** @brief 向东约 17 km；走廊 2–3 区域。 */
#define VMAP_ROUTE_CROSS_EAST_LONG_DEMO_TO_LON 118.500000
#define VMAP_ROUTE_CROSS_EAST_LONG_DEMO_TO_LAT 32.228000

/** @brief 向西约 9 km；走廊 2 区域。 */
#define VMAP_ROUTE_CROSS_WEST_DEMO_TO_LON 118.250000
#define VMAP_ROUTE_CROSS_WEST_DEMO_TO_LAT 32.240000

/** @brief 向南约 12 km；走廊 2 区域（纬网格）。 */
#define VMAP_ROUTE_CROSS_SOUTH_DEMO_TO_LON 118.330000
#define VMAP_ROUTE_CROSS_SOUTH_DEMO_TO_LAT 32.130000

/** @brief 东北对角约 25 km；走廊 3+ 区域。 */
#define VMAP_ROUTE_CROSS_NE_DEMO_TO_LON 118.450000
#define VMAP_ROUTE_CROSS_NE_DEMO_TO_LAT 32.380000

/** @brief 远东约 30 km；滚动细化/多区域压测。 */
#define VMAP_ROUTE_CROSS_FAR_E_DEMO_TO_LON 118.580000
#define VMAP_ROUTE_CROSS_FAR_E_DEMO_TO_LAT 32.260000

/** @brief 稀疏道路重路由压测：显式起点于恒丰路以南，东边界终点。 */
#define VMAP_ROUTE_REROUTE_DEMO_FROM_LON 118.341000
#define VMAP_ROUTE_REROUTE_DEMO_FROM_LAT 32.230500
#define VMAP_ROUTE_REROUTE_DEMO_TO_LON   VMAP_ROUTE_BORDER_DEMO_TO_LON
#define VMAP_ROUTE_REROUTE_DEMO_TO_LAT   VMAP_ROUTE_BORDER_DEMO_TO_LAT

/* Packed map layout under VMAP_TILE_DIR (see pack_map.py). */
#define VMAP_MAP_INDEX_FILE     "map.idx"
#define VMAP_SHARD_EXT          "vpk"

#endif /* VMAP_CONFIG_H */
