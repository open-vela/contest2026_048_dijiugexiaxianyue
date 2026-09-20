# vela_my_vendor_tools 使用说明

SF32LB52 **my_vendor** 板级的构建、烧录与串口监视辅助脚本，用法风格类似 ESP-IDF 的 `idf.py` 或 SiFli 的 `sf_sdk_tools.py`。

| 项 | 路径 |
|----|------|
| 实现 | `vendor/my_vendor/docs/tools/vela_my_vendor_tools.py` |
| 推荐入口 | `vendor/my_vendor/build_board.py`（薄封装，参数相同） |
| 板级配置 | `vendor/my_vendor/boards/sf32lb52/my_vendor/configs/nsh/` |
| NuttX 输出 | `cmake_out/my_vendor_nsh/` |
| Boot 镜像 | `vendor/my_vendor/boot_loader/bin/` |
| 烧录清单 | `vendor/my_vendor/boot_loader/config/nsh/sftool_param.json`（镜像列表） |
| 分区地址 | `vendor/my_vendor/boot_loader/config/nsh/ptab.json`（实时计算） |
| 烧录快照 | `cmake_out/my_vendor_nsh/flasher_args.json`（build 后生成，类似 IDF） |

相关文档：[board_guide.md](../board_guide.md)、[build_guide.md](../build_guide.md)、[boot_2sfbl.md](../boot_2sfbl.md)、[factory_firmware.md](../factory_firmware.md)、[sd_partition.md](../sd_partition.md)、[nuttx_boot_flow.md](../nuttx_boot_flow.md)、[nuttx_ovnx_image.md](../../scripts/nuttx_ovnx_image.md)。

---

## 1. 快速开始

在 **openvela 根目录**（含 `build.sh` 与 `nuttx/`）执行：

```bash
# 日常：编译固件 + 烧录 + 串口监视（不含 NAND 文件系统）
python3 vendor/my_vendor/build_board.py build flash monitor

# 首次或全量（固件 + bicycle 资源分区）
python3 vendor/my_vendor/build_board.py build-all flash-all monitor
```

也可直接调用实现脚本（路径更长，行为一致）：

```bash
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py build flash monitor
```

子命令可**组合**，按书写顺序依次执行，例如 `build flash monitor`、`build-fs flash-fs`。

---

## 2. 命令一览

### 2.1 构建

| 命令 | 作用 |
|------|------|
| `build` | 编译 NuttX（`build.sh --cmake`）；结束后按需自动 `build-boot`、`build-fs`、`wrap` |
| `build-boot` | 仅构建 `ftab.bin` + `bootloader.bin` → `boot_loader/bin/` |
| `build-factory` | 编译工厂变体 `configs/nsh-factory` → `cmake_out/my_vendor_nsh-factory/nuttx.flash.bin` |
| `build-fs` | 仅打包 NAND LittleFS → `boot_loader/bin/fs_root.bin` |
| `build-all` | **main** → **factory** → **始终** `build-boot` → wrap → 若启用 bicycle 则**始终** `build-fs` |
| `pack-sd-img` | 打 SD 整盘 `.img`（ftab + bootloader + **main** + **factory** + LFS 种子） |
| `wrap` | 将 `nuttx.bin` 包装为 `nuttx.flash.bin`（OVNX 头 + CRC，烧录用） |
| **`pack-fw`** | 同上，并改名为 `cmake_out/.../fw/1.0.0-Helm-One.bin` 供 OTA（别名 `pack-ota`） |
| `menuconfig` | Kconfig 图形/终端配置 |
| `savedefconfig` | 将当前配置写回 `defconfig` |

### 2.2 烧录

| 命令 | 烧录内容 |
|------|----------|
| `flash` | `ftab.bin` + `bootloader.bin` + `nuttx.flash.bin`（**不含**文件系统 / factory） |
| `flash-factory` | 仅工厂固件 @ ptab `factory`（SD：`0x62500000`）；不动 ftab/bootloader/main/fs |
| `flash-fs` | 仅 `fs_root.bin` @ `0x63800000` |
| `flash-all` | `sftool_param.json` 中的**全部**条目（含 factory 与 `fs_root.bin`） |

