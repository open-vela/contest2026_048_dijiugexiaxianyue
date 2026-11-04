# SD 卡异常与恢复（my_vendor）

**结论先写**：SD 卡"死掉且只能靠断电恢复"这个顽疾，根因**不在卡上，而在 SDIO 控制器里** ——
控制器的命令状态机卡在 `CMD_BUSY`，之后静默拒绝所有命令，现象和"卡哑了"完全一样。
唯一能把它救回来的是**模块级 RCC 复位**（`HAL_RCC_ResetModule(RCC_MOD_SDMMC1)`），
这是从官方驱动 `drv_sdio.c` 的 `rt_hw_sdio_timeout_handle()` 里对齐过来的。
加上它之后，现场第一次出现了 `SD: RECOVERED`，**不需要重启就能恢复**。

本文写"卡异常时会发生什么、我们怎么判、怎么救、怎么验"，以及**还剩下什么没解决**。
分区布局与偏移见 [sd_partition.md](sd_partition.md)；**时钟分支拓扑与"为什么用不了
外部 48 MHz"见本文 §8**；外设看门狗见 [diag.md](diag.md)。

---

## 1. 现象与根因

### 1.1 现象

卡异常时的现场特征（改造前）：

- 任意一次 SD 读写失败后，**后续所有** SD 操作全部失败，没有一次自愈；
- 失败要烧掉 **2~4 秒/块**（4 次重试 + 重识别全走完），把调用它的线程一起拖死；
- 声音层面的连带伤害：`ble_companion` 会阻塞在 SD 上（栈回溯终点是
  `sd1_send_cmd` ← `sf32lb_sd_cmd12`），表现为**蓝牙连不上**；
- 最后只能整机断电，插拔无效（卡是板载焊接的，也没法插拔）。

### 1.2 根因

失败现场的取证行是：

```
SD: CMD17 addr=0x… n=8 FAILED after 4 tries (rca=0x… sr=0x00000001 why=noerr_but_cmd_busy)
```

`sr=0x00000001` 只有 `CMD_BUSY` 这一位，**没有任何错误位**（不是 CRC、不是超时、不是 FIFO 错）。
含义是：控制器认为"上一条命令还在执行"，因此拒绝下发新命令 —— 而且永远不会自己好。

所以这不是数据错误、不是坏块、不是卡进入某种异常状态，而是**控制器自身的状态机停在了一个
自认为合法的中间态**。这也解释了两件事：

1. 为什么"换低速时钟重试""重识别卡"这类手段全都无效 —— 卡是好的，问题是主机端的控制器；
2. 为什么只有整机断电有效 —— 只有那才会复位这个模块。

### 1.3 被证伪的假设

排查过程中有两条假设被**实测**排除，不必再回头捡：

- **"某个坏块/磨损页"**：曾怀疑首次失败总落在 LFS 块 194（偏移 `0xC2000`）。
  `ctl sd read 200c2000 8` 实测 8 个块**全部正常**（约 880 µs）；而另一次记录的首次失败
  偏移是 `0x20000000`（块 0）。"4/4 都落在块 194"从未成立。
- **"DVFS 切频（72 → 240 MHz）打断 SD 时钟"**：三次在失败瞬间采样 HCLK，**都没变**。
  切频确实会让卡上时钟抖动（SD 跟着 HCLK 走，见 [sd_partition.md](sd_partition.md)），
  但它不是首次失败的原因。

---

## 2. 判死闸门：先止血，再治病

根因解决之前，先要阻止"一张死卡拖垮整机"。做法是**判死 + 快速失败**：

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `SF32LB_SD_DEAD_AFTER` | 3 | 连续 3 次重识别失败即判死 |
| `SF32LB_SD_DEAD_RETRY_MS` | 30000 | 判死后每 30 s 尝试硬复位一次 |
| `SF32LB_SD_HARD_DOWN_MS` | 200 | 硬复位时接口保持关闭的时长 |
| `SF32LB_SD_RETRY_CLOCK_HZ` | 6 MHz | 重试/恢复期间用的低速时钟 |
| `SF32LB_SD_IO_RETRIES` | 3 | 单次 I/O 的重试次数（判死后被闸门短路） |

判死后 `sf32lb_sd_dead_fail_fast()` 让所有入口**立刻返回 `-EIO`**，不再等 4 次重试。

