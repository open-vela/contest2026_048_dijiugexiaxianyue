# 低功耗优化方案（Phase 1 调查结论）

- 日期：2026-10-29
- 范围：`vendor/HelmOne`（SF32LB52x / HCPU / my_vendor_nsh 产品固件）
- 性质：Phase 1 只读调查的汇总；**不含代码改动**。证据见文末索引。
- 结论先行：**本轮只做运行时功耗**（亮屏、记录/导航、GNSS/BLE 开时的平均电流），
  不看待机/睡眠。运行时的大头按证据排序是 **背光、显示/渲染工作量、GNSS/BLE 射频**；
  CPU 空转微优化合计只有 1~3% 量级，不该排在前三。
  §2 与 Phase 4 的睡眠/PM 路线保留作背景资料，**不在本轮范围**。

---

## 0. 五条硬结论

1. **deep sleep / standby 在本设计上不可用。** 镜像 `.text` 在
   PSRAM `0x10000000`（`scripts/ld.script:21-26,42`，由 2SFBL 从 NAND 载入），
   PSRAM 挂在 DLL2=288 MHz；掉 PSRAM 电 = 代码取指失败。HAL 的
   `hpsys_dll2_limit[D0/D1] = 0`（`bf0_hal_rcc.c:59-66`）也从硬件层拒绝 48/24 MHz。
   ⇒ **只剩 light sleep（PMR=1，原地恢复）和 hibernate（等价关机）两种。**
2. **HCLK 已经无法再降。** 产品 DVFS 只剩 **144 / 240 MHz**
   （`sf32lb_dvfs.c:91-101`，72/96 已被用户先后取消）；空闲也钉在 144。
3. **当前没有真正的低功耗态。** `CONFIG_PM` 未开，`up_idle()` 只有裸 `wfi`，
   PMR 恒 ACTIVE（`sf32lb52_idle.c:22-40`；`.config:1183`）。
4. **板上没有任何电流检测。** ETA9184 只有 PULSE/STAT/DISCHRG 三根数字脚，
   电量档位自评"不可信"；电池只有电压 ADC（`/dev/adc0`）。
   ⇒ 绝对 mA 必须靠外部表；固件只能给相对判据（`test idle` 的 true idle%、
   `sys bat` 的 mV 斜率）。
5. **现有证据指向"省电大头在板级"，但必须由 M0 预算确认。** 背光恒定 20%、
   面板从不 SLEEP IN、GNSS 连续跟踪到静止 10 min、BLE 连接间隔 15 ms、
   DCDC 常 FPWM。这些每一项都可能比"把 1 ms tick 改成 tickless"更值钱；
   在拿到 S0~S4 的实测差之前，不把它们当成定论。

---

## 0.5 运行时功耗：先动哪三块（本轮目标）

> 用户口径（2026-10-29）：只看设备**运行中**的功耗，不看待机/睡眠。
> 下面每一项的绝对 mA 都要实测；排序依据是"是否恒定负载 / 是否可归零 / 改动代价"。

| 排名 | 负载 | 现状（事实） | 可动的杠杆 | 代价 / 风险 |
|---|---|---|---|---|
| **1** | **背光** | UI 默认 **20%**（PWM 16/80），骑行中恒定；只有静止 10 min 一次归零，无中间档 | 白天 0~10%（半透半反靠环境光）、夜间 20%；按时间/设置切换；无操作降档 | 可读性/观感；需产品定策略 |
| **2** | **显示 / 渲染** | 数据页脏区驱动；**主页提示环每帧 `translate_y` 无条件 invalidate ⇒ 约 50 fps**；地图泵 20 ms、单瓦片 15~38 ms@240 MHz | 提示环改"每 500 ms 移 1 px"（~2 fps）或停；动画帧率上限；地图按位移阈值重绘 | 视觉流畅度；地图是核心功能 |
| **3** | **GNSS** | MAX-M10S **1 Hz 连续跟踪、全星座**；无 CFG-RATE / CFG-PMS | 星座裁剪（GPS+BDS）、PSM/duty-cycle、非导航时降频 | 定位质量/TTFF；需路测 |
| 4 | BLE | 请求 **15 ms**（实际看 `le interval=` 日志，手机可能已抬到 30 ms） | 40~100 ms + latency；传感器 central 已是默认档 | 通知延迟；需 App 联调 |
| 5 | CPU 频点 | 144 常驻；地图泵 240 | 长驻留（静止数据页 >30 s）重开 72 档 | 历史上 72/96 因换档 stall/WDT 被取消；**高风险，最后考虑** |
| 6 | 传感器/SD/diag | IMU 10 Hz、baro/mag 2 Hz、ADC 0.5 Hz；日志积压时 2 s 写 SD | baro/mag 1 Hz、日志降级 | 数据分辨率/现场取证；收益小 |

