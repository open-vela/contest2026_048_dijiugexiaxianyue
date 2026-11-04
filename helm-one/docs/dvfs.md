# SF32LB52 HCPU DVFS 设计说明

本文说明 my_vendor 产品的 HCPU 动态调频目标、算法、优先级、配置和验证方法。
实现是面向 UI 延迟的混合 governor，不使用 Vela `CONFIG_PM` / `pm_idle`。
OpenVela PM 管理睡眠状态，本模块只管理运行态 HCLK。

## 1. 设计目标

- 静止和轻负载时尽量停在 72 MHz。
- UI 输入到来时，不等待 CPU load 历史窗口，立即提供足够的首帧性能。
- 持续负载根据真实 CPU 需求自动使用 96、144 或 240 MHz。
- 升频及时，降频保守，避免频繁抖动和 LCD/SD 重配开销。
- 保留启动、地图、充电和关机路径对 240 MHz 的确定性要求。
- 所有状态可通过 NSH 查看，所有人工强制操作可随时恢复自动模式。

## 2. 硬件边界

支持的 HCLK 档位为：

- 72 MHz：S0，最低安全运行档。
- 96 MHz：S0。
- 144 MHz：S0，默认 UI 交互下限。
- 240 MHz：S1，最高性能档。

72/96/144 MHz 都使用 S0 电压域，240 MHz 使用 S1。DLL2 始终保持
288 MHz，使 PSRAM XIP 和 NAND 正常运行。

不允许下降到 48/24 MHz。它们属于 D1/D0 路径，会关闭 XT48/DLL2；当前固件
没有像 SiFli 官方低频示例那样先迁移 Flash 时钟和关闭 DLL2，因此进入这些档位
会破坏 PSRAM 代码和数据访问。

## 3. 总体架构

最终目标频率按以下优先级仲裁：

1. `ctl dvfs 72|96|144|240` 的调试 force。
2. `boot/map/charge/off` floor hold，固定要求 240 MHz。
3. 用户交互 boost，默认要求至少 144 MHz。
4. PID0 idle 负载 governor。

force 是调试覆盖，因此即使有交互或 hold，强制档位仍优先。执行
`ctl dvfs auto` 后重新回到混合 governor。

`awake` 只表示屏幕处于唤醒状态，不是 240 MHz floor。亮屏但静止时仍可下降到
72 MHz。

## 4. 负载 governor

### 4.1 负载来源

每 250 ms 读取一次 `clock_cpuload(0)`。PID0 就是 `ps` 中的 IDLE 线程，因此
governor、`ps` 和 `sys` 使用同一口径。当前 cpuload 时间常数约为 2 秒，它适合
判断持续负载，但无法预测第一次按键带来的页面创建和重绘。

### 4.2 容量预测

先把当前频率和 idle 换算为等效 CPU 需求：

```text
busy_percent = 100 - idle_percent
need_mhz     = current_mhz * busy_percent / 100
```

升频侧预留 20% idle：

```text
up_min_mhz = need_mhz * 100 / (100 - 20)
```

降频侧要求低档仍预留 30% idle：

```text
down_min_mhz = need_mhz * 100 / (100 - 30)
```

计算结果向上取整到 72/96/144/240 MHz 档位。20% 与 30% 构成 Schmitt 回差：
升频条件比旧策略宽松，降频条件更保守。

例如 72 MHz、idle 17% 时，等效需求约 59 MHz。为了保留 20% idle，需要约
74 MHz，因此 governor 选择 96 MHz。旧的 5% 阈值只有 idle 低于约 5% 才会
升档，日志中 idle 6% 仍停在 72 MHz 正是由此造成。

### 4.3 升降档时序

- 升档立即更新，可根据需求向更高档位移动。
- 降档候选必须连续稳定 2000 ms。
- 每次最多下降一档，例如 144 → 96 → 72。
- 候选负载恢复或用户再次交互时，重新开始降档计时。

逐档下降避免从 240 MHz 直接跌到 72 MHz，也减少频繁重配 SysTick、SD 和
LCDC 的风险。

### 4.4 worker 心跳与自愈

