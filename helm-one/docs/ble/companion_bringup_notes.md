# BLE Companion Bring-up 手记（P0 固件侧）

**日期**：2026-08-14 · **配套**：[companion_impl_plan.md](companion_impl_plan.md) v0.7

> **历史手记（2026-08）**：§2.4 的“免配对”已被 2026-09-20 的配对准入 +
> `ENCRYPT_REQUIRED` 取代；当前以 [pairing_and_radio.md](pairing_and_radio.md) 为准。

方案文档讲「设计成什么样」，这份讲「为什么这么设计」和「跑起来踩了什么坑」。
协议细节一律以 `companion_impl_plan.md` + `companion_proto.h` 为准，这里不重复。

---

## 1. 这一轮改了什么

| 文件 | 改动 |
|------|------|
| `ble_companion/ble_companion.c` | 主循环移除 `statfs()`；名字前缀 / 广播间隔改走 Kconfig；notify 失败提升为常态错误日志（按错误码变化打印） |
| `bsp/sifli_ap.c` | 自启挂到 `lcd_bringup_main()` 末尾；`CONFIG_LCD` 关闭时才退回 `board_late_initialize()` |
| `chips/sf32lb52/Kconfig` | 新增 `_AUTOSTART` / `_NAME_PREFIX` / `_ADV_INTERVAL_MS`；删除已无用的 `_STORAGE_PATH` |
| `include/companion_proto.h` | 新增 `COMPANION_STORAGE_FREE_UNKNOWN` 约定 |
| `configs/nsh/defconfig` | 打开 `_AUTOSTART` |
| `docs/ble/companion_impl_plan.md` | 升 v0.4：验收结果 + §7.2 运行时问题 |

结果：开机 **12.6 秒**自动广播 `MyBike-3412`，手机端读写订阅全通，空闲 **0.0% CPU**。

---

## 2. 设计思路

### 2.1 一功能一特征，而不是一条通用管道

选它的代价是特征数量会随功能线性增长，好处是**每条特征的语义、方向、权限、频率都是自解释的**：手机端拿任意一个 BLE 调试助手就能逐条点，不需要先实现一层帧解析器才能看到东西。这一轮的验收完全是用通用调试助手点出来的，没写一行 App 代码，这个收益是实打实的。

代价在 `0xFF17` 通知上确实暴露了（单条特征装不下一条完整的 Android 通知），所以只在那一条上加了 8 字节分片头 + TLV，**没有**把整个协议改成通用帧。局部复杂换全局简单。

### 2.2 状态用「订阅即发 + 变化即发 + 5 秒心跳」三条触发

只靠心跳，App 打开界面要等最多 5 秒才有数据；只靠变化推送，App 无法区分「设备没变化」和「设备挂了」。三条一起才能让 App 用一个简单规则判活：**超过 2 个心跳没收到就认为链路异常**。

`last_ctrl_op` 回显字段是为了让控制命令**不需要单独的响应特征** —— App 写完 `0xFF13`，从下一帧状态里看到回显就知道设备收到了，省掉一条特征和一套请求/响应配对逻辑。

### 2.3 GATT 回调只搬数据，Notify 一律由主循环发

回调跑在蓝牙栈的线程上下文里。**在回调里做任何耗时的事，都是在阻塞协议栈本身**。所以回调只做「拷贝 + 置标志 + 立即返回」，真正的发送放到 500 ms 的主循环 tick。

这条纪律在这轮救了一次：`statfs()` 卡住时（见 §3.1）卡的是应用主循环，蓝牙栈还能正常回 `disconnected` 事件；如果当初把它写在读回调里，故障会直接变成协议栈死锁，排查难度完全不同。

### 2.4 免配对（不 bond、不加密）

Companion 的数据是状态、控制、定位，不涉及隐私密钥。一旦要求配对，手机会弹系统配对框，用户体验断裂，而且 CCCD 加了 `AUTHEN_REQUIRED` 后**光是订阅状态就会拉起配对**。所以 `bt_device_set_bondable_le(false)`，CCCD 权限只给 `READ | WRITE`。

敏感操作（固件升级、清数据）留到 P4 再单独要求配对，而不是现在给所有特征都上锁。

### 2.5 自启的时机是设计约束，不是实现细节

