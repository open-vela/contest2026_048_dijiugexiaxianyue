# SD 启动挂载 LittleFS 慢（8 s → 0.47 s）排查记录

2026-08-13。现象是 SD 卡启动时 `board_late_initialize()` 里挂载 rootfs 要花 8 秒。
结论是 **SD 的 LittleFS 几何参数照搬了 NAND**，导致挂载要读 5.4 MB 元数据，
而驱动又是单块 PIO、每 512 字节约 0.5 ms。

相关：[build_guide.md](../build_guide.md)、[sd_partition.md](../sd_partition.md)、
[nuttx_boot_flow.md](../nuttx_boot_flow.md)、
[tools/vela_my_vendor_tools.md](../tools/vela_my_vendor_tools.md)、
[mtp_simple/README.md](../../boards/sf32lb52/helmone/services/mtp_simple/README.md)。

2026-08-23 补记：驱动改成 512/4096 之后，若整盘仍烧 **8 月 11 日那份 ~33 MiB `my_vendor_sd.img`（NAND 几何）**，MTP 能枚举但文件夹打不开。见第 8 节。

2026-09-07 补记：写路径已按官方 `drv_sdio.c` 做成 CMD24/CMD25 + DMA（应答后再
`DATA_START`）。MTP 上传另有协议/LFS 锁问题，见 [mtp_upload.md](mtp_upload.md)。

2026-09-15 补记：CMD17/写超时先 CMD12+CMD13 回 TRAN；DATA 态禁止 CMD8。见第 7 节末。

---

## 1. 现象

```
[    0.014266] INFO: SD/eMMC automount starting...
[    0.025432] INFO: SD card init ok (rca=0x0001, SDHC/SDXC)
[    0.026154] INFO: SD MTD registered at /dev/sd0 (off=0x5e9000 size=0xffa00000)
[    8.023170] INFO: LittleFS mounted /dev/sd0 -> /mnt/lfs      <-- 7.997 s
```

时间全部落在 `sf32lb_sd_mount_littlefs()` 的 `mount()` 调用里；卡识别只用了 11 ms。

---

## 2. 根因

三层叠加，前两层是几何参数造成的元数据放大，第三层是驱动的固定开销。

### 2.1 几何参数照搬 NAND

`sf32lb_sdio.c` 给 MTD 报的是 `blocksize=2048`（→ LittleFS `read_size`/`prog_size`）、
`erasesize=131072`（→ LittleFS `block_size`）。这套值对 SPI NAND 是物理约束（页 2048、
块 128 KiB，没得选），但 **SD 卡扇区是 512 字节、而且不需要擦除**（`sf32lb_sd_erase()`
是靠写 0xFF 模拟的），所以对 SD 来说这两个是完全自由的参数，却被设成了最坏的一组。

后果：

- LittleFS 的目录元数据是**日志式提交**，每次提交都会 pad 到 `prog_size`。打包时
  610 个文件 + 40 个目录 ≈ 1300 次提交 × 2 KiB ≈ **2.6 MB 元数据**，散落在约 30 个
  metadata dir pair 里。
- 128 KiB 的 block 让每个小文件都独占一整块 —— 3 MB 的内容做成了 27 MB 的镜像。

### 2.2 挂载要把每个元数据块正向 + 反向各扫一遍

`lfs_mount()` 必须沿 metadata tail list 遍历全部 mdir，每个 mdir 会发生两次全块扫描
（`nuttx/fs/littlefs/littlefs/lfs.c`）：

| 阶段 | 函数 | 行为 |
|---|---|---|
| 正向 | `lfs_dir_fetchmatch()` | 从 `off=0` 逐提交向后扫并校验 CRC，直到遇到无效 tag |
| 反向 | `lfs_dir_getgstate()` → `lfs_dir_get()` → `lfs_dir_getslice()` | 从块尾**倒着**逐 tag 回扫，注释原文 "iterate over dir block backwards (for faster lookups)" |