### 2.3 其它

| 命令 | 作用 |
|------|------|
| `monitor` | 串口终端（默认 1000000 波特，支持 NSH / bttool） |
| `clean` | Ninja `clean`（保留 `.config`） |
| `distclean` | `build.sh distclean`，删除 CMake 输出目录 |
| `fullclean` | 删除 `cmake_out/my_vendor_nsh/` 及 `nuttx/.config` 等 |
| `complete bash\|zsh` | 输出 shell 补全脚本 |
| `complete install` | 写入 `~/.bashrc` / `~/.zshrc` |
| `help` | 打印帮助 |

---

## 3. 构建流程说明

### 3.1 `build` 自动步骤

`build` 在 `build.sh` 成功后依次检查：

1. **boot 镜像** — `boot_loader/bin/` 缺少 `ftab.bin` 或 `bootloader.bin` 时，自动执行 `build-boot`（与单独跑 `build-boot` 相同）。
2. **文件系统** — `defconfig` 含 `CONFIG_MYVENDOR_BICYCLE=y` 且缺少 `fs_root.bin` 时，自动执行一次 `build-fs`（**仅缺失时**构建，不强制刷新）。
3. **OVNX 包装** — `AUTO_WRAP_NUTTX=True` 时生成或更新 `nuttx.flash.bin`（供二级 boot 校验与烧录），并在同目录 `fw/` 下写出 OTA 文件（`1.0.0-Helm-One.bin`）。单独打包可跑 **`pack-fw`**。
4. **flasher_args** — 写入 `cmake_out/my_vendor_nsh/flasher_args.json`。

若 `nuttx.bin` 比 `ftab.bin` 新，会提示建议重新 `build-boot`（分区体积变化时必须）。

### 3.2 `build-fs` 做什么

1. `boards/.../my_vendor/mkfs/` — 烧录文件系统内容（直接编辑此目录）
2. `scripts/build_fs_root.sh` — 生成 LittleFS 镜像 `boot_loader/bin/fs_root.bin`

运行时挂载点统一为 `/mnt/lfs`（NAND 或 SD/eMMC，见 `sf32lb_nand.c` / `sifli_ap.c`）。大体积字体/图片**不要**打进 ROMFS，走此分区。

### 3.3 `build` 与 `build-all` 的区别

| | `build` | `build-all` |
|--|---------|-------------|
| 产品 NuttX + wrap | ✓ | ✓ |
| 工厂 NuttX + wrap | ✗（单独 `build-factory`） | ✓ **每次** |
| `build-boot` | 仅 boot 镜像**缺失**时自动构建 | **每次**执行（app/factory 编完后再 boot） |
| `fs_root.bin` | 仅**不存在**时自动构建 | bicycle 开启时**每次**重建（boot 之后） |

改动了 `mkfs/` 资源后，用 `build-fs` 或 `build-all`，再 `flash-fs` 或 `flash-all`。

---

## 4. 烧录流程说明

烧录通过 **sftool**。地址**不在** `vela_my_vendor_tools.py` 里写死，而是：

1. **`sftool_param.json`** — 定义烧哪些镜像（`path` + `ptab_img`）
2. **`ptab.json`** — 按 `base + offset` 计算各 `img` 分区地址
3. **`flasher_args.json`** — `build` / `build-boot` / `build-fs` 结束后写入 `cmake_out/my_vendor_nsh/`，`flash*` 优先读此文件

逻辑实现在 `vendor/my_vendor/scripts/flash_args_lib.py`；也可手动生成：

```bash
python3 vendor/my_vendor/scripts/gen_flasher_args.py
```

示例（地址随 `ptab.json` 变化，勿手抄）：

