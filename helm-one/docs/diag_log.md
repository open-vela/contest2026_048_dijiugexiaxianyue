# 诊断日志落盘（diaglog）

运行时日志原本**没有任何持久化**：`SD:` / `gnss:` / `dvfs:` / `LFS:` 全部只出现在控制台，
重启即消失 —— 而现场最需要的那次失败，往往正是重启前的那一次。
本文写这套落盘机制**做什么、怎么实现、只收什么、以及有什么限制**。

相关：[diag.md](diag.md)（宿主线程）、[sd_recovery.md](sd_recovery.md)（为什么卡上的日志不可靠）、
[gnss_ble_diag_log.md](gnss_ble_diag_log.md)（各条日志的含义）。

---

## 1. 规格（用户给定）

- **RAM 先缓存一点**，然后**定期**写盘；
- **每次开机一个文件**；同一次开机一直跑就**按天一个**；
- 目录里**最多 10 个**，超出按时间删最旧（与 coredump 同一套做法，只是前缀是 `d` 不是 `n`）；
- 环 **2 KB** 就够（"不指望它能做什么"）；
- **只落 error 级别**的日志（见 §4）；
- 死机时，环里**尚未落盘**的内容随 coredump 一起写出去。

---

## 2. 为什么不用 NuttX 自带的文件通道

NuttX 是**有**现成机制的：`CONFIG_SYSLOG_FILE` + `syslog_file_channel(path)`，
带轮转（`SYSLOG_FILE_ROTATIONS` / `FILE_SIZE_LIMIT`）和会话分节标记；
另外还有 `CONFIG_SYSLOG_CHAR`。两个在本 defconfig 里都是关的，**但这不是本项目不用它的原因**。

**决定性的原因是写入路径**：`syslog_dev_write()` 最后是

```c
ret = file_write(&syslog_dev->sl_file, buffer, buflen);   /* drivers/syslog/syslog_device.c */
```

也就是**在调用者自己的上下文里做阻塞写**。本产品的文件系统是坐在**板载 SD 卡**上的 LittleFS，
卡不好的时候一个块要 2~4 秒 —— 那样**任何一次 `syslog()`**（GNSS 读线程、BLE、SD 驱动自己、
中断路径）都会阻塞在卡上。这正是已经查明过一次的故障形态：最早那个"蓝牙异常"就是
`ble_companion` 阻塞在死掉的 SD 上（栈回溯终点 `sd1_send_cmd`）。

所以自建通道不是重复造轮子，而是把 sink 做成**非阻塞**：
**回调只搬字节进 RAM 环，一块存储也不碰**；所有 SD I/O 收进 diag 线程按固定节奏做。

---

## 3. 实现结构

```
任意上下文（含中断）          diag 线程（500 ms 一拍）              崩溃路径
   syslog()                      myvendor_diaglog_flush()        myvendor_coredump
      │                                  │                             │
      ▼                                  ▼                             ▼
[ RAM 环 2 KB ]  ──定期刷──▶  /mnt/kv/diag/dNNN_*.txt        （环内容快照）
  只搬字节，不 I/O              追加写，失败留到下次              写进崩溃转储文件
```

| 文件 | 角色 |
| --- | --- |
| `services/myvendor_diaglog.c` / `include/myvendor_diaglog.h` | 实现 |
| `bsp/bringup.c` | 注册通道 + 开机**立刻刷一次**（挂载信息最值钱，不等 500 ms） |
| `services/myvendor_diag.c` | 主循环里每拍刷一次 |
| `bsp/myvendor_coredump.c` | 崩溃时把环里未落盘的内容附在转储文件后面 |

**两个放置决定，都有理由：**

1. 刷盘点放在 diag 线程的**主循环体**里，而**不是**那个被 `DIAG_GRACE_MS` 门控的槽位遍历里 ——
   否则开机头 25 秒的日志永远不会落盘，而那正是最需要的一段。
2. 刷盘**只在 diag 线程做**。出错点那边往往正持着 `g_sd_lock`（例如 SD 失败当场），
   在那里调 `flush()` 会自锁。

---

## 4. 只落异常级别

### 4.1 为什么必须解析文本

NuttX **没有**任何 per-channel 的级别过滤可用：

- `g_syslog_mask` 是在 `nx_syslog()` 里、**进 channel 之前**做全局过滤的（`vsyslog.c`），
  改它等于改所有通道；
- `SYSLOG_IOCTL` 只提供**使能 / 禁用**和 CRLF 开关，没有级别掩码；
- channel 回调 `sc_putc` / `sc_write` **拿不到 priority 参数**。