反向扫描找的 `MOVESTATE` tag 正常情况下并不存在，所以每次都会一路倒扫到 `off=0`；
而且每步只取 4 字节 tag（`lfs_bd_read` 的 `hint=sizeof(ntag)`），读缓存每次都 miss，
等于把整块又完整读一遍。trace 实测 block 14 的模式：

```
off=0 → 57344 正向扫
off=18432 → 0   反向扫
```

单个 128 KiB 块被读了 92 次 × 2048 = 188 KiB，30 来个 mdir 合计 **5.4 MB**。

### 2.3 驱动把每个请求拆成单块 CMD17

`sf32lb_sd_read_region()` 按 512 字节循环，2048 字节的 MTD 请求 = 4 次 `CMD17`，
于是 2726 次读变成 **10904 次 512 字节传输**。

$$10904 \times 0.73\,\text{ms} \approx 8.0\,\text{s}$$

和日志吻合。

---

## 3. 定位方法

### 3.1 主机侧：直接量 `lfs_mount()` 的读取量

新增 `scripts/mklfs_disk/lfs_mount_prof.c`，用**设备的几何**挂载真实的 `fs_root.bin`
并统计读次数/字节数，改代码前就能验证几何是否有效，不用烧板子：

```bash
# openvela 根目录
gcc -O2 -w -I nuttx/fs/littlefs/littlefs \
    vendor/HelmOne/scripts/mklfs_disk/lfs_mount_prof.c \
    nuttx/fs/littlefs/littlefs/lfs.c \
    nuttx/fs/littlefs/littlefs/lfs_util.c -o /tmp/lfs_mount_prof

# 用法: <fs_root.bin> <region-size> [read_size] [block_size] [trace]
/tmp/lfs_mount_prof vendor/HelmOne/boot_loader/bin/fs_root.bin 0xffa00000 512 4096
```

加第 5 个参数（任意值）会逐条打印 `block=/off=/size=`，用来看正向/反向扫描模式。

不同几何的实测对比（同一棵 `mkfs/` 树，region `0xffa00000`）：

| read/prog | block | block_count | 镜像 | 挂载读取 | 折合 512B 扇区 |
|---|---|---|---|---|---|
| 2048 | 131072 | 32720 | 27616 KiB | 5452 KiB | 10904 |
| 2048 | 8192 | 523520 | 4538 KiB | 1156 KiB | 2312 |
| 512 | 32768 | 130880 | 8575 KiB | 1533 KiB | 3067 |
| 512 | 8192 | 523520 | 5169 KiB | 483 KiB | 967 |
| **512** | **4096** | **1047040** | **3902 KiB** | **434 KiB** | **868** |

### 3.2 设备侧：`SDPROF` 计量

`sf32lb_sdio.c` 里的 `SF32LB_SD_PROFILE`（默认 1，置 0 全部编译掉）会打印：

```
SDPROF: seq1  256 x 512 B in 126864 us -> 1033 KB/s, 495 us/call   连续单页读
SDPROF: seqN  32 x 4096 B in 126556 us -> 1035 KB/s, 3954 us/call  一次请求 8 页
SDPROF: seek  64 x 512 B in 41231 us -> 794 KB/s, 644 us/call      1 MiB 跨步
SDPROF: clk  div=6   (~24000 kHz) 439 us/sector                    时钟扫描
SDPROF: clk  div=12  (~12000 kHz) 485 us/sector
SDPROF: clk  div=24  (~6000 kHz) 576 us/sector
SDPROF: lfs_mount took 473848 us (ret=0)
SDPROF: mount bread=868(868 pg) hw_rd=868 blk 428244 us ...
```

判读要点：

- `seq1` 和 `seqN` 总时间几乎一样（126864 vs 126556 µs）→ MTD 请求变大没用，
  因为驱动内部仍按扇区拆 `CMD17`，两者命令数相同。瓶颈是**每条命令的固定成本**。