| 镜像 | ptab_img | 来源目录 |
|------|----------|----------|
| `ftab.bin` | `ftab` | `boot_loader/bin/` |
| `bootloader.bin` | `bootloader` | `boot_loader/bin/` |
| `nuttx.flash.bin` | `main` | `cmake_out/my_vendor_nsh/` |
| `nuttx.flash.bin`（工厂） | `factory` | `cmake_out/my_vendor_nsh-factory/`（`build-all` / `pack-sd-img` / `flash-all` / `flash-factory`） |
| `fs_root.bin` | `fs_root` | `boot_loader/bin/` |

若 `flasher_args.json` 不存在，`flash` 会按 ptab + sftool_param **现场计算**（与生成文件内容一致）。

### 4.1 三种 flash 模式

```
flash          →  ftab + bootloader + nuttx          （日常迭代产品固件）
flash-factory  →  factory 槽 only                     （工厂 MTP 固件）
flash-fs       →  fs_root.bin only                    （只更新 /mnt/lfs）
flash-all      →  ftab + bootloader + main + factory + kv + lfs
                 （出厂 / 换板；不含 /mnt/fat）
```

`/mnt/fat`（地图 + 字体）**不能**串口 sftool（sparse 段数会撑爆命令行），用 `pack-sd-img` / `burn-sd`。

- 普通 **`flash` 不要求** `fs_root.bin` 或 factory 镜像存在，也不写 kv/lfs/fat。  
- **`flash-all`** / **`pack-sd-img`** 缺少 factory 或清单内文件会报错并提示 `build-all`。  
- **`flash-fs`** 仅需 `fs_root.bin`；会先 `erase_region` 擦除 FS 分区，再 sparse 写入（与 `mkfs/` 完全一致）。

烧录前建议**复位开发板**，复位后约 2 秒内开始下载。

### 4.2 整片擦除

```bash
python3 vendor/my_vendor/build_board.py flash --force
```

`--force` 向 sftool 传递 `--erase-all`，先擦整片 NAND 再写入（避免旧 ftab 导致 skip）。

---

## 5. 全局选项

| 选项 | 说明 |
|------|------|
| `-p` / `--port` | 串口（如 `/dev/ttyUSB0`、`COM19`）；默认用脚本顶部 `PORT` 或自动检测 |
| `-j` / `--jobs` | 并行编译任务数；默认 CPU 核心数 |
| `-b` / `-B` / `--baud` | `monitor` 波特率（默认 `1000000`） |
| `--force` | `flash*` 时 NAND 整片擦除 |
| `--dtr {0,1}` | monitor DTR（CH340 接 RESET 时无输出可试 `--dtr 0`） |
| `--rts {0,1}` | monitor RTS |
| `--no-reset` | monitor 打开时不发硬件复位 |
| `--no-smart-bttool` | 禁用 bttool 本地回显 |

---

## 6. 脚本顶部配置

切换板子或环境时，编辑 `vela_my_vendor_tools.py` 顶部常量（无 `-b` 板名参数）：

| 常量 | 含义 |
|------|------|
| `BOARD_CONFIG` | 板级 `configs/nsh` 相对 openvela 根的路径 |
| `BOOT_CONFIG` | NAND 分区与 sftool 配置目录 |
| `BOOT_BIN_DIR` | `ftab.bin` / `bootloader.bin` / `fs_root.bin` 输出目录 |
| `SIFLI_SDK` | SiFli SDK 路径（`build-boot` 用）；`None` 则自动探测 |
| `PORT` | 默认串口；`None` 为自动检测 |
| `MONITOR_BAUD` | 监视波特率 |
| `AUTO_WRAP_NUTTX` | build/flash 前自动 wrap |
| `JOBS` | 默认 `-j`（`0` = 全部核心） |

**输出目录**不由脚本指定，遵循 openvela 规则：`cmake_out/<板目录名>_<配置名>/`，当前为 `cmake_out/my_vendor_nsh/`。

---

## 7. 常用工作流

