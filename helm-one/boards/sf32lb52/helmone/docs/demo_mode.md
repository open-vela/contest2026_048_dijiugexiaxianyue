# 演示模式（展会 / 拍摄用）

一遍 = **七步**，跑完回到第一步循环，直到手动关掉。目的是把"导航 / 暂停继续 / 偏航重规划 /
全国地图 / 主题"这几件最有看点的事**自动**演一遍，不用一个人一直戳屏幕。

| # | 屏幕中间横幅 | 干什么 | 默认时长 |
|---|--------------|--------|----------|
| 1 | `模拟 GPX 导航`   | 开本机 GPX 的**跟线导航**，再把同一条 GPX **回放成 GNSS**（模拟骑行）；**页面巡览**：地图→表盘→仪表台→全数字 | 30 s |
| 2 | `导航结束`        | **清干净**：停导航、停回放/路线模拟、收掉演示骑行（下一步要一块干净画布） | 2 s |
| 3 | `模拟坐标点导航`  | **假 GPS 按 GPX 从头骑**（`gnss sim` 那条轨迹），同时**规划到固定坐标点**、导航叠在上面；同样带页面巡览 | 30 s（**从规划完毕起算**） |
| 4 | `暂停 / 继续`     | 起一条演示骑行，6 s 后走**真实中键长按**路径暂停（录制暂停 + 假 GPS 冻结），10 s 后走真实继续路径；页面巡览：表盘→仪表台→全数字 | 20 s |
| 5 | `城区偏航重规划`  | 北京城区坐标点导航 + 故意绕路的 `北京.gpx` 当假 GPS ⇒ 真判定偏航 ⇒ 真 reroute 画新线；落地后页面巡览 | 34 s |
| 6 | `全国地图解析`    | 结束导航，在全国 5 个风景点上跳（每个 **10 秒**；**切换前 1.5 s 报下一个点的名字**） | 51 s |
| 7 | `主题切换`        | 切到另一套主题（日光 ↔ 夜间）；待机态页面巡览：主页面→表盘→数据 | 15 s |

一遍约 **3 分 2 秒**。第 1、3 步如果提前到终点/规划失败会提前进下一步（下限 5 s）。

> 第 3 步的计时**从规划落地才开始**（`map_page_nav_planning()` 期间把本步起点一直往后挪，
> 上限 `BICYCLE_DEMO_PLAN_WAIT_MAX_MS`）—— 规划那几百毫秒到几秒不算进"骑"的 30 秒里。

> 「导航结束」那一步是**用户现场要求的**："不然上一个导航可能对下一个有影响"。GPX 跟线导航
> 留下的不只是叠层 —— 还有 `nav_gpx_course`、GPX 窗口、到达锁存、以及**仍在跑的回放/路线模拟**；
> 直接在上面叠一次 `nav_plan_sim`，规划起点与到达判定都可能被上一条路线带跑。

## 第 4 步：暂停 / 继续（2026-11-03 补）

用户口径："没有把骑行暂停，继续的状态演示出来（主要是 gps 一直在跑，中键不会停下来一会）"。
所以这一步**不是**只把暂停条画出来（旧的 `paused` 单场景才是那种）：

* 暂停调 `helm_shell_toggle_ride()` —— 与 **KEY2 长按完全同一条函数**（暂停录制 + 弹暂停条）；
* 同时 `bicycle_gpx_sim_set_hold(true)`：回放线程原地待命、对外定位速度报 0
  ⇒ 屏上速度从 22 km/h 掉到 0、位置不再前进；
* 继续调 `helm_shell_ensure_ride()` —— 与暂停态下 **KEY1 短按同一条恢复路径**，
  再 `hold(false)` 从**原位置**继续骑（不回到起点）。

> 暂停停留必须 > 4 s：`bicycle_runtime` 的 SOG 丢弃保持 `RT_SOG_HOLD_MS = 4000 ms`，
> 速度要 4 s 后才开始往下走（再加 ~1.6 s 降到 0）。现在留 10 s，屏上能看到一段
> 稳定的 `0 km/h`；调短了观众看不出差别。