> **踩过的坑**：这个闸门最初只装在 `sf32lb_sd_read_block_retry` / `write_block_retry` 上，
> 但 LittleFS 的多块读走的是 **`sf32lb_sd_rt_xfer_retry`**（日志里 `SD: CMD17/CMD18 … FAILED
> after 4 tries` 就是它打的，且没有 `run=` / `clk=` 字段）。结果判死声明发出去了，
> 每块还是要烧 2~4 秒。**闸门必须装在所有会走"4 次重试 + 重识别"的入口上**，
> 三处入口都要覆盖（可用反汇编确认各自调用了一次 `sd1_dead_fail_fast`）。

---

## 3. 硬复位：唯一真正救活的手段

### 3.1 关键的那一行

```c
HAL_RCC_ResetModule(RCC_MOD_SDMMC1);   /* → HPSYS_RCC_RSTR2.SDMMC1 */
```

放在 `sd1_init()` 里，也就是所有重新初始化的**唯一入口**。

官方驱动在**每一次超时**时都做这件事（`drv_sdio.c` 的 `rt_hw_sdio_timeout_handle()`：
`dump_sdio_reg()` → `HAL_RCC_ResetModule(RCC_MOD_SDMMC1)` → `mdelay(1)` → `recov_sdio_reg()`）。

**为什么之前 5 次硬复位尝试全都失败**：那些尝试只关/开了 `ENR2` 的 RCC 门控
（`HPSYS_RCC_ENR2_SDMMC1`），**从来没有复位模块本身**。关时钟再开时钟不会把控制器的
状态机清零。断电之所以有效，只是因为整机复位顺带复位了这个模块。

> 教训：**"这个芯片没有控制器复位"是我得出的错误结论** —— 我在 SD 寄存器块里找，
> 当然找不到；复位在 RCC 里。找不到某能力时，先去看官方驱动怎么用它，再下结论。

### 3.2 关机时序 `sd1_hw_quiet()`

重新初始化前必须先让卡安静下来，顺序不能乱：

```c
hwp_sdmmc1->DCR   = 0;
hwp_sdmmc1->IER   = 0;
hwp_sdmmc1->SR    = 0xffffffff;
(void)sd1_wait_card_idle();
(void)sd1_send_cmd(0, 0);            /* CMD0 需要时钟，必须放在停钟之前 */
HAL_Delay_us(1000);
hwp_sdmmc1->CLKCR = SD_CLKCR_STOP_CLK;
```

> **踩过的坑**：`CLKCR = 0` **并不会停钟**。`SD_CLKCR_STOP_CLK`（bit 0）**为 1 才是停**，
> 写 0 只是把分频器复位成 ÷1 并清掉停钟位。判据来自 `HAL_SDMMC_CLK_SET(hsd, div, en)`：
> `en=0` 时置的就是这一位。

### 3.3 恢复流程与日志

判死状态下每 `SF32LB_SD_DEAD_RETRY_MS`（30 s）尝试一次：
`sd1_hw_quiet()` → `ENR2` 关 `SF32LB_SD_HARD_DOWN_MS`（200 ms）→ `sf32lb_sd_identify()`。

```
SD: hard reset #1 (interface down 200 ms)
SD: re-identify OK (after 1 mute tries)
SD: RECOVERED by hard reset #1 (was dead 30000 ms)
```

现场实测（build `202609161647`，第一个看到 `RECOVERED` 的版本）：判死 → 硬复位 →
`SD card init ok` → 重识别成功 → 完整恢复（星历注入 99 帧、`df` 里两个 LittleFS 都在），
两次日志里复现 4 次以上。

### 3.4 时钟重设 `sd1_set_clock()`

改成**停钟 → 改分频 → 放钟**，而不是直接改写 `CLKCR`：

```c
CLKCR = SD_CLKCR_STOP_CLK;                                    /* 停 */
CLKCR = (div-1)<<DIV_Pos | VOID_FIFO_ERROR | STOP_CLK;        /* 改 */
CLKCR &= ~SD_CLKCR_STOP_CLK;                                  /* 放 */
```

理由：**不要在时钟还在跑的时候改分频**，那会在卡上产生一段宽度不可预测的时钟。

---

## 4. 失败取证：`sr=` 与 `why=`

原来只有一句"FAILED"，看不出是哪种失败。现在两条失败日志都带状态寄存器明细：

