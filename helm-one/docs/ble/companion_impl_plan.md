# BLE Companion 实施方案（决策记录 + P0 规格）

本文记录 **BLE Companion**（码表 ↔ 安卓 App）的四项架构决定、锁定后的协议规格、以及 P0 的实现范围与验收标准。

- 设计取舍的理由、真机调试踩的坑与排查方法见 [companion_bringup_notes.md](companion_bringup_notes.md)
- 传感器 Central（双角色、发现顺序、与 FS/MTU 隔离）见 [ble_sensor.md](ble_sensor.md)
- 配对模型（连接准入 / 手机记录 / 配对窗口）、广播三档与间隔、射频节奏与空轮退避、跨线程纪律见 [pairing_and_radio.md](pairing_and_radio.md)
- 协议总体设计与背景见 [companion_design.md](companion_design.md)（v0.1，本文在其基础上收窄并修正）
- 板级蓝牙栈、HCI、`bttool` 联调与排障见 [bluetooth_guide.md](../bluetooth_guide.md)
- USB 侧文件传输见 [mtp_simple/README.md](../../boards/sf32lb52/helmone/services/mtp_simple/README.md)

**版本**：v0.10（协议版本仍为 `COMPANION_PROTO_VERSION = 0x0005`）
**更新**：2026-10-18
**状态**：全程 BLE/GATT。Classic 关闭。文件夹文件管理走 `0xFF15`/`0xFF16`。HCI RX `-ENOMEM` 走 supervisor **两级恢复**（adapter cycle → LCPU 复位 + task 重建，5 min 限频），见 §7.4。窗口外已绑定手机优先定向回连，见 [directed_reconnect.md](directed_reconnect.md)。

---

## 1. 需求

| 编号 | 需求 | 承载 |
|------|------|------|
| 1 | 与 App 文件传输（复用现有 MTP 逻辑） | 文件夹与小文件：GATT `0xFF15`/`0xFF16`；大地图仍走 USB MTP |
| 2 | 与 App 各类通信（如收取手机通知） | `0xFF17`，分片 + TLV |
| 3 | 数据收发（如接收手机控制数据） | `0xFF13` 控制、`0xFF12` 状态、`0xFF14` GNSS |

---

## 2. 四项架构决定

### 决定 1：协议形态 — 一功能一特征（方案 A）

每个功能占用独立 GATT 特征，不做多路复用信道。

| | 内容 |
|---|---|
| **优点** | 简单直观；各特征互不干扰，GNSS 高频写不会被文件传输阻塞 |
| **优点** | 固件 attr table 与 App 特征发现一一对应，调试时 nRF Connect 可直接逐个验证 |
| **缺点** | 新增功能需新增特征，固件 attr table、App 发现逻辑、协议版本需同步变更 |
| **权衡** | 放弃了「加一个 svc 号即扩展」的能力，换取实现与调试的直观性 |

**必要修正（不改变本决定）**：`0xFF17` 通知特征原设计的载荷为 `[type:1][len:1][payload]`，长度字段仅 1 字节（最大 255）且无分片机制，**无法承载一条真实的安卓通知**（app 名 + 标题 + 正文常超 255 字节）。因此保留「一功能一特征」的拓扑，仅为该特征的**载荷**增加 8 字节分片头 + TLV 编码（见 §3.6）。其余特征格式不变。

### 决定 2：文件传输复用方式 — 路径寻址 + 薄 transfer_backend（方案 B）

App 以路径（如 `/tracks/2026-08-13.gpx`）寻址，不引入 PTP object handle；不改动 `mtp_simple.c`。

| | 内容 |
|---|---|
| **优点** | 无状态。BLE 断线频繁（手机进兜、走远），路径重连即用，handle 表会失效 |
| **优点** | 不触碰正在工作的 USB MTP 路径。`mtp_simple.c` 为 5677 行单文件，catalog 含 30+ 个 static 函数（handle 保号快照、子目录驱逐等），且 MTP + UI 稳定性是踩完 EPIC GPU 死锁才换来的（见 `todo.md`） |
| **优点** | 仍复用真正有价值的部分：沙箱校验、条目过滤规则、PSRAM 分块缓冲、USB/BLE 互斥 |
| **缺点** | BLE 与 USB 各有一套列目录代码；将来若需统一文件视图需再合并 |

**结论依据**：object handle 是 PTP 协议的要求，不是文件传输的要求。

### 决定 3：落地范围 — 固件 P0 + Flutter 骨架（方案 B）

两侧同时做到一个可回归的端到端闭环，再往上堆功能。

| | 内容 |
|---|---|
| **优点** | 端到端可测。仅做固件的话只能用 nRF Connect 验证 GATT 表与读写，验证不了 App 侧的编解码、订阅、重连 |
| **优点** | 传输层验证过之后，FS / 通知 / 控制每一项都只是在既有骨架上加协议 |
| **缺点** | 一轮要写两个仓库，工作量大于只做固件 |

**注**：需求编号中文件传输为 1，但它是协议层最重的消费者，因此**构建顺序**上排在 P1，先让链路与状态回路跑通。

### 决定 4：Flutter 原生代码 — 前台服务先加，通知监听下一轮