- 时钟扫描是区分"总线受限"和"命令延迟受限"的关键。三点近似线性：
  减半 +46 µs、减到 1/4 +137 µs（≈ 3 × 46），说明 24 MHz 下数据相位就是 **46 µs**
  （512 B / 4-bit / 24 MHz 理论值 43 µs，吻合），剩下 **约 393 µs 与时钟无关**。
  → 时钟设置没问题，提高时钟基本没用。
- `mount` 那行的 `bread` 数应与主机工具算出的扇区数一致（868），用来确认设备上
  跑的几何和镜像一致。

---

## 4. 修改内容

### 4.1 `chips/sf32lb52/sf32lb_sdio.c`

- **几何（本次提速的全部来源）**：`SF32LB_SD_PAGE_SIZE` 2048 → **512**，
  `SF32LB_SD_ERASE_SIZE` 131072 → **4096**。
- **时钟不再硬编码**：原来是写死的 `CLKCR = 359 << DIV`（400 kHz）和 `5 << DIV`
  （24 MHz），改为 `sd1_set_clock()` 从 `HAL_RCC_GetHCLKFreq(CORE_ID_HCPU)` 反算分频
  （SD1 分频语义是 `HCLK / (DIV + 1)`），新增 `SF32LB_SD_ID_CLOCK_HZ` /
  `SF32LB_SD_TRAN_CLOCK_HZ`。
- **新增 `sf32lb_sd_reclock()`**：供 HCLK 变化后重算分频。
- **4 GiB 窗口钳位**：`sf32lb_sd_initialize()` 里防止 32 位卡地址回绕到低地址把
  boot 镜像盖掉。
- 日志增强：`SD card init ok` 带 `hclk=`/`clkdiv=`，`SD MTD registered` 带
  `page=`/`erase=`/`nblocks=`。
- 新增 `SF32LB_SD_PROFILE` 计量块（见 3.2）。

### 4.2 `boards/sf32lb52/helmone/bsp/sifli_ap.c`

在 `BSP_Board_EnableHclk240()` 之后调用 `sf32lb_sd_reclock()`。

这是**顺带修的独立隐患**：HCPU 从 144 升到 240 MHz 后，写死的 `/6` 会让卡跑到
40 MHz，超出 SD default speed 的 25 MHz 上限，影响挂载之后所有对 `/mnt/lfs` 的访问。

### 4.3 `scripts/mklfs_disk/mklfs_disk.c`

几何从编译期常量改成运行期参数 `-r`（read/prog）、`-b`（block），默认 512/4096；
`LFS_BLOCK_COUNT_DEFAULT` 换成 `LFS_IMAGE_SIZE_DEFAULT`（100 MiB）；增加 block 必须是
prog 整数倍的校验。改成参数是因为这个工具 NAND/SD 共用，而 NAND 的 2048/131072 是
物理约束不能动。

### 4.4 `scripts/build_fs_root.sh`

从 `storage.conf` 导出 `FS_STORAGE`，按介质选几何并传给下游：

| BOOT_STORAGE | read/prog | block |
|---|---|---|
| `nand` | 2048 | 131072 |
| `sd` / `emmc` | 512 | 4096 |

同时把 `--block-size` 传给 `gen_fs_root_sparse.py`，保证空块检测粒度一致。

### 4.5 `scripts/mklfs_disk/lfs_mount_prof.c`（新增）

主机侧诊断工具，见 3.1。

### 4.6 defconfig 未改动

试过加 `CONFIG_FS_LITTLEFS_CACHE_SIZE_FACTOR=4`，但需要 full reconfigure、
收益只有 17%（868 → 740 扇区），而且 LittleFS 对大块顺序文件读本来就会绕过 cache，
因此撤销。两个 defconfig 保持原样（各项 factor 都是 1）。

---

## 5. 效果

