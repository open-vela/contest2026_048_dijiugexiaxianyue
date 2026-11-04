# 板子调试工作流（`vela_my_vendor_tools.py`）

> 全量命令表：[`vela_my_vendor_tools.md`](vela_my_vendor_tools.md)。  
> **本文是日常调试口径**：怎么编、怎么烧、怎么看日志——**尤其是：不要抢 tty**。  
> 开发主树：`vendor/my_vendor`（开发机）；本仓 `helm-one/` = 构建树里的 `vendor/HelmOne`，
> 是从开发主树导出的打包快照，非特殊声明勿手改。

入口（openvela 根目录，脚本是根上的软链）：

```bash
cd /home/jinsc/SDK/vela/openvela
./vela_my_vendor_tools.py …          # 或 python3 vela_my_vendor_tools.py …
# 等价：python3 vendor/HelmOne/docs/tools/vela_my_vendor_tools.py …
```

`BOARD_CONFIG` 固定指向 **`vendor/HelmOne/.../configs/nsh`**。

---

## 1. 一句话：不需要抢 tty

串口由常驻 **`serial_hub`** 独占打开；人、脚本、AI、烧录都是 **挂到 hub 上**，不再互相 `open(/dev/ttyACM*)` 互踢。

| 场景 | 正确做法 | 错误做法 |
|---|---|---|
| 主终端看日志 / 敲 NSH | `monitor -p /dev/ttyACM0` | 再开一个 minicom/screen 直连同一口 |
| 第二终端 / AI 跟日志 | `monitor --follow` 或 `monitor-ctl --follow --no-stdin` | `cat /dev/ttyACM0`、再抢一把直连 |
| 往板子发命令 | `monitor sys` / `printf '…\n' \| monitor-ctl` | 临时 `echo > /dev/ttyACM0` |
| 烧录 | 直接 `flash`（工具会 **lease 让出 → sftool → 收回**） | 先 `pkill` monitor / 手动关串口窗口 |
| 停掉串口服务 | `monitor stop`（真要收工时） | `pkill -f serial_hub` / 删 `/tmp/vela-serial-*.lease` |

**烧录时主窗口可以继续开着**：flash 只让 hub **短暂 release**（关物理 fd、客户端不断），结束后 **acquire + 复位脉冲**，日志缓冲还在，`monitor-ctl` 也不用重连。

禁止清单（会制造「看起来像抢 tty」的假故障）：

1. **不要** `pkill` hub / monitor（烧录只走工具自带的 release/acquire）。
2. **不要**手动删 `/tmp/vela-serial-*.lease`（除非确认 hub 已死且 flock 残留）。
3. **不要**再用第二个程序直连同一 `/dev/ttyACM*`（CH340 一换持有者常带复位，日志断、状态乱）。

模型：

```
/dev/ttyACM0  ──独占──►  serial_hub（lease + abstract mux + spool）
                              │
          ┌───────────────────┼───────────────────┐
          ▼                   ▼                   ▼
     monitor 交互        monitor --follow      flash（release/acquire）
     / monitor-ctl
```

- 信物：`/tmp/vela-serial-<串口名>-<hash8>.lease`
- 落盘日志：`~/.cache/vela/serial/<串口名>-<hash8>.log`（可 `tail -f`）
- 第一个客户端会自动拉起 hub；之后都是 mux，**不抢 TTY**。

---

## 2. 常用命令（按顺序可组合）

```bash
# 编译 + 烧录 + 交互监视（一条龙）
./vela_my_vendor_tools.py build flash monitor -p /dev/ttyACM0

# 只编 / 只烧（monitor 开着也能烧，见 §1）
./vela_my_vendor_tools.py build -p /dev/ttyACM0
./vela_my_vendor_tools.py flash -p /dev/ttyACM0

# 交互主窗口（默认挂 hub；打开时通常带一次硬件复位抓启动日志）
./vela_my_vendor_tools.py monitor -p /dev/ttyACM0
./vela_my_vendor_tools.py monitor --no-reset -p /dev/ttyACM0   # 手测勿打断板子时

# 第二终端 / 脚本 / AI：只跟日志，不抢口
./vela_my_vendor_tools.py monitor --follow -p /dev/ttyACM0
./vela_my_vendor_tools.py monitor --follow --no-stdin -p /dev/ttyACM0

# 发一条 NSH（仍走 hub）
./vela_my_vendor_tools.py monitor sys -p /dev/ttyACM0
printf 'ctl pair phone\n' | timeout 5 ./vela_my_vendor_tools.py monitor-ctl -p /dev/ttyACM0

# 查 hub 状态 / 真收工
./vela_my_vendor_tools.py monitor --status -p /dev/ttyACM0
./vela_my_vendor_tools.py monitor stop -p /dev/ttyACM0
```