| | 内容 |
|---|---|
| **前台服务：必须，且优先** | 不加则 App 进后台后 BLE 连接被系统回收，**需求 1 与 3 同样失效**（骑行时手机在兜里、屏幕黑着正是主场景），因此它是全部功能的前提 |
| **NotificationListenerService：P1** | 安卓读系统通知的唯一途径，无替代方案 |
| **缺点/成本** | 通知使用权拿不到运行时弹窗，`permission_handler` 不适用，须跳 `ACTION_NOTIFICATION_LISTENER_SETTINGS` 由用户手动授予，需单独设计引导 UI |

---

## 3. 协议规格 v0.3

权威定义见 `boards/sf32lb52/helmone/include/companion_proto.h`，本节为其说明。

### 3.1 广播

| 项目 | 值 |
|------|-----|
| 设备名 | `CONFIG_MYVENDOR_PRODUCT_NAME` + `-XXXX`（LE 地址后两字节大写十六进制），现为 `Helm One-XXXX` |
| ADV 数据 | `02 01 06` Flags（LE General + BR/EDR Not Supported）+ `03 03 10 FF` Complete 16-bit Service UUID `0xFF10` |
| Scan Response | Complete Local Name |
| 类型 | Legacy `ADV_IND`，interval 320（200 ms） |
| 配对 | 配对窗口内 `bondable=true`，窗口外 `false`；Companion 特征/CCCD 全部 `ENCRYPT_REQUIRED`。定向回连见 [directed_reconnect.md](directed_reconnect.md)。不启用 Classic。 |

App 侧按 **Service UUID `0xFF10`** 过滤扫描结果，而非按名字前缀 —— 名字可被用户改、UUID 不会。

### 3.2 服务与特征

**Primary Service `0xFF10`**

| 特征 | UUID | 属性 | 方向 | 阶段 |
|------|------|------|------|------|
| Protocol Version | `0xFF11` | Read | 设备→App | **P0** |
| Device Status | `0xFF12` | Read, Notify | 设备→App | **P0** |
| Control | `0xFF13` | Write | App→设备 | **P0** |
| GNSS Fix | `0xFF14` | Write, Write NR | App→设备 | **P0** |
| FS Command | `0xFF15` | Write | App→设备 | P1 |
| FS Data | `0xFF16` | Write, Notify | 双向 | P1 |
| Notification | `0xFF17` | Write | App→设备 | P1 |
| Track Point | `0xFF18` | Notify | 设备→App | P2 |

所有多字节字段一律 **小端**。

### 3.3 Protocol Version（`0xFF11`，Read）

`uint16` = `0x0005`。App 连接后首先读取，版本不匹配时提示升级而非盲目通信。

### 3.4 Device Status（`0xFF12`，Read + Notify）

16 字节状态字段 + 6 字节 BD_ADDR 拖车（线格式 22 字节，协议 0x0005 未升版本）。Classic 关闭后拖车仍填适配器地址，App 可忽略。

```c
struct companion_status
{
  uint16_t flags;            /* COMPANION_STATUS_* 位域 */
  uint8_t  battery_pct;      /* 0..100，0xFF = 未知 */
  uint8_t  last_ctrl_op;     /* 回显最近处理的 Control opcode */
  uint32_t storage_free_kb;  /* /mnt/lfs 剩余空间 KiB */
  uint32_t uptime_sec;       /* 设备运行秒数 */
  uint32_t gnss_rx_count;    /* 累计收到的 GNSS Fix 写入次数 */
};
```

状态位：

```c
#define COMPANION_STATUS_RECORDING     (1u << 0)  /* 正在录制 */
#define COMPANION_STATUS_GPS_PHONE     (1u << 1)  /* 正在使用手机 GNSS */
#define COMPANION_STATUS_GPS_INTERNAL  (1u << 2)  /* 正在使用内置 GNSS */
#define COMPANION_STATUS_STORAGE_FULL  (1u << 3)
#define COMPANION_STATUS_FS_BUSY       (1u << 4)  /* 文件传输进行中 */
#define COMPANION_STATUS_USB_MTP_BUSY  (1u << 5)  /* USB MTP 占用，BLE FS 将返回 BUSY */
#define COMPANION_STATUS_SENSOR_HR     (1u << 8)  /* 心率已连接 */
```

**发送时机**：订阅后立即发一次；此后状态变化时发，且至少每 5 秒一次心跳。

### 3.5 Control（`0xFF13`，Write）

`[opcode:1][param…]`

| opcode | 名称 | 说明 | 阶段 |
|--------|------|------|------|
| `0x00` | `NOP` | 保留 | P0 |
| `0x01` | `START_RECORD` | 开始录制 | P0（仅置位 + 回显） |
| `0x02` | `STOP_RECORD` | 停止录制 | P0（仅置位 + 回显） |
| `0x03` | `PING` | 立即回一次 Device Status Notify | **P0** |
| `0x04` | `TIME_SYNC` | `[unix_sec:u32le][tz_min:i16le]`，写系统钟/RTC，并更新本地时区 | P0 |
| `0x10` | `FS_ABORT` | 中止当前 FS 传输 | P1 |
| `0x11` | `SYNC_PREP` | 进入同步态，降低其他 Notify 频率 | P1 |

任何 opcode 处理后都写入 `last_ctrl_op` 并触发一次 Status Notify。App 在 GATT 就绪后立即写一次 `TIME_SYNC`，之后每小时一次（前台恢复时若已超时也会补发）。