## 第 5 步：城区偏航重规划（2026-11-03 定稿：北京城区 + 绕路 GPX）

旧的 `nav_warn` / `nav_replan` 单场景只是把横幅强设成"偏航/重新规划"，**没有路线、没有偏航、
也没有新线**。这一步走真实链路：

1. **假 GPS**：回放 `/mnt/lfs/mtp/import/北京.gpx`（用户用 gpx.studio 生成，20 个点、点稀疏，
   **故意在城市里绕了一段**）；
2. **坐标点导航**：`map_page_nav_plan()` 从 `39.919940,116.350961` 规划到
   `39.916015,116.358281`（北京城区）；用**非 sim 版**，回放继续当定位（与第 3 步同一条经验）；
3. GPX 的绕路和规划路线不一致 ⇒ `map_page` 的偏航判定（阈值 50 m、持续 3 s）自己触发
   reroute：横幅进"重新规划"、底栏报原因、新线从当前位置重画；
4. 重规划落地（`map_page_nav_replanning()` 变回 false）后，剩下的时间交给页面巡览。

> **为什么不用人为加偏移**：2026-11-03 用户定"这个应该是需要一个在城区模拟运行的场景"，
> 所以直接用一条故意绕路的城区 GPX 触发偏航，比给定位硬加 120 m 更真实。
> 之前那版 `bicycle_gnss_sim_set_offset()` 已随本次改动删除。

> **规划时机（上板实测）**：
> · **起点必须用 GPX 首点所在城区**，回放先起、等 `BICYCLE_DEMO_REROUTE_SETTLE_MS`（2.5 s）
>   让地图/瓦片跟上再提交规划；若 8 s 内没落地会自动**重试一次**（冷机首图可能慢）。
> · 规划图现在按 **from 坐标所在格子** 加载（`map_page_ensure_plan_graph()`），不再用
>   当前显示区域 —— 否则地图还停在上一站时，北京的点会被吸到 869 km 外
>   （`both ends off-road`），这是 2026-11-03 上板定位到的真因。
> · GPX 的绕路段在 ~300 m 处，45 km/h 约 20 s 才骑到 ⇒ reroute 实测在骑起来 ~14~20 s
>   触发，所以这一步给 34 s、等待窗 26 s（用 18 s 会在触发前一拍放弃）。

> ⚠ 上一版固定起点 `118.341,32.2305`（滁州恒丰路）**落在区域图 clip 之外**：日志
> `snap dest` 被吸到 2 km 外、`Dijkstra fail expanded=355/584`、`corridor: segment 0 fail`
> ⇒ 规划永远不落地，屏上表现为"这一步什么都没发生"。排查这类"规划静默失败"要临时把
> `vmap_route_log.h` 的 `VMAP_NAV_VERBOSE_LOG` 改成 1 才能看到 `[nav] snap/compute/
> corridor/plan_done` 全过程；`VMAP_NAV_ERR` 才是一直开的。

> 8 s 内既没等到路线、`map_page_nav_planning()` 也已落下 ⇒ 打
> `demo: reroute plan failed (no route in …)` 并直接进页面巡览。

> 判据日志：`demo: reroute plan 39.919940,116.350961 -> 39.916015,116.358281 (gpx …北京.gpx)` →
> `demo: reroute riding …` → `demo: reroute off_route=…m (gpx detour)` →
> `demo: reroute replanning started` → `demo: reroute done …`。
> 导航就绪后 18 s 内没判出偏航会打 `demo: reroute not triggered in …`。

## 页面巡览（2026-11-03 补）

用户："展示的还不够全，没有自动在每个页面跳转…最起码在主界面每个页面展示一下，最好带着页面一起触发。"
所以全量循环里不再整段停在一页（以前导航段一直在地图、暂停段一直在表盘），而是按步骤自动轮页：

| 步骤 | 巡览表 | 每页 |
|------|--------|------|
| 1 GPX 导航 / 3 坐标点导航 | 地图 → 表盘 → 仪表台 → 全数字 | 7 s |
| 4 暂停 / 继续 | 表盘 → 仪表台 → 全数字 | 20 s / 3 ≈ 6.7 s |
| 5 城区偏航重规划 | 先停地图页演偏航/重规划；**落地后**再 地图 → 仪表台 → 全数字 → 表盘 | 4 s |
| 7 主题切换（已收骑行） | 主页面（地图+信息卡）→ 表盘 → 数据 | 5 s |
| 6 全国跳点 | 不轮（本来就是看地图） | — |