| 项 | 改动前 | 改动后 |
|---|---|---|
| 挂载耗时 | 7.997 s | **0.474 s**（17x） |
| 挂载读取 | 5452 KiB / 10904 扇区 | 434 KiB / 868 扇区（12.6x） |
| `fs_root.bin` | 28311552 B (27 MB) | 3989504 B (3.9 MB) |
| `my_vendor_sd.img` | 34476032 B (34 MB) | 10186752 B (10 MB) |
| 挂载期 RAM（rcache+pcache+lookahead） | 6144 B | 1536 B |
| 每个打开文件的 cache | 2048 B | 512 B |

RAM 变小是因为三个缓冲都按 `cache_size`/`lookahead_size` 分配
（`lfs_init()` / `lfs_file_opencfg()` 里的 `lfs_malloc`），而 `lookahead_size` 是
自动计算 `min(alignup(block_count,64)/8, read_size)`，被 `read_size` 卡在 512。
`block_count` 涨到 1047040 不占内存 —— lookahead 是固定大小的滑动窗口位图，
不是整盘位图。

---

## 6. 注意事项

### 6.1 改几何必须重新打包并重新烧录

几何写在三处，必须一致：

1. `chips/sf32lb52/sf32lb_sdio.c` 的 `SF32LB_SD_PAGE_SIZE` / `SF32LB_SD_ERASE_SIZE`
2. `scripts/build_fs_root.sh` 的 `LFS_PROG` / `LFS_BLOCK`（传给 `mklfs_disk -r/-b`）
3. 同上传给 `gen_fs_root_sparse.py --block-size`

不一致时挂载报 **errno 22（`EINVAL`）**：`lfs_rawmount()` 读到超级块后会比对
`superblock.block_size` / `block_count`，不符就直接 `LFS_ERR_INVAL`。特征是
`SDPROF: mount bread=7`（只读了几个扇区就失败）：

```
[    0.489900] SDPROF: lfs_mount took 5059 us (ret=-1)
[    0.490550] SDPROF: mount bread=7(7 pg) hw_rd=7 blk 4814 us
[    0.491729] ERROR: mount(/dev/sd0, /mnt/lfs, littlefs) failed: 22
```

这里没有 autoformat，所以只会挂载失败，不会静默重新格式化。重烧：

```bash
python3 vendor/HelmOne/build_board.py build-fs flash-fs monitor
# 整盘 img: pack-sd-img + burn-sd
```

### 6.2 主机镜像的"空白值"是 0x00，设备擦除写的是 0xFF

`mklfs_disk.c` 的 `file_erase()` 是 no-op、镜像用 `ftruncate` 起始为稀疏零，
所以主机镜像里"已擦除"区域读回 0x00；而 `sf32lb_sd_erase()` 在设备上写 0xFF。
这个不一致目前无害（LittleFS 写任何块之前一定先 erase），影响只是首次写某个 mdir
时会多做一次 compaction。`gen_fs_root_sparse.py` 的 `block_is_blank()` 同时认 0x00
和 0xFF。

---

## 7. 遗留：单块 CMD17 的 ~390 µs 固定开销

当前读写方式**未改动**，仍是纯 CPU 轮询的 programmed I/O + 单块命令：

```
sd1_setup_read()               DCR/DLR: 512 字节、4-bit、DATA_START
CMD17                          READ_SINGLE_BLOCK
while (SR & DATA_DONE == 0);   忙等
for (i = 0; i < 128; i++)      CPU 逐字搬 FIFO
memcpy(dst, g_sd_blkbuf, 512);
```

没有 DMA、没有 `CMD18`/`CMD25` 多块、没开高速模式（`CMD6 SWITCH_FUNC`）。
代码里 `IER = SD_IER_DATA_DONE_MASK` 只置了外设中断使能位，NuttX 侧没有
`irq_attach` SDMMC1（52x 启动文件里也没有这个向量），实际是纯轮询，那行是从
bootloader 抄来的遗留。