### 7.1 只改应用 / NuttX 代码

```bash
python3 vendor/my_vendor/build_board.py build flash monitor
```

### 7.2 改了分区表或 bootloader

```bash
python3 vendor/my_vendor/build_board.py build-boot flash
# 或 build 会在缺 boot 时自动 build-boot
```

### 7.3 改了 bicycle 图片 / 轨迹等资源

```bash
python3 vendor/my_vendor/build_board.py build-fs flash-fs
```

`flash-fs` 会先擦除 FS 分区再写入 sparse 段，设备 `/mnt/lfs` 与 `mkfs/` 一致。

### 7.4 新板或出厂镜像

```bash
python3 vendor/my_vendor/build_board.py build-all flash-all --force
```

### 7.5 改 Kconfig

```bash
python3 vendor/my_vendor/build_board.py menuconfig
python3 vendor/my_vendor/build_board.py savedefconfig
python3 vendor/my_vendor/build_board.py build
```

改 `defconfig` 后若 CMake 未刷新，可在 `cmake_out/my_vendor_nsh` 执行 `cmake --build . --target resetconfig` 再 `build`。

---

## 8. 串口监视（monitor）

- 依赖 Python `pyserial`；优先使用 `serial.tools.miniterm`。
- NSH：Enter 发送 LF（`\n`），与 NuttX readline 一致。
- 默认打开串口时**硬件复位**，便于抓取 SFBL 起的启动日志；不需要时用 `--no-reset`。
- NSH Tab 补全需固件含 `CONFIG_READLINE_TABCOMPLETION=y` 并已重新 `build`。
- **Ctrl-S 暂停显示 / Ctrl-Q 恢复（本地行为，不下发给板子）**：monitor 自己在
  `console.getkey` 上拦这两个键（并把控制台 tty 的 `IXON` 关掉 —— pyserial 的
  `Console.setup()` 只清 ICANON/ECHO/ISIG，不清 IXON，于是 Ctrl-S 会被终端线路
  规程当成 XOFF：只停屏幕输出，monitor 的 write 阻塞 → 读线程停 → 串口内核缓冲
  填满 → **板子那几 KB 丢掉**，Ctrl-Q 再把积压一次喷出来）。现在的语义是：
  暂停期间**串口照读不丢**，内容进内存缓冲；**Ctrl-Q 把缓冲按原顺序补上屏**，
  然后接着实时打印。没有提示行，也不落盘。
  缓冲上限 `MONITOR_PAUSE_MAX`（1 MiB），超出丢**最旧**的（保住与实时流相接的
  那一段）；补屏按 `MONITOR_PAUSE_CHUNK`（4 KiB）分块写，边补边继续收串口
  （一次倒完会让内核串口缓冲溢出丢字节）；`MONITOR_READ_TIMEOUT_S`（0.2 s）
  是补屏的响应上限 —— 读线程要靠 `read()` 返回才回到循环顶做补屏。
  注意：`--no-smart-bttool` 走的是 pyserial CLI 子进程，没有这套拦截。

### 8.1 一个串口，两个终端：那个 `/tmp` 文件就是转发枢纽

**谁先跑谁建它**（`monitor` 或 `monitor-ctl` 都行），文件名 = **工具路径 + 串口名**：

```
/tmp/vela_my_vendor_tools-<工具路径 hash>-<串口名>.sock    # 例：…-e7858aef-ttyACM0.sock
/tmp/vela_my_vendor_tools-<…>.log                         # 落盘日志（追加，可 tail -f）
/tmp/vela_my_vendor_tools-<…>.pid                         # 持有者 pid（排查用）
```

- 同一工作区 + 同一串口 ⇒ 同一文件 ⇒ `monitor` 与 `monitor-ctl` 天然会师（根目录的
  `./vela_my_vendor_tools.py` 软链接和 `docs/tools/vela_my_vendor_tools.py` 是同一份）。
