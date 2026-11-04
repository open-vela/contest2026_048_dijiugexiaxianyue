# GNSS 速度：来源、失效模式与过滤方案（实施计划）

**日期：2026-09-17。** 设计依据来自本轮现场数据 + 业界惯例检索（u-blox 官方论坛关于
`gSpeed`/`velN`/`velE`/`sAcc` 的说明、多普勒 vs 位置差分的对比、以及自行车/商用定位设备的
静止判定与距离门实践）。

**进度**：**步骤 1 已实施**（"静止门 + 显示归零"，见 §3.1 的实现记录）；**步骤 2 的探针
已实施**（UBX-NAV-PVT 到没到，见 §3.2 的实现记录）；步骤 2 的解析、步骤 3/4 待做。

相关：[gnss_ble_diag_log.md](gnss_ble_diag_log.md)（`link`/`reader gap` 等字段含义）、
[ble/ble_sensor.md](ble/ble_sensor.md)（BLE 侧，不相关但同属板上诊断）。

---

## 1. 现状（2026-09-17 核对结果）

| 项 | 位置 | 说明 |
| --- | --- | --- |
| 速度源开关 `g_gnss_rmc` | `ctl/myvendor_devctl.c:77`，KV 键 `MYVENDOR_DEVCTL_GNSS_RMC_KEY` | **默认 1**；**true = 信模块的多普勒 SOG**，false = 走 UI 里的自定义滤波 |
| 开关语义的包装 | `ui/bicycle/src/bicycle_runtime.c:237`（`rt_trust_rmc()`）、`:1402`（`bicycle_runtime_gnss_solver_rmc()` / `_set_...()`） | 设置项切换时会 `rt_filt_reset()` |
| 自定义滤波（位置差分） | `bicycle_runtime.c` `rt_spd_push()`：`dc = cum_m - spd[last].cum_m` / `dt` | `RT_SPD_N=40` 环、`RT_SPD_CUM_EPS_M=0.05 m`（**太小，形同虚设**）、`dt<150 ms` 合并 |
| HDOP | `myvendor_gnss.c:3767` GGA → `fix->hdop_x10`；经 `rt->gnss_hdop_x10`（`bicycle_status_bar.c:128`）到 UI | **已解析、已到 UI** ✅ |
| 质量分档函数 | `bicycle_runtime.c:338` `rt_conf_t rt_conf_of(prev, q, hdop_x10, valid)` | 按 HDOP ≤9 / ≤12 / ≤18 / ≤22 分高/中/低 —— **架子现成，但没拿它门控速度** |
| sAcc / hAcc | 无 | NMEA 不提供，需 UBX-NAV-PVT |

**注意**：`myvendor_devctl.c:77` 的注释写的是"默认自定义滤波"，与代码语义（true = 信 RMC，即
多普勒）**相反**，顺手修掉。

## 2. 两条裸值都不能用（现场结论）

- **多普勒 SOG**：物理正确、低延迟、不依赖几何；但**弱信号（低 C/N0）时噪声大** ——
  实速 ~20 km/h 时读数在 **17↔25 之间跳**（用户现场数据）。且 **NMEA 的 SOG 是未滤波的**，
  u-blox 的 `UBX-NAV-PVT.gSpeed` 才是滤波过的。
- **位置差分**：有速度时相对平滑；但**静止/低速时彻底崩** —— 1 Hz 下 ±2~5 m 位置噪声 ÷ 1 s
  = **7~18 km/h 假速度**，现场表现为"静止时 10 km/h 跳"，并在 **3D 质量高→低跳变**时冒出。

→ 结论：**不是"选哪条"，而是"门控 + 平滑 +（可选）融合"**。

## 3. 实施方案

### 步骤 1：显示层（用现有 HDOP，立刻见效）

**✅ 已实施（2026-09-17）** —— 落点与做法（`ui/bicycle/src/bicycle_runtime.c`）：