**排序依据：**

- 背光是唯一"恒定且可归零"的负载：只要屏幕亮着就在耗，白天关掉不损失可读性
  （产品定位就是半透半反）。它是否最大单点，用 M1（`ctl bl 0/20/50/100`）一测就知道。
- 显示/渲染是骑行中最大的 **CPU + QSPI** 项；主页提示环把本可静止的画面变成 ~50 fps。
- GNSS/BLE 是射频项，占空比由协议/配置决定，不随 UI 变化。
- CPU 空转（约 1.5k 次 WFI 退出/s）看着吓人，但每次退出成本低，
  把 cpuload/按键/diag 降频的合计收益**估算只有 1~3%**，不要先做这个。

> **纠错（重要）：** 把提示环动画"放慢"并不能降帧率。`lv_obj_set_style_translate_y`
> 是 transform 属性，LVGL 每次都无条件 `lv_obj_invalidate()`（`lv_obj_style.c:362-364`），
> 与数值是否变化无关。要么停掉，要么改成"每 500 ms 改一次整数位移"的 `lv_timer`
> （`lv_obj_set_y` 才有相等就返回的短路）。

### 0.6 一个判据：CPU 还是板级？

在骑行/演示场景下，分别在地图页和数据页跑 `test idle 30`：

- **true idle% 低（<30%）** ⇒ CPU/渲染是主项，先做显示/地图那条线；
- **true idle% 高（>70%）但整机电流仍高** ⇒ 主项在板级（背光/GNSS/面板/BLE），
  CPU 侧再优化也没用。

---

## 1. 现状（事实，带出处）

### 1.1 时钟 / 电源 / 睡眠

| 项 | 现状 |
|---|---|
| HCLK | DVFS 144/240；空闲 144；boot/map/charge/off 有 240 floor |
| 睡眠 | 无。PMR=ACTIVE，`up_idle` = `wfi` + DWT 记账 |
| 低功耗时钟 | RC32 已开、XTAL32 可选；RTC 已注册为 `/dev/rtc0` |
| 唤醒源 | HAL 支持 RTC/LPTIM1/LP2HP/GPIO1/PIN0..20；产品**一个都没接** |
| 关机 | `ctl off` → 拉低 PA29 电源锁存（真断电）；`HAL_PMU_Reboot()` 仅兜底 |
| 3.3V DCDC | TPS63802，PA27 恒高 = **FPWM**（轻载 PFM 未用） |

### 1.2 显示 / 背光

| 项 | 现状 |
|---|---|
| 面板 | NV3031A，**片内 GRAM**，4-line QSPI @24 MHz，无 TE；按需写窗口，主机不连续扫描 |
| 静止时 | 面板**仍供电、仍自刷新**，只关背光；`LCD_IdleModeOn`/`LCD_DisplayOff` 有实现但产品从不调用 |
| 背光 | GPTIM1 CH4 / 20 kHz；UI 100% = PWM 80%；**默认 UI 20% 恒定**；档位 0/20/40/60/80/100；220 ms 渐变 |
| 管理 | **无中间变暗**；静止 10 min → 背光 0；静止 1 h → 断电关机 |
| 常驻动画 | 主页信息卡提示环：22×22、±4 px、1400 ms 往返、无限；约每 16 ms 置脏一次 ⇒ 主页近似 50 fps 刷新 |

### 1.3 无线