```
SD: CMD%u addr=0x%llx n=%lu FAILED after %d tries (rca=0x%04x sr=0x%08lx why=%s)
```

`why=` 由 `sd1_sr_why()` 按优先级从最高的错误位开始判定，取值是一个**常量字符串链**
（刻意不用 `snprintf` 拼装，见 [mimosa 写入门槛](tools/vela_my_vendor_tools.md)）：

| `why=` | 含义 |
| --- | --- |
| `cmd_to` | 命令阶段超时（无响应） |
| `cmd_crc` | 命令 CRC 错 |
| `data_to` | 数据阶段超时 |
| `data_crc` | 数据 CRC 错 |
| `startbit` | 起始位错误 |
| `fifo_ur` / `fifo_or` | FIFO 下溢 / 上溢 |
| `cache_err` | 缓存错误 |
| `noerr_but_cmd_busy` | **无错误位，但控制器卡在 CMD_BUSY**（就是本次的根因签名） |
| `noerr_cmd_done` / `noerr_idle` | 无错误位且状态正常（用于区分"失败但状态已回正常"） |

排查时先看 `why=`：出现 `noerr_but_cmd_busy` 就是控制器又卡住了，
按 §3 的硬复位路径处理；出现 CRC / 超时一类则更像信号完整性问题（时钟太快、走线），
可以考虑降 `SF32LB_SD_RETRY_CLOCK_HZ`。

---

## 5. 文件系统层的处置：拿 FAT 当"卡还活着"的证据

卡恢复之后还有一个**独立**的问题：LittleFS 的 RAM 内状态不会自愈，**必须重挂**。
但"要重挂"和"卡还坏着，重挂是白费"必须分清，否则会做错事。

判据用 **FAT 探卡**（用户提出的思路）：`/mnt/fat` 与两个 LFS 在**同一张卡**上，
所以"FAT 还能读"就是"卡没坏"的**独立证据**。这比看 `g_sd_dead` 强 —— 后者是 SD 层自己的
意见，而且只在多次重识别失败后才置起，中间有一段窗口。

| FAT | LFS | 含义 | 处置 |
| --- | --- | --- | --- |
| 读不到 | — | 卡的问题 | 交给 SD 层（判死 + 硬复位轮询），**别去重挂 LFS** |
| 能读 | 读不到 | 卡是好的，坏的是 LFS 实例 | **重挂** |
| 能读 | 能读 | 健康 | — |

实现：`sf32lb_sd_fs_ok()` 判、`sf32lb_sd_fs_heal()` 修，由 diag 线程的 **FS 槽**定期调用
（见 [diag.md](diag.md)）。

> **两个坑**：
> 1. `/mnt/kv` **不能**直接重调 `sf32lb_sd_mount_kv()` —— 它开头
>    `if (g_sd_kv_mtd != NULL) return OK;` 会立刻返回成功却根本没重挂。
>    恢复路径必须走 `umount` + 按设备名 `mount`，不重复注册 MTD。
> 2. **不要在持有 SD 总线锁的路径里调用 `_fs_heal()`**：mount 内部还要走 SD I/O，
>    `g_sd_lock` 不可重入，会自锁。只从 diag 线程调。

---

## 6. 开机挂载阶梯：rw → ro → 重启

开机必须挂上（用户要求"不能放行"），三级阶梯：

| 级 | 动作 | 说明 |
| --- | --- | --- |
| 1 | 读写挂载，最多 `SF32LB_SD_LFS_MOUNT_TRIES`(3) 次 | 退避 100 → 300 → 900 ms（×3）；**每次重试前先完整重识别卡**，而不是只 umount 睡一觉 |
| 2 | 只读挂载（`data = "ro"`） | 刻意**不调** `sf32lb_sd_grow_lfs()` —— 那个函数会写超级块，只读尝试里写就自相矛盾，而且它很可能正是读写挂载失败的原因 |
| 3 | `HAL_PMU_Reboot()` | 只读也挂不上就重启；**没有循环次数上限**（"无限重启就无限"） |

第 2 级的只读状态由 `g_lfs_readonly` 记录，对外是 `sf32lb_sd_lfs_readonly()`。

**`autoformat` 的边界**（"LFS 永不格式化"）：
- `/mnt/lfs` 上**任何**情况下都不 autoformat（会格式化掉轨迹）；
- `/mnt/kv` 只有在 `sf32lb_lfs_grow_super()` 返回 `-ENOENT`（**空白切片**）时才 autoformat，
  损坏不再触发格式化；