| 改动 | 位置 | 说明 |
| --- | --- | --- |
| 新增静止门常量 | `:86-88` | `RT_STILL_ENTER_M 3.0f` / `RT_STILL_EXIT_M 8.0f` / `RT_STILL_HOLD_MS 4000u`（半径 + 双阈值迟滞，取自"位置停在半径内够久即判停"的行业惯例） |
| 新增 `rt_still_gate()` / `rt_still_reset()` | `:249` / `:279` | 自锚点累计位移 < ENTER 且持续 HOLD → 判静止；>= EXIT（迟滞）→ 退出并重新锚定 |
| **补上缺失的静止门** | `:1055` | `g_filt.cum_m`（位移窗的输入）原来**完全没有静止门**，噪声直接累进里程 → 这是"静止 10 km/h"的根因 |
| 显示路径门改用同一个判定 | `:665` | 原来 `dist < RT_FILT_STILL_M`（**0.8 m**，远小于 ±2~5 m 的位置噪声，形同虚设） |
| **静止时显示归零** | `:883` | 在 slew 之前覆盖 `out = 0`，治"3D 高→低跳变时不动也有几 km/h"；离开静止从 0 按 slew 爬升，无阶跃 |
| 复位 | `rt_filt_reset()` 调 `rt_still_reset()` | 切换速度源 / 丢星复位时清干净 |

**仍未做**：中值滤波（3~5 点）—— 它针对的是**运动中的**抖动（20 km/h 读数 17↔25），
而根因更可能在"拿到的是未滤波的 NMEA SOG"，所以放在步骤 2 一起解决更合理。

**原计划（保留备查）**：
在 `bicycle_runtime.c` 的速度输出处（`g_speed_disp` 的赋值点，`:636` 附近是"无定位清零"分支，
主计算在 `rt_spd_push()` 之后）加三层：

1. **中值滤波**：保留最近 **3~5** 个原始速度，输出**中值**而不是均值 —— 对 17↔25 这类尖峰最有效，
   且比平均更不拖延迟（自行车码表/运动手表通用做法）。
2. **质量冻结**：复用现成的 `rt_conf_of()`；当档位为**最低档**（HDOP 差）时，
   **输出上一次可信值**（不跟随测量，也不要置 0）—— 治"3D 高→低跳变"。
3. **静止迟滞**：`v < 3 km/h` 持续 **3~5 s** → 输出 `0`；离开静止需连续 **2~3 次** 超过阈值
   （双阈值，避免在 3 km/h 附近抖）。

### 步骤 2：UBX-NAV-PVT（治本）

#### ✅ 探针已实施（2026-09-17）—— 先只回答"模组到底发不发 PVT"

本轮**不解析** PVT：先打开输出、再在读线程数帧。**配置侧被 ACK** 与**接收侧真有帧**
是两件独立的事（现有 `cfg ack` 只说明前者），所以两边都留了证据。

| 改动 | 位置（`services/myvendor_gnss.c`） | 说明 |
| --- | --- | --- |
| 新增两个配置键 | `GNSS_CFG_MSGOUT_PVT_UART1 = 0x20910007` / `_UART2 = 0x20910008` | 值 `1` = 每个历元输出一帧。UART1 是模组默认输出口（现有 NMEA 就在这个口上），先试它 |
| 开机配置时下发 | `gnss_configure_nav()` | 结果并进 `gnss: cfg ack dyn=… elev=… cno=… spd=… aid=… pvt=%d`：`1` 成功；`-1` = 模组 NAK（键或口不支持）；`0` = 250 ms 内没等到 ACK。UART1 失败会再试 UART2 并单独打一行 `gnss: pvt out uart1=… uart2=…` |
| 读线程数帧 | `gnss_thread()` 的 UBX 状态机（`rx_st` 2→5） | `link` 行新增 `pvt=`：**只统计整帧且校验通过**的 `cls=0x01 id=0x07`。校验和是逐字节增量累加的；倒库路径那份**故意不合并**（那条路是验证过的，探针不该动它） |
| 首帧里程碑 | 同上 | 本会话第一帧 PVT 打一行 `gnss: first pvt len=92`。M10 的 PVT 定长 **92 字节**，别的长度说明固件/协议版本不同 |
| 计数进其它诊断行 | `silence trig`（`win_pvt=`）、`reader exit`（`pvt=`） | 判"模组静默"时能顺手看到 PVT 有没有在流 |