### 3.6 GNSS Fix（`0xFF14`，Write / Write Without Response）

24 字节，结构见 `struct companion_gnss_fix`。App 骑行中以 **1–5 Hz** 用 Write Without Response 推送。

设备侧收到后：`gnss_rx_count++`、置 `COMPANION_STATUS_GPS_PHONE`；超过 5 秒未收到则清除该位。

### 3.7 Notification（`0xFF17`，Write）— P1

**8 字节分片头**（这是对 v0.1 `[type:1][len:1]` 的修正）：

```text
 offset size field
 0      1    hdr_ver   = 0x01
 1      1    type      0=通用 1=来电 2=短信 3=应用通知 4=日程
 2      1    flags     bit0=FIRST bit1=LAST
 3      1    msg_id    消息 id，同一消息的各分片相同
 4      2    total     整条消息 payload 总字节数
 6      2    off       本分片在 payload 中的字节偏移
 8      …    payload   TLV 序列片段
```

重组后的 payload 为 TLV 序列：

```text
[tag:1][len:2][value…]

tag 0x01  app_name   UTF-8
tag 0x02  title      UTF-8
tag 0x03  body       UTF-8
tag 0x04  timestamp  uint32 Unix 秒
tag 0x05  package    UTF-8，安卓包名
tag 0x06  notif_key  UTF-8，用于后续「已读/清除」联动
```

设备侧上限：整条消息 `total ≤ 1024`，超出丢弃；单条 TLV value 截断到显示所需长度。

### 3.8 FS Command / FS Data（`0xFF15` / `0xFF16`）— P1

**路径寻址**：文件管理器根是 `/mnt/lfs`。**App 只发送相对路径**（根目录发空；`Track/a.gpx` 而不是 `/mnt/lfs/Track/a.gpx`），由 `transfer_backend` 拼接并做沙箱校验。仍接受绝对 `/mnt/lfs/…`。`/` 与 `.` 也视为 LFS 根。

**OTA 专属通道**：相对路径 `fw` / `fw/…`（以及 `/mnt/kv/fw/…`、旧绝对路径 `/mnt/lfs/fw/…`）落到 `/mnt/kv/fw`，与 LFS 沙箱分开。列 LFS 根看不到固件槽。2SFBL 从 KV `/fw/*.bin` 加载。

FS Command（`0xFF15`，Write）：`[cmd:1][tid:1][payload…]`
FS Data（`0xFF16`，Write + Notify），8 字节帧头：

```text
 offset size field
 0      1    cmd     回显对应的 cmd
 1      1    tid     事务 id，App 分配，设备回显
 2      1    flags   bit0=LAST bit1=ERR bit2=ACK_REQ
 3      1    status  0=OK，非 0 为 errno
 4      2    seq     事务内分片序号
 6      2    len     本片 payload 长度
 8      …    payload
```

| cmd | 名称 | Command payload |
|-----|------|-----------------|
| `0x01` | `FS_STATFS` | 无 |
| `0x02` | `FS_LIST` | `[cursor:2][path…]` 分页续传 |
| `0x03` | `FS_STAT` | `[path…]` |
| `0x04` | `FS_READ` | `[offset:4][len:4][path…]` 下载；`offset` 即续传点 |
| `0x07` | `FS_WRITE_OPEN` | `[size:4][flags:1][path…]` `flags` bit0=续传 |
| `0x08` | `FS_WRITE_DATA` | 数据走 FS Data 写入，`seq` 从 0 连续 |
| `0x09` | `FS_WRITE_CLOSE` | `[crc32:4]` 整文件 CRC |
| `0x0A` | `FS_DELETE` | `[path…]` 同时删除残留 `*.part` |
| `0x0B` | `FS_MKDIR` | `[path…]` |
| `0x0C` | `FS_RENAME` | `[old_len:1][old][new]` 移动；`*.part` 一并改名 |

`FS_LIST` 条目编码：`[type:1][size:4][mtime:4][name_len:1][name…]`，`type` 0=文件 1=目录。

**下行流控**：设备侧的 FS 读取用 `gatts_callbacks_t::on_notify_complete` 作为发送信用驱动下一分片，不使用 sleep，也不要求 App 逐片 ACK。`ACK_REQ` 仅用于超长传输的保活探测。

**完整性**：`FS_WRITE_CLOSE` 携带整文件 CRC32（NuttX `crc32part`，初值 0，多项式 `0xedb88320`），设备校验后才提交（先写 `*.part` 再 rename）。

**续传**：

- **下载**：`FS_READ` 的 `offset` 就是已收到的字节数。分片 `seq` 必须从 0 连续；缺号或等不到 `LAST` 时，用已收到长度再发一次 READ。
- **上传**：`WRITE_OPEN` flags bit0=1 时不截断已有 `*.part`，回复 `[offset:4][crc32:4]` 为已写入前缀。App 从该 offset 继续 `WRITE_DATA`，CLOSE 仍用**整文件** CRC。断线或 CLOSE 校验失败会**保留** `*.part`；只有 `CTRL_FS_ABORT`（0x10）才删除它。
- `WRITE_OPEN` 成功载荷固定 8 字节（协议 0x0004）。