实现：`demo_tour_start()` / `demo_tour_tick()`（`bicycle_demo.c`），底层就是周期性调
`helm_shell_demo_page()` 锁页面；换步/停演示都会 `demo_tour_stop()` 并清页面锁。
判据：`demo: tour page <name>`，一轮下来应依次看到 map/ride/dash/data。

> 暂停条是全局 chrome，轮页不影响它；偏航步必须先把地图留在前台，否则看不到“偏航 → 重规划
> → 新线”这 3 秒，所以巡览从重规划落地后才开始。

## 横幅

提示挂在 **LVGL top layer**（`lv_layer_top()`）上 —— 盖住地图页、菜单、导航横幅，
且**只在演示期间存在**（关掉就删对象）。它刻意**不用**通知横幅那套：演示期间不该被
"来电话 / 收到通知"挤掉位置。样式与全局通知同一档（黑底半透 200 + 白字 15px TTF），
位置是屏幕正中间（`DEMO_BAN_DY` 可调）。

**每次露脸 2 秒**（`BICYCLE_DEMO_BAN_MS`）就自己收掉 —— 横幅是"报户口"用的，
不一直压着地图。全国跳默认只在开始时提示一次；想让**每换一个点都再提示 2 秒**，
把 `BICYCLE_DEMO_BAN_PER_POINT` 改成 1。

## 怎么开

* **菜单**：长按 KEY1 → 设置 → 系统 → **演示模式**（开关）。开起来后菜单会自动收起
  （演示要看地图）。
* **控制台**：

```
bicycle_nsh demo on        # 开
bicycle_nsh demo off       # 关（还原主题与缩放）
bicycle_nsh demo toggle
bicycle_nsh demo status    # 当前在哪一步：gpx_nav / plan_nav / pause / reroute / nation / theme / off
bicycle_nsh demo list      # 单场景全表（含 pause_run / reroute_run）
bicycle_nsh demo run pause_run     # 单独跑"暂停/继续"
bicycle_nsh demo run reroute_run   # 单独跑"城区偏航重规划"
```

`demo status` 读的是 ctl 状态槽，里面存的是 **ASCII 步名**（槽只有 16 字节，中文会被截成
半个 UTF-8 字）；横幅/菜单上那套中文文案是另一处（`bicycle_demo_status()`）。

## 改料的地方

全在 `ui/bicycle/src/bicycle_demo.h`（时长、GPX 路径、坐标点、缩放）+ `bicycle_demo.c`
顶部两张表：

| 想改什么 | 改哪 |
|----------|------|
| 第 1/3 步时长、导航结束那步、全国跳每点停留、主题步时长 | `BICYCLE_DEMO_NAV_MS` / `BICYCLE_DEMO_NAV_END_MS` / `BICYCLE_DEMO_HOP_PERIOD_MS` / `BICYCLE_DEMO_THEME_MS` |
| 第 3 步等规划的上限 | `BICYCLE_DEMO_PLAN_WAIT_MAX_MS` |
| 暂停步：骑多久 / 停多久 | `BICYCLE_DEMO_PAUSE_AT_MS` / `BICYCLE_DEMO_PAUSE_HOLD_MS`（必须 > 4 s） |
| 偏航步：骑多久 / 偏多少米 / 兜底超时 | `BICYCLE_DEMO_REROUTE_RIDE_MS` / `BICYCLE_DEMO_REROUTE_OFF_M` / `BICYCLE_DEMO_REROUTE_TIMEOUT_MS` |
| 偏航步的起终点 / 假 GPS | `BICYCLE_DEMO_REROUTE_FROM_LON/LAT` → `..._TO_LON/LAT`；回放 `BICYCLE_DEMO_REROUTE_GPX`（`/mnt/lfs/mtp/import/北京.gpx`，同时放在 `mkfs/lfs/mtp/import/` 里随 LFS 烧录） |
| 横幅停留多久 / 切换前提前多久点名 | `BICYCLE_DEMO_BAN_MS` / `BICYCLE_DEMO_ANNOUNCE_MS` |
| 跑不跑循环 | `BICYCLE_DEMO_LOOP`（0 = 一遍就停） |
| 回放哪条 GPX | `BICYCLE_DEMO_GPX`（找不到时自动退到"最新的一条本机轨迹"） |
| 第 3 步的目的地 | `BICYCLE_DEMO_PLAN_LON/LAT` |
| 全国跳跳哪几个点 | `bicycle_demo.c` 的 `k_nation_pts[]`（当前：太湖 / 阳朔 / 九寨沟 / 呼伦贝尔 / 三亚湾） |