**光加 DMA 收益很小**：393 µs 里属于 CPU 的只有 128 次外设读 + 一次 512 B memcpy，
144 MHz 下约 10~25 µs，不到 6%。剩下大部分应是卡内部把扇区读进缓冲的时间
（推断，未直接测；同一段代码三次测出 439/495/644 µs 这种随地址抖动符合卡侧延迟特征）。

**DMA 的价值在于它是多块传输的前提**。vendor 自己就是这么做的：RT-Thread 的
`drv_sdio.c` 用外部 DMAC 在内存和 `hwp_sdmmc1->FIFO` 之间搬数据，且 DMA 是强制的
（没定义 `SDIO_USING_DMA` 直接 `#error`）。硬件也按这个思路设计：
`SD_FDLR.RD_VALID/RD_VALID_EN` 是 DMA 请求水位、`SD_BLR.BURST_LEN` 是 AHB 突发长度。

**为什么现在不能直接改多块**：当前代码是等 `DATA_DONE` 之后才掏 FIFO，这只在一个
512 字节块能整个装进 FIFO 时成立。要做 `CMD18` 必须边收边搬，而 PIO 下判断水位要读
`DBGR2.VALID_DATA_COU`，那个寄存器没在 `SD_TypeDef` 里暴露（落在 `RSVD1[101]` 里），
FIFO 深度也没文档。用 DMA 才能绕过 —— DMAC 按 `FDLR` 水位自己搬。

收益估算，一次 4096 字节（8 扇区）：

| 方式 | 耗时 | 吞吐 |
|---|---|---|
| 现在（8 × CMD17） | 8 × 439 ≈ 3512 µs（实测 3954） | ~1 MB/s |
| CMD18 + DMA | 390 + 8 × 46 ≈ 758 µs | ~5 MB/s |

对**读文件**（字体、图片）直接生效，因为大块顺序读会绕过 cache、单次请求到
`block_size` = 4096。对**挂载**还需顺便把 `CONFIG_FS_LITTLEFS_CACHE_SIZE_FACTOR`
调大（现在 cache 是 512，挂载时每次只请求 512 字节，多块用不上），那样挂载可再降到
约 0.2 s。

真要做的话需要接管 DMAC 通道、处理 `CMD12 STOP_TRANSMISSION` 和写完成后的 DAT0
busy 等待，失败模式是挂载不上，建议按"先 `CMD18` + PIO 小步验证 FIFO 行为，
再上 DMA"的顺序，并保留多块失败自动退回单块的兜底。

**2026-08-23 更新（读已做）：** 读路径已按 RT-Thread `drv_sdio.c` 做成
**CMD17/CMD18 + DMA**（多块后 CMD12）。`nsh> test sdio read` 上 seq CMD18 约
**3.1 MB/s**，单块 512 B 约 **0.76 MB/s**。

**2026-09-07 更新（写已对齐官方）：** 写改为 **CMD24/CMD25 + DMA**（DMAC1 CH3 /
`DMA_REQUEST_57`）。顺序是：DMA 内存→FIFO → 发命令 → **等卡应答后再 `DATA_START`**
（对应官方 IRQ 里的 CMDREND）。此前「CMD 前就开数据相」会把卡留在 data 态，那条
时序不要再用。DMA 失败回退 PIO CMD24。I/O 重试可短暂降到 6 MHz，**成功后恢复
24 MHz**。`CACHE_SIZE_FACTOR=8` 仍是 4 KiB 上限。MTP 侧另见 [mtp_upload.md](mtp_upload.md)。

**2026-09-15 更新（超时恢复）：** CMD17/写失败后**先 CMD12 + CMD13**，卡回到 TRAN
就重试。只有已经 IDLE 才 CMD0/CMD8。DATA 态发 CMD8 会把一次读超时放大成整卡
1 Hz `re-identify`。CMD17 单块超时也要 CMD12，不只是 CMD18 多块。见
[sd_partition.md §4.5](../sd_partition.md)。