**`FS_STATFS` 拒绝服务**：返回 `ERR` + `EOPNOTSUPP`（95）。本机 LittleFS 的 `statfs()` 会遍历全部元数据，在装满地图的 SD 上可达数分钟，会饿死蓝牙栈（见 bring-up 手记）。剩余空间仍通过 Status 的 `storage_free_kb = 0`（未知）表达。

**`FS_STAT` 回复载荷**：`[type:1][size:4][mtime:4]`。

### 3.9 文件管理器（协议 0x0005，GATT）

文件夹式 list / upload / download / delete / rename / mkdir **全部走 GATT** `0xFF15`/`0xFF16`。Classic / RFCOMM / SPP 关闭：LCPU `controller_enable_bit=0x01`，广播 Flags `0x06`，`# CONFIG_BT_CLASSIC`。

吞吐：连接后仍请求 7.5–15 ms、2M PHY、DLE 251；实测小米端约 **15 KB/s**（1 PDU / 15 ms），按此出货。地图 ~1.5 GB 仍走 USB MTP。

Device Status `0xFF12` 仍为 **22 字节**（协议不升版本）：16 字节状态 + BD_ADDR 6 字节。App 不再用该地址开 RFCOMM。

---

## 4. transfer_backend 设计（P1）

新增 `boards/sf32lb52/helmone/transfer_backend/`，为 BLE FS 提供，并可在将来被 `mtp_simple` 复用：

| 职责 | 说明 |
|------|------|
| 沙箱校验 | 强制 `/mnt/lfs` 前缀（文件管理器）；`fw/` 单独落到 `/mnt/kv/fw`。拒绝 `..` 与符号链接逃逸 |
| 条目过滤 | 复用 `mtp_simple.c` 中 `catalog_entry_skipped` 的隐藏/系统文件规则 |
| 分块缓冲 | 复用 `mtp_psram.c` 的 PSRAM 缓冲池，chunk 4 KiB |
| **占用互斥** | `transfer_owner_acquire(USB \| BLE)`。USB MTP 会话活跃时 BLE FS 返回 BUSY 并置 `COMPANION_STATUS_USB_MTP_BUSY`；反之亦然 |
| 原语 | `statfs` / `list` / `stat` / `read` / `write` / `delete` / `mkdir` / `rename` |

**互斥是本模块最要紧的职责**：USB MTP 与 BLE FS 会写同一个 LittleFS，而地图 tile 读取已经在用 `myvendor_mtp_lfs_quiesce()` 让路，三方并发是真实隐患。

---

## 5. P0 实现范围

### 5.1 固件

| 项 | 内容 |
|----|------|
| 新增 | `boards/sf32lb52/helmone/services/ble_companion/`，NSH 命令 `ble_companion` |
| 保留 | `ble_test_app` 继续作为底层 BT 冒烟测试；新增 `nsh-btadv` 构建 + `ble_test advtest` 定向矩阵（见 [directed_reconnect.md](directed_reconnect.md)） |
| 头文件 | `include/companion_proto.h` 升级到 v0.3 |
| Kconfig | `MYVENDOR_BLE_COMPANION` 及其子项（`chips/sf32lb52/Kconfig`），见下表 |
| 构建 | `ble_companion/CMakeLists.txt` + 板级 `CMakeLists.txt` 加 `add_subdirectory` |
| defconfig | `configs/nsh/defconfig` 补齐蓝牙配置（从 `nsh-driver` 移植） |

功能：广播、`0xFF11` 读、`0xFF12` 状态 Notify（22 字节含 BD_ADDR 拖车）、`0xFF13` 控制、`0xFF14` GNSS、GATT `0xFF15`/`0xFF16` 文件管理器。Classic 关闭。

GATT 回调保持薄：仅拷贝数据 + 置标志；FS 的目录/读写在 companion 线程泵出，该线程优先级 8，低于 `BT LW WQ`。

配置项（`menuconfig` 里在 `MYVENDOR_BLE_COMPANION` 下）：

| 配置 | 默认 | 说明 |
|------|------|------|
| `_AUTOSTART` | `n`（`configs/nsh` 里置 `y`） | 开机自启，无需串口敲命令。启动点的约束见 §7.2 |
| `MYVENDOR_PRODUCT_NAME` | `"Helm One"` | 广播名 = 产品名 + `-` + LE 地址末两字节 |
| `_ADV_INTERVAL_MS` | `200` | 广播间隔，按 0.625 ms 单位向下取整 |
| `_PRIORITY` / `_STACKSIZE` | `8` / `12288` | 优先级必须 **低于** `BT_LONG_WQ_PRIO=10`，否则 SD 读写会饿死主机栈 |
| `_STORAGE_PATH` | `"/mnt/lfs"` | BLE 文件管理器沙箱；OTA `fw/` 在 `/mnt/kv/fw` |
| `_DEBUG` | `n` | 打印每条 Control 与每帧 GNSS。**量产必须关**：App 以 1-5 Hz 推 GNSS，开着会刷爆串口 |

### 5.2 Flutter