- 恢复路径（`_fs_heal`）**一律不传** `autoformat`。

---

## 7. 验卡工具

判死之后 LFS 层已经不下发 I/O 了，常规读写全部立刻 `-EIO`，于是**没法再判断卡还响不响应**。
所以有两条刻意**绕过闸门**的工具：

| 命令 | 作用 |
| --- | --- |
| `ctl sd read <hex_off> [n]` | 裸读连续 n 个 512 B 块，**逐块报时**（坏页的典型特征是某一块明显慢或失败，只看总结果分辨不出） |
| `ctl sd write <hex_off> [n]` | 往 n 块写 `0xFF` 并回读校验。**只允许卡尾 1 MiB**（唯一不属于任何文件系统的区域），驱动内部还有一道不可绕过的闸门 |
| `sys clk` | 内含只读的 `SDCLK:` 探针：打印 CSR 的 MPI 源选择、DLL2、`ENR2` 门控、`CLKCR`，用来反推 SDCLK 是否等于预期 |

注意 `ctl sd write` 是**破坏性**的：它按 LittleFS 的"擦除"方式（8 次 512 B 编程填 0xFF）写数据，
所以目标区必须真的是弃用区。

---

## 8. SD 时钟拓扑：为什么"外部 48 MHz"用不了

曾经的想法是"给 SD 单独接一个固定时钟源，它就不再跟着 HCLK 抖了"。**做不到**，
原因在 52x 的时钟矩阵里，值得记下来免得再试一遍：

1. **`HPSYS_RCC_CSR` 里没有 `SEL_SDMMC`**。可选的只有
   `SEL_SYS / SEL_MPI1 / SEL_MPI2 / SEL_PERI / SEL_TICK / SEL_USBC`
   （外加 `DWCFGR_SEL_SYS`）。硬件上 SD 和 MPI **共用** `CG MPI1/2` 这一路。
2. **MPI/SD 的 mux 选项只有 `SYSCLK` / `DLL2` / `DLL3`，没有 48 MHz 入口。**
   `SystemFixClock = 48 MHz` 属于 **PERI 域**（USART/SPI 那些），**接不到 SD 上**。
3. **SD 是从 MPI 分支在 `MPI_PSCLR` 之前取 HCLK 的** —— `MPI_PSCLR.DIV`
   是"每个 MPI 各自的分频"（就是 `sys clk` 里那个 `psclr`），**SD 不在它后面**。
   实测也是这样：SDCLK 始终 = HCLK/(CLKCR.DIV+1)。
4. `HPSYS_CFG_SYSCR_SDNAND` 是 **AHB 地址映射位**（SD2 还是 MPI3），
   不是时钟门控，改它不能解耦时钟。

**唯一可行的解耦手段是 DLL2**：它是独立 PLL、频率固定、不随 DVFS 变，
经 `SEL_MPI1/2` 接入。但它有一个来自官方驱动的硬约束：

```c
/* bf0_hal_rcc.c: HAL_RCC_HCPU_ConfigDvfs() —— 从 S 档切向 D 档时 */
dll2_limit = hpsys_dll2_limit[new_dvfs_mode];     /* D0/D1 = 0, S0/S1 = 288 MHz */
if (HAL_RCC_HCPU_GetDLL2Freq() > dll2_limit) {
    r = HAL_ERROR;                                /* 直接拒绝这次切档 */
    goto __EXIT;
}
```

也就是说：**只要 DLL2 在跑（非零频率），S → D 的切档就会被拒绝**。

好消息是**这条约束目前不咬我们**：本产品的档位只有
72/96/144（S0）和 240（S1），**从不进 D0/D1**（D 档会关 XT48/DLL2，
而我们还依赖 PSRAM XIP，见 [dvfs.md](dvfs.md) §2）。S↔S 之间切换时
`dll2_limit` 是 288 MHz，DLL2 只要不超过它就通过。

结论：**要用 DLL2 给 SD 解耦是可行的，但代价是"以后永远不能用 D0/D1 低功耗档"**
——这两件事互斥。当前 DLL2 的实际状态用 `sys dvfs` 的 `dll2` 字段和只读的
`SDCLK:` 探针看（它们会直接报出 mux 选择与反推的 SDCLK）。

---

