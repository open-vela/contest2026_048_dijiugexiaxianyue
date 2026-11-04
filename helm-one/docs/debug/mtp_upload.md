# MTP 上传慢 / gvfs「Could not send object」（2026-09-07）

产品固件、Ubuntu gvfs + libmtp，往 `/mnt/lfs/mtp` 拷文件。SD 是 ~14 GiB LittleFS
（含 packed 地图），不是空卡。

相关：[mtp_simple/README.md](../../boards/sf32lb52/helmone/services/mtp_simple/README.md)、
[mklfs.md](mklfs.md)、[sd_partition.md](../sd_partition.md)、
官方对照 `SiFli/SDK/2.4/.../drv_sdio.c`。

---

## 1. 现象怎么变的

| 阶段 | 主机 | 串口要点 |
|------|------|----------|
| 立刻失败 | 进度 0，`Could not send object` | `SendObjectInfo OK` 后 DATA 被 ZLP drain 吃掉，或 `SendObject` 里 `open(O_TRUNC)` 卡住 2 KiB OUT 队列 |
| 半途失败 | 卡在 ~165/365 KB，约 26 s | USB 已收完，LittleFS `write()` 堵在 MTP 线程，`0x100d` 超时 |
| 能拷但极慢 | 一瞬间跳到 ~200 KB，总时间 20 s+ | `rx 390ms` + `0x100d 410ms`，然后 `lfs write 23830ms`；`GetObjectInfo sz=0`；`GetObjectHandles import sync=24210ms` |
| 2026-09-07 后 | 小文件复制框很快结束 | 后台仍可能 `lfs slice 32768/… 23750ms`（新建 CTZ 的 `lfs_alloc`） |
| 大文件 | 18.9 MB PDF 约 110 s（~170 KB/s） | `chunked remain=… (malloc failed or too big)`，USB 与 LFS 交替；结束后 `sz=` 正确 |

对照：`nsh> test lfs` 写**已有** `boot_marker.txt` 32 KiB 约 0.8 s（无 `O_CREAT`）。
不能用这条否定 MTP：MTP 是在 `import/` **新建**文件，第一次 `write(32KB)` 会走 packed 卷的 `lfs_alloc`。

---

## 2. 问题点（分层）

### 2.1 USB（已修）

- gadget OUT 队列 **`CONFIG_USBMTP_NRDREQS=32` × 64 B = 2 KiB**。USB 热路径上
  `sched_yield` / `usleep` 会把同优先级 UI 插进来，365 KB `SendObject` 拖到 20 s+。
- `SendObjectInfo` 之后不要 drain ZLP：会吞掉紧随其后的 `SendObject` DATA。
- 非零 lookahead 收到的 OUT 字节要 stash（`g_out_stash`），不能当垃圾丢掉。
- `SendObjectInfo` 的 fd 一直拿到 `SendObject`；禁止再 `open(O_TRUNC)`。

### 2.2 MTP 协议 / 单线程（已修）

LittleFS **整卷一把锁**。MTP poll 线程一旦进 `write()`/`opendir()`，EP0 和 bulk
都停。gvfs 在 `0x100d` 之后立刻 `GetObjectPropList` / `GetObjectHandles`：

- 若 `write()` 还在跑 → 列目录 `sync=24s`，复制框假死。
- 若 catalog / `stat()` 看到 0 字节占位 → `GetObjectInfo sz=0`，gvfs 当空文件。

`test lfs` 不经过这条协议路径，所以「LFS 测试很快、MTP 仍慢」可以同时成立。

### 2.3 SD 写（已按官方驱动改）

旧路径：读 CMD17/18+DMA，写 **PIO CMD24**。第一次 I/O 失败后降到 **6 MHz 且 staying**，
后续 LittleFS 元数据/写入爬行。

官方 `drv_sdio.c`：

- **DMAC1 CH3 / `DMA_REQUEST_57`**，SRAM bounce。ExtDMA 是内存拷贝，**不能**喂 SD FIFO。
- **读**：`DATA_START` 在 CMD 之前。
- **写**：先 DMA 内存→FIFO，发 CMD24/25，**CMD 应答后再 `DATA_START`**（IRQ 里 CMDREND）。
  应答前开数据相会把卡留在 data 态（此前 CMD25+DMA 失败的原因）。
- 卡时钟上限 **24 MHz**；官方不永久降到 6 MHz。