---

## 8. 2026-08-23：旧整盘镜像让 MTP「打不开」

### 8.1 现象

SD 启动、MTP 能枚举、主机能看见设备，但资源管理器里**文件夹/文件打不开或极慢**。
当时容易当成 USB/MTP 协议问题。分层测试（`nsh> test layers`，不跑 MTP、不对
`/mnt/lfs` 做 `statfs`）在当前驱动上 **PASS** 之后，用现在的 512/4096 重新
`build-fs` + `pack-sd-img` 刻录，MTP 恢复正常。

### 8.2 根因

几何三处早已对齐（见 6.1），但 **`boot_loader/bin/my_vendor_sd.img` 仍是改几何之前
打的那份**：

| 镜像 | 日期（当时） | 大小 | 几何 |
|---|---|---|---|
| 错误（仍被刻录） | 2026-08-11 | ~33–34 MiB | NAND：read/prog=**2048**，block=**131072** |
| 正确 | 2026-08-23 | ~9.7–10 MiB | SD：read/prog=**512**，block=**4096**，blocks=**1047040** |
| `fs_root.bin` | 同上 | ~3.9 MiB（稀疏宿主文件） | 逻辑窗口仍是 ptab 的 ~4 GiB |

`mkfs/` 大约 **610** 个小文件（主要是 `vmap/` 瓦片）。128 KiB 块下每个小文件独占
一整块，元数据日志又按 2 KiB `prog_size` 填充；MTP 展开目录 / 打开文件等于在扫
那套放大后的树，表现为「打不开」。512/4096 之后同一棵树挂载只读约 400 KiB，
列目录和打开都正常。

错配还有两种硬失败，不要和「慢」混在一起：

- 超级块 `block_size`/`block_count` 与 MTD 不一致 → 挂载 **EINVAL (22)**，见 6.1。
- 只 `flash` 了固件、整盘 img 没重打 → 下次 `burn-sd` 又把 33 MiB 旧几何写回去。

### 8.3 正确打包（SD）

`build_fs_root.sh` 在 `BOOT_STORAGE=sd` 时必须打出
`read/prog=512, block=4096, blocks=1047040`。openvela 根目录：

```bash
./vela_my_vendor_tools.py build-fs
./vela_my_vendor_tools.py pack-sd-img
./vela_my_vendor_tools.py burn-sd --sd /dev/sdX --yes   # 整盘节点，不要写分区
```

卡留在板子上时用串口只写 FS（先 BOOT+RESET）：

```bash
./vela_my_vendor_tools.py flash-fs -p /dev/ttyACM0
```

SD 的 `flash-fs` **不会**整区擦除（4 GiB 串口擦不现实）。`dd conv=sparse` 也只写
镜像里有数据的扇区（大约卡头 10 MiB）。卡上更后面的旧大文件 / 旧 128 KiB 元数据
不会被清掉。要干净：新卡、先擦除，或 `nsh> test lfs format -y`（会清空卷）后再
`flash-fs`。

### 8.4 打包时不要做的事

- 不要用 NAND 的 2048/131072 打 SD 的 `fs_root.bin` / `my_vendor_sd.img`。
- 不要对 `/mnt/lfs` 做 `statfs` / `statvfs`：`lfs_fs_size()` 会走遍已用块；空闲
  4 GiB 也很快，**塞满产品文件后会卡死**（MTP `GetStorageInfo` 已改为 MTD 几何，
  见 mtp_simple README）。
- 不要对已挂载的 LittleFS 做 raw MTD 写入。
- 改 `SF32LB_SD_PAGE_SIZE` / `SF32LB_SD_ERASE_SIZE` 之后必须 **build-fs 且重打
  整盘 img**，只编译固件不够。
