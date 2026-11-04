# 看门狗定时器链表损坏与中断优先级修复

**结论**：本板的 `wdog: clipped` 族来自定时器链表 `g_wdactivelist` 被异步改写。
NuttX 用 BASEPRI 保护该链表，而 SiFli HAL 把 DMA 中断和 BLE 邮箱中断设在了
BASEPRI 可屏蔽范围之外；两者的回调都会走到 `nxsem_post` → `wd_cancel/wd_start`，
因此可以在 `enter_critical_section()` 中途插入，把链表链接写坏。

2026-10-19 的 `0835` 镜像在 `vendor/HelmOne` 中把这两类中断的优先级钳到
BASEPRI 档位；同版上板 31,965 秒（8.9 小时）内 `wdog: clipped`、`stop feed`、
DVFS 恢复、BT RX 停顿均为 0。`0747`（未钳制）曾在上板 20 分钟内出现 2 次 clip。

## 症状与现场

`0747` 的两次事件（板时 1055.98 s / 1256.27 s）：

```
wdog: clipped corrupt g_wdactivelist (1) bad=... rescued=5
wdog: clip why=0x40 ...
wdog: clip dropped pid=4 iwdg_feed / pid=12 board_senso / ...
diag: dvfs unhealthy
diag: dvfs restart (unhealthy ~2100 ms, n=3/4)
sf32lb52 bt rx chain stalled ... avl=0
sf32lb52 bt rx chain: kick wqueue ... ret=0
```

- 每次 clip 后约 2–4 秒出现 DVFS 恢复，随后 BT RX 链停顿；`kick wqueue` 能救回。
- 板子没有复位，也没有 `[wdt] stop feed`；也就是说，`wd_start.c` 的裁剪/rescue
  把损坏变成了“可存活”，但没有消除写坏链表的人。
- `bad` 节点曾在 `board_sensor` 与 `g_lpwork` 的静态定时器之间轮换；
  `prev=20008858` 就是 `g_wdactivelist` 表头本身。

## 机制

本板 `__NVIC_PRIO_BITS = 3`，CMSIS 的 `NVIC_SetPriority()` 会把无符号优先级
左移 5 位写入 NVIC；NuttX 的 `NVIC_SYSH_DISABLE_PRIORITY` 在本构建中是 `0x80`。

| 中断 | 原优先级 | NVIC 值 | 与 BASEPRI 0x80 |
|---|---|---|---|
| UART2 RX / SDMMC1 等 DMA | `0`（`dma_config.h`） | `0x00` | 可越过 |
| BLE 邮箱 `LCPU2HCPU` | `3`（`ipc_hw.c`） | `0x60` | 可越过 |

危险路径有两条：

1. DMA 完成 → `uart_datareceived` → `nxsem_post` → `wd_cancel` / `wd_start`
2. BLE 邮箱 → HPWORK 队列 → `wd_start` / `wd_cancel`

只要中断在 `wd_start_abstick()` / `wd_cancel()` 的临界区里到达，链表就可能被
半截摘链、重复插入或写入过期邻居指针；下一次 `wd_expiration()` 或 `wd_insert()`
走到坏节点时触发 `clip`。这也是为什么 clip 的下游总是 DVFS 定时器和 BT RX
队列定时器——它们和任务 waitdog 共用同一张 `g_wdactivelist`。

## 修改

### 1. DMA 中断优先级钳制

文件：`chips/drivers/hal/bf0_hal_dma.c`（`DMA_AllocChannel()`，约 302 行）

```c
uint32_t irq_prio = hdma->Init.IrqPrio;
#ifdef __NuttX__
/* DMA callbacks can wake NuttX waiters (UART RX -> uart_datareceived
 * -> nxsem_post -> wd_cancel). Priorities above BASEPRI can preempt
 * enter_critical_section() and mutate the watchdog list midway.
 * CMSIS takes an unshifted priority; NuttX's threshold is raw. */
const uint32_t min_kernel_prio =
    NVIC_SYSH_DISABLE_PRIORITY >> (8U - __NVIC_PRIO_BITS);

if (irq_prio < min_kernel_prio)
{
    irq_prio = min_kernel_prio;
}
#endif
NVIC_SetPriority(irq_type, irq_prio);
```

`0x80 >> (8 - 3) = 4`，即 DMA 中断最低被抬到优先级 4（NVIC `0x80`），
正好落在 BASEPRI 的屏蔽范围内。

### 2. BLE 邮箱中断优先级钳制

文件：`chips/sf32lb52/ipc_queue/porting/os/vela/ipc_os_port.h`
（`os_interrupt_start()`，约 30 行）