所以只能让级别出现在文本里，再解析：打开 `CONFIG_SYSLOG_PRIORITY=y`，
`vsyslog.c` 会用 `"[%6s] "` 配 `g_priority_str[]` 往每条消息前缀里插入
`[ ERROR]` / `[  WARN]` / `[  INFO]` / `[NOTICE]` / `[  CRIT]` / `[ EMERG]` / `[ ALERT]` / `[ DEBUG]`
（注意是 6 字符**右对齐**）。通道就按这些**固定字面量**匹配，
不依赖它在行内的位置 —— 时间戳格式、CPU/TID/PREFIX 开关怎么变都不影响。

### 4.2 门槛是 WARNING，而不是字面的 ERR

**这是刻意的偏离**，理由是恢复过程本身**全是 WARNING**：

| 级别 | 典型日志 |
| --- | --- |
| `LOG_ERR` | `SD: CMD… FAILED after N tries (sr=… why=…)`、`SD: declared DEAD`、`LFS: mounted READ-ONLY` |
| **`LOG_WARNING`** | **`SD: hard reset #N`**、**`SD: still dead after hard reset`**、**`SD: RECOVERED by hard reset`**、`SD: re-identify OK`、`LFS: remount`、`diag: <slot> unhealthy`、`dvfs: gov re-armed by diag`、`gnss: restart begin` |

只留 ERR 会得到"坏了"而没有"后来怎么好的" —— 恰恰丢掉长测要观察的东西。
门槛是一个常量：`DIAGLOG_MAX_SEV`（`myvendor_diaglog.c`），要收紧成严格 ERR 改一个词。

### 4.3 逐行判定，续行继承

判定必须**按行**，不能按 write，原因有两个：

1. `syslog_write_foreach()` 在 `CONFIG_SYSLOG_CRLF` 下把一条消息拆成**正文**和 `"\r\n"`
   **两次** write；
2. 一条**多行**消息（`serr("a\nb\n")` 这种）只有**第一行**带级别前缀，续行是裸文本。

规则因此是：**带记号的行走记号的判定；不带记号的续行继承上一行的判定**。
这样既保住多行 Error 的正文，也不会把多行 Info 的续行误收进来。

入环时同时**剥掉 ANSI 转义序列**（日志开头有 `\e[0m`，级别记号前有颜色如 `\e[31m`），
落盘文件是干净文本，`grep` 和肉眼都清爽。

### 4.4 硬拦截

```c
#ifndef CONFIG_SYSLOG_PRIORITY
#  error "myvendor_diaglog 需要 CONFIG_SYSLOG_PRIORITY=y 才能按级别过滤"
#endif
```

少了它，所有行都判不出级别 → **静默地一条都不记**。那是唯一会让整个长测白跑的失败模式，
所以宁可编译失败。

---

## 5. 落盘位置与文件生命周期

```
/mnt/kv/diag/dNNN_YYYYMMDD_HHMMSS.txt      （LFS 视角 /diag/…）
/mnt/kv/diag/seq                           序号计数器（本目录自己的）
```

**刻意与崩溃转储分开**：`/mnt/kv/coredump/` 是 coredump 的地盘（`nNNN_*.txt`），
本模块用自己的 `/mnt/kv/diag/`，上位机按目录区分。命名形状沿用 coredump 那套，
但**序号计数器是独立的** —— 共用会互相把对方的编号推高。

### 5.1 上位机抓取（BLE Companion FS）

App 的「日志抓取」页经 BLE FS 把这两类日志一起拉到本机，靠设备侧
`transfer_backend.c` 的 `xfer_resolve()` 映射：

| 协议路径 | 设备实际路径 | 判定函数 |
| --- | --- | --- |
| `coredump` / `coredump/…` | `/mnt/kv/coredump/…` | `coredump_request()` |
| `diag` / `diag/…` | `/mnt/kv/diag/…` | `diag_request()` |

两者是**独立的路径命名空间**，不能合并：`coredump_request()` 只认 `coredump`
前缀，`diag_request()` 只认整个 `diag` 分量（所以 `diagnostics` 之类不会被误吞）。
加 diag 映射之前，App 只能看到崩溃转储，诊断日志虽然在盘上却取不出来。

路径与文件名**不用 `snprintf` 拼装**：数字由手写的固定宽度编码器写入。
一是绕开仓库的路径拼接写入门槛，二是这一段代码本身更容易审计（没有可变格式串）。