| 文件 | 内容 |
|------|------|
| `lib/ble/companion_proto.dart` | UUID、状态位、opcode、`CompanionStatus` 解码 |
| `lib/ble/gnss_codec.dart` | `companion_gnss_fix` 编码（24 字节小端） |
| `lib/ble/companion_client.dart` | 连接、发现 `0xFF10`、MTU 协商 247、高连接优先级、订阅 `0xFF12`、写控制/GNSS、GATT FS、断线重连 |
| `lib/ui/ble/companion_debug_page.dart` | 调试页，驱动端到端验证 |
| `lib/ui/ble/companion_fs_browser.dart` | 文件夹浏览器，走 GATT FS |
| `android/.../CompanionForegroundService.kt` | 前台服务保活 |
| `AndroidManifest.xml` | `FOREGROUND_SERVICE`、`FOREGROUND_SERVICE_CONNECTED_DEVICE`、`POST_NOTIFICATIONS` |

扫描过滤改为 **Service UUID `0xFF10`**，不要按广播名关键字过滤。

### 5.3 本轮落地的文件

固件（`vendor/HelmOne/`）：

| 文件 | 状态 |
|------|------|
| `docs/ble/companion_impl_plan.md` | 新增（本文） |
| `boards/sf32lb52/helmone/include/companion_proto.h` | 升级到 v0.3 |
| `boards/sf32lb52/helmone/services/ble_companion/ble_companion.c` | 新增 |
| `boards/sf32lb52/helmone/include/transfer_backend.h` | 新增：路径沙箱 + 文件原语 |
| `boards/sf32lb52/helmone/transfer_backend/transfer_backend.c` | 新增 |
| `boards/sf32lb52/helmone/services/ble_companion/companion_fs.c` | GATT `0xFF15`/`0xFF16` 文件管理器 |
| `boards/sf32lb52/helmone/CMakeLists.txt` | 加 `add_subdirectory(ble_companion)` |
| `chips/sf32lb52/Kconfig` | 加 `MYVENDOR_BLE_COMPANION` 及其子项 |
| `boards/sf32lb52/helmone/configs/nsh/defconfig` | 开启蓝牙 + §7.1 的三项修正 |

App（Flutter 仓库）：

| 文件 | 状态 |
|------|------|
| `lib/ble/companion_proto.dart` | 新增，`companion_proto.h` 的镜像 |
| `lib/ble/gnss_codec.dart` | 新增 |
| `lib/ble/companion_client.dart` | 新增 |
| `lib/ble/companion_foreground_service.dart` | 新增 |
| `lib/ui/ble/companion_debug_page.dart` | 新增，驱动 §5.4 验收 |
| `lib/app/app_routes.dart` / `app_pages.dart` | 注册 `/ble/companion` |
| `android/.../CompanionForegroundService.kt` | 新增 |
| `android/.../MainActivity.kt` | 加 MethodChannel 桥 |
| `android/app/src/main/AndroidManifest.xml` | 前台服务 + 权限声明 |

### 5.4 P0 验收结果（2026-08-14 实测通过）

固件侧用手机蓝牙调试助手对连验证，**1-7 全部通过**；8 待 Flutter App 联调。

| # | 项 | 结果 |
|---|----|------|
| 1 | 启动并打印 LE 地址与广播名 | 通过：`advertising as "MyBike-3412"`，开机 12.6 s 起广播，连续 5 次复位无失败 |
| 2 | 扫描到 `MyBike-XXXX` 并连接 | 通过：手机与 Linux `bluetoothctl` 均按 `0xFF10` 扫到并连上 |
| 3 | 读 `0xFF11` = `0x0003` | 通过 |
| 4 | 订阅 `0xFF12` 收到 16 字节状态 | 通过：`01 00 FF 01 00000000 B3040000 03000000` |
| 5 | 写 `0x03`（PING）回显 | 通过：`last_ctrl_op` 回显；写 `0x01` 后 `flags` 置 `RECORDING` |
| 6 | 写 `0xFF14` 计数与 `GPS_PHONE` | 通过：3 帧 → `gnss_rx_count=3`，经纬度解析正确；停推 5 s 后 `GPS_PHONE` 自动清除 |
| 7 | MTU 协商 | 通过：`MTU -> 20` 后升到 **193**（payload 190）。注：193 由手机侧决定，Android 上限 247 |
| 8 | App 切后台 5 分钟不断连 | **待验证**（需 Flutter 前台服务真机跑） |

链路稳定性：单条连接维持 **6 分钟以上零断连**，全程无 `status notify failed`。空闲时 `ble_companion` 占 **0.0% CPU**、IDLE 100%。

App 侧（`flutter analyze` 通过、`flutter build apk --debug` 通过）尚未与固件真机对连，`/ble/companion` 联调页仍待走一遍。

---

## 6. 后续路线

| 阶段 | 内容 |
|------|------|
| **P0** | 链路 + 状态 + 控制 + GNSS（本轮） |
| **P1 固件** | GATT FS `0xFF15/16` 文件管理器；吞吐（2M/DLE/短 interval）；`0xFF17` 通知仍待做 |
| **P1 App** | 联调页文件浏览器走 GATT；`NotificationListenerService` |
| **P2** | `0xFF18` Track Point |
| **P3** | `ble_sensor` Central 心率 / CSC / CPS。NSH `test sensor` 已可连 HR+电量；约束见 [ble_sensor.md](ble_sensor.md) |
| **P4** | 量产安全策略；LE 地址改从 OTP 读取 |

---

## 7. 已知约束与坑

### 7.1 开启蓝牙时踩到的三个编译/内存问题

在 `configs/nsh` 上开启蓝牙时依次遇到，均已在 defconfig 中修掉并注明原因：