见 §3.1（2）。结论：**蓝牙适配器的开启必须排在 LCD 初始化之后**，而且要用「LCD 真的初始化完了」这个事件来排序，不能用延时猜。所以自启挂在 `lcd_bringup_main()` 末尾。

### 2.6 `storage_free_kb` 改报「未知」而不是硬算

见 §3.1（1）。一个只用于显示的字段，不值得付「遍历全盘元数据」的代价，更不值得为它把蓝牙栈饿死。协议里字段保留（线格式不变），值恒为 `COMPANION_STORAGE_FREE_UNKNOWN`（0），**App 必须把 0 当未知，不能当磁盘满**。

---

## 3. 调试过程的问题点

### 3.1 固件运行时（两个，详见 plan §7.2）

摘要，完整分析和数据在 `companion_impl_plan.md` §7.2：

1. **`statfs()` 把蓝牙栈饿死**。LittleFS 的 `statfs` 要遍历全盘元数据，这张卡 4 GB 分区装了 1.55 GB 数据、SD 只有 660 KB/s，单次远超 10 秒周期 → `ble_companion` 吃 99.3% CPU，优先级 10 的 `BT LW WQ` 被饿死。**`nsh> df -h` 在这块卡上会卡死 shell**，不是板子挂了。
2. **开机自启 hardfault**。挂在 `board_late_initialize()` 时每次开机必崩且带走 NSH，崩溃点精确落在 ST7789S 62.5 MHz SPI 初始化那一刻。已排除栈溢出（有 896 字节余量）。改挂 LCD 完成之后解决。

两者的共同底层隐患：**`CONFIG_BT_LONG_WQ_PRIO=10`**。Zephyr 里数值小＝优先级高，zblue 移植到 NuttX 后沿用数值，语义反了，导致蓝牙主机栈的优先级低于系统里几乎所有任务。P2 做吞吐优化时要正面处理。

### 3.2 构建 / 烧录工具链

**改了 `defconfig` 不会自动生效** —— 这是本轮最浪费时间的一个坑。`build_board.py build` 用的是 `cmake_out/<board>/.config`，改板级 `defconfig` 后它**不会**重新 configure，于是编译「成功」了、烧进去了，但改动根本没进镜像（我第一次加自启就是这样，`#ifdef` 直接把代码挡掉了，白跑一轮 build + flash）。

改完 `defconfig` 必须先重新应用：

```bash
cmake --build cmake_out/helmone_nsh --target resetconfig
grep -n MYVENDOR_BLE_COMPANION cmake_out/helmone_nsh/.config   # 一定要核对
```

`resetconfig` 会把板级 `defconfig` 拷进 `.config` 再跑 `olddefconfig`。**养成习惯：改完 defconfig 先 grep `.config` 确认符号真的在里面，再开始编译。**

**镜像体积变化后必须 `build-boot`**。`ftab.bin` 里记着镜像长度，体积变了不重新生成，ROM 会按旧长度加载。本轮镜像从 1.16 MB 涨到 1.62 MB，属于必须重跑 `build-boot` 的情况。

**`sftool` 的两个陷阱**：

- 非 TTY 环境下**完全不打印进度条**（indicatif 在管道里静默），看起来像什么都没干；
- **失败时仍然返回 exit code 0**。

所以脚本里不能靠返回值判断烧录成功，要抓 `Verify success!`；想看进度就套个伪终端：

```bash
script -qec "./vela_my_vendor_tools.py build flash -p /dev/ttyUSB0" /dev/null \
  | tr '\r' '\n' | grep -E 'Verify success|Downloaded successfully|Error'
```

另外 `sftool` 偶发 `Error: Failed to execute write_flash command`，直接重试就好（本轮遇到 2 次，重试均成功）。它对内容一致的区段会打印 `No need to re-download, skip!`，这反过来是个有用的校验手段：**重跑一次烧录，如果三段全 skip，说明 flash 里就是你刚构建的镜像。**

### 3.3 串口自动化

CH340 上 **DTR 接 BOOT 选择、RTS 接 RESET**。要正常启动，两根都必须处于释放态：