- 不同工作区副本、或另一块板子 ⇒ 不同文件，互不干扰（`-p` 决定串口名那一段）。
- **都走了它就消失**：最后一个流式客户端退出后约 5 s（`MONITOR_BROKER_IDLE_S`），持有者收工，
  socket 与 pid 文件一起没了；`.log` 留着方便事后翻，下次谁先跑谁再建。
- 客户端数量不限；`VELA_MONITOR_SOCK` / `--sock` 可覆盖路径。

```bash
# 终端 1：操作者——原命令，不用加参数
./vela_my_vendor_tools.py build flash monitor -p /dev/ttyACM0

# 终端 2：AI/脚本——**不带命令就是监听**（以前它只打印一行状态就退出）
./vela_my_vendor_tools.py monitor-ctl -p /dev/ttyACM0                    # 监听 + stdin 按行转发
./vela_my_vendor_tools.py monitor-ctl -p /dev/ttyACM0 --no-stdin         # 只监听
./vela_my_vendor_tools.py monitor-ctl -p /dev/ttyACM0 --follow-replay 0  # 不回放历史
./vela_my_vendor_tools.py monitor-ctl -p /dev/ttyACM0 --status           # 一行状态
./vela_my_vendor_tools.py monitor-ctl -p /dev/ttyACM0 --rx               # 只读环形缓冲
./vela_my_vendor_tools.py monitor-ctl -p /dev/ttyACM0 sys                # 一次性：发一条、收输出
```

- 终端 2 也可以**第一个跑**：那时它把枢纽建起来（内部拉起一个持有串口的 broker），
  之后你跑的 `monitor` 看到枢纽活着就**挂上去当客户端**，不会自己去开串口
  （同一个 tty 两个进程会互相抢字节）。
- 这类客户端走的是**套接字流式协议**，不经过 miniterm，和操作者窗口互不打扰；
  Ctrl-C 只断开自己，串口与另一个终端不受影响；重新挂上按 `replay`（默认 256 KiB）补回错过的。

实测（2026-09-19）：先只跑 `monitor-ctl -p /dev/ttyACM0` → 枢纽建立、连续收到 4 KB+；随后跑
`monitor` → 打印"客户端模式"并同时收到同一份数据；从终端 2 发 `echo …`，板子执行、**两个终端
都看到**；两边都退出 → socket 与 pid 自动消失、串口释放。

### 8.2 那"文件"其实是个程序：`serial_hub.py`

`/tmp` 里那个端点的**持有者**是一个独立程序：`docs/tools/serial_hub.py`。谁都能用它 ——
不只本工具，别的串口工具照同一个约定接上就行。

```bash
python3 vendor/my_vendor/docs/tools/serial_hub.py /dev/ttyACM0 -b 1000000            # 前台
python3 vendor/my_vendor/docs/tools/serial_hub.py /dev/ttyACM0 --reset              # 起来先复位（抓 SFBL 起）
python3 vendor/my_vendor/docs/tools/serial_hub.py /dev/ttyACM0 --no-idle-exit       # 手工长驻
```

- 端点按**串口**命名：`/tmp/vela-serial-<串口名>.sock`（一块板子一个端点）。本工具的
  `monitor` / `monitor-ctl` 用同一个名字，所以"手工起 hub + 跑 monitor"、"先跑
  monitor-ctl + 后跑 monitor"都能会师。
- **它自己会死**：最后一个客户端离开后 5 s（`--idle-timeout`）自己退出并删掉套接字；
  期间若串口正被让给 flash（`release`）则顺延。手工起时如果一直没人连，它不会自尽
  （从"有过客户端又离开"才开始计时），要它长驻加 `--no-idle-exit` 更明确。
- 协议是逐行 JSON（换任何语言都能接）：`status` / `rx` / `send` / `tx`(base64) /
  `attach`(流式，带 `replay`) / `release` / `acquire` / `pulse` / `shutdown`，
  详见脚本头部注释。