| 现象 | 原因 | 处理 |
|------|------|------|
| `bignum_core.c: error: r7 cannot be used in 'asm' here` | zblue 的 `BT_ECC` 会 `select CRYPTO_MBEDTLS`；mbedtls 的 bignum 内联汇编要占 `r7`，而 `CONFIG_FRAME_POINTER=y` 时 `r7` 是帧指针 | `# CONFIG_MBEDTLS_HAVE_ASM is not set`。舍弃汇编而非帧指针：纯 C bignum 只影响配对时的 ECC 速度，帧指针要留给 `build_board.py` 的崩溃地址解析 |
| `#error "MBEDTLS_PADLOCK_C defined, but not all prerequisites"` | `MBEDTLS_PADLOCK_C` 默认 `y` 且依赖 `HAVE_ASM`，但它是 VIA x86 加速器 | `# CONFIG_MBEDTLS_PADLOCK_C is not set` |
| SRAM 从 91 KB 暴涨到 **317 KB**（共 512 KB） | 框架静态分配 `g_gatt_client[BLUETOOTH_GATTC_MAX_CONNECTIONS]`，默认 8 × `GATT_CLIENT_ELEMENT_MAX=200` = **147 KB `.bss`**，而 P0 只做 GATT **Server** | 按两个传感器而非八个满配对端裁剪：`GATTC_MAX_CONNECTIONS=2`、`ELEMENT_MAX=40`、`SERVICE_MAX=8`、`CHAR_PER_SERVICE_MAX=16` |

裁剪后的实测占用（`configs/nsh`，含 bicycle UI 与 LVGL）：

| | 开蓝牙前 | 开蓝牙后 | 说明 |
|---|---|---|---|
| Flash | 1.16 MB | **1.62 MB** | `main` 分区 4 MB，余量充足 |
| SRAM | 90.9 KB | **176.3 KB** | 共 512 KB；蓝牙净增约 85 KB（zblue 50 KB + 框架 20 KB + mbedtls 11 KB） |
| `ble_companion` 自身 | — | 3.7 KB / 522 B | |

**P3 传感器**：不要把 `GATT_CLIENT_ELEMENT_MAX` 拉回 200 来做 discover-all。按 UUID 只发现 HR/CSC/CPS + BAS；溢出时 SAL 必须保持连接。详见 [ble_sensor.md](ble_sensor.md)。

### 7.2 真机 bring-up 踩到的两个运行时问题

两个都不是编译期能发现的，都是烧进板子跑起来才暴露：

**（1）`statfs()` 在这张卡上会把整个蓝牙栈饿死**

`companion_tick()` 原本每 10 秒调一次 `statfs()` 填 `storage_free_kb`。实测该调用**根本回不来** —— `nsh> df -h` 发出后 20 秒无任何输出且提示符不再出现，NSH 被整个卡住。原因是 LittleFS 的 `statfs` 要遍历全盘元数据统计已用块，而这张卡分区 `0xffa00000`≈4 GB、装了 1.55 GB 数据，SD 实测只有 **660 KB/s**（启动日志 `SDPROF`，`lfs_mount` 单独就花 690 ms），单次遍历远超 10 秒周期，于是每个 tick 都在重新遍历。

后果：`ble_companion`（优先级 100）吃掉 **99.3% CPU**、状态恒为 `Ready`，IDLE 掉到 0.3%，而 zblue 主机栈工作队列 `BT LW WQ` 只有优先级 **10**，被彻底饿死。

处理：**从主循环彻底移除 `statfs()`**，`storage_free_kb` 恒报 `COMPANION_STORAGE_FREE_UNKNOWN`（0）。App 必须把 0 当"未知"而不是"磁盘满"。P1 做文件传输时若需要真实剩余空间，**不要**走 `statfs`，改为自己维护计数或只在明确的用户操作里算一次并缓存。

**（2）开机自启的时机会 hardfault**

把自启挂在 `board_late_initialize()` 里（与 `test_app` 同位置）后，**每次开机必崩**，且把 NSH 一起带走：

```
Assertion failed panic: at arm_hardfault.c:186  task: BT LW WQ  process: ble_companion
PC: 100051e9  LR: 100051e9  SP: 20021db8  xPSR: 00000003
```

排查要点，避免下次走弯路：

- `PC`/`LR` 都指向 `_assert` 本身，`R1=R10=0xba=186` 正是 `arm_hardfault.c:186` 的行号，`R2` 指向文件名字符串 —— 这组寄存器是**断言现场**，不是出错现场，从它反推不出故障点。
- 一度怀疑 `BT LW WQ` 栈溢出（`CONFIG_BT_LONG_WQ_STACK_SIZE=1400`，空闲就用了 54.5%），但 `nm` 查到 `bt_lw_stack_area` 在 `0x20021a38 + 0x578`，栈顶 `0x20021FB0`，故障时 `SP=0x20021db8` 只用了 504 字节、**还剩 896 字节**——**不是栈溢出**。
- 真正的相关性来自时间线：崩溃**每次都精确落在 `ST7789S hw-SPI init (62.5 MHz)` 那一刻**，即适配器开启（`adapter state -> 1`）与 LCD 初始化并发的瞬间。手动 `nsh> ble_companion &` 从不复现，因为那时 LCD 早已初始化完毕。