走的是 `gnss_cfg_valset_u1()`（RAM 层，和 dynmodel 等一样）：模组掉电即失效，所以每次
会话开头 `gnss_reconfig_after_vcc()` 都会重发一遍。

**判据（板子上怎么读）** —— 开机、等定位、再看 **1~2 分钟**的 `gnss: link` 行。
`link` 行所有计数都是**窗口增量**（打完即清零），`pvt=` 也一样：

1. `pvt=` **在涨**（1 Hz ⇒ 每 15 s 窗口约 +15；明显偏小也说明在发）→ 模组确实在发 PVT，
   可以进下一步（解析）。
2. `pvt=` **恒 0** 而 `cfg ack … pvt=1` → 配置被 ACK 了但收不到帧：键生效的口不是我们
   在读的那个口（或这颗固件收下了键却没有实际输出）。下一步改试 legacy `CFG-MSG`
   （`0x06 0x01`，payload = class `0x01` / id `0x07` / rate `1`），与现有
   `gnss_enable_zda()` 同一套写法。
3. `cfg ack … pvt=-1`（NAK）→ 这颗固件不认这两个键，同样退回 legacy `CFG-MSG`。
4. `gnss: first pvt len=92` 出现 = "有帧"的最强单行证据；长度不是 92 就先查协议版本。

**✅ 探针结论：模组确实在发 PVT（2026-09-17 现场，build 202609171617）** ——

```
[   4.331] gnss: cfg ack dyn=1 elev=1 cno=1 spd=1 aid=1 pvt=1     ← UART1 键直接 ACK，没走 UART2
[   4.972] gnss: first pvt len=92                                  ← 定长 92 字节，协议版本对
[   9.502] … ubx=5 pvt=5 …     [  24.742] … ubx=5 pvt=5 …          ← 每个 5 s 窗口 +5 ≈ 1 Hz
[  60.722] … ubx=61 pvt=5 …    [  65.877] … ubx=65 pvt=5 …         ← 只有倒库窗口那两行 ubx 被 DBD 帧顶高
```

即：`pvt` 稳定 1 Hz 增长、`ubx` 与 `pvt` 除倒库外完全相等（PVT 是唯一的 UBX 流量）。
**下一步实施解析**（见下），不需要再回退 legacy `CFG-MSG`。

#### ✅ 值也解出来了（2026-09-17，同一轮）—— 先只打日志，不进快照

`gnss_pvt_report()`（`GNSS_PVT_LEN 92`）把关键字段按 u-blox 的偏移解出来打一行，1 Hz，
与 `pub … spd=` 同频、同单位，方便直接比：

```
gnss: pvt fix=3 gnss=1 sv=9 gs=2019 vn=-1 ve=3 sacc=180 hacc=4200 pdop=105
           └ 2D/3D  └ gnssFixOK  └ 用了几颗  └ centi-km/h（= km/h ×100，与 pub spd= 同单位）
                                                  └ sAcc mm/s、hAcc mm、pDOP 0.01（原始单位）
```

- 偏移：fixType 20、flags 21、numSV 23、hAcc 40、velN 48、velE 52、velD 56、gSpeed 60、
  sAcc 68、pDOP 76（protocol 27，载荷定长 92 B）。
- **首帧另打一份十六进制原文**（`gnss: pvt raw <hex>`，分 3 行），用来核对解码对不对 ——
  这就是"想看 raw 值"的直接答案，不必去抢 UART（`test gnss raw` 与读线程共用同一个
  RX FIFO，会被优先级更高的读线程饿着，见 [gnss_ble_diag_log.md](gnss_ble_diag_log.md) §1.8）。
- 都挂在 `CONFIG_MYVENDOR_GNSS_LOG` 下（与 `pub`/`link extra` 同一开关）；关掉时连
  那 92 B 载荷都不收。