### 2.4 LittleFS 分配（仍在）

14 GiB packed 地图上，**新建** 365 KB 文件的第一刀 `write(32KB)` 实测 **~24 s**
（`lfs_alloc` 扫空闲块）。后续每 32 KB 约 150 ms。`LOOKAHEAD_SIZE=8192` 已加大，
改 lookahead 必须 **整编固件**。

---

## 3. 本轮改动

### 3.1 `sf32lb_sdio.c`

- 写走 DMA CMD24/CMD25，bounce `g_sd_mblkbuf`，最多 8×512 B（对齐 erase 4096）。
- 写：`up_clean_dcache`（不要 invalidate 写缓冲）；应答后再 `DCR |= DATA_START`。
- DMA 失败回退 PIO CMD24。
- 先在当前 24 MHz 重试，再降 6 MHz；**成功后恢复 24 MHz**，禁止 `staying`。
- 启动日志：`CMD17/CMD18 read, CMD24/CMD25 write`。
- **2026-09-15**：I/O 超时先停 DMA、**CMD12**、**CMD13** 回到 TRAN 再重试。
  DATA 态禁止 CMD8。CMD17 失败也发 CMD12。见 [sd_partition.md §4.5](../sd_partition.md)。

### 3.2 `mtp_simple.c`

| 项 | 行为 |
|----|------|
| ≤2 MiB | `malloc` 整包，USB 先收完（SRAM 2 KiB burst `g_usb_rx`），再回 `0x100d` |
| 刷盘 | 更低优先级后台线程写 32 KB 片；MTP poll 继续答 GetObjectInfo |
| catalog | 刷盘中报 ObjectInfo 大小，禁止 `stat` 0 字节；禁止 evict 正在上传的对象 |
| 列目录 | 刷盘中 **跳过 `opendir`/`sync_children`**（避免和 `write()` 抢 LFS 锁） |
| >2 MiB | 仍 USB+LFS 分块（`g_io_buf` 4 KiB）；`0x100d` 等全部写完 |

### 3.3 其它

- `CONFIG_FS_LITTLEFS_LOOKAHEAD_SIZE`：2048→8192（`nsh` / `nsh-factory`）。
- `test lfs`：各段 begin/end、open/write/fsync、mkdir、总耗时。

---

## 4. 怎么看日志

成功的小文件（365 KB GPX）：

```
SendObject rx 390ms write deferred bytes=364765 ram=364765
SendObject lfs flush in background bytes=364765
op 0x100d … 410ms
GetObjectPropList … 0ms          ← 主机立刻能问，复制框可结束
SendObject lfs slice 32768/… 23750ms   ← 后台分配，传输页还在
SendObject lfs write … ret=0
```

大文件（>2 MiB）没有 `flush in background`，只有：

```
SendObject chunked remain=… (malloc failed or too big)
SendObject rx …ms write deferred bytes=… ram=0
op 0x100d …           ← 时间 ≈ 整段 USB+写
GetObjectInfo … sz=<实际大小>
```

`nsh> test lfs` 若写的是已有根文件，**不能**用来证明新建 MTP 文件也一样快。

---

## 5. 还开着的问题

- **新建文件第一刀 `lfs_alloc` ~24 s**（packed 14 G）。复制框已不堵；设备传输页会等到
  `lfs write … ret=0`。**刷盘未完成时不要拔线。**
- **>2 MiB** 仍走分块，18.9 MB 约 110 s（~170 KB/s）。要再快需要独立改这条路径
  （更大 RAM 窗、或后台队列）。
- ExtDMA **不是** SD 修复手段。
- SD `bread`/`bwrite` 上不要 `usleep`（会拖到 LCD 在 48 MHz 初始化）。
- IWDT 15 s：刷盘线程和 SD DAT0 等待里打 `work_beat`；USB 热路径禁止 yield。

---

## 6. 涉及文件

| 文件 | 角色 |
|------|------|
| `chips/sf32lb52/sf32lb_sdio.c` | SD DMA 读写、时钟恢复、CMD12+CMD13 解 DATA |
| `services/mtp_simple/mtp_simple.c` | 上传、catalog、刷盘线程 |
| `nsh/test/test_lfs.c` | 分段耗时 |
| `configs/nsh/defconfig`、`nsh-factory/defconfig` | lookahead 8192、`USBMTP_NRDREQS=32` |