> **踩过的坑（已修）**：文件名戳解析的索引整体差一位（`d` + `NNN` + `_` 占 0..4，
> 我按 4 起算，于是把 `_` 当成数字读）。后果是**每个文件都解析失败、清理永不触发**，
> "最多 10 条"形同虚设。同时 `drop_oldest` 的哨兵用了 `0`，而降级名 `dNNN.txt` 的合法戳
> 正是 0，两者混淆；现在用 `LONG_MAX` + `have` 标志（与 coredump 的 `UINT64_MAX` 同构）。

### 5.2 运行期清理（2026-09-25 补齐）

原先清理只挂在**开机**与**按天滚动**两个点上 —— 设备不重启、不跨天，目录就只涨不落；
而且旧实现的删除失败是**静默 break**（`drop_oldest` 直接 `return false`，没人看返回值），
堆到把 `/mnt/kv` 撑满都没人知道。用户现场就是这两句话：目录里"还是很多"、"清不掉"。
现在改成：

| 事件 | 行为 |
| --- | --- |
| 开机 | 新建一个文件（序号 +1） |
| 跨天 | 再开一个（"一直开机就一天一个"） |
| 超过 `DIAGLOG_KEEP`（=10） | 按时间戳删最旧的 |
| **运行期每拍** | `myvendor_diaglog_prune()` 补一次清理（coredump 侧同形：`myvendor_coredump_prune()`）；diag 线程每拍调用，两处都自带限频 |
| **成功一次即止** | 本次运行清成功一次就整场空转（用户口径："一次运行只需要清理成功一次就不清理了"）；**失败**才按 `DIAGLOG_PRUNE_RETRY_S = 600 s` 重试，且失败每次都出声 |
| 目录里的"非日志" | 除 `seq` 与能识别的日志名外**一律当垃圾删掉**（用户口径："除了 seq 和日志，其他的都当垃圾清理"） |
| 环满 | **丢新字节**，不阻塞、不覆盖未落盘内容 |
| 写失败 / 删不掉 | 内容留在环里下次再试；**首次**失败报一行，之后静默（否则"完全没落盘"会无声无息）；**绝不格式化、绝不复位** |

三处细节值得单独记（都是"看起来没事"的类型）：

1. **"能识别的日志名"是确切形状**，不是"像日志"：`dNNN_YYYYMMDD_HHMMSS.txt`（24 字符）
   或降级名 `dNNN.txt`（8 字符），并**兼容早期沿用的 `n` 前缀**。以前是 `len < 24 就算数`，
   于是 `nonsense.txt` 既进不了计数、也进不了垃圾清理 —— 永久占着目录。现在它会被清掉。
2. **计数与删除必须同一把尺子**：数文件时若只认 `d` 前缀、而解析函数认 `d`/`n` 两种，
   改名之前落下的 `n*` 文件就"占着目录但不算数"，永远不会被删。现在两边都走
   `diaglog_name_stamp()`。
3. **序号不再跳号**：文件名**只算一次**并缓存（`s_path_ready` / `s_seq`）。旧实现每次进入
   `myvendor_diaglog_start()`（首次注册失败时每 ~10 s 会重试进来一次）都消耗一个序号，
   现场表现为 `d665 / d667 / d669…`，每次开机跳 2。

失败一律 WARNING 出声，与 coredump 侧同形，便于一起 grep：

```
diaglog: prune stuck count=12 keep=10
diaglog: prune unlink /mnt/kv/diag/d665_20260925_100431.txt failed errno=5
[coredump] prune stuck count=13 keep=10
```

两个目录的保留个数**各 10**（`DIAGLOG_KEEP` 与 `CONFIG_MYVENDOR_COREDUMP_KEEP`），
曾经一边 10 一边 20，用户口径是"每个只保留 10 个文件"，故 2026-09-25 统一。

---

## 6. 配置依赖（三个都会"静默生效/静默失效"）

| 配置 | 值 | 少了会怎样 |
| --- | --- | --- |
| `CONFIG_SYSLOG_REGISTER` | `y` | `syslog_channel_register` **链接不上**（直接编译失败，安全） |
| `CONFIG_SYSLOG_MAX_CHANNELS` | **`2`** | **危险**：默认值是 1，而 `syslog_channel.c` 在 `MAX_CHANNELS == 1` 时执行 `g_syslog_channel[0] = channel;`，也就是**注册会替换掉默认通道**，**串口输出全部消失** —— 而串口是唯一能看见板子的窗口 |
| `CONFIG_SYSLOG_PRIORITY` | `y` | 级别过滤失效（有 `#error` 拦着） |