第 1、3、5 步的行驶速度用**导航模拟速度**（`nav sim speed`，默认 45 km/h），不是另开一个档。

### 怎么挑全国跳的点（**实测工具**）

`zcode/analysis/demo_spot_scan.py` 直接解瓦片里的 VTIL 图层（ROAD/WATER/WATERWAY/FOREST），
对任意经纬度报"最近道路多少米 / 窗内路迹条数 / 水面 / 水线 / 林地"：

```
python3 zcode/analysis/demo_spot_scan.py                        # 跑内置候选清单
python3 zcode/analysis/demo_spot_scan.py "太湖=120.29,31.49"     # 单点
```

用户 2026-09-29 的口径：**必须"在路上"（最近道路 ≤ 60 m）**，**最好同时有水有林**。
当前 5 个点的实测值（最近路 / 路迹 / 水 / 水线 / 林）：

| 点 | 最近路 | 路迹 | 水 | 水线 | 林 |
|----|--------|------|----|------|----|
| 太湖（无锡岸） | 1 m | 3586 | 70 | 324 | 39 |
| 阳朔 | 9 m | 1014 | 70 | 46 | 159 |
| 九寨沟 | 44 m | 153 | 0 | 10 | 16 |
| 呼伦贝尔 | 41 m | 307 | 16 | 30 | 3 |
| 三亚湾 | 9 m | 840 | 14 | 9 | 5 |

⚠ **不满足这个口径的地标**（实测）：青海湖（湖边 1.2 km 内没有可画的路、也没有林地）、
洱海（2.1 km）、千岛湖（2.8 km）、长白山（1.0 km）、武夷山（0.9 km）、黄山（0.6 km）、
泸沽湖（2.0 km）、敦煌（无林地）。**别凭地标名气选点** —— 先跑脚本。

### ⚠ 两条 2026-09-29 上板翻车换来的硬规矩

1. **跳点坐标必须先离线核过瓦片**，而且要看 **3×3 邻格**：全国图是按 **3 km 格**打包的，
   格与格之间**有洞**（实测 `(100.45,36.85)` 青海湖、`(128.06,42.02)` 长白山天池那两个格
   **根本没有 vpk**，跳过去整屏白）。现在表里这 5 个点所在格的 3×3 邻格全有 vpk，
   用的是**格心**坐标。换点前照这个查一遍：
   `lon{ix//4}/lat{iy//4}/x{ix}_y{iy}.vpk`，`ix=floor(lon/0.032899)`、`iy=floor(lat/0.0269493)`。
2. **永远不要改瓦片缩放（`z,N`）**：这张全国图**只按 z14 打包**（逐个 vpk 核过，里面全是
   z14 瓦片）。把视图拉到 z11 就是请求一批不存在的瓦片 ⇒ 整段跳点全白屏。
   要"看宽一点"只用 `s,F`（缩放），别动 z。
3. **第 3 步的规划起点显式取"轨迹起点"**（`demo_gpx_first_pt()` 直接读 GPX 首个 `<trkpt>`），
   不要问"当前定位"：回放引擎是 *全国跳 → 路线模拟 → GPX 回放* 三级优先，**回放不是永远
   说了算**。实测日志里那一次取到的是**上一条路线的终点**（= 上次规划的目的地），
   于是"从目的地规划到目的地"→ 一条 **21 m** 的路线 → 一开就报到达。