| 项 | 现状 |
|---|---|
| GNSS | u-blox **MAX-M10S-00B-01**，38400 baud，USART2 |
| 配置 | 只发了 CFG-VALSET（dynModel/minElev/minCno/speedThrs/ackAiding/PVT/BDS） |
| 速率 | **没有 CFG-RATE / CFG-PMS / duty-cycle** ⇒ 出厂默认 **1 Hz 连续跟踪** |
| 电源 | 模组 VCC 由 **PA43** 控制，只在静止 10 min 时切断（有备份电源，不丢星历） |
| BLE | 射频在 LCPU；ADV 200 ms（配对窗）/ 稳态 4 s；**连接间隔请求 15 ms** |

### 1.4 软件节拍（"空转也会被叫醒"的项）

- 1 ms SysTick（1000 次/s，非 tickless）+ 200 Hz cpuload BTIM2（5 ms）。
- `bicycle` 主循环 10 ms（100 次/s）调 `lv_timer_handler`；返回值未用于延长睡眠。
- 板级按键线程 10 ms、LVGL 按键 20 ms；传感器 50 ms；GNSS park 200 ms；
  DVFS governor 250 ms；diag 500 ms；地图 GNSS 拍 200 ms；状态栏 1 s；
  `helm_ui_cb` 500 ms；`helm_rot_cb` 3 s；IWDT 2 s；ETA 4 s。
- 合计约 **1.5k 次/s WFI 退出**（还没有算 UART/BLE/按键/LCDC 中断）。

### 1.5 静止（still）态到底关了什么

`helm_idle_enter()`（`helm_idle.c:455-472`）只做：
`helm_anim_suspend(true)` + 覆盖层 + 背光 0 + **GNSS 断电** + 释放 AWAKE hold。
**没做**：面板 SLEEP IN / 断电、LCDC 关闭、BLE 关闭、传感器降频、任何 PMR 睡眠。
⇒ 静止后仍：144 MHz、1 ms 节拍、面板自刷新、BLE、IMU、UI 定时器。

---

## 2. 为什么不能"直接上 PM"（约束推演）

| 方案 | 可行性 | 原因 |
|---|---|---|
| deep sleep / standby | ❌ | PSRAM 掉电 = 镜像丢；官方 `bf0_pm.c` + `.retm_data` 段被本树整体丢弃 |
| 48 / 24 MHz | ❌ | HAL 硬拒绝（DLL2 必须 0）；且 .text 在 PSRAM |
| light sleep（PMR=1） | ✅ 待验证 | 原地恢复；但 HAL 只写 PMR，实际门控范围未知，**必须实测** |
| hibernate | ✅ 但无收益 | 等于关机 + 定时开机；现有 PA29 断电已覆盖 |
| NuttX PM 框架 | ✅ 框架在，胶水缺 | `arm_pminitialize()` 未实现（`CONFIG_PM=y` 直接链接失败） |
| tickless | ⚠ 需改造 | `sifli_oneshot.c` 有 LPTIM1 实现，但与 `CONFIG_TIMER_ARCH` 双时基冲突，直接开会 NULL 解引用 |

**推论：** 唯一值得投入的 CPU 侧路线是 **light sleep**；而 tickless 只有在
light sleep 成立后才有意义（让单次驻留从 1 ms 拉长到百毫秒级）。
如果 light sleep 实验证明"PMR=1 与 WFI 没差别"，那么 PM + tickless 这 1~2 周
就不该做，预算应全部投到板级（显示/GNSS/BLE）。

---

## 3. 优化清单（按"先测量、后改动"排序）

> 收益列全部标 **待测**：没有电流表就不写假数字。`估算` 只给相对量级。

### Phase 0 — 测量与可行性（不改产品行为）

