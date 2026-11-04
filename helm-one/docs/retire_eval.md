# 抽换件/驱动"能否恢复上游"的评估计划（分支 `retire-eval`）

> 缘起（2026-09-24 用户）："可以建立一个分支，以计数器的形式还原各类驱动，评估是否能恢复"。
> 依据：当天建成的计数器框架（`ctl cnt`，见 `myvendor_diag.c` 的 `g_cnt_mods[]`）——
> **周期报表可关、异常一律 ERROR 关不掉**，正好当"能否恢复"的客观判据。
>
> 本文件的基线版本 = 分支起点 `master@29ca5c83`（当日全部修复），板上对应固件
> `build_date 20260924121201`（ELF 归档 `my_vendor+nsh+20260924121201+bde982f0`）。

## 0. 方法（一次只动一项）

1. 在本分支上**一次只恢复一项**，单独 commit（commit message 写清"恢复什么/判据/观察窗"）。
2. **判据只用计数器与崩溃账**（见 §3），不用"感觉变好了"。
3. **观察窗**：至少 30 分钟 + 至少一次相关触发（见每项"触发"栏）。
4. 失败即 **`git revert` 该 commit** 并重新烧基线固件；每项都记进 `zcode/analysis/findings.md`。
5. 恢复"防崩溃类"（C 类）**必须先有触发器**：没有触发的"没崩"不算证据（本项目栽过）。

## 1. A 类：把芯片驱动恢复成 SDK 自带那份（收益最大）

判据对照表（我们 vs `vendor/sifli/chips/sf32lb52/` 的同名文件，`diff` 计行）：

| 驱动 | SDK 行 | 我们行 | 差异行 | 在跑什么 |
|---|---|---|---|---|
| `sf32lb52_bt_adapter.c` | 985 | 2094 | **1229** | BLE 控制器（邮箱/IPC、LCPU、HCI skip、force reset） |
| `sifli_uart.c` | 1019 | 1774 | **951** | USART（DMA 环形 + IDLE + harvest/recover） |
| `sf32lb52_bth4.c` | 691 | 1182 | **617** | BLE HCI 传输楔子 |
| `sf32lb_adc.c` | 444 | 588 | 348 | 电池 ADC |
| `sf32lb_usbdev.c` | 2217 | 2326 | 233 | USB（本板策略关着） |
| `sifli_oneshot.c` | 818 | 914 | 96 | tickless/oneshot |
| `sifli_allocateheap.c` | 188 | 253 | 83 | 堆划分 |
| `sf32lb_timer.c` | 383 | 452 | 71 | 定时器（含 64 位除法修复） |
| `sf32lb_rtc.c` | 466 | 523 | 65 | RTC |
| `sf32lb52_lcpu_boot.c` | 221 | 271 | 58 | LCPU 引导 |
| `sifli_start.c` | 233 | 265 | 58 | 启动/时钟 |
| `sf32lb_iwdg.c` | 262 | 307 | 57 | 独立看门狗 |
| `sifli_i2c.c` | 719 | 733 | 54 | I2C（IMU/气压计） |
| `sifli_lowput.c` | 78 | 81 | 49 | 控制台底层字符输出 |
| **零差异** | — | — | **0** | `sf32lb_flash.c` / `sf32lb_pwm.c` / `sifli_gpio.c` / `sifli_irq.c` / `sifli_spi.c` |

**A1（建议先做）= `sifli_uart.c`**：数据面已切到"直连环 + 信号量"，DMA/IDLE 的
**恢复**逻辑是否还需要，正好可以用计数器直接判。

- 恢复内容：用 `vendor/sifli` 那份替换，只保留**一行**板级钩子
  （`sifli_uart_rx_put()` 里按 `Instance == USART2` 调 `myvendor_gnss_uart_feed()`）。
- 判据（观察窗 ≥30 min，触发：GNSS 正常收数 + 至少一次 VCC/重连）：
  `gnss_uart: DRIVER=direct cnt` 的 **`drop=0`**、**`feed` 持续增长**；
  `gnss: cnt link` 的 **`bad=junk=0`**；`[ ERROR]` 计数不新增（尤其 `rx ring overflow`）。
- 风险：SDK 版是按**纯中断 RX** 写的（`uart_rx_dma_flag=0` 时走 `UART_IT_RXNE`），
  换成它可能丢掉 DMA 环形 ⇒ 必须在**高波特率/突发**下看 `drop`。

**A2 = `sf32lb52_bth4.c`**（617 行差异）：判据 = `bt rx chain cnt` 的
**`rearm=0 qfail=0`** + `pending=0`、`diag: ble not ok` 0 次、手机链路稳定。
**A3 = `sf32lb52_bt_adapter.c`**（1229 行差异，今天两处修复就在里面）：最后做，
且只恢复"能明确对应某个已消失症状"的段落，整文件回退等于把 skip 闩锁与开机窗口问题请回来。

## 2. B 类：抽换件里的"行为放宽"（可以试）

| 项 | 症状 | 恢复后看什么 |
|---|---|---|
| `zblue/patch_id_scan_addr.py` | 外设链路在时 `LE Set Random Address` 被拒 ⇒ 观测者起不来 | 传感器 `st=4`、`[scan] observer on`、`HR connect failed` 计数 |
| `zblue/patch_net_buf_timeout.py` | `K_FOREVER` 分配不可中断 ⇒ 控制器静默时线程卡死 | `bt stall`/`rxage`、`myvendor net_buf: pool … giving up` 计数 |
| `mtd/ftl.c`（仅 9 行差异，**未读**） | 未知 | 先读 diff 再定 |

## 3. C 类：抽换件里的"防崩溃"（**先不做**）

`sched/wd_start.c` 401 行、`sched/wd_cancel.c`、`sched/sem_post.c` 195 行、`sched/sem_waitirq.c`、
`sched/sched_removereadytorun.c`、`zblue/patch_scan_stop_null.py`、`patch_conn_ready_null.py`、
`patch_gatt_ccc_pool.py`、`patch_hci_core.py`、`bluetooth/sal_*` 五个、`fs/*` 两个、`freetype/ftsystem.c`、
`usbdev/*`、`zblue/init.c`+`long_wq.c`+`system_work_q.c`、`sf32lb52_sdio.c` 的 SD 复位修复……

这些每一项都能指向**一次真实崩溃或断言**（见 `docs/pitch/overrides.md` 的逐文件头注释与
`zcode/analysis/` 的取证）。恢复它们 = 把崩溃请回来，**只有触发器出现时才谈得上评估**；
在此之前写"能恢复"就是拿"没崩"当证据 —— 明确禁止。

## 4. 判据基线（今天在板上实测，作为对照）

```
sf32lb52 bt rx chain cnt: pend=0 ind=2940 drain=2893 rearm=0 qfail=0 preopen=0 pending=1 running=1 avl=1 irq_en=1 rxage=40 ms
gnss_uart: DRIVER=direct cnt rx=1794792 feed=4662 drop=0 wait=4398 eintr=1 tx=17216 txerr=0
gnss: cnt link rd=31 byte=3586 nmea=120 bad=0 ubx=0 pvt=0 junk=0 pub=100 wr_fail=0 gap_max=18999
diag: cnt notif rx=6 merged=0 throttled=1 evicted=0
diag: cnt isrprint console-chars-in-isr=0
```
（uptime 2158 s，`build 2026-09-24 12:11:59`）