4. **第 3 步用 `map_page_nav_plan()`，不要用 `map_page_nav_plan_sim()`**（用户 2026-09-29：
   "坐标点导航的轨迹是错的，不应该重头开始那条 GPX 吗"）。sim 版会
   `map_page_nav_gnss_for_sim()`：**停掉 GPX 回放、把定位源切成路线模拟** ⇒ 屏上记下来的
   "骑行轨迹"变成那条规划路线。非 sim 版只停**路线模拟**，回放照跑 ⇒ 假 GPS 从轨迹起点
   一路骑，导航只是叠在上面的一层。
   · 副作用（正常现象）：回放走的路和规划路线不一致时，导航会报偏航并按 8 s 节流重规划 ——
     想少一点就把目的地选在回放轨迹的经过点上。

## 判据（日志）

```
demo: start (gpx=...)                              # 开
demo: step1 gpx nav <path> @45 km/h                # 第 1 步起来
demo: step2 plan 118.36008,32.23223 -> 118.37629,32.24587 @45 km/h   # 第 3 步（起点=轨迹起点）
demo: pause demo start (<path>)                    # 第 4 步起来
demo: pause (KEY2-long path) paused=1 hold=1       # 真暂停 + 假 GPS 冻结
demo: resume paused=0 hold=0                       # 真继续 + 放开回放
demo: tour page ride / dash / data / map           # 页面巡览翻页（每翻一页一行）
demo: reroute demo plan 118.341000,32.230500 -> 118.376293,32.245873  # 第 5 步起来
demo: reroute riding at Nms                        # 路线就绪，开始骑
demo: reroute off-route e=…fm n=…fm                # 拉偏（垂直行进方向 120 m）
demo: reroute off_route=120m                       # 偏航真的被判出来了
demo: reroute replanning started                   # 进入重规划
demo: reroute done at Nms, offset cleared          # 新线落地、回到路线
demo: nation 5 pts x 10000 ms                      # 第 6 步起来
demo: hop -> 太湖 / 阳朔 / 九寨沟 / 呼伦贝尔 / 三亚湾   # 每跳一个点一行
demo: theme -> night                               # 第 7 步起来
demo: cycle N done, back to step1                  # 一遍跑完
demo: stop (N cycle(s) done)                       # 关
```

起不来时是 `demo: xxx failed`（控制台 INFO/WARN）。

**查"导航为什么秒到达/绕远"**：`demo: step2 plan …` 给出的起点/终点拿去和
`[nav] compute: snap start (…)` 对一眼 —— 两者不一致就说明定位源不是你以为的那个。

**查"规划线画出来了没"**：`demo: step2 route ready pts=N len=X km arrow=on`（规划落地时一行），
再配 `[nav] paint: seen=1 seg=i/N off=…` —— 那个 `seg=i/N` 是**骑行者投影在第 i 段**（不是
"画了 i 段"），`seen=1` 就说明路线叠层这一帧在画。路线本身连**起点绿点/终点红标**一起画
（`vmap_route.c` 的 `paint_pins`）。

**绘制层次**（`vmap_view_stamp_nav_then_track`）：底图 → **导航线** → 标签 → **REC 轨迹** →
REC 数字 ⇒ 第 3 步里"回放的 GPX 轨迹"画在"规划路线"**之上**，重合路段看到的是前者。
两条都在，只是重合处以后者为准。

**方向箭头**：第 3 / 5 步进入时显式调 `map_page_set_arrow_visible(page, true)`（规划导航自己
不会打开箭头 —— 之前进过"回放 / 只看轨迹"的话箭头一直是关的），规划落地时再点一次。

## 已知口径 / 边界

* **不落盘**：演示骑行结束时会**丢弃**未保存的 GPX（否则每演示一遍就多一条 TRK 记录，
  轨迹里还会多一条横跨全国的长线、里程涨到几千公里）。
* **会还原**：停止时主题恢复成开演示前那套（主题名会写 `/mnt/kv/ui_theme`），地图缩放
  也还回去。