- 本工具需要"有人持有串口"而当时没人持有时，会**直接把 serial_hub.py exec 起来**
  （所以 `ps` 里看到的就是这个程序，不再是伪装成 monitor 的进程）。

### 8.4 卡死与自愈（2026-09-20 现场）

「hub 卡死」在串口这条线上其实是**三个不同的故障**，现象像、修法不同：

**(1) 读线程死了 —— socket 还活着，板子却"哑"了。**
判据：`monitor-ctl --status` 回 `ok:true`、往口里写命令也不报错，但屏幕 / `--rx` 不再出数据。
真因：monitor 自己的读线程在让出/收回（烧录）的竞态里吃到一次 `SerialException` 就
`alive=false` 直接退出，`miniterm.join()` 之后没人重启它 —— 串口再没人排空，板子的输出
全堆在内核 tty 缓冲里。取证：

```bash
kill -USR1 <monitor_pid>     # faulthandler 已注册；栈会打进 monitor 的日志
# 修复前：进程里没有一条在读串口的线程（只剩 socket 服务 + 键盘线程）
```

修好的行为：`SifliMiniterm.reader()` 遇断线不再退出，而是**关 fd → 等串口回来 → 重开 → 接着读**，
并留一行 `*** monitor: 串口断开，等它回来（读线程不再退出） ***`。

**(2) 客户端不读，把读线程钉死。**
`push()` 是在读线程里对每个流式客户端 `sendall`：某个"只连不读"的客户端（终端被暂停、
跟随进程僵住）会把读线程钉住，串口随之中断排空。现在客户端套接字用 **1 s 超时**，超时就
**摘掉这个客户端**并留一行 `*** serial-hub: 某个客户端读取超时，摘掉它（可重新 attach） ***`；
`ser.write_timeout` 也设了 1 s，写不动就报错给请求方而不是无限等。

**(3) 谁在持有串口 / 怎么起一个靠谱的持有者。**
`monitor` 在"没有常驻 broker"时会把 broker 跑在**自己进程内**（legacy 路径），一旦它自己的
读线程出问题，整个口就没人排空。长期挂机建议显式起独立 hub：

```bash
kill <旧 monitor/broker pid>          # 它的 socket 随之消失
nohup setsid python3 vendor/my_vendor/docs/tools/serial_hub.py /dev/ttyACM0 -b 1000000 \
      --sock /tmp/vela-serial-ttyACM0.sock --no-idle-exit >>/tmp/vela-hub-fixed.log 2>&1 &
```

端点路径不变，`monitor` / `monitor-ctl` 会自己接回来（broker 模式下断开即 2 s 重试）。
`serial_hub.py` 的读循环自带"断线 → 等回来 → 重开"，比 in-process broker 更抗造。

### 8.5 常驻 broker（`monitor --broker`，可选）

**要解决的问题**：想做到"连 monitor 都退出（比如去烧录）也不断开"，就必须有人替你拿着串口。
直连模式做不到这一点 —— 串口和套接字都绑在交互式 monitor 进程上，退出即关。

**做法**：`monitor --broker` 会自动 fork 一个 `monitor-broker`（前台常驻），由它独占串口、
维护 RX 环形缓冲（64 KiB）与落盘日志、服务套接字；交互式 `monitor` 变成它的**流式客户端**。
不想要 broker 就不加这个参数，一切照旧。

```bash
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py monitor --broker   # 客户端
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py monitor-ctl --rx   # 同时用
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py monitor-broker status|stop
```

- 客户端**随时退出/重开**：串口不断、套接字不断；重开时按 `replay`（默认 256 KiB）把
  错过的那一段补回屏幕。
- 落盘日志：`/tmp/vela-my-vendor-monitor-<uid>.log`（追加写，可直接 `tail -f`）。
- 端口与客户端都归 broker，所以 `flash` 可以**在 monitor 开着的时候**借用串口：
  `release`（broker 关 fd、读循环挂起，缓冲保留）→ sftool → `acquire` + `pulse`
  （补一次复位，让烧完的开机日志也进缓冲）。分界线会写进日志：
  `*** monitor: 串口已让给 flash ***` / `*** monitor: 已补发复位脉冲 ***`。