处理：自启改为**挂在 `lcd_bringup_main()` 末尾**（`board_lcd_initialize()` 返回之后）而不是 `board_late_initialize()` 里，用真实的完成事件排序，而不是猜一个延时。`CONFIG_LCD` 关闭时才退回 `board_late_initialize()` 启动。改后连续 5 次复位均在 12.6 s 起广播、零崩溃。

**底层隐患（尚未处理）**：`CONFIG_BT_LONG_WQ_PRIO=10` 意味着蓝牙主机栈工作队列的优先级低于系统里几乎所有任务（NuttX 默认任务是 100，LCD 线程是 110）。Zephyr 里数值小＝优先级高，zblue 移植到 NuttX 后直接沿用了数值，语义正好反过来。上面两个问题都是它的表现。后续只要有高优先级任务长时间占 CPU（LVGL 重绘、MTP 传输），BLE 就可能再次异常。P2 做吞吐优化时应正面评估调高该优先级。

### 7.3 其他

来自 [bluetooth_guide.md](../bluetooth_guide.md) 与实测：

- **不要让 App 写 GAP Device Name（`0x2A00`）**，会触发 SMP 配对并可能导致异常断开。
- Companion 的 CCCD/特征权限是 `READ | WRITE + ENCRYPT_REQUIRED`（当前实现；配对窗口内完成 L2）。**不要**再加 `AUTHEN_REQUIRED`，否则订阅状态会拉起 MITM 配对。
- Rev B 芯片（SF32LB52-N16，`rev_id ≥ A4`）须使用 Rev B 邮箱地址，`sf32lb52_bt_adapter.c` 已按 revision 选择 RX ring。
- 蓝牙异常后清理：`rm -rf /data/misc/bt` + `reboot`。
- `CONFIG_BT_PRIVACY` 与 `CONFIG_BT_SMP` 有编译期依赖，关闭 SMP 前先确认。
- Android 每条连接只允许**一次** MTU 交换。App 侧因此给 `device.connect()` 传 `mtu: null` 关掉 flutter_blue_plus 自带的请求，再显式 `requestMtu(247)`。
- 以 `FOREGROUND_SERVICE_TYPE_LOCATION` 启动前台服务时，若定位权限未授予，Android 会抛 `SecurityException`。`CompanionForegroundService` 因此按**实际已授予的权限**决定类型集合，而不是照搬 manifest 的声明。
- LE/Classic 公共地址从 eFuse UID 生成（本板无 NOR 厂测 OTP MAC）。广播名为 `CONFIG_MYVENDOR_PRODUCT_NAME` + `-` + MAC 后两字节（现为 `Helm One-XXXX`）。换板或换 MAC 后手机需忘掉旧设备。
- 从 Linux 侧用 BlueZ 做 GATT 客户端时，订阅 `0xFF12` 后**恰好 2 秒链路被断开**且收不到通知；同一固件在手机上订阅正常、连接 6 分钟不断。判定为主机侧行为，不影响 App，暂不深究。用 `bluetoothctl` 扫描验广播是可靠的，做 GATT 联调请用手机。
- `sftool` 偶发 `Error: Failed to execute write_flash command`，直接重试即可成功；它在非 TTY 下不打印进度条，且**失败时仍返回 exit code 0**，脚本里不能只看返回值判断烧录成功，要抓 `Verify success!`。

### 7.4 HCI RX `-ENOMEM`（`bt rx callback: -12`）自恢复原先处理不了

2026-09-05 真机：KEY1 翻页后 `ps` 仍正常（`bicycle_ui` ~20% CPU，BLE 线程多在等信号量），随后串口被刷满：

```text
sf32lb52 bt rx callback: -12
```

`-12` 是 `-ENOMEM`，不是控制器自己报错。路径：

```text
LCPU HCI → sf32lb52_bt_adapter RX worker
        → sf32lb52_bth4 notify_host
        → uart_bth4_receive（往 /dev/ttyHCI0 环形缓冲写）
        → zblue h4_rx_thread read()
```

`uart_bth4` 在 `circbuf_space` 不够装下一包 + H4 头时返回 `-ENOMEM`。`CONFIG_UART_BTH4_RXBUFSIZE` 已经是 8096，再加大缓冲解决不了：**根本没人在读 tty**。

**根因（H4）**：`apps/external/zblue/zblue/port/drivers/bluetooth/hci/h4.c` 的 `get_rx()` 对 ACL IN（以及非可丢弃事件）用 `K_FOREVER` 拿 `net_buf`。ACL 池空时 `h4_rx_thread` 永久堵住，停止 `read()`，环形缓冲被 LCPU 灌满，`-12` 就一直打。上游只把 **legacy** `LE_ADVERTISING_REPORT` 标成可丢弃；扩展/周期广播报告仍会占池。

**为什么 `ble_supervisor` 当时没动**：

1. 只盯广播停滞（12 s）和 ADV/SCAN/GATT 窗口失败（60 s 内 3 次），**不看 HCI RX 失败**。
2. `g_adv_active` 在 HCI 已死后仍可为 true（停广播回调到不了）。
3. `companion_supervisor_pump()` 在 `companion_phone_connected()` 为真时**整段跳过** adapter cycle。HCI 已死则 disconnect 回调也不会来，应用层卡在「已连接」僵尸态，恢复永远不会跑。