```python
ser.dtr = False; ser.rts = False          # 打开前就设好，避免开口瞬间复位
ser.open()
ser.dtr = False                            # BOOT = 运行态
ser.rts = True; time.sleep(0.1); ser.rts = False   # 复位脉冲
```

波特率 **1000000**（`MONITOR_BAUD`）。如果启动完全无输出、或只打印 `SFBL`，就是 DTR/RTS 拉高把芯片停在 ROM 下载模式了。

还有一点：**关闭串口有可能让板子复位**（驱动恢复 RTS 默认电平）。本轮一次主机端扫描失败就是这个原因 —— 脚本收尾关口时把板子复位了，`ble_companion` 随之消失。打开自启之后这个问题自然消失。

### 3.4 主机侧联调工具

| 工具 | 状态 | 说明 |
|------|------|------|
| `bluetoothctl --timeout N scan le` | 可用 | 验广播最省事，能看到名字和 RSSI |
| `bleak`（Python） | 需要 venv | Ubuntu 有 PEP 668 保护，`pip install` 直接装会被拒，建 venv 即可 |
| `btmon` | **需要 root** | 拿 HCI 层断连原因码用，无免密 sudo 就用不了 |
| 手机 BLE 调试助手 | **推荐** | GATT 联调的主力 |

**BlueZ 有个坑**：从 Linux 侧订阅 `0xFF12` 后**恰好 2 秒链路被断开**，且一条通知都收不到；同一份固件在手机上订阅正常、连接 6 分钟不断、固件侧全程无 `notify failed`。判定为主机侧行为，不影响 App。

**结论：扫描验广播用 `bluetoothctl`，GATT 联调用手机。** 别在 BlueZ 的行为上浪费时间去查固件。

### 3.5 一个自伤操作

`pkill -f mcu_log.py` 会把**自己所在的 shell 也匹配掉**（这条命令的命令行里就含这个字符串），命令瞬间自杀。杀自己启动的后台脚本要按 PID 精确杀，或者让匹配串不出现在命令行里。

---

## 4. 排查方法论（下次直接用）

**崩溃寄存器可能是断言现场，不是故障现场。** 本轮 `PC`/`LR` 都指向 `_assert`，`R1=R10=0xba=186` 正好是 `arm_hardfault.c:186` 的行号、`R2` 指向文件名字符串 —— 从这组寄存器反推故障点是徒劳的。先认出「这是断言自己的上下文」，能省很多力气。

**静态栈溢出可以用 `nm` 直接排除**，不用猜：

```bash
prebuilts/gcc/linux-x86_64/arm-none-eabi/bin/arm-none-eabi-nm \
  --print-size --radix=x cmake_out/helmone_nsh/nuttx | grep bt_lw_stack_area
# 20021a38 00000578 B bt_lw_stack_area  →  栈范围 0x20021a38..0x20021FB0
```

拿故障时的 `SP` 和这个范围比一下即可。本轮 `SP=0x20021db8`，只用了 504 字节、还剩 896 字节，一句话排除了溢出这条错误方向。

**把两端拉到同一条时间线上。** 单看主机侧只知道「断了」，单看固件侧只知道「收到 disconnected」。把串口日志和 GATT 客户端日志打到同一个时间轴后，`subscribe -> 1` 之后恰好 2.0 秒断连这个规律立刻浮出来，性质（主机侧 vs 固件侧）也随之清楚。

**先量化再动手改。** 怀疑 `statfs` 慢，就去给 `df` 计时（结果是 20 秒不返回）；怀疑 CPU 被占，就连采三次 `ps`（0.0% vs 99.3%）；怀疑竞态修好了，就连续复位 5 次（全部 12.6 秒起广播）。竞态问题**一次通过不算通过**。

**`ps` 的列要数清楚。** `STACK USED FILLED CPU` 四列紧挨着，`0008088 0001204 14.8% 99.3%` 里 14.8% 是栈占用率、99.3% 才是 CPU。看错一列会把结论带反。交叉验证的办法：看 IDLE 那行的 CPU%，正常空闲应该接近 100%。

---

## 5. 当前状态与未决项

**已通过**（固件侧，手机实测）：广播、`0xFF11` 读、`0xFF12` 订阅+通知、`0xFF13` 控制+回显、`0xFF14` GNSS 计数与过期清除、MTU 193、免配对、6 分钟零断连、开机自启、空闲 0% CPU。