```c
uint32_t encoded = NVIC_EncodePriority(NVIC_GetPriorityGrouping(),
                                       priority, sub_priority);
const uint32_t min_kernel_prio =
    NVIC_SYSH_DISABLE_PRIORITY >> (8U - __NVIC_PRIO_BITS);

/* The mailbox callback can queue HPWORK, which starts/cancels watchdogs.
 * It must not preempt NuttX's BASEPRI critical sections. */
if (encoded < min_kernel_prio)
{
    encoded = min_kernel_prio;
}

NVIC_SetPriority(irq_number, encoded);
```

这里必须钳制 **编码后** 的值，因为 `HAL_NVIC_SetPriority()` 走的是
`NVIC_EncodePriority()`；直接比较未编码的 `priority` 会把分组偏移算错。

### 3. 同批的防御性修改（不是根因修复）

| 文件 | 修改 | 作用 |
|---|---|---|
| `bsp/myvendor_watchdog.c` | `cpu_ok = idle || work || ui`；UI 心跳也算 CPU 活着 | 防止可运行任务长期占住 CPU、idle/work 不推进时误停喂狗；持续异常时打 `idle/work stale but ui alive` |
| `vela_override/sched/wd_start.c` | 摘链/裁剪前校验双向链接；`INSERT LINK BAD` 与裁剪报告移到 diag 任务上下文 | 损坏后能安全裁剪/rescue 并留下现场，避免在临界区或 ISR 里 `syslog` |

`wd_start.c` 在 `0747` 里已经存在，不能阻止 clip；它只是把故障变成可恢复、
可取证。真正减少 clip 的是上面两条中断优先级钳制。

## 官方 SiFli 代码状态

| 副本 | DMA 优先级 | 邮箱优先级 | 是否修复 |
|---|---|---|---|
| `vendor/sifli`（HEAD `cc164c9`，blob `61f45e32…`） | `NVIC_SetPriority(irq_type, hdma->Init.IrqPrio)` | `HAL_NVIC_SetPriority(irq_number, priority, sub_priority)` | 否 |
| `vendor/vela_sifli`（参考树 `trunk-5.5`） | 同上 | 同上 | 否 |
| `vendor/HelmOne`（`fc64ee8c`，blob `be35d92f…` / `7bbd0015…`） | BASEPRI 钳制 | 编码后钳制 | **是** |

- 官方 `dma_config.h` 里 UART2/SDMMC1 等 DMA 优先级都是 `0`，邮箱调用固定传 `3`。
- 因此**在 openvela/NuttX 下，官方 SiFli 板级树也存在同样的代码级条件**；
  是否现场复现取决于它是否启用 UART DMA RX、BLE 邮箱以及中断时序。
- SiFli 原生 SDK 跑 RT-Thread/FreeRTOS，临界区语义不是 NuttX 的 BASEPRI，
  所以同一份 HAL 在原生 SDK 里不一定构成问题。这个修复因此保留 `#ifdef __NuttX__`。
- 产品构建只编译 `vendor/HelmOne/chips`（`.config` 的
  `CONFIG_ARCH_CHIP_CUSTOM_DIR="../vendor/HelmOne/chips/sf32lb52"`），
  官方两份副本不参与本镜像链接。

## 上板证据

| 镜像 | 中断钳制 | 观察窗口 | 结果 |
|---|---|---|---|
| `202610190747` | 无 | 20 分钟 idle | 2 次 `wdog: clipped`，2 次 DVFS restart，2 次 BT RX stall |
| `202610190835` | 有 | 41.4 分钟 idle | 0 clip / 0 stop feed / 0 DVFS / 0 RX stall |
| `202610190835` | 有 | **31,965 秒（8.9 h）** | 上述标记全部为 0；无复位、无 HardFault |

`0835` 的外设回归：CPS 复位 5 次有效断连、GATT 全断 5 轮、GNSS 断电 5 轮、
三者同时 5 轮，全部恢复；SD `seq` 1 MiB 与 `chunk` 64 KiB 均 PASS，SDDIAG 全 0。

## 边界

- 这是概率性竞态；`0835` 的 32k 秒单 boot 无事件是强证据，但不是隔离 A/B
  实验。要证明就是这两条钳制，需要单独回退其中一条再跑长测。
- 本轮没有复现 n028/n029 的 idle/work 同时过期窗口，`idle/work stale but ui alive`
  兜底分支仍未被实际命中。
- `wd_start.c` 的防御性检查继续保留；即使中断优先级修正后，它仍是链表被其他
  路径写坏时的最后一道保护。

## 相关

- `vela_override/sched/wd_start.c` 的 clip / rescue 现场格式
- `docs/diag.md` 的 `/mnt/kv/diag` 取证口径
- `docs/pitch/README.md` 的上游补丁规范（本修复若要提交给 SiFli/Vela 上游，
  需按该规范附 git id、blob 与改动前后代码）