- 子命令**可组合、按书写顺序执行**：`build flash monitor`。
- `-p` **必须给真实节点**（`/dev/ttyACM0`），**不要给** `/dev/serial/by-id/...` 软链（sftool 不认）。
- **脚本化永远显式 `-p`**：不给端口时会 `input()` 等人选，后台会 EOF。
- 旧名 `monitor-ctl` / `ctl` 仍可用，等价于 `monitor` 的客户端模式。

---

## 3. 端口纪律（编号会挪）

- `ttyACM*` 会因插拔别的 USB 串口整体挪位。
- 认板子：`ls /dev/serial/by-id/` → `readlink -f` 看当前 `ttyACMx`；**烧录用真实节点**。
- 分析脚本可用 by-id；**烧录 / hub 用真实路径**。
- 口在、无字节、`sftool: Failed to connect` ⇒ **先量供电**，别先怀疑 ftab。
- 只有 `SFBL`/`CSFBL` 没下文 ⇒ ROM/等待态，补一次复位（monitor 打开默认会带）。

---

## 4. 烧录语义（哪些会动 KV）

| 命令 | 写什么 | 备注 |
|---|---|---|
| `flash` | ftab + bootloader + **main** | **不碰 KV**（coredump 证据安全） |
| `flash-fs` | `fs_root.bin` | 动文件系统侧，慎用 |
| `flash-all` | boot + factory + main + fs | 全量 |
| `flash-factory` | 仅 factory 槽 | |

- sftool `--after no_reset` ⇒ 烧完停在下载窗，**需要一次复位才启动**（flash 收回时的 pulse / monitor 打开复位正好做这件事）。
- 两段：`erase_region` → `write_flash`。
- **判成败不要管道吞退出码**：`… flash | tee` / `| tail` 看的是管道末尾命令的码。

---

## 5. 看屏幕（判断板子死活）

```bash
~/OneDrive/soft_conf/linux特殊环境/linux_tools/ffplay-cam.sh snap   # → /tmp/cam-snap.jpg
```

怀疑「板子死了」时先 snap：屏在跑多半是抓取/串口侧问题。

---

## 6. 读崩溃

```bash
# NSH 里（经 monitor / monitor-ctl，不另开 tty）
#   cat /mnt/kv/coredump/<名字>

./vela_my_vendor_tools.py --elf /path/to/nuttx decode 0x10120ebe
```

- ELF 归档：`/home/jinsc/SDK/vela/elf/`（每次 build 自动存）——事后对行号靠它。
- monitor 默认带崩溃解码（`--no-decode` 关）。
- 卡住时：`kill -USR1 <monitor-pid>` 打线程栈。

---

## 7. 常见坑

| 现象 | 处置 |
|---|---|
| 第二终端一开板子复位 / 日志断 | 有人直连了 tty；改走 `monitor --follow` |
| 烧录前先杀 monitor 才安心 | 不必；直接 `flash`，让工具 release/acquire |
| `The specified port '…by-id…' does not exist` | 给真实 `-p /dev/ttyACMx` |
| 后台 `flash` 报 `EOFError` | 没给 `-p` |
| `build \| tail` 看起来成功其实失败 | 别用管道判退出码 |
| 改 Kconfig 默认不生效 | 默认值不回灌已有 `cmake_out/*/.config`；defconfig 显式钉住并重生 config |
| 烧完不启动 | 需要复位脉冲 |
| hub 起不来 / 口粘了全角顿号 | 检查 `-p` 是否 `/dev/ttyACM0、` |
| Ctrl-C 杀不掉板内命令 | 无 `CONFIG_TTY_SIGINT` 时别跑不可打断阻塞命令 |

---

## 8. Agent / 自动化建议口径

```bash
# 跟日志（不抢口）
./vela_my_vendor_tools.py monitor --follow --no-stdin -p /dev/ttyACM0

# 发 ctl（不抢口）
printf 'ctl pair open\n' | timeout 6 ./vela_my_vendor_tools.py monitor-ctl -p /dev/ttyACM0

# 改完代码
./vela_my_vendor_tools.py build flash -p /dev/ttyACM0
# ↑ monitor 可一直开着；不要 pkill、不要另开串口
```