- **停 broker 用 `monitor-broker stop`，不要 `pkill -f "... monitor"`**：broker 是
  `monitor` fork 出来的，`ps` 里两者命令行一模一样，按命令行杀会误杀 broker。
  定位进程看 pid 文件 `/tmp/vela-my-vendor-monitor-<uid>.pid`。

**当前状态（2026-09-19）**：broker 侧（端口持有、缓冲/落盘、release/acquire/pulse、
`monitor-ctl`、flash 借用）已实机验证。当天那个"客户端几十秒后不再上屏 / 掉线"已经修掉：
根因是**服务端把控制连接的读超时当成了断开** —— 流式客户端（`--follow`、交互式 monitor
的壳）平时只读不写，静默 30 秒就被服务端踢掉。现在读超时视为正常、继续等；实测监听端
连跑 75 s 不断，同期 monitor 出了 1049 行。

### 8.4 后台跑 monitor：`docs/tools/pty_run.py`

`monitor` 走 pyserial `miniterm`，而它的 `Console` 需要**真 tty**：`nohup ... monitor &`
或 `... > log &` 会直接抛 `termios.error: (25, 'Inappropriate ioctl for device')`。
`pty_run.py` 给子进程一个 pty 并把输出写进日志（追加）：

```bash
python3 vendor/my_vendor/docs/tools/pty_run.py /tmp/mon.log \
    python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py -p /dev/ttyACM0 monitor --no-decode &
```

`--no-decode` 是避坑：崩溃地址解码在某些输入上会让**客户端**读线程停住（带 broker 时
丢的只是屏幕显示，数据仍在 broker 缓冲里，`--rx` 可补看）。

---

## 9. Tab 补全

```bash
eval "$(python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py complete bash)"
# 或持久安装
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py complete install
source ~/.bashrc
```

**补全覆盖到哪一层（2026-09-19 重写）**：生成的是**纯 bash 词表**，按 Tab 不启动 Python（快），
而且不再只有第一层：

| 位置 | 候选 |
|---|---|
| 第一个词 | 全部子命令（按前缀过滤） |
| 链式位置 | `build flash mo<Tab>` → 还能再跟的子命令（`monitor`/`monitor-broker`/`monitor-ctl`） |
| 子命令之后 | 该子命令自己的选项（`monitor-ctl --` → `--status --rx --follow --no-stdin …`；`flash --fl` → `--flash-medium --flash-baud`） |
| `-p` / `--port` 之后 | **真实串口**（`/dev/ttyACM*`、`/dev/ttyUSB*`、`/dev/serial/by-id/*`；没有则退回文件名） |
| `-M` / `-b` / `-fb` / `-j` 之后 | `nand sd emmc nor` / 常用波特率 / 常用并行数 |
| `--sock` `--elf` `-i` `--image` `--sd` 之后 | 文件路径 |
| `monitor-broker` 之后 | `status stop` |
| `monitor-ctl ctl …` | 板级命令：`radio sensor bl sd wt watch mtp notif log dvfs gnss`，再往下还有取值（`ctl radio on|off`、`ctl dvfs auto|off|low|high|48…240`、`ctl log err|warn|…`） |

改完当前 shell 生效一次就够：`source <(python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py complete bash)`。

### 9.1 工作区环境怎么加载（`*ubuntu_get_env.sh`，2026-09-19 改写）

启动脚本里有个 `cd()` 钩子：**每次 cd 进含 `*ubuntu_get_env.sh` 的目录就 source 它**（openvela
根目录就有一份）。原来那份有三个问题，已改：