## 9. 限制与已知未完成

1. **触发条件仍未查明。** 我们知道了"卡住之后是什么样、怎么救"，但**不知道控制器为什么
   会进入 `CMD_BUSY`**。最强的剩余嫌疑是残留的 7 处 `DCR = 0` —— 它们出现在"中止等待 +
   重试"路径上（`sd1_wait_write`、`sf32lb_sd_read_block`、`sf32lb_sd_program_staged`、
   `sf32lb_sd_rt_xfer` ×3），而本文件自己的头部注释就警告过
   *"Aborting that wait with DCR=0 wedges the card"*。
   已按官方驱动对齐，去掉了 `sd1_unstick_data()` 里那一处：改后的版本抓到的日志里
   `noerr_but_cmd_busy` 出现 **0 次**（但这是 n=1 的样本，不能当结论）。
   **只有等 `noerr_but_cmd_busy` 再次出现时才继续清理剩下 7 处**，否则就是无证据改动。
2. **恢复成本仍然高。** 判死 → 硬复位的最坏间隔是 30 s，也就是卡坏了之后最多要等半分钟
   才尝试救；这是刻意的（避免频繁硬复位本身变成新的扰动源）。
3. **只读挂载期间的写者尚未接线。** `sf32lb_sd_lfs_readonly()` 已导出但**还没有消费者**：
   轨迹记录 / 设置持久化 / 星历落盘应当据此跳过写入。
   > 顺带澄清一个直觉错误：这件事的收益是**省掉每 500 ms 一次失败 open 的噪音**，
   > **不是**减少磨损 —— 只读 LFS 的写被 LittleFS 的**块层**拦下
   > （`littlefs_write_block` / `erase_block` / `sync_block` 各自返回 `-EACCES`），
   > 根本到不了 SD 卡。
4. **GNSS 读线程的永久卡死是另一个问题**，与 SD 无关（`poll()` 超时靠 tick 驱动，
   被时间基准扰动打断）。见 [gnss_ble_diag_log.md](gnss_ble_diag_log.md)。
5. **用不了外部 48 MHz**（52x 没有 `SEL_SDMMC`，MPI/SD 的 mux 只有 SYSCLK/DLL2/DLL3）。
   SD 从 MPI 分支在 `MPI_PSCLR` **之前**取 HCLK。唯一能解耦的 DLL2 与 D0/D1 低功耗档
   互斥。详见 §8。

---

## 10. 验证记录

| 项 | 结果 |
| --- | --- |
| `sd1_init()` 里的模块复位 | 现场**首次** `SD: RECOVERED by hard reset #1`，两次日志复现 4 次以上，恢复后星历注入 99 帧、两个 LFS 正常 |
| 判死闸门覆盖全部入口 | 反汇编确认三处入口各调用一次 `sd1_dead_fail_fast`；判死后不再出现每块 2~4 s 的延迟 |
| 设备不再被死卡拖垮 | 蓝牙可连、GNSS 保持 `hz=1`、无 IWDT 重启、切频 `wait=0 ms` |
| FAT 当卡的证据 | `df` 与 `statfs` 判据一致；卡坏时重挂 LFS 不再被误触发 |
| 已排除项 | 坏块（块 194 读清）、DVFS 打断（失败瞬间 HCLK 未变 ×3）、开机擦写（`hw_wr=0 erase=0`）、PSRAM 分配（`+1 ms`）、挂载质量（`try 1/3 ok in 48 ms`）、SysTick 基准（`en=1 int=1 src=1`） |

---

日期：2026-09-16。

- 2026-09-16：新增模块级 RCC 复位（根因修复，首次 `RECOVERED`）、判死闸门补齐到
  `rt_xfer_retry`、`sd1_sr_why()` 取证、硬复位轮询、`sd1_set_clock()` 停钟改分频、
  `sd1_unstick_data()` 去掉 `DCR = 0`、FAT-as-oracle 重挂、开机挂载阶梯 rw→ro→重启、
  kv `autoformat` 收窄到空白切片、`ctl sd read|write`、`SDCLK:` 探针。

相关：[sd_partition.md](sd_partition.md)、[diag.md](diag.md)、
[gnss_ble_diag_log.md](gnss_ble_diag_log.md)、[dvfs.md](dvfs.md)、
[tools/vela_my_vendor_tools.md](tools/vela_my_vendor_tools.md)。