**P1**：文件夹式文件管理走 GATT `0xFF15`/`0xFF16`。Classic 关闭（LCPU `controller_enable_bit=0x01`，Flags `0x06`）。`FS_STATFS` 直接拒绝。companion 任务优先级 **8**。

吞吐：**按实测 ~15 KB/s 出货，不再追空口加速。** 小米 Central 稳定 15 ms interval（`interval=12`），约 1 个 ATT PDU / 连接事件。HCI 走最稳路径：CH1 512 @ `0x2007FE00`，`LE_READ_BUFFER_SIZE` stub 251×6，LCPU NCP 原样转 zblue，TX 等环空再写。不要再改 mailbox / 假 NCP / 把 ACL 个数报成 16。大文件走 USB MTP。

**未决**：

1. `CONFIG_BT_LONG_WQ_PRIO=10` 的优先级语义问题（见 §3.1）。
2. 真实剩余空间不能走 `statfs`。`FS_STATFS` 现返回 `EOPNOTSUPP`。
3. 改了 `defconfig` 后必须 `resetconfig` 再编。

传感器 Central 的双角色坑（discover-all 拆链、Central 连接参数、FS MTU 被心率覆盖）见 [ble_sensor.md](ble_sensor.md)。
手机 RPA/身份地址导致 FS「0 帧超时」见 [pairing_and_radio.md](pairing_and_radio.md) §1.3。

---

## 5.1 FS「CCCD=01 却 0 帧超时」（2026-09-21）

**不要先怀疑 App 没订 CCC。** 串口若已有 `fs ccc DROPPED (not phone)` / `fs cmd DROPPED`，
说明设备把帧丢掉了。典型对拍：

| 侧 | 日志 |
|---|---|
| 连接 | `phone connected 52:05:…`（RPA） |
| CCC/写 | 回调地址变成 `78:D8:…`（身份地址）→ DROPPED |
| 修好后 | `phone peer refresh 52:05:… -> 78:D8:…` 或正常 `fs ccc … -> 1` |

处置在 `companion_is_phone()`（单手机槽刷新 peer + `companion_fs_on_connect`），
Doxygen 与权威说明见 `pairing_and_radio.md` §1.3。

### 5.2 App `cccd=01` 但 MCU `subscribe -> 0`（2026-09-21）

**不重启 MCU 也能判**：

| 侧 | 日志 |
|---|---|
| App | `0xFF16 已订阅 cccd=01 00` → `FS 发命令 … subscribed=true` → `0 帧` |
| MCU | `fs ccc … raw=0000 -> 0` → `fs cmd … ccc=0` → `aborted (no ccc …)` |

说明命令到了设备（不是 DROPPED / 不是没连上），但 **Notify CCC 在设备侧是关的**。
App 描述符读回常走 Android 本地缓存，会假阳性 `01`。另：空闲退订写的 CCC=0
可能迟到覆盖到下一次连接。

App 侧处置（`companion_client.dart`）：订阅时双写 CCC；0 帧时强制重写（不信读回）；
空闲退订拉长到 10 分钟；发现前清 GATT 缓存。重装/热重载 App 后只需 **BLE 断再连**
（不必复位 MCU）。

---

## 6. HCI RX 卡住（2026-09-05）

串口刷 `sf32lb52 bt rx callback: -12`、`ps` 里 `bicycle_ui` 还在跑、自恢复却不动：不是 LCPU 自己挂了，是 **zblue `h4_rx_thread` 在 `get_rx(K_FOREVER)` 上堵死**，`/dev/ttyHCI0` 不再读，`uart_bth4` 环形缓冲满后回 `-ENOMEM`。

supervisor 当时只看广播/SCAN/GATT，**不看这条 HCI 失败**；而且应用层若还以为手机连着，会整段跳过 adapter cycle。HCI 已死时 disconnect 到不了，于是卡在僵尸「已连接」。

处理与验收日志见 [companion_impl_plan.md](companion_impl_plan.md) **§7.4**；串口对照见 [bluetooth_guide.md](../bluetooth_guide.md) FAQ。不要把缓冲再加大当修复。companion 自己卡住时的后手见 [diag.md](../diag.md)。