**整个调频体系只有一个 worker。** ondemand、charge hold、hop 全都挂在 `gov_worker`
这一个**自我重挂**的 work 上。它的调度方式是"干完这一拍，重新把自己投递回队列"，
所以**一旦某次重挂失败，调频会永久静默停摆** —— 没有报错、没有日志，
现场表现就是用户报了很久的那个"**跑久了 DVFS 就不调频了**"。

改造前这条路径完全不可观测。现在有两层：

**第一层：心跳。** `gov_worker()` 每次运行都更新 `g_gov_last_ticks` 并累加 `g_gov_runs`，
可用 `sf32lb_dvfs_gov_stat(&age_ms, &runs)` 读出。正常情况下 `age` 应该是**个位数毫秒**；
如果它一直很大且不再变小，就是 worker 已经死了。

**第二层：检查式重挂 + 自愈。**

```c
if (work_queue(...) < 0) {
    syslog(LOG_ERR, "dvfs: gov re-arm FAILED %d after %lu runs, self-heal retry\n", ...);
    work_cancel(...);              /* 清掉可能的残留状态再重投 */
    if (work_queue(...) < 0) {
        syslog(LOG_ERR, "dvfs: gov self-heal FAILED %d; diag must kick\n", ret);
    }
}
```

**第三层：diag 兜底。** diag 的 **DVFS 槽**盯着 `age`，超过 `SF32LB_DVFS_GOV_STALE_MS`
就判为不健康，调用 `sf32lb_dvfs_gov_kick()` 外部把 worker 接回来。
恢复动作会留下一条 `LOG_WARNING`：

```text
diag: dvfs unhealthy
diag: dvfs restart (unhealthy N ms, n=K)
dvfs: gov re-armed by diag (recoveries=1)
```

`sf32lb_dvfs_gov_recoveries()` 返回"被外部接回来"的累计次数，**正常应恒为 0**；
它增长就说明第一、二层漏掉了某次停机。

现场已经复现并自愈过一次：governor 在 **142 s** 处停摆，
`diag: dvfs unhealthy` → `diag: dvfs restart` → `dvfs: gov re-armed by diag (recoveries=1)`，
全程无需重启。

> 这印证了用户长期报告的"约 2000 s 后 DVFS 停止"确实存在，而且是**重挂失败**，
> 不是负载判断出错。

## 5. 交互 boost

CPU load 是历史值。只靠 ondemand governor，第一次切页已经卡顿后才能观察到
busy 上升。因此物理按键增加独立的交互快路径：

1. KEY1、KEY2 或 PWR 的按下/释放边沿调用 `sf32lb_dvfs_interact()`。
2. HCLK 立即提高到至少 144 MHz。
3. boost 截止时间刷新为当前时间后 1000 ms。
4. governor 档位同时抬到至少 144 MHz，并清除已有降档候选。
5. 连续按键持续刷新期限；停止操作后按正常降档规则逐档回落。

按下边沿保证声音反馈和长按检测及时；释放边沿重新刷新期限，覆盖实际页面回调
及随后的 LVGL 创建、布局和重绘。

如果 144 MHz 下持续 idle 仍低于 20%，负载 governor 会继续升到 240 MHz。
因此交互 boost 只是最低性能保证，不会限制重负载的最高档位。

## 6. 240 MHz floor hold

- `boot`：bringup 设置，bicycle UI 接管后释放。
- `map`：地图瓦片泵开始时设置，结束时释放。
- `charge`：ETA9184 正在充电时设置，拔线或充满后释放。
- `off`：关机动画和存盘开始时设置。

MTP 不直接控制 DVFS。USB 连接并充电时由 `charge` hold 提供 240 MHz。

所有 hold 使用位图，可叠加；只有对应位全部释放后，频率才重新由交互和负载
governor 决定。

## 7. 切频过程

切频通过 `HAL_RCC_HCPU_ConfigHCLK()` 完成，并在临界区内失效 I-Cache。成功后
依次重配：

- SysTick。
- SD 时钟分频。
- LCDC 时钟。
- NV3031A 背光 PWM 占空比。

背光已经关闭时不会因切频重新打开。GNSS USART2 和 SPI LCD 外设时钟来自
48 MHz peripheral clock，不直接跟随 HCLK。

所有 governor 状态由 `g_lock` 保护。tick 截止时间使用有符号差值比较，可正确
处理 32 位 tick 回绕。