`bt_adapter_disable`/`enable` 本身不关 tty、也不 `circbuf_reset`；真正有用的是 `h4_close` 里 `k_thread_abort` + `h4_open` 再拉一条 RX 线程。所以 cycle 仍然是正确的升级手段，前提是**请求得出来**。

**处理**（均在 vendor，不改 zblue git）：

| 层 | 文件 | 做什么 |
|----|------|--------|
| 抽干 tty | `vela_override/zblue/patch_net_buf_timeout.py`（构建期补丁，改 zblue `host/buf.c`） | 扩展/周期广播 `K_NO_WAIT` 可丢；ACL/ISO `K_MSEC(50)`，其它事件 `K_MSEC(100)`，拿不到缓冲就丢包继续读 |
| 识别卡住 | `chips/sf32lb52/sf32lb52_bt_adapter.c` | `notify_host < 0` 记失败窗口；成功则清；日志约 1 Hz，带 `n=` 与 `stall=` |
| 升级恢复（两级） | `ble_supervisor.c` + `ble_companion.c` | 连续失败 ≥ 2.5 s 且最近一次失败在 500 ms 内 → 请求 adapter cycle；**一次 cycle 仍失败 → 升级为 LCPU 复位 + companion task 重建**；原因是 adapter 时即使「手机已连接」也执行；cycle 开头清僵尸 `connected` 和 stall 计数 |

**升级请求的限频与保留（2026-10-18）**：控制器复位受 **5 分钟限频**约束。
`companion_supervisor_pump()` 取走升级请求后，如果
`companion_controller_recover()` 因限频返回 false，会调用
`ble_supervisor_defer_escalation()` 把请求重新挂起；
`cycle_requested()` / `pending_reason()` 把 `g_escalation_req` 纳入判断，
`take_adapter_cycle()` 在升级挂起时让路。这样限频窗口过后 pump 会自动再试，
不会因为普通 cycle 重试已关掉而静默停住。
host 回归测试：`services/ble_companion/tests/ble_supervisor_test.c`
（`make -C boards/sf32lb52/helmone/services/ble_companion/tests run`）。

短暂扫表把缓冲打满一下，**不要**当 cycle；只有多秒连续投递失败才升级。

验收日志：

- 压力下 `-12` 应消失或只闪一下（H4 自己抽干）。
- 真卡住约 2.5 s 后应出现 `sf32lb52 bt rx callback: -12 (n=… stall=… ms)`，接着 `ble_supervisor: request adapter cycle reason=adapter (immediate)` 和 `adapter cycle begin`。
- adapter cycle 仍失败后：`BLE recovery escalate: N adapter cycles failed, controller reset requested` → `BLE recovery: forcing LCPU controller reset` → companion task 重建；若复位被 5 分钟限频挡住，应看到请求被保留而不是静默消失。

排障入口也记在 [bluetooth_guide.md](../bluetooth_guide.md) FAQ。

companion 堵在同步 HCI 上、本节约 2.5 s 的 cycle **请求不出来** 时，由独立 [diag 线程](../diag.md) 在心跳停 15 s 后硬复位 LCPU；进程没了才 SIGTERM/spawn。不要在 HCI 飞行中 `task_delete`。

---

## 8. 修订记录

| 版本 | 日期 | 说明 |
|------|------|------|
| v0.3 | 2026-08-13 | 决策定稿：一功能一特征 + 路径寻址 + 两侧 P0；修正 `0xFF17` 载荷分片；Status 扩展为 16 字节含回显字段 |
| v0.4 | 2026-08-14 | 固件侧 P0 真机验收通过（§5.4）。移除主循环 `statfs()`，`storage_free_kb` 改报未知；自启改挂 `lcd_bringup_main()` 末尾并加 `_AUTOSTART` / `_NAME_PREFIX` / `_ADV_INTERVAL_MS` 配置项；notify 失败提升为常态错误日志。新增 §7.2 运行时问题记录 |
| v0.5 | 2026-08-14 | P1 固件 FS：`transfer_backend` + `0xFF15`/`0xFF16`。明确不走 Classic SPP。`FS_STATFS` 拒绝。companion 优先级改为 8 |
| v0.6 | 2026-08-14 | 协议 0x0004：上传续传（WRITE_OPEN flags + 8 字节回复，断线保留 `*.part`），下载用 READ offset；删除/移动会处理残留 `*.part`。路径上限 256 |
| v0.8 | 2026-08-15 | 关闭 Classic：LCPU BLE-only，Flags `0x06`，文件夹传输回到 GATT。吞吐：2M PHY、DLE、7.5–15 ms interval、WWR、notify 无间隔。协议仍为 0x0005 |
| v0.9 | 2026-09-05 | §7.4：HCI RX `-ENOMEM`（`bt rx callback: -12`）。H4 `get_rx` 超时抽干 tty；adapter 统计 stall；supervisor 对 HCI 卡住立刻 cycle，且不再因僵尸「已连接」跳过 |
| v0.10 | 2026-10-18 | §7.4：cycle → LCPU 复位两级阶梯；5 min 限频时 `ble_supervisor_defer_escalation()` 保留请求（host 测试 `tests/ble_supervisor_test.c`）。窗口外已绑定手机定向回连（RPA/60 s/回退）见 [directed_reconnect.md](directed_reconnect.md)；Companion 全特征 `ENCRYPT_REQUIRED` |