| ID | 事项 | 做法 | 收益 | 成本 |
|---|---|---|---|---|
| **M0** | 6 场景基线 | 用现成 `ctl` 命令跑 S0~S6（见 §4），记 `sys bat` mV / `test idle` | 得到预算拆分 | 0.5~2 h（用户） |
| **M1** | 背光单变量 | 亮屏下 `ctl bl 0/20/50/100` 各 10 min | 背光每档的 mA | 40 min |
| **M2** | GNSS 单变量 | 需临时加"亮屏但 `myvendor_gnss_idle_sleep(true)`"入口（1 行命令） | GNSS 模组电流 | 0.5 d |
| **M3** | light sleep spike | `ctl pmlight 1`：`up_idle` 里 PMR=1→WFI→清 PMR，默认关；带 tick 单调性 + PSRAM 校验 + 计数器 | **决定 PM 线是否值得做** | 0.5 d |
| **M4** | 面板 SLEEP IN | 静止态发 `0x28`，唤醒时 re-init + 全屏重绘；默认关 | 面板静态电流 | 1 d |

**M3 的判据（缺一不可）：**
1. 30 s 内不死机、`clock_systime_ticks()` 与墙钟不漂 >1%；
2. PSRAM 中 known pattern 不变、代码仍可执行；
3. GNSS 不丢字节、SD 无超时、LCDC 正常；
4. **电池电流相对 S1 有可测下降**（否则 light sleep 就是空操作）。
   ⚠ 睡前必须 `myvendor_watchdog_halt()`，否则 IWDT 15 s 必复位。

### Phase 1 — 不改 UI 的软件减负（低风险）

| ID | 事项 | 位置 | 收益 | 风险 |
|---|---|---|---|---|
| P1-1 | cpuload 200→50 Hz | `defconfig` `SCHED_CPULOAD_TICKSPERSEC` | 150 次/s 中断 | 低（2 s 时间常数不受影响） |
| P1-2 | 静止态暂停 `ui/rot/status` 定时器 | `helm_idle.c` 调 `helm_shell_pause_for_cover()`；注释已声称暂停但代码没做 | 500 ms/1 s/3 s 三项 | 低（覆盖层自带时钟） |
| P1-3 | diag FS 探针 500 ms→5 s | `myvendor_diag.c:451-466` | 每秒 6 次 `stat()` | 低 |
| P1-4 | 按键线程 10→20 ms | `sf32lb52_buttons.c:35` | 50 次/s | 低（长按判定 10 ms 粒度变 20 ms） |

合计预计只有 **1~3% 量级的 CPU 空转**；不要指望它解决续航，它的价值是
"把底噪让出来，便于后面 A/B 看清真实差异"。

### Phase 2 — 显示链（最大单点，需产品决策）

| ID | 事项 | 说明 | 收益 | 风险 |
|---|---|---|---|---|
| **P2-1** | 背光分级变暗 | 例如无操作 60 s → 当前档 50%，5 min → 最小档，10 min → 0（现状只有 0） | **高**（背光是恒定负载） | 观感；需要用户定策略 |
| **P2-2** | 静止态面板 SLEEP IN | 背光已 0 时发 `0x28`，省面板电荷泵/振荡器；唤醒 re-init | 中高 | 唤醒 100~300 ms、可能闪屏 |

| **P2-3** | 提示环动画减速/停 | 22×22 环从 1400 ms 改 3~5 s，或静止/低电时停 | 中（主页刷新 50 fps → 0.2~1 fps） | **视觉变更，必须用户点头** |
| **P2-4** | DCDC 轻载 PFM | PA27 拉低进 PFM；静止态切，亮屏切回 FPWM | 中（待测） | 纹波可能影响 ADC/音频，需示波器 |

> ⚠ P2-2 不要走"打开 `NV3031A_APP_REINIT_PANEL`"这条路：它会在 idle 直接断
> PA10 并拉低复位（`bsp_pinmux.c:767-783`），唤醒要完整 re-init + 全屏重绘；
> 这个宏当前被有意注释在 `include/drv_io.h:88`。优先试面板 SLEEP IN（0x28），
> 它保留 GRAM，唤醒成本低得多。

### Phase 3 — 无线（大电流，需配套 App 配合）