| 问题（实测） | 现在 |
|---|---|
| 每 cd 一次起 3 个 Python 跑补全生成，**0.25 s/次** | 补全生成成静态文件放 `~/.cache/vela-completions/*.bash`，之后只 `source`（纯 bash）→ **0.01 s/次**；工具文件更新会自动重生成 |
| PATH 不幂等：22→23→24→25，`bloaty` 重复 3 份 | `path_add_once`：只加一次，稳定 23 条 |
| 补全抄在这份文件里，和工具自己的 `complete install` 两处来源、会漂移 | 不再手抄，只调用工具生成 |

- 真源放在仓库：`docs/tools/vela_ubuntu_get_env.sh`（`re.sh` / `repo sync` 会清掉根目录，根目录那份是薄壳）；
  原文备份 `vela_ubuntu_get_env.sh.orig-20260919`。
- 想让"**离开工作区也还原**"（PATH 不带去别的项目）：`docs/tools/vela_ubuntu_env_hook.sh` —— 一个最小
  号的 direnv（进：存 PATH 再加载；出：还原）。用法二选一：新 shell 里 source 它一次，或把启动脚本里
  那段 `_load_ubuntu_env` / `cd` 换成 source 它。

---

## 10. 与 build.sh 的关系

`build` / `menuconfig` / `savedefconfig` / `distclean` 内部调用：

```bash
./build.sh vendor/my_vendor/boards/sf32lb52/my_vendor/configs/nsh/ --cmake ...
```

不用封装脚本时可直接 `build.sh`，产物路径相同。`vela_my_vendor_tools` 额外提供：boot/fs 自动构建、OVNX wrap、按模式拆分的 flash、monitor、补全。

---

## 11. 故障排查

| 现象 | 处理 |
|------|------|
| `未找到 SiFli SDK` | 设置脚本顶部 `SIFLI_SDK` 或导出环境变量 |
| `flash` 报缺 ftab/bootloader | `build` 或 `build-boot` |
| `flash-all` 报缺 fs_root.bin | `build-fs` 或 `build-all` |
| `flash-all` / `pack-sd-img` 报缺 factory | `build-factory` 或 `build-all` |
| bicycle 无资源 / 字体 | 确认已 `flash-fs` 或 `flash-all`，且 `/mnt/lfs` 已挂载 |
| `module resource has no attribute getpagesize` | 勿让 SiFli SDK 的 `PYTHONPATH` 污染 NuttX 构建；脚本已过滤，避免在 build 前 `source export.sh` 后不再清理 |
| monitor 无输出 | 试 `--dtr 0 --rts 0`；确认波特率与 `CONFIG_UART_BAUD` 一致 |
| monitor 打字无实时回显、Tab 才出字 | 默认崩溃解码按行缓冲导致；已修复 `vela_elf_resolve.py`；临时可加 `--no-decode` |
| 改过 defconfig 未生效 | `resetconfig` 后重新 `build` |
| msh 出现 `unknown: ~ATSF32!`、flash 连不上 | 不要在 2SFBL 里 `reboot`；用工具自带 RTS 脉冲或 msh `download`。见 [boot_2sfbl.md](../boot_2sfbl.md) 第 7 节 |
| `Failed to download stub: Timeout` | 确认 stub 阶段 `--compat true`（工具已两阶段烧录）；关掉占用 ACM 的 monitor |
| 烧录只有约 20 KB/s | 整段 `--compat true` 会把 payload 拆成 256 B+10 ms；工具已改为 compat 只灌 stub |

Boot / OVNX 细节见 [nuttx_ovnx_image.md](../../scripts/nuttx_ovnx_image.md)；2SFBL msh 与烧录窗口见 [boot_2sfbl.md](../boot_2sfbl.md)；NAND 分区说明可执行 `boot_loader/scripts/print_nand_boot_help.sh`。

---

## 12. 命令速查

```bash
# 构建
build | build-boot | build-factory | build-fs | build-all | wrap | pack-fw | menuconfig | savedefconfig

# 烧录
flash | flash-factory | flash-fs | flash-all          # 可加 -p PORT、--force

# 维护
monitor | clean | distclean | fullclean | complete | help
```