- **`gs`/`vn`/`ve` 是原始值，没有过任何 sanity/包络** —— 正是要看这个：跟同一秒的
  `pub … spd=`（未滤波的 NMEA SOG）并排，17↔25 那类抖动到底谁抖、`sAcc` 在弱信号下
  涨到多少，一眼能比。

**接线已做（2026-09-17，build 202609171641）** —— 值不再只是日志：

| 落点 | 位置 | 说明 |
| --- | --- | --- |
| 快照增字段 | `include/myvendor_sys.h` | `speed_pvt_centi_kmh`（gSpeed）、`speed_acc_mm_s`（sAcc）、`pos_acc_mm`（hAcc）、`pvt_fix_type`、`pvt_gnss_ok`、`pvt_valid` |
| 入库 / 贴附 | `myvendor_gnss.c` `gnss_pvt_store()` / `gnss_pvt_attach()` | PVT 帧到达时存原始值（centi-km/h、mm/s）；`gnss_publish()` 里 ≤2.5 s 就贴到这份 fix 上，否则清成无效。**与日志开关无关**，没开 `GNSS_LOG` 也工作 |
| 观测 | `pub` 行 | 新增 `pvt=%d gs=%u sacc=%u` —— 与 `spd=` 同行，直接比"未滤波 SOG ↔ 滤波 gSpeed" |
| 观测（`sys`） | `sys/sys_main.c`、`sys/myvendor_sys.c` | `sys onboard` / `sys gnss`（以及 `sys` 全量 dump 的 `onboard` 行）在速度后追加 `gs … km/h  sacc … m/s  fix … ok …`；**没有 PVT 就不打**（手机那份就没有）。两边共用 `myvendor_sys_gnss_print_pvt()` |
| UI 选源 | `bicycle_runtime.c` `rt_dop_kph()` | **PVT 可用就用 gSpeed**（模组滤波过的多普勒），否则回退 NMEA SOG —— 针对的正是 17↔25 |
| UI 门控 | `rt_conf_of()` 新增 sAcc 输入 | **相对**判据 `sAcc > max(0.6 m/s, 30% × v)` → 判最低档（混合权重 0.10，基本交给位移窗）。只降不升；PVT 不可用时 sAcc=0，门恒不触发，行为与改动前一致 |
| UI 冻结 | `rt_shape_speed()`（`dop_bad && hAcc > RT_POS_BAD_M`） | **两个源都不可信时保持上一次输出**：sAcc 判多普勒不可信、hAcc 又说位置不可信（>10 m）时，位移窗自己的噪声就有几 km/h —— 现场残余的"跳 4 km/h"正是它贡献的（那一程 hAcc 22~47 m）。位置尚可时不动，位移窗仍是更好的源 |
| 手机源 | `sys/myvendor_sys.c` | 手机协议（0xFF14）没有 PVT 这几项：取 fix 前先整份清零，否则未清零栈结构里的垃圾会被当成 `pvt_valid` / `sAcc` |

门槛为什么这么取：sAcc 是 1σ 速度误差，绝对阈值换个速度就不成立，所以用相对判据；0.6 m/s
是低速地板（相对误差在低速必然发散），30% 是"误差不超过读数三成"。现场那批无定位/2D 的帧
（gs 2~31 km/h 乱跳、sAcc 2.7~9.9 m/s）按此必然判低。

**下次骑行要看的**：`gnss: pub … spd=… pvt=1 gs=… sacc=…` —— `spd` 与 `gs` 是同一个历元的
未滤波/滤波两个多普勒，`sacc` 告诉你那一秒的读数能不能信；§5 的恒速验证现在有同屏数据了。

**仍未做**：`velN`/`velE` 低速段替代（PVT 里已经在收，只是还没用）；以及这行 `pvt` 日志
到时候折进 `pub` 或去掉。

**探针期间的已知副作用**（别误判）：

- **实测 `hz=` 没变**（现场 2026-09-17，`link` 行全程 hz=1）：PVT 与 NMEA 属于同一个历元
  突发，在同一个 `read` 批次里到达。只有倒库窗口里会出现 2。判"模组输出率"仍然看
  `byte`/`nmea`，**不看 `hz`**。