* **真实骑行中拒绝启动**（提示"请先结束当前骑行"）：演示会改里程/轨迹/主题。
* **第 1 步的顺序是"先开导航、再起回放"**：`map_page_nav_from_gpx()` 开头会
  `bicycle_gpx_sim_stop()`（它默认"跟线用真实定位"），顺序反了回放会被它一脚踢掉。
* **第 5 步的定位来自路线模拟**：`map_page_nav_plan_sim()` 自己会停掉回放、改由
  `bicycle_route_sim_*` 驱动（路线模拟就是骑手）；偏航偏移只加在定位出口上。
* **暂停步 / 偏航步只改合成定位的出口**：`bicycle_gpx_sim_set_hold()`（速度 0 + 位置不前进）
  与 `bicycle_gnss_sim_set_offset()`（东/北偏移）都不动回放/路线模拟自己的进度；
  `bicycle_demo_stop()`、换步、`demo_scene_leave()` 都会 `demo_sim_reset()` 清零。
  漏掉一处，下一段演示就会带着上一次的冻结/偏移。
* **跳点表只在一次跳点期间有效**：`bicycle_gnss_hop_stop()` 与"跳够次数自动停"都会清掉，
  所以 `bicycle_nsh gnss hop`（渲染压测）的城市序列口径**不受演示影响**。

## 代码位置

* `ui/bicycle/src/bicycle_demo.c` / `.h` —— 状态机 + 横幅（`bicycle_demo_poll()` 挂在
  UI 主循环循环顶，与 `bicycle_ui_ctl_poll()` 同处）。
* `ui/bicycle/src/bicycle_gpx_sim.c` —— 自定义跳点表；演示用的 `bicycle_gpx_sim_set_hold()`
  （暂停冻结）与 `bicycle_gnss_sim_set_offset()`（偏航横向偏移）。
* `ui/bicycle/src/lvgl_page/helm_menu.c` —— 设置页那一行（`VAL_DEMO` / `ACT_DEMO_TOGGLE`）。
* `include/myvendor_bicycle_ctl.h` —— `..._OP_DEMO` 与 `..._STATE_DEMO`（NSH 查询用）。

## 字模（本次一并补了）

这套文案引入的新字在点阵字库里**原来没有**，而**菜单行只走点阵**（`helm_row_set` 的
`title_font = NULL`）—— 缺字 LVGL 是**整块不画**（不是画方块）。所以：

* 新增**按档**符号文件 `fonts/helm_mism4_symbols_{12,15,22}.txt`（原文件头里的裁剪集 +
  本次要补的字）：`mism4_12` 补 **主切国天太山拟换析涯湖演珠角青题**、`mism4_15` 再补 **好**，
  `mism4_22` 不动（演示文案不在 22px 渲染）。
* `tools/gen_helm_mism4.py` 改成**优先读按档文件**（没有才退回共用的 `helm_mism4_symbols.txt`）。
* `tools/gen_helm_fonts.py` 的扫描表加了 `src/bicycle_demo.c`（UI 字库按源码里的字符串自动
  收集符号，横幅/菜单文案才进得去）。
* 生成（`npx lv_font_conv`，本机可复现；`mism4_22` 重生成后逐字节不变可作证）：

```
python3 vendor/HelmOne/boards/sf32lb52/helmone/ui/bicycle/tools/gen_helm_mism4.py
python3 vendor/HelmOne/boards/sf32lb52/helmone/ui/bicycle/tools/gen_helm_fonts.py
```

⚠ **别再用共用字表那条路重生成点阵**：2026-09-26 按调用点裁剪过一轮（每档单独用字，
417/398/383 个汉字），拿 510 字的共用表重生成会把裁剪成果冲掉、flash 多 ~56 KB
（`git show HEAD:<font>.c` 的文件头里记着每档当初那份裁剪集）。

**以后演示文案要改字，先按上面两行走一遍**，再用 `zcode/analysis/font_glyph_scan.py` 复查
（注意：那个脚本只比 `mism4` 单表，会误报「勾/绑」—— 有效覆盖是 **mism4 ∪ helm_ui 回退**）。

横幅本身走 `helm_font_sys(15, helm_font_title())`：**优先系统 TTF（全覆盖）**，
点阵只是它没就绪/工厂固件时的回退。