| ID | 事项 | 说明 | 收益 | 风险 |
|---|---|---|---|---|
| **P3-1** | BLE 连接间隔 15→50/100 ms + slave latency | `ble_companion.c:1453-1467` | **高**（射频占空比降 3~7 倍） | App 通知延迟；需手机侧接受 |
| **P3-2** | GNSS 按场景降频 | 非记录/非导航：CFG-RATE 1 Hz→0.2 Hz；记录中保持 1 Hz | **高**（模组连续跟踪是大头） | 定位/轨迹质量；需路测 |
| **P3-3** | GNSS duty-cycle / PSM | `CFG-PMS`（MAX-M10S 支持）；只在停车/静止用 | 高 | TTFF、冷启动星历 |
| **P3-4** | 静止 park 10 min → 2 min | 非记录时更快断 GNSS VCC | 中高 | "室内看时间"场景会没星数 |
| **P3-5** | 星座裁剪 | `ctl gnss bdsonly` 已验证可用；可做成"省电模式" | 中 | 定位质量 |

### Phase 4 — 架构（**不在本轮范围**；仅当用户重新开启睡眠目标）

> 2026-10-29 用户明确"只看运行时功耗" ⇒ 本节保留作背景，本轮不执行。

| ID | 事项 | 工时 | 前置 |
|---|---|---|---|
| **P4-1** | NuttX PM + light sleep + wakelock | 2~4 d | M3 通过 |
| **P4-2** | tickless（LPTIM1 lower-half） | 3~5 d | P4-1；先解决 `ARCH_TIMER` 双时基 |
| **P4-3** | 静止态 72 MHz 档（重开） | 1~2 d | SD 时钟夹持路径已有；需 A/B |
| **P4-4** | 官方 `.retm_data` / `bf0_pm.c` 移植 | 3~5 d | **不建议**：收益是 deep sleep，但 PSRAM 仍不可掉电 |

### 明确不做

- 48/24 MHz：HAL 拒绝 + PSRAM XIP，做了也切不进去。
- deep sleep / standby：同上。
- 单独上 tickless（不配 light sleep）：只省 1 ms 节拍开销（**估算 <1%**），收益不够本。
- hero 搜星动画：当前 `standby` pane 永久隐藏，**不耗电**；不要再为它优化。
- 系统页 statfs：已从 2 s 放宽到 30 s，且只在系统页。

---

## 4. 测量协议（可直接照做）

### 4.1 场景矩阵（全部有现成命令，无需等 10 min）

| 场景 | 进入 | 隔离出的量 |
|---|---|---|
| **S0 全开** | `ctl idle stay` + `ctl bl 100` | 峰值/基准 |
| **S1 静止屏** | `ctl idle on` | 背光 0 + GNSS 断 + 面板仍供电 |
| **S2 S1 + BLE 关** | `ctl radio off` → `ctl idle on` | BLE 广播/连接的板级成本 |
| **S3 S1 + 强制 144** | `ctl dvfs 144` → `ctl idle on` | 频率 vs 外设 |
| **S4 背光扫描** | 亮屏下 `ctl bl 0/20/50/100` 各 10 min | 背光每档 mA |
| **S5 亮屏 + GNSS 断** | 需 M2 的临时入口 | GNSS 模组电流 |
| **S6 关机漏流** | `ctl idle hour` | 断电后漏流（表测） |

### 4.2 每次要记的读数

```text
sys                 # bat <pct>% <mv>mV / bl <applied>% / dvfs 行 / slot / build_date
test idle 30        # true% (WFI+DWT) —— idle% 是可信的；win_ms 在 144 MHz 会低估 1.67×
ctl cnt now         # BLE/GNSS 计数是否在动
sys dvfs            # HCLK / gov / holds / idle
```

### 4.3 没有电流表时（电压斜率法）

1. 满电静置 ≥1 h，记 `V0`；
2. 固定场景跑 ≥2 h，每 5 min 记一次 `sys bat` mV；
3. **丢掉前 30 min**（极化/弛豫），用后段线性区算 `ΔV/Δt`；
4. 按 `myvendor_bat_soc.h:39-52` 的 OCV 表换 SOC：
   `I_avg(mA) = 450 × (SOC0−SOC1)/100 / T(h)`；