- `ubx=` 会跟着 `pvt=` 一起涨（PVT 也进这个计数）。
- **注入窗口内 `pvt` 不计数**：注入（含配置等用 `gnss_ubx_read_frame()` 收 ACK 的短窗口）是
  那条路径自己读串口，不经过读线程的状态机；窗口过去后照常涨。**倒库窗口照常计数** ——
  倒库本来就是读线程状态机在收（`gnss_dbd_on_frame()` 只从那里调）。
- 冷启动时模组可能要先跑起导航解算才发 PVT —— **判"不发"至少要在有定位之后再等一轮**。
- 串口负担可忽略：92 B + 8 B 帧头 ≈ 100 B/s，相对现有 ~800 B/s 的 NMEA。

**下一轮（探针通过后）**：解析这 92 字节到 `fix` 与快照（`gSpeed`/`sAcc`/`velN`/`velE`/
`fixType`/`numSV`），把步骤 1 的质量门从 HDOP 换成 `sAcc` —— 字段表见下。

我们已经在用 UBX 发配置和注入星历（`gnss: cfg ack ...`），通道现成。加一条周期 1 的
`UBX-NAV-PVT`（定长 92 字节），一次拿到：

| 字段 | 用途 |
| --- | --- |
| `gSpeed` | **滤波过的**多普勒地面速度，替换未滤波的 NMEA SOG（预期直接改善 17↔25） |
| `velN` / `velE` | 未滤波分量；按 u-blox 官方建议，**低速段**用它 |
| **`sAcc`** | 速度精度估计（mm/s）→ **步骤 1 的"质量冻结"门从 HDOP 换成 sAcc**（业界认为 sAcc 正是这个用途） |
| `hAcc` / `pDOP` / `numSV` / `fixType` | 位置质量与跳变判据 |

改造点：模块配置表加一条输出 + `myvendor_gnss.c` 加 PVT 解析（与现有 `minmea_parse_*` 并列）
+ `fix` 结构体与快照增字段。**注意**：PVT 是 UBX，不走 NMEA 行解析，要接在 UBX 分帧
（`GNSS_UBX_PAY_MAX 2048`）那条路上。

### 步骤 3（顺手）：里程/轨迹与显示速度解耦

- 距离门阈值提到业界量级：`max(3 m, 30% × v)`（现在 `RT_SPD_CUM_EPS_M = 0.05 m` 太小）；
- **停点归并**：半径 **20~50 m** 内停留够久即判停，把停点塌缩成质心再累加里程 ——
  这才是"用轨迹点算"的正确落点（算里程/运动状态，**不**算瞬时速度）。

### 步骤 4（后备）：1-D 卡尔曼

状态 = 速度；两个观测（多普勒、位置差分）按各自方差加权（方差来自 `sAcc` 与位置精度/Δt）。
1-D 在 MCU 上是几个 float，代价可忽略。**只有 1~3 都做完仍不满足才上。**

## 4. 另有两个配套动作

- **模块输出率**：现场观察从 2 Hz 掉到了 1 Hz（每 5 s 窗口 NMEA 字节 5000→2500、句子 100→70）。
  **速率翻倍，噪声按 √N 下降**，对 17↔25 有直接帮助 —— 顺手确认/恢复。
- **动态模型**：u-blox 的 Stationary / Pedestrian 档会施加零速约束，能压低速抖动；
  本模块 NAK `bike`（已知），值得试它接受的档位。优先级低于上面。

## 5. 验证方法

1. **静止**：放在窗外静置 ≥10 min，`sys`/UI 速度应恒为 0，且期间不应出现 >3 km/h 的毛刺；
2. **恒速**：20 km/h 左右匀速（骑行或转台），显示值抖动应显著小于当前的 17↔25；
3. **质量跳变**：记录 `sys onboard` 里的 HDOP/sAcc 与速度同屏，确认"HDOP 变差时速度被冻结"
   而不是跳变；
4. **里程**：静止 10 min 的里程增量应为 0（当前按 ~40 m/min 的假位移累加）。
