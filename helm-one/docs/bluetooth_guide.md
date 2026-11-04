# SF32LB52 蓝牙使用指南（my_vendor）

本文说明 `vendor/HelmOne` 在 SF32LB52 上启用蓝牙的**架构**、**配置**、**编译验证**与**常见问题**。  
驱动代码源自 `vendor/sifli`（参考分支 [`sf32lb52-fix-split`](https://github.com/open-vela/vendor_sifli/tree/sf32lb52-fix-split)，提交 `29db90c`）。

编译烧录见 [build_guide.md](build_guide.md)；上电与 bringup 顺序见 [nuttx_boot_flow.md](nuttx_boot_flow.md)。  
Companion / 传感器应用见 [ble/README.md](ble/README.md)、[ble/ble_sensor.md](ble/ble_sensor.md)。

---

## 1. 能力概览

| 项目 | 说明 |
|------|------|
| 芯片 | SF32LB52 双核：HCPU（应用）+ LCPU（蓝牙协议栈 ROM） |
| HCI 传输 | H4，虚拟串口 `/dev/ttyHCI0` |
| 主机协议栈 | Zephyr zblue（`CONFIG_BT_*`）+ OpenVela Bluetooth Framework |
| 模式 | Classic BR/EDR + BLE（`CONFIG_BT_CLASSIC`、`CONFIG_BLUETOOTH_BLE_SUPPORT`） |
| 板级初始化 | `sf32lb52_devkit_lcd_bringup()` 末尾调用 `sf32lb52_bt_initialize()` |

默认 **`nsh-driver`** 配置已打开蓝牙（见下表）。**`nsh`** 为省资源默认关闭（见 [3.2 关闭蓝牙](#32-关闭蓝牙省-flash--sram)）。

---

## 2. 软件架构

```text
  应用 / NSH / Bluetooth Framework
              │
              ▼
  zblue (CONFIG_BT) ── HCI H4 ── /dev/ttyHCI0
              │
              ▼
  sf32lb52_bth4.c (NuttX bt_driver + uart_bth4)
              │
              ▼
  sf32lb52_bt_adapter.c (HCPU↔LCPU IPC mailbox)
              │
              ▼
  LCPU 蓝牙 ROM + lcpu_patch + RF 校准 (bt_rf_fulcal)
```

**关键源文件**

| 文件 | 作用 |
|------|------|
| `chips/sf32lb52/sf32lb52_bth4.c` | 注册 `/dev/ttyHCI0`，对接 NuttX 蓝牙 netdev |
| `chips/sf32lb52/sf32lb52_bt_adapter.c` | HCPU 与 LCPU 间 HCI 环形缓冲与 IPC 队列；HCI 投递失败窗口 |
| `chips/sf32lb52/sf32lb52_lcpu_boot.c` | LCPU 蓝牙 ROM 配置、NVDS、RF 校准 |
| `frameworks/connectivity/bluetooth/.../hci_h4.c`（Vela 文件）+ `vela_override/zblue/patch_net_buf_timeout.py` | HCI 读线程：读侧瞬时错误有界重开；`get_rx` 的 `K_FOREVER` 改成有上限轮询，避免卡死 tty。见 `docs/pitch/ble-hci-h4-rx-reopen.md` |
| `chips/drivers/cmsis/sf32lb52x/lcpu_patch*.c` | LCPU 固件补丁 |
| `chips/drivers/cmsis/sf32lb52x/bt_rf_fulcal.c` | RF 全校准 |
| `middleware/bluetooth/patch/sf32lb52/` | LCPU 蓝牙补丁头文件 |
| `boards/.../bsp/sifli_ap.c` | bringup 中调用 `sf32lb52_bt_initialize()` |

`CMakeLists.txt` 在 `CONFIG_UART_BTH4=y` 时才会编译上述 BT 驱动源文件。

---

## 3. 默认配置（defconfig）

`boards/sf32lb52/helmone/configs/nsh-driver/defconfig` 中已启用（`nsh` 已关闭，见 3.2）：

| 配置项 | 含义 |
|--------|------|
| `CONFIG_DRIVERS_BLUETOOTH=y` | NuttX 蓝牙驱动框架 |
| `CONFIG_UART_BTH4=y` | **HCI H4 伪串口**（启用芯片驱动的开关） |
| `CONFIG_BLUETOOTH=y` | OpenVela 蓝牙总开关 |
| `CONFIG_BLUETOOTH_FRAMEWORK=y` | Bluetooth Framework |
| `CONFIG_BLUETOOTH_FRAMEWORK_LOCAL=y` | 本机运行（非跨核 rpmsg） |
| `CONFIG_BLUETOOTH_BLE_SUPPORT=y` | BLE 支持 |
| `CONFIG_BLUETOOTH_SERVICE=y` | 蓝牙服务 |
| `CONFIG_BLUETOOTH_STACK_LE_ZBLUE=y` | LE 协议栈 zblue |
| `CONFIG_BLUETOOTH_STACK_BREDR_ZBLUE=y` | BR/EDR 协议栈 zblue |
| `CONFIG_LIBUV=y` | Bluetooth Framework / bttool 依赖 |
| `CONFIG_LIBUV_EXTENSION=y` | libuv 扩展（bttool 异步命令循环） |
| `CONFIG_BLUETOOTH_TOOLS=y` | 编译 **bttool**、**adapter_test** |
| `CONFIG_BT=y` / `CONFIG_BT_H4=y` | zblue + H4 |
| `CONFIG_BT_UART_ON_DEV_NAME="/dev/ttyHCI0"` | zblue HCI 设备名 |
| `CONFIG_BLUETOOTH_SERVICE_HCI_UART_NAME="/dev/ttyHCI0"` | 服务层 HCI 设备名 |

| `CONFIG_BSP_USING_BTIM1=y` | BT 定时器 |

`rcS` **不**自启蓝牙；测试时在 NSH 手动执行 `bttool` 即可（见第 6 节）。

### 3.1 开启蓝牙（完整选项）

`nsh-driver/defconfig` 仍保留完整蓝牙选项；若要在 `nsh` 上重新打开，在 menuconfig 中启用第 3 节表格中的各项，并执行 `savedefconfig`。

### 3.2 关闭蓝牙（省 Flash / SRAM）

当前 **`configs/nsh/defconfig` 默认已关闭蓝牙**。若暂时不用 BLE/Classic、也不需要 `bttool`，建议按本节操作，可显著减小固件（约 **400+ KB Flash**、**~210 KB 运行时 SRAM**，以 `analyze_firmware_size.py` 实测为准）。

#### 两个总开关

| 配置 | 层级 | 作用 |
|------|------|------|
| **`CONFIG_BLUETOOTH`** | 应用 / 协议栈 | OpenVela Bluetooth Framework、zblue（`CONFIG_BT_*`）、`bttool` 等 |
| **`CONFIG_UART_BTH4`** | 板级驱动 | HCI H4 伪串口 `/dev/ttyHCI0`、`sf32lb52_bth4`、bringup 里 `sf32lb52_bt_initialize()` |

只关其中一个可能仍编进部分库或仍初始化控制器；**两项都关**才算干净。

#### menuconfig 路径（推荐）

```bash
cd ~/SDK/vela/openvela
python3 vendor/HelmOne/docs/tools/vela_my_vendor_tools.py menuconfig
```

在菜单中关闭（可用 `/` 搜索符号名）：

| 搜索 / 菜单路径 | 配置 | 说明 |
|-----------------|------|------|
| `BLUETOOTH` | `CONFIG_BLUETOOTH` | **软件总开关**（Applications → Bluetooth） |
| `UART_BTH4` | `CONFIG_UART_BTH4` | **HCI 驱动总开关**（Device Drivers → Serial） |
| `BLUETOOTH_TOOLS` | `CONFIG_BLUETOOTH_TOOLS` | `bttool`、`adapter_test` |
| `MYVENDOR_BLE_TEST_APP` | `CONFIG_MYVENDOR_BLE_TEST_APP` | NSH 命令 `ble_test` |
| `EXAMPLES_HCITEST` | `CONFIG_EXAMPLES_HCITEST` | HCI 测试例程 |

保存并写回 defconfig：

```bash
python3 vendor/HelmOne/docs/tools/vela_my_vendor_tools.py savedefconfig
```

修改 Kconfig 后建议全量重编：

```bash
python3 vendor/HelmOne/docs/tools/vela_my_vendor_tools.py fullclean
python3 vendor/HelmOne/docs/tools/vela_my_vendor_tools.py build
```

用体积脚本确认蓝牙库已消失：

```bash
python3 vendor/HelmOne/scripts/analyze_firmware_size.py --brief
```

排行中不应再出现 `Bluetooth framework`、`Bluetooth (zblue)`、`bttool` 等项。

#### 可选：进一步精简

在确认无其他功能依赖时，可在 menuconfig 中一并关闭：

| 配置 | 说明 |
|------|------|
| `CONFIG_LIBUV` / `CONFIG_LIBUV_EXTENSION` | 主要为 Bluetooth Framework / `bttool` 服务 |
| `CONFIG_UNQLITE` | 本板 defconfig 中用于 `BLUETOOTH_STORAGE_UNQLITE_SUPPORT` |
| `CONFIG_DRIVERS_BLUETOOTH` | NuttX 通用蓝牙驱动框架（无 `UART_BTH4` 时通常可关） |

`CONFIG_BSP_USING_BTIM1` 为通用定时器，**与蓝牙无强绑定**，关闭蓝牙时**不必**关闭。

#### 重新开启

对 `nsh`：menuconfig 中重新勾选 `BLUETOOTH`、`UART_BTH4` 及第 3 节表格中的依赖项 → `savedefconfig` → `fullclean` → `build`。  
或直接参考仍启用蓝牙的 `configs/nsh-driver/defconfig` 合并相关行。

---

## 4. 编译与烧录

```bash
# openvela 根目录
python3 vendor/HelmOne/build_board.py build flash monitor
```

或：

```bash
./build.sh vendor/HelmOne/boards/sf32lb52/helmone/configs/nsh/ --cmake -j$(nproc)
```

修改配置后建议全量重编：

```bash
python3 vendor/HelmOne/build_board.py fullclean
python3 vendor/HelmOne/build_board.py build
```

固件体积会随 zblue / Bluetooth Framework 增大，可用 `scripts/analyze_firmware_size.py` 查看 `libzblue.a` 等占比。

---

## 5. 启动流程

1. NuttX 启动 → `board_late_initialize()` → `sf32lb52_devkit_lcd_bringup()`
2. `CONFIG_UART_BTH4` 下调用 `sf32lb52_bt_initialize()`：
   - `uart_bth4_register("/dev/ttyHCI0", ...)`
   - `sf32lb52_bt_zblue_init_once()`
3. 首次打开 `/dev/ttyHCI0` 时，`sf32lb52_bth4.c` 会：
   - `sf32lb52_bt_controller_init()` → 建立 IPC
   - `sf32lb52_bt_controller_enable()` → 唤醒 LCPU、加载 NVDS、等待 RX ring 就绪
4. 上层 Bluetooth Framework / zblue 通过 HCI 与控制器通信。

串口日志中可关注 `ttyHCI0`、`sf32lb52_bt`、`HCI`、`bluetooth` 等关键字。

---

## 6. 手动测试（bttool）

当前为 `CONFIG_BLUETOOTH_FRAMEWORK_LOCAL=y`：**无需** `bluetoothd` 自启动；在 NSH 手动运行 `bttool` 时，Framework 会在进程内拉起蓝牙服务。

### 6.1 检查 HCI 设备节点

```bash
ls /dev/ttyHCI0
```

节点存在表示 `sf32lb52_bt_initialize()` 已成功注册驱动。

### 6.2 适配器状态（`state` 返回值）

| 值 | 含义 |
|----|------|
| 0 | OFF（关闭） |
| 1 | BLE_TURNING_ON |
| 2 | BLE_ON |
| 3 | TURNING_ON（BR 正在打开） |
| 4 | ON（完全就绪，可广播/连接） |

**原则**：上一条命令未等到目标状态前，不要连续敲下一条。`enable` 后应看到状态逐步到 **4** 再执行 `adv`。

### 6.3 推荐操作顺序

#### A. 上电后首次测试（最稳妥）

```bash
nsh> ls /dev/ttyHCI0
nsh> bttool
bttool> enable
# 等待回调：Adapter state changed ... 最终为 4
bttool> state
bttool> get name
bttool> get leaddr
bttool> adv start -m legacy -r 020106038F03FF080956656C612D4254
# 纯 BLE 广播（无经典蓝牙标志）；手机用 nRF Connect 扫描 Vela-BT
```

#### B. 同一会话内关闭再打开（不要 `q`）

```bash
bttool> adv stop
bttool> disable
# 等待 state 回到 0
bttool> state
bttool> enable
# 等待 state 到 4
bttool> adv start -m legacy -r 020106038F03FF080956656C612D4254
```

#### C. 结束测试并退出

```bash
bttool> adv stop
bttool> disable
# 确认 state 为 0
bttool> q
```

#### D. 退出 `bttool` 或崩溃之后再次测试

**必须先重启板子**，再按 **A** 流程重来。不要在未重启时立刻 `bttool` → `disable` → `enable`（易触发空设备名断言崩溃）。

```bash
nsh> reboot
# 启动完成后
nsh> bttool
bttool> enable
```

### 6.4 命令输入注意

- 每条命令**单独一行**，等出现 `bttool>` 再输入；避免 `commandenable` 这类粘连。
- `adv`、`get`、`set`、`enable` 均为 **bttool 子命令**，须在 `bttool>` 下执行，不要在 `nsh>` 下直接敲。
- 广播名 **Vela-BT** 见上方 `-r` 广播包；**勿用 `-D`**（会带双模 Flags 触发系统配对）。适配器名 **XIAOMI VELA-xxx** 用 `get name` 查看。

常用子命令：`enable`、`disable`、`state`、`get name`、`get leaddr`、`adv start` / `adv stop`。  
完整说明见 OpenVela：`docs/zh-cn/device_dev_guide/connection/bluetooth/functionality_test/bttool_cmd.md`

同配置还会编入 **`adapter_test`**（一次性 Adapter API 冒烟，非交互式）。

### 6.5 手机侧扫描与连接（nRF Connect）

连接已能建立（`connection state: 3`）后马上变 `0`，多为 **SMP 配对未完成** 导致断开，不是板子重启。

**配对是双向的**：手机弹窗时，串口往往会出现 `[SSP][CONSENT]`，板端也必须确认：

```bash
# 手机地址换成日志里的，transport 0 = BLE
bttool> pair confirm 6E:54:76:78:41:D1 0 1
```

若只在一侧点「配对/取消」，另一侧超时会断开；部分手机取消后还会**暂时关闭系统蓝牙**（板子仍正常）。

**0xFF00 `ble_test` 联调固件**（如 `nsh-driver` 的 `CONFIG_MYVENDOR_BLE_TEST_APP` 构建）可能关闭 `BT_SMP` / `BT_PRIVACY`，此时读 `0xFF05`、写 `0xFF02` 不弹配对（`0xFF01` Notify 在无 SMP 时不可用）。
**产品 `nsh` / Companion 固件**则 `CONFIG_BT_SMP=y`、`CONFIG_BT_PRIVACY=y`，Companion 特征/CCCD 全部 `ENCRYPT_REQUIRED`；必须先配对，再访问（见 [ble/pairing_and_radio.md](ble/pairing_and_radio.md)）。

**仍使用 SMP 的旧固件时**：

1. nRF Connect 设置里关闭 **Bond / 绑定**（若有）。
2. 手机点 **配对**，同时串口执行 `pair confirm <addr> 0 1`。
3. 设置 → 蓝牙里**忽略**曾配对过的 Vela-BT。

板子主动推送（需已配对且开了 Notify）：

```bash
bttool> gatts notify_custom 6E:54:76:78:41:D1 hello
```

测试前可清存储：

```bash
nsh> rm -rf /data/misc/bt
```

### 6.6 BLE 外设通信推荐顺序（GATT Server）

```bash
nsh> rm -rf /data/misc/bt          # 上次崩溃后建议执行
nsh> bttool
bttool> enable                     # 等到 state = 4
bttool> set bondable 0
bttool> gatts register 3
bttool> gatts start 3
bttool> adv start -m legacy -r 020106038F03FF080956656C612D4254
# nRF Connect 连接 → 读 0xFF05 / 写 0xFF02 / 订阅 0xFF01
```

| 特征 UUID | 作用 |
|-----------|------|
| `0xFF01` | Notify/Indicate（板子 → 手机） |
| `0xFF02` | Write（手机 → 板子） |
| `0xFF05` | Read 演示（`Hello VELA!`） |

---

## 7. 应用开发提示

- HCI 统一入口为 **`/dev/ttyHCI0`**，Framework 与 zblue 配置须与此一致。
- 产品级 BLE（码表传感器、手机同步）建议在 Bluetooth Framework 上封装 GATT 服务，而非直接操作 HCI。
- 与 USB MTP 并存时注意带宽与功耗；大量 USB 传输时 BLE 连接可能受影响。
- 参考 OpenVela 官方排障文档：  
  `docs/zh-cn/device_dev_guide/connection/bluetooth/how_to_analyze_bluetooth_issues.md`

---

## 8. 常见问题

### `rx ring invalid: buf=0 expected=492`

Rev B 芯片（`rev_id >= A4`，常见 SF32LB52-N16 系列）的 LCPU 邮箱地址与旧版不同。HCPU 若读错共享内存，会看到 `buf=0`。`my_vendor` 已在 `sf32lb52_bt_adapter.c` 按芯片 revision 自动选择 `LCPU2HCPU_MB_CH1_BUF_REV_B` 地址，并在 `sf32lb52_lcpu_boot.c` 为 A4+ 下发 LCPU 蓝牙配置（对齐 SiFli SDK `SF32LB52X_REV_AUTO` 行为）。

驱动里 `sf32lb52_bt_wait_rx_ring_ready()` 的 **2000×1ms 是超时上限**，环一旦就绪就立刻返回，正常 `enable` 一般在 **几百毫秒级**（含 LCPU 上电约 500ms），不是每次固定等 2 秒。

### `/dev/ttyHCI0` 不存在

| 可能原因 | 处理 |
|----------|------|
| `CONFIG_UART_BTH4` 未启用 | 检查 defconfig，重新编译 |
| bringup 未执行 | 确认 `sifli_ap.c` 中 `sf32lb52_bt_initialize()` 未被裁掉 |
| 编译未链入 BT 源文件 | 确认 `chips/sf32lb52/CMakeLists.txt` 中 `LCPU_BT_SRCS` 已加入 |

### `sf32lb52_bt_initialize failed`

- 查看完整串口 log；常见为 LCPU 未就绪或 IPC 队列打开失败。
- 确认 `lcpu_patch.c`、`bt_rf_fulcal.c` 已编入（依赖 `CONFIG_UART_BTH4`）。

### 手机连接后立刻断开（nRF Connect 弹配对）

| 日志 | 含义 |
|------|------|
| `connection state: 3` | LE 已连接 |
| `gatts_connect_callback` | GATT 链路 OK |
| `connection state: 0` | 配对未完成/取消后断开 |

**当前固件**：连接后执行 `pair confirm <手机addr> 0 1`，并在手机点「配对」。

**0xFF00 `ble_test` 联调固件**（SMP 关闭的构建）重编译烧录后不会弹配对框；**产品 Companion 固件**相反，必须配对（`CONFIG_BT_SMP=y`，特征 `ENCRYPT_REQUIRED`）。

### 已绑定手机不自动回连 / 一直回退无定向

先看 `ctl pair phone`（记录应为身份地址 `type=0`）、`ctl pair list`（`sec=2 bonded=1`）
和 `bt_keys kv: <addr> -> RAL rl_size=3 entries=N`；`entries=0` 说明 IRK 没进控制器 RAL。
窗口外正常应先出现 `phone RPA cached ...`，再 `adv directed (paired; cached RPA ...)`；
若是 `no fresh RPA` / `directed cooldown` / `directed adv no connection`，见
[ble/directed_reconnect.md](ble/directed_reconnect.md)。首次开机/重启后无 RPA 缓存，
第一次回连走无定向是设计行为。

### 手机连接后崩溃（`mm_malloc` / `unqlite_kv_store`）

日志在 `gatts_connect_callback` 之后、`sysworkq` 写 `/data/misc/bt/bt_storage.db` 时断言，多为**配对/绑定信息或 GATT 缓存**落盘时堆损坏（与 tmpfs + unqlite 及内存压力有关）。

**规避**：

1. 用 **nRF Connect**，勿用系统蓝牙设置连接。
2. `set bondable 0`；配对弹窗选**取消**。
3. 崩溃后 `rm -rf /data/misc/bt` 再测。
4. 板级 defconfig 已关闭 `GATTS_CACHE`、`BT_SMP_ENFORCE_MITM`（需重编译烧录）。

### 退出 `bttool` 后再次 `enable` 崩溃（`Adapter Name:` 为空）

Framework 在退出 `bttool` 后会丢失进程内适配器上下文；若未重启就再次 `enable`，可能出现空设备名断言。

**操作规避**（无需改 Framework）：

1. 退出前先 `adv stop` → `disable`，确认 `state` 为 0，再 `q`。
2. 退出或崩溃后**先 `reboot`**，再按文档 6.3 节流程 A 测试。
3. `disable` / `enable` 仅在**同一会话**内循环，不要跨 `q` 重复开关。

### HCI 命令返回 `Command Disallowed (0x07)`

- 多为控制器未完全 enable 或 RX ring 未同步；`sf32lb52_bt_adapter.c` 中已有等待 ring ready 逻辑，若仍出现可尝试重启或检查 LCPU 固件补丁是否匹配芯片 revision。

### `sf32lb52 bt rx callback: -12`

`-12` 是 **`-ENOMEM`**。LCPU 仍在往主机灌 HCI，`uart_bth4` 往 `/dev/ttyHCI0` 环形缓冲写时空间不够。缓冲已经 8096 字节，持续刷这条说明 **zblue H4 RX 线程不再 `read()`**，不是「再加大一点就好」。

常见根因：`h4.c` 的 `get_rx()` 对 ACL 用 `K_FOREVER`，`net_buf` 耗尽后读线程永久堵住。`ps` 里 `bicycle_ui` 仍可有 20% CPU，BLE 工作线程多在等信号量，**看起来不像整机挂了**。

`ble_supervisor` 原先不监视这条路径，且 `companion_phone_connected()` 为真时跳过 adapter cycle。HCI 已死则不会收到 disconnect，状态机会停在僵尸「已连接」。

当前固件：

1. 读侧在 Vela 的 `hci_h4.c`（`h4_rx_reopen`，见 `docs/pitch/ble-hci-h4-rx-reopen.md`）；缓冲区等待上限在 `vela_override/zblue/patch_net_buf_timeout.py`。广播报告可丢；ACL/事件拿不到就丢包并继续抽 tty。
2. adapter 连续投递失败 ≥ 2.5 s → supervisor 请求 adapter cycle；一次 cycle 仍失败 → 升级为 **LCPU 复位 + companion task 重建**。控制器复位受 5 分钟限频；限频时 `ble_supervisor_defer_escalation()` 保留请求，窗口过后自动重试（见 `ble/companion_impl_plan.md` §7.4）。

对照日志：`sf32lb52 bt rx callback: -12 (n=… stall=… ms)` → `ble_supervisor: request adapter cycle reason=adapter (immediate)` → `adapter cycle begin`。完整因果见 [ble/companion_impl_plan.md](ble/companion_impl_plan.md) §7.4。

companion 自己堵在同步 HCI 上、监督泵进不去时，看独立 **diag** 线程（`diag: ble unhealthy` → 心跳仍在则 `request companion adapter cycle`，心跳停了则 `force LCPU reset (thread stalled)` → 仍不行才 `companion thread respawn`）。不要把「心跳超时」当成进程已死去 SIGTERM：那会再排队 `bt_le_adv_stop` 并把 zblue worker assert 掉。说明见 [diag.md](diag.md)。

### 固件体积过大

- 关闭 `CONFIG_BT_CLASSIC`（仅要 BLE 时）、`CONFIG_BLUETOOTH_TOOLS`、未使用的 Profile（AVRCP 等）。

### 与 `vendor/sifli` 的差异

- `my_vendor` BT 驱动与 `vendor/sifli` 同源；sifli `dev-ai-contest-2026` 在 `cc4704a` 曾为竞赛临时关闭 `CONFIG_UART_BTH4`，`my_vendor` 已按 `29db90c` 重新启用。

---

## 9. 相关文档

| 文档 | 内容 |
|------|------|
| [build_guide.md](build_guide.md) | 编译、烧录、menuconfig |
| [nuttx_boot_flow.md](nuttx_boot_flow.md) | 上电与 bringup 时序 |
| [board_guide.md](board_guide.md) | 板卡与 repo 配置 |
| [required_patches.md](required_patches.md) | 干净树上必改的 zblue `id.c`（双角色扫描） |
| [开发计划.md](开发计划.md) | 码表产品 BLE 应用场景 |
| [ble/README.md](ble/README.md) | BLE 方案设计（Companion、FS over BLE、传感器） |