### 7.1 SD 时钟必须"夹住"再切

SD 的时钟是从 HCLK 分频来的，所以切频期间**卡上的时钟会跟着抖**。
顺序因此是**三段**，而不是简单地在切完之后重设分频：

```c
sf32lb_sd_clk_hold();                 /* 1. 取总线锁 + 停 SDCLK（SD_CLKCR_STOP_CLK） */
HAL_RCC_HCPU_ConfigHCLK(gear);        /* 2. 切 HCLK，此时卡上没有时钟 */
sf32lb_sd_reclock();                  /* 3. 重设分频；它会自动放钟并解锁 */
```

为什么不能省掉第 1 步：只靠第 3 步补设分频的话，从"HCLK 已经切了"到"分频被更新"
之间存在一段窗口，卡上跑的是**用旧分频去除新 HCLK** 得到的时钟，
实测这个窗口能被文件系统占住 `g_sd_lock` 拖到 **4.86 s**。

**失败路径也必须补 `reclock()`**：`ConfigHCLK` 返回失败时时钟无法确定处于什么状态，
所以失败分支同样调 `reclock_peripherals()`，保证 SDCLK 不会被留在停住的状态。

> 相关坑：`HAL_SDMMC_CLK_SET(hsd, div, en)` 里 `en=0` 置的是 `SD_CLKCR_STOP_CLK`
> （bit 0 **为 1 才是停**），写 `CLKCR = 0` 只是复位分频器。详见
> [sd_recovery.md](sd_recovery.md) §3.2。

注意切频**不是**首次 SD 故障的原因：三次在失败瞬间采样 HCLK 都**没变**
（见 [sd_recovery.md](sd_recovery.md) §1.3）。这里的夹持是为了消除一个
**独立的**窗口，不是为了治那个根因。

## 8. Kconfig 参数

- `CONFIG_MYVENDOR_DVFS`：启用产品 DVFS。
- `CONFIG_MYVENDOR_DVFS_IDLE_UP=20`：预计 idle 低于该值时升档。
- `CONFIG_MYVENDOR_DVFS_IDLE_DOWN=30`：低档必须保留的 idle，必须大于 UP。
- `CONFIG_MYVENDOR_DVFS_INTERACTIVE_MHZ=144`：交互频率下限；非标准档向上取整。
- `CONFIG_MYVENDOR_DVFS_INTERACTIVE_MS=1000`：每次输入边沿刷新后的 boost 时长。
- `CONFIG_MYVENDOR_DVFS_DOWN_HOLD_MS=2000`：同一低档候选的稳定等待时间。

nsh defconfig 显式保存这些参数，避免 Kconfig 默认值变化导致产品行为漂移。

### 8.1 工厂固件不调频

**nsh-factory（工厂固件）把 `CONFIG_MYVENDOR_DVFS` 关掉**：工厂模式不允许调频，
即使执行 `ctl dvfs 72|96|144|240` 也不会改变 HCLK。关掉之后：

- `bringup_hclk240()` 走 `#else` 分支：`BSP_Board_EnableHclk240()`
  （内部 `HAL_RCC_HCPU_ConfigHCLK(240)`）把 HCLK **固定钉在 240 MHz**，
  并自行 `sifli_systick_reclock()` / `sf32lb_lcd_reclock()` / `sf32lb_sd_reclock()`；
  governor 完全不启动，频率此后不再变化。
- `sf32lb_dvfs.h` 编译成空实现桩：governor、交互 boost、hold 全部失效。
  `ctl dvfs` 报 `ctl dvfs: CONFIG_MYVENDOR_DVFS is off` 并返回失败；
  `sys dvfs` 无输出。
- diag 的 dvfs 槽同样被 `#ifdef` 去掉，不会把"没有 governor"误判成"调频停摆"。
- 切频路径（含 §7.1 的 SD 时钟夹持）整体不参与，工厂镜像少一类时钟风险源。

## 9. NSH 诊断

```text
sys dvfs
ctl dvfs auto
ctl dvfs 72
ctl dvfs 96
ctl dvfs 144
ctl dvfs 240
```

`sys dvfs` 示例：

```text
dvfs HCLK 144 MHz auto idle 56% gov 144 boost 144/780ms holds awake (0x2) dll2 288000000
```