镜像里可验证通道顺序正确：`g_syslog_channel[0]` 指向 `&g_default_channel`（串口），
`[1]` 留给 diaglog。

**可见代价**：开了 `CONFIG_SYSLOG_PRIORITY` 之后，**控制台每行都带级别词**了，例如
`[…] [ ERROR] SD: …`。这是给用户主要调试窗口带来的格式变化。

> **构建陷阱**：改 defconfig **不会**自动生效 —— CMake 只在 defconfig 的**路径**变化时
> 重新初始化配置，同路径改内容是静默忽略的。改完要删 `cmake_out/<cfg>/.config`、
> `touch nuttx/CMakeLists.txt` 再构建，并 **grep 生成的 `.config` 确认值真的落地**。
> 详见 [board_guide.md](board_guide.md) §7.3。

---

## 7. 限制

1. **环满丢的是新字节。** 2 KB 环 + 500 ms 刷一次，异常稀疏时几乎总是空的；
   真正刷屏时保住的是"较早"的那一段，最新的会丢。这是"尽量落盘就好"的语义。
2. **刷盘会占用 diag 线程。** 正常情况下是毫秒级；卡处于"要坏没坏"窗口时，
   一次 flush 可能耗时数秒（走的是带判死闸门的路径，不会退化成每块 2~4 s）。
   代价与 GNSS 槽断电约 1.5 s 同类。
3. **只读挂载期间不再落盘。** 只读 LFS 下 `open(O_WRONLY)` 返回 `-EACCES`，
   内容留在环里（崩溃时仍能随转储写出）。写者接线（`sf32lb_sd_lfs_readonly()`）
   仍是待办，见 [sd_recovery.md](sd_recovery.md) §8。
4. **稳态写入是明知代价。** 这套机制本质上是**定期往那张正在退化的卡上写**。
   用户明确选择了这个取舍（"尽量落盘就好了"），而且只落异常级别之后写入量已大幅下降 ——
   但它是真实存在的额外负载，排查卡本身的问题时要记得把它计入。
5. **`serr()` 是空的。** `nuttx/include/debug.h` 里 `serr` = `_err` 需要
   `CONFIG_DEBUG_SCHED_ERROR`，而它是**关**的，于是 `serr` = `_none`
   （`do { if (0) syslog(...); } while (0)`）。实证：镜像里搜不到
   `"ERROR: sifli_spibus_initialize"`、`"WARN: mkdir"` 这些字符串。
   也就是说**代码里用 `serr` 写的那批"错误报告"从未输出过**，自然也不会落盘。
   真正会出现的异常都是直接 `syslog(LOG_ERR/WARNING, ...)` 写的。
   要打开需 `CONFIG_DEBUG_SCHED_ERROR=y`（会连带打开 NuttX 调度器的错误输出），**尚未开启**。

---

## 8. 验证

主机侧测试（`#include` **真实源文件**，只桩掉 `up_irq_save/restore`、
`syslog_channel_register` 和三个 nuttx 头）共 **20 条断言全过**：

- 级别留存/丢弃（ERROR/WARN 留，INFO/DEBUG/NOTICE 丢）；
- 有颜色与无颜色两种前缀都能识别；
- 级别记号被切成两次 write 仍能识别；
- 逐字节 `putc` 与整块 `write` 等价；
- 多行 Error 的续行留存、多行 Info 的续行丢弃；
- `INFO → ERROR → INFO` 状态能复位；
- 超长无记号文本（>64 B）不越界、不误收；
- 环满时最早的仍在、最新的被丢。

写测试的过程立刻抓出一个真 bug：`g_pfx` 没有 NUL 结尾却传给了 `strstr`（会越界读），
缓冲区已改为 `PFX_MAX + 1` 并在追加时补 `\0`。

镜像侧验证：`/mnt/kv/diag`、`/mnt/kv/diag/seq` 两个字面量在 `nuttx` 里；
五个级别记号字面量都在；`g_ring` = `0x800`（2 KB）；
`myvendor_diaglog_start/flush/snapshot` 均在 `.text`（未被 `--gc-sections` 回收）；
四个调用点齐全（start ×1、flush ×2、snapshot ×1）。

---

## 9. 日志级别口径（2026-09-25 定稿）

落盘门槛是 WARNING（§4.2）：**只有 WARNING 及以上会进 `/mnt/kv/diag`**。所以级别不是
"心情"，而是"这条信息要不要留下来"。定稿后的分档：