5. 平台区约 **1 mV ≈ 0.9 mAh**；2 h 内 3~4 mV 的差 ≈ 1.5 mA 分辨率。
   ⚠ 该表是通用单节锂电曲线、非本电芯；只能**同表同法做相对比较**。

---

## 5. 建议的执行顺序（运行时）

```text
第 1 天：骑行场景基线（不改码）
  地图页 / 数据页各跑 `test idle 30` + `sys dvfs`，同时记 `sys bat` mV 与 `le interval=`
  判据：idle 低 → 渲染是主项；idle 高但电流高 → 背光/GNSS/面板是主项

第 2~3 天：背光策略（最可能是最大单点）
  先 M1 单变量测 `ctl bl 0/20/50/100`，再定白天/夜间默认与降档曲线

第 3~5 天：显示 / 渲染
  提示环改成"每 500 ms 移 1 px"（~2 fps）或停；必要时加动画帧率上限；
  地图按位移阈值重绘（现有 rebase 守卫已部分做到）

第 2 周：射频
  GNSS 星座裁剪 / PSM（路测）→ BLE 连接间隔联调

可选、单独立项：72 MHz 长驻留档（高风险，先做换档 A/B）
不在本轮：light sleep / PM / tickless / hibernate
```

**排序理由：** 背光与显示是"每时每刻"的负载，且不依赖协议；GNSS/BLE 由协议
占空比决定；CPU 微优化合计估算只有 1~3%。睡眠/PM 线已在 §2 说明其约束，
按用户 2026-10-29 的口径**不在本轮执行**。

---

## 6. 需要你决定 / 提供的

1. **有没有外部电流表**（USB 表、台式电源、电流探头任一）。没有就只能用
   §4.3 的电压斜率法，分辨率 ~1.5 mA、单场景 ≥2 h。
2. **电芯实际容量**是否为 450 mAh（所有估算的系数）。
3. **背光策略**：能不能接受"无操作 60 s 变暗、10 min 全黑"？夜间默认亮度是否更低？
4. **提示环浮动动画**能否改成"每 500 ms 移 1 px"（~2 fps）或直接停？它是主页 ~50 fps 刷新的唯一原因；只放慢时长没用（见 §0.5 纠错）。
5. **BLE 连接间隔**能否从 15 ms 放宽到 50~100 ms（+ slave latency）？手机 App 的通知延迟会变。
6. **GNSS 策略**：非记录/非导航时降频（1 Hz→0.2 Hz）、静止 2 min 就断 VCC，是否可接受？
7. 是否允许**临时改码**做 M2（亮屏+GNSS 断电）和 M4（面板 SLEEP IN）两个对照实验。

---

## 7. 文档债务（本轮顺带发现）

- `helm_idle.c:9` 文件头仍写"HCPU 由 DVFS ondemand 落到 72/96"——**已过时**，现在只有 144/240。
- `docs/map/perf/PUMP_PERIOD.md:80`、`docs/map/MEASUREMENTS.md:34,448` 仍写
  `CONFIG_USEC_PER_TICK=10000`（10 ms）——当前构建是 **1 ms**。
- `docs/dvfs.md` §10 的验证步骤仍以 72 MHz 为起点，需要按两档改写。
- `sifli_start.c:245-247` 注释说"tickless 用 LPTIM"，但当前实际是 SysTick。

---

## 8. 证据索引

| 工作包 | 内容 | 路径 |
|---|---|---|
| WP-LP1 / R1 | PM/tickless/PMU/链接约束/改动清单/11 条风险 | `docs/agent/evidence/WP-LP1/R1/ANALYSIS.md` |
| WP-LP2 / R1 | 运行态唤醒源表（26 项）/显示链路/静止态/耗电排序 | `docs/agent/evidence/WP-LP2/R1/ANALYSIS.md` |
| WP-LP3 / R2 | 电源脚清单/GNSS 配置/测量方法/6 场景 | `docs/agent/evidence/WP-LP3/R2/ANALYSIS.md` |
| — | 阶段记忆 | `docs/agent/AGENT_COORDINATION.md`（2026-10-29 段） |