字段含义：

- `HCLK`：硬件当前实际档位。
- `auto/force`：是否处于人工强制模式。
- `idle`：最近一次 PID0 idle 样本。
- `gov`：负载 governor 当前档位。
- `boost`：交互频率下限和剩余时间；`0/0ms` 表示已结束。
- `holds`：当前 hold 名称与位图。
- `dll2`：DLL2 当前频率，正常应保持 288000000。

频率实际变化时也会输出 `dvfs: HCLK=... idle=... gov=... boost=...` 日志。

`ctl dvfs` 会额外打印 **worker 心跳**，用来判断调频是否还在跑（见 §4.4）：

```text
dvfs gov last run 3 ms ago, 12345 runs, 0 recoveries
```

`age` 个位数毫秒、`recoveries` 恒为 0 是正常态。`age` 持续偏大说明 worker 已停摆；
`recoveries` 增长说明第一、二层的重挂/自愈漏掉了某次停机，靠 diag 兜回来的。

## 10. 验证方法

### 10.1 交互响应

1. 执行 `ctl dvfs auto`，等待 HCLK 回落到 72 MHz。
2. 按 KEY1 切页。
3. 确认页面回调开始前已经出现 144 MHz 切频日志。
4. 连续切页时确认 boost 期限不断刷新，不回落到 72 MHz。

### 10.2 持续负载

1. 在 72 MHz 下制造持续 UI 或计算负载。
2. idle 低于 20% 时应自动选择至少 96 MHz。
3. 144 MHz 仍不足时应继续到 240 MHz。

### 10.3 降档

1. 停止交互和高负载。
2. 确认同一低档候选稳定约 2 秒后才下降。
3. 确认每次只下降一档，最终回到 72 MHz。

### 10.4 优先级

- force 模式应覆盖交互 boost 和 hold。
- map/charge/off hold 应固定在 240 MHz。
- `ctl dvfs auto` 应恢复自动仲裁。
- 屏幕亮且空闲时允许回到 72 MHz。

## 11. 调参原则

- UI 首帧仍卡顿：优先提高 `INTERACTIVE_MHZ` 或延长 `INTERACTIVE_MS`。
- 连续负载升档太慢：提高 `IDLE_UP`，不要先缩短降档等待。
- 频率抖动：提高 `IDLE_DOWN` 或延长 `DOWN_HOLD_MS`。
- 功耗偏高：先缩短 `INTERACTIVE_MS`，再评估降低交互档位。
- `IDLE_DOWN` 必须大于 `IDLE_UP`，建议至少保留 10 个百分点回差。

## 12. 源码位置

- `chips/sf32lb52/sf32lb_dvfs.c`
- `boards/sf32lb52/helmone/include/sf32lb_dvfs.h`
- `boards/sf32lb52/helmone/ui/bicycle/src/Vendor/Board/lv_port/lv_port_buttons.c`
- `chips/sf32lb52/Kconfig`
- `boards/sf32lb52/helmone/configs/nsh/defconfig`
- `boards/sf32lb52/helmone/configs/nsh-factory/defconfig`

---

日期：2026-09-16。

- 2026-09-16：补 §4.4 **worker 心跳与自愈**（重挂失败会导致调频永久静默停摆，
  这是"跑久了不调频"的机制），diag DVFS 槽兜底，`ctl dvfs` 增加心跳行；
  补 §7.1 **SD 时钟夹持**（切 HCLK 前 `clk_hold()`、之后 `reclock()`，
  失败路径也要 `reclock()`，因为文件系统可能占住总线锁把窗口拖到 4.86 s）。
- 2026-09-16：新增 §8.1 **工厂固件关闭 DVFS**：nsh-factory 不允许调频，
  `ctl dvfs` 失效、HCLK 固定 240 MHz（infra 本来就留好了 `#else` 分支和空实现桩）。
  顺带修掉 `sf32lb_dvfs.h` 桩函数里用了 `NULL` 却没包含 `<stddef.h>` 的问题 ——
  它使 DVFS-off 组合根本无法编译，说明该路径此前从未被编译过。

相关：[sd_recovery.md](sd_recovery.md)、[diag.md](diag.md)、
[system_states.md](system_states.md)。