| 档 | 用在哪 | 例子 |
| --- | --- | --- |
| `LOG_ERR` | **真故障**：不能自愈、需要人介入或数据可能丢 | `no free reader stack`、`LFS: mounted READ-ONLY`、`SD: declared DEAD`、`instance alloc fail`、`ctl: stack used …` 超限 |
| `LOG_WARNING` | **可恢复异常**（后面紧跟自愈/重放；留在归档里够复盘） | `[radio] heal:`、`SD: … FAILED after N tries`（下一行就是重放/重识别）、`gov re-armed by diag`、`prune stuck` |
| `LOG_INFO` | 例行与正常结束（**不落盘**，控制台可见） | 会话建立/结束、`hci_disc`、服务起停、`bicycle started …` |
| `LOG_DEBUG` | 排查工具（**默认掩码里没有 DEBUG 位**，常态一行不出） | `hci tx/rx` 命令级追踪、GATTC 槽表 dump |

配套的三条**归属**规则：

1. **服务层统一 `syslog()`**（不用 `printf`）：`ble_companion` / `ble_sensor` 等模块各自
   用 `LOGI/LOGW/LOGE` 宏统一加模块前缀（`ble_sensor: ` 等），一条事件一行。
2. **界面层只走 LVGL 日志**：UI 代码用 `LV_LOG_INFO/WARN/ERROR`，由 LVGL 栈自己的
   print 回调（`lv_nuttx_entry.c` 的 `syslog_print()`）落到 syslog —— 我们**不再注册
   第二个回调**（`lv_nuttx_init()` 会把它覆盖掉，注册了也是白注册）。
   - 运行级别 `CONFIG_LV_LOG_LEVEL_WARN`（本板）：LVGL 只出 warn/error；
   - ⚠ `LV_LOG_USER`/`LV_LOG()` **不受该级别限制**（LVGL 的门是 `LV_LOG_LEVEL <= LV_LOG_LEVEL_X`，
     USER 的序号在 ERROR 之上）⇒ 我们自己的代码**不许再用 USER 档**，一律用
     INFO/WARN/ERROR，这样 `CONFIG_LV_LOG_LEVEL` 才能在**编译期**把整段（连字符串）
     剔掉；USER 档的字符串永远会进镜像；
   - `syslog_print()` 的 `priority[]` 表**只决定报出去的级别，不是过滤杠杆**（别在那儿做过滤）。
3. **中断里只计数、不打印**：所有"中断里打印了字符"的观测都只是累加
   （`g_isr_console_chars` / `g_isrprint_*` 快照），输出一律由 diag 线程负责，
   以 ERROR 级上报并可**点名**（IPSR/CFSR/HFSR + 正文开头，见 myvendor_diag.c）。

> **顺带一条非日志的 LVGL 要求**（同样会在开机打 ERROR，别误判成日志问题）：
> 开了 freetype/thorvg 的 LVGL 要求 `lv_nuttx_init()` 所在线程的**可用栈 ≥ 32768**。
> "可用栈"= 配置值减去 NuttX 启动任务时切走的 argv 块 + TLS 块（本板实测 96 B），
> 所以配置至少要 32864；本板 `CONFIG_MYVENDOR_BICYCLE_STACKSIZE = 34816`
> （可用 34720，`nsh> ps` 实测）。详见 [board_guide.md](board_guide.md) §7.3.1。

---

日期：2026-09-16（首次实现），2026-09-17 补级别过滤与独立目录。

- `202609161736`：首版落盘（与 coredump 同目录）。
- `202609170303`：改为**只落异常级别**（`CONFIG_SYSLOG_PRIORITY` + 逐行过滤）。
- `202609170308`：迁到**独立目录** `/mnt/kv/diag/`，序号计数器独立。
- `202609162100`：`xfer_resolve()` 增加 `diag` → `/mnt/kv/diag` 映射
  （`XFER_DIAG_ROOT` + `diag_request()`），App 的「日志抓取」页据此能把诊断日志
  和崩溃转储一起拉走。见 §5.1。
- `202609250952`（之后）：**运行期清理**（§5.2）、垃圾清理、`n*` 遗留前缀兼容、
  序号不再跳号；保留个数与 coredump 统一为 **10**；级别口径定稿（§9：SD 重试类
  可恢复异常 ERR→WARN、HCI 追踪 INFO→DEBUG、UI 侧 `LV_LOG_USER`→`LV_LOG_INFO`）。

相关：[diag.md](diag.md)、[sd_recovery.md](sd_recovery.md)、
[gnss_ble_diag_log.md](gnss_ble_diag_log.md)、[sd_partition.md](sd_partition.md)。
