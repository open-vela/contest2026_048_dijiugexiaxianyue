# 自行车码表 BLE 方案设计（Companion + 传感器）

> **历史设计（v0.1，2026-06-15）**：当前实现以 [companion_impl_plan.md](companion_impl_plan.md)
> + [pairing_and_radio.md](pairing_and_radio.md) 为准；文中“无 ENCRYPT / bondable=false /
> `CONFIG_BT_MAX_CONN=4`”等均为历史值，勿直接照做。定向回连见
> [directed_reconnect.md](directed_reconnect.md)。

本文描述 **SF32LB52 / OpenVela** 自行车码表产品的 **纯 BLE** 通信方案：与手机 App 的数据交换、与外设传感器的连接，以及和现有 `mtp_simple` 的复用关系。

- 板级蓝牙栈、驱动、联调命令见 [bluetooth_guide.md](../bluetooth_guide.md)
- 产品阶段规划见 [开发计划.md](../开发计划.md)
- USB 插线传文件仍走 [mtp_simple](../../boards/sf32lb52/helmone/services/mtp_simple/README.md)（与 BLE 互补，非 App 主路径）

**版本**：v0.1（初步方案）  
**更新**：2026-06-15

---

## 1. 目标与原则

### 1.1 业务需求

| 方向 | 需求 |
|------|------|
| 手机 → 码表 | GNSS/海拔等定位信息 |
| 手机 ↔ 码表 | GPX/轨迹文件上传、下发；类文件系统操作（列目录、删、建文件夹） |
| 手机 → 码表 | 通知推送（非必要） |
| 外设 → 码表 | 心率（首发）；后续踏频、功率、车灯等 |

### 1.2 设计原则

1. **全部走 BLE**：手机 App 不依赖 Classic SPP，Android / iOS / Flutter 共用一套 GATT 协议。
2. **双角色**：对手机为 **Peripheral（GATT Server）**；对传感器为 **Central（GATT Client）**。
3. **文件语义复用 `mtp_simple`**：目录、handle、读写/delete 与 USB MTP 共用 backend；BLE 上只换传输帧，不另起一套文件协议。
4. **配对从简**：联调期 Companion 特征不要求加密；避免写 GAP Device Name（0x2A00）触发 SMP。
5. **GATT 回调薄、业务厚**：解析/存储/UI 走消息队列或 DataBroker，不在蓝牙回调里做重活。
6. **记录与展示分离**：GPX 记录优先 **`gpx/` 组件** + Companion App；bicycle `DP_Recorder` 保持 X-TRACK 行为。详见 [companion_recording.md](companion_recording.md)。

---

## 2. 总体架构

```text
                         ┌──────────────────────┐
                         │   手机 App (Flutter)  │
                         │   BLE Central         │
                         └──────────┬───────────┘
                                    │ GATT
                         ┌──────────▼───────────┐
                         │      自行车码表         │
                         │  ┌──────────────────┐  │
                         │  │ ble_companion    │  │  Peripheral
                         │  │ Service 0xFF10   │  │
                         │  └────────┬─────────┘  │
                         │  ┌────────▼─────────┐  │
                         │  │ transfer_backend │  │  catalog + FS ops
                         │  │ (与 mtp 共用)     │  │
                         │  └────────┬─────────┘  │
                         │  ┌────────▼─────────┐  │
                         │  │ ble_sensor       │  │  Central
                         │  └────────┬─────────┘  │
                         └───────────┼────────────┘
                 ┌──────────────────┼──────────────────┐
                 ▼                  ▼                  ▼
            心率带/手表           踏频/速度            功率计 …
            HR 0x180D            CSC 0x1816          CPS 0x1818
```

### 2.1 与 USB MTP 的关系

| 通道 | 场景 | 协议 |
|------|------|------|
| **BLE Companion** | 骑行中 / 无线同步 GPX、推 GNSS | 本文 |
| **USB MTP** | 插线维护、开发、大文件批量拷贝 | `mtp_simple` |

两者共享 **`transfer_backend`**（catalog、路径 ↔ handle、读写 LittleFS/SD），仅 **transport** 不同。

### 2.2 与 Classic BT

手机 App **不使用 SPP/Classic**。固件可保留 `CONFIG_BT_CLASSIC` 直至确认无需后再关以省 Flash；App 路径不依赖 Classic。

---

## 3. 手机链路：Companion 服务

### 3.1 广播

| 项目 | 建议 |
|------|------|
| 广播名 | `CONFIG_MYVENDOR_PRODUCT_NAME` + MAC 后两字节（如 `Helm One-A3F2`） |
| 广播包 | Flags `0x06`（LE General + BR/EDR Not Supported 仅 LE） |
| Scan Response | Complete Local Name + Service UUID `0xFF10`（16-bit） |
| 连接 | Legacy ADV_IND；量产可评估 Extended ADV |

参考联调：`ble_test` / `bttool adv start -m legacy -r 020106038F03FF080956656C612D4254`（产品名改为 Helm One）。

### 3.2 GATT 服务定义

**Primary Service：`0xFF10` — Bike Companion**

| 特征 | UUID | 属性 | 方向 | 说明 |
|------|------|------|------|------|
| Protocol Version | `0xFF11` | Read | 设备→App | `uint16` 协议版本，当前 `0x0001` |
| Device Status | `0xFF12` | Read, Notify | 设备→App | 状态位域（见 3.4） |
| Control | `0xFF13` | Write | App→设备 | 控制命令（见 3.5） |
| GNSS Fix | `0xFF14` | Write, Write Without Response | App→设备 | 手机定位下行（见 3.6） |
| Track Point | `0xFF18` | Notify | 设备→App | 内置 GNSS 轨迹点（App 写 GPX，见 companion_recording.md §4） |
| FS Command | `0xFF15` | Write | App→设备 | 文件系统命令（见第 4 节） |
| FS Data | `0xFF16` | Write, Notify | 双向 | 文件系统数据/响应（见第 4 节） |
| Notification | `0xFF17` | Write | App→设备 | 可选，简化通知 TLV（见 3.7） |

标准服务（协议栈自带，只读展示）：

| 服务 | UUID | 注意 |
|------|------|------|
| Generic Access | `0x1800` | **Device Name 只读**，App 勿写 `0x2A00` |
| Device Information | `0x180A` | 型号、固件版本、序列号 |
| Battery（可选） | `0x180F` | 码表电量 |

### 3.3 UUID 与权限

- 16-bit UUID：`0xFF10`–`0xFF17` 为厂商自定义块。
- Companion 全部特征权限：**Read / Write 均不要求 `ENCRYPT` / `AUTHEN`**（联调与量产初期）。
- Notify 特征需 Client Characteristic Configuration Descriptor（CCCD）。

### 3.4 Device Status（Notify）

```c
/* 位域，Notify 时在首字节起按 little-endian 打包 */
#define COMPANION_STATUS_RECORDING      (1u << 0)  /* 正在录制 */
#define COMPANION_STATUS_GPS_PHONE      (1u << 1)  /* 使用手机 GNSS */
#define COMPANION_STATUS_GPS_INTERNAL   (1u << 2)  /* 使用内置 GNSS */
#define COMPANION_STATUS_STORAGE_FULL    (1u << 3)
#define COMPANION_STATUS_FS_BUSY        (1u << 4)  /* 文件同步进行中 */
#define COMPANION_STATUS_SENSOR_HR      (1u << 8)  /* 心率已连接 */
/* 后续：CSC、CPS、车灯… */
```

### 3.5 Control（Write）

| opcode | 值 | 说明 |
|--------|-----|------|
| `CTRL_NOP` | 0x00 | 保留 |
| `CTRL_START_RECORD` | 0x01 | 开始录制 |
| `CTRL_STOP_RECORD` | 0x02 | 停止录制 |
| `CTRL_FS_ABORT` | 0x10 | 中止当前 FS 传输 |
| `CTRL_SYNC_PREP` | 0x11 | 可选：进入同步态（降频其他 Notify） |

### 3.6 GNSS Fix（Write / Write Without Response）

App 在骑行中以 **1–5 Hz** 写入（推荐 **Write Without Response**）。

```c
struct companion_gnss_fix {
  int32_t  lat_e7;           /* 纬度 × 1e7，WGS84 */
  int32_t  lon_e7;           /* 经度 × 1e7 */
  int32_t  alt_mm;           /* 海拔 mm */
  uint16_t speed_centi_kmh;  /* 0.01 km/h */
  uint16_t course_deg;       /* 0–359 */
  uint32_t utc_sec;          /* Unix 时间戳 */
  uint8_t  fix_quality;      /* 0=无效 1=2D 2=3D */
  uint8_t  satellites;
  uint8_t  hdop_x10;
  uint8_t  reserved;
};  /* 共 24 字节 */
```

码表侧写入内部 **GNSS 数据源**；若同时有内置 GNSS，由策略模块选择 `GPS_PHONE` / `GPS_INTERNAL` / 融合。

### 3.7 Notification（Write，可选）

TLV 格式，单包不超过 MTU-3：

```text
[type:1][len:1][payload:len]
type: 0=普通 1=来电 2=短信 …
payload: UTF-8 标题/正文（截断）
```

跨平台不做 ANCS；由 App 读取系统通知后转发。

---

## 4. 文件系统 over BLE（FS Command / FS Data）

### 4.1 设计思路

- **语义**对齐 `mtp_simple` / PTP：`List`、`GetObject`、`SendObject`、`DeleteObject`、建目录等。
- **传输**为 BLE 自定义帧，**不**使用 USB PTP bulk 容器。
- 设备端：`ble_fs_dispatch(cmd, payload)` → `transfer_backend` → LittleFS（默认 `/mnt/lfs`，轨迹目录如 `/tracks`）。

### 4.2 帧格式（FS Data 通道）

**App → 设备（Write FS Data）** 与 **设备 → App（Notify FS Data）** 使用相同帧头：

```text
 offset  size   field
 0       1      cmd_id      /* 与 FS Command 呼应，或响应时填原 cmd */
 1       1      flags       /* bit0=LAST bit1=ERR bit2=ACK_REQUEST */
 2       2      seq         /* 序号，单调递增 */
 4       2      len         /* payload 长度 */
 6       len    payload
```

**FS Command（0xFF15）**：仅写 1 字节 `cmd_id` + 可选短参数（≤18 字节，或参数走首包 FS Data）。

### 4.3 命令集（v0.1）

| cmd_id | 名称 | 方向 | 说明 |
|--------|------|------|------|
| `0x01` | `FS_LIST_STORAGE` | App→Dev | 列出存储；响应含 storage_id、容量 |
| `0x02` | `FS_LIST_OBJECTS` | App→Dev | 参数：parent_handle 或 path；响应 object handle 列表 |
| `0x03` | `FS_GET_OBJECT_INFO` | App→Dev | 参数：handle；响应 name、size、is_dir |
| `0x04` | `FS_READ_OPEN` | App→Dev | 参数：handle、[offset]；准备读文件 |
| `0x05` | `FS_READ_DATA` | App→Dev | 请求下一块；Dev Notify 数据 |
| `0x06` | `FS_READ_CLOSE` | App→Dev | 结束读 |
| `0x07` | `FS_WRITE_OPEN` | App→Dev | 参数：parent、name、size |
| `0x08` | `FS_WRITE_DATA` | App→Dev | 写入一块 |
| `0x09` | `FS_WRITE_CLOSE` | App→Dev | 参数：crc32；提交文件 |
| `0x0A` | `FS_DELETE` | App→Dev | 参数：handle |
| `0x0B` | `FS_MKDIR` | App→Dev | 参数：parent、name |

**P0 实现**：`0x01`–`0x06`、`0x07`–`0x09`、`0x0A`。  
**P1**：`0x0B`、Move/Rename（可扩展 `0x0C`/`0x0D`）。

### 4.4 读文件流程（码表 → 手机，GPX 上传）

```text
App                          设备
 |-- FS_READ_OPEN(handle) ---->|
 |<-- Notify OK + size --------|
 |-- FS_READ_DATA ------------>|
 |<-- Notify DATA chunks ------|  (重复直到 LAST)
 |-- FS_READ_CLOSE ----------->|
 |<-- Notify OK ----------------|
```

### 4.5 写文件流程（手机 → 码表，GPX 下发）

```text
App                          设备
 |-- FS_WRITE_OPEN ----------->|
 |<-- Notify OK + handle -------|
 |-- FS_WRITE_DATA (chunks) --->|
 |-- FS_WRITE_CLOSE + crc ---->|
 |<-- Notify OK ----------------|
```

### 4.6 与 mtp_simple catalog 的对应

| BLE FS | mtp_simple / PTP |
|--------|------------------|
| `FS_LIST_OBJECTS` | `GetObjectHandles` |
| `FS_GET_OBJECT_INFO` | `GetObjectInfo` |
| `FS_READ_*` | `GetObject` / `GetPartialObject` |
| `FS_WRITE_*` | `SendObjectInfo` + `SendObject` |
| `FS_DELETE` | `DeleteObject` |
| `FS_MKDIR` | `SendObjectInfo` (Association) + mkdir |

重构时从 `mtp_simple.c` 抽出 **`transfer_backend`**，`ble_fs_dispatch` 与 `mtp_dispatch` 并列调用同一 catalog。

### 4.7 性能与参数

| 项目 | 建议 |
|------|------|
| MTU | 协商至 **247+**（Android/iOS 通常支持） |
| PHY | 支持则开 **2M PHY** |
| 连接间隔 | 同步文件时请求 **15–30 ms** |
| 吞吐 | 实测约 **15 KB/s**（小米 15 ms interval）；1 MB GPX 约 **70 s**。大文件走 USB |
| 断点续传 | `FS_READ_OPEN` / `FS_WRITE_OPEN` 带 **offset**（P1） |

同步期间置 `COMPANION_STATUS_FS_BUSY`；App 显示进度条，支持 `CTRL_FS_ABORT`。

---

## 5. 外设链路（传感器）

码表作为 **BLE Central**，与手机 Companion **独立连接**（不同角色、不同对端地址）。实现、NSH 与真机约束见 **[ble_sensor.md](ble_sensor.md)**。

### 5.1 标准 Profile（优先）

| 传感器 | Service UUID | Characteristic | 数据 |
|--------|--------------|----------------|------|
| 心率 | `0x180D` | HR Measurement `0x2A37` | Notify，BPM |
| 踏频/速度 | `0x1816` | CSC Measurement `0x2A5B` | Notify |
| 功率 | `0x1818` | CP Measurement `0x2A63` | Notify |

三台独立设备，各占一个 GATTC 槽。同一条链路上先发现电量 `0x180F`（`0x2A19` 有 Notify 则订 CCC，否则 Read），再发现该槽位主服务并订测量。不要 `discover all`。

### 5.2 绑定与扫描

- 用户「添加传感器」：UI 触发扫描，过滤 Service UUID。NSH：`test sensor scan` / `connect <idx>`（按地址）。
- 已绑定：按 **保存的 MAC** 定向重连（见广播后再连，不按名字）。
- 数据解析后写入 **`bike_telemetry`** / `companion_bridge`，供 UI / 录制消费。

### 5.3 车灯等

后续可用厂商自定义 Service 或预留 **`0xFF20`** 块；与 Companion `0xFF10` 分离。

### 5.4 连接数与隔离

`CONFIG_BT_MAX_CONN=4`：**手机 1 + HR/CSC/CPS**。手机连接与传感器可并行。

传感器连上时 **不得**改 Companion 的 MTU/`g_ctx.peer`/FS 会话；Central **不得**对传感器强制 7.5 ms / 1 s 监督 / PHY / DLE。原因与 HCI `0x16` 见 [ble_sensor.md](ble_sensor.md) §3。

---

## 6. 设备端软件模块（OpenVela）

### 6.1 目录规划（建议）

```text
vendor/HelmOne/boards/sf32lb52/helmone/
  transfer_backend/       # 从 mtp_simple 抽出
    catalog.c
    fs_ops.c
    transfer_backend.h
  ble_companion/
    ble_companion_main.c  # NSH 命令 / 自启动
    companion_gatt.c      # 0xFF10 注册与特征
    companion_gnss.c
    ble_fs_dispatch.c
  ble_sensor/             # 后续
    sensor_manager.c
  mtp_simple/             # 保留 USB 路径
  ble_test_app/           # 可演进合并进 ble_companion
```

### 6.2 API 栈

- OpenVela **Bluetooth Framework**：`bluetooth_create_instance`、`bt_adapter_*`、`bt_gatts_*`、`bt_le_start_advertising`。
- 传感器侧：`bt_gattc_*`、扫描 API（Framework / zblue）。
- HCI：`/dev/ttyHCI0`，需 `CONFIG_UART_BTH4=y`，见 [bluetooth_guide.md](../bluetooth_guide.md)。

### 6.3 任务与栈

| 任务 | 栈建议 | 说明 |
|------|--------|------|
| `ble_companion` | 8192+ | 含 Framework 与 FS 缓冲 |
| `ble_sensor` | 4096+ | 扫描/多连接 |
| FS 大缓冲 | PSRAM | 与 `mtp_simple` 类似，chunk 4 KiB |

GATT 回调内仅 **拷贝 + 投递消息**；文件读写、catalog 扫描在工作线程执行。

---

## 7. 手机 App（Flutter）

### 7.1 依赖

| 能力 | 包 |
|------|-----|
| BLE | `flutter_blue_plus` |
| GNSS | `geolocator` |
| 权限 | `permission_handler` |
| 本地 GPX/文件 | `path_provider`、`file_picker` 等 |

### 7.2 模块

```text
lib/
  ble/companion_client.dart    # 连接、特征发现
  ble/gnss_codec.dart          # struct ↔ Uint8List
  ble/fs_client.dart           # FS Command/Data 协议
  features/sync/               # 远程目录、上传下载
  features/ride/               # 骑行推 GNSS
```

Android / iOS **同一套 Dart**，无需 SPP Platform Channel。

### 7.3 典型用户流程

1. 扫描 `0xFF10`（广播名形如 `Helm One-*`）→ 连接 → 发现服务。
2. 骑行：订阅 `Device Status`，以 WWR 写 `GNSS Fix`。
3. 同步轨迹：`FS_LIST_OBJECTS` → `FS_READ_*` 或 `FS_WRITE_*`。
4. 断开前 `FS_READ_CLOSE` / `CTRL_FS_ABORT` 收尾。

---

## 8. 安全与配对

### 8.1 联调 / 早期量产

| 配置 | 建议 |
|------|------|
| `bondable` | `false`（`bt_device_set_bondable_le`） |
| Companion 特征 | 无 `ENCRYPT` / `AUTHEN` |
| Device Name GATT | **只读**（勿开 `DEVICE_NAME_GATT_WRITABLE_ENCRYPT`） |
| SMP | 可关闭 `CONFIG_BT_SMP`（见 [bluetooth_guide.md](../bluetooth_guide.md) 注意与 `BT_PRIVACY` 编译关系） |

### 8.2 App 侧注意

- **不要**在 nRF Connect 等工具里写 **GAP Device Name**，会触发配对且可能导致异常断开。
- 测试 Companion 的 **0xFF05/0xFF02** 或本文 **FS** 特征即可。
- 异常后：`rm -rf /data/misc/bt` + `reboot`（见 bluetooth_guide  FAQ）。

### 8.3 量产加固（可选）

- 仅对「删除全部轨迹 / OTA」类 Control 要求配对。
- Companion 读写 GPX 仍保持无配对，降低支持成本。

---

## 9. 配置清单（defconfig 参考）

与 [bluetooth_guide.md](../bluetooth_guide.md) 一致，并确保：

```kconfig
CONFIG_UART_BTH4=y
CONFIG_BLUETOOTH=y
CONFIG_BLUETOOTH_FRAMEWORK=y
CONFIG_BLUETOOTH_FRAMEWORK_LOCAL=y
CONFIG_BLUETOOTH_BLE_SUPPORT=y
CONFIG_BLUETOOTH_BLE_ADV=y
CONFIG_LIBUV=y
CONFIG_LIBUV_EXTENSION=y
CONFIG_BT=y
CONFIG_BT_CLASSIC=y          # 可选，App 不用 Classic 时可后续关闭
```

Rev B 芯片（SF32LB52-N16 等）需 **`CONFIG_UART_BTH4`** + 驱动内 **RX ring Rev B 地址**（见 bluetooth_guide §8）。

---

## 10. 实施阶段

| 阶段 | 内容 | 交付 |
|------|------|------|
| **R1** | `gpx/` 组件（queue + 批写 + 记录线程）；App 侧 GPX | 见 [companion_recording.md](companion_recording.md) |
| **P0** | `transfer_backend` 抽取；`ble_companion` 广播 + Control/Status + P0 FS | App 会话同步、路线下发 |
| **P1** | App 推 GNSS Fix 展示；MTU/PHY；`ble_sensor` 心率 | 展示与日常骑行闭环 |
| **P2** | CSC/CPS；Notification；Mkdir/Move | 传感器与体验完善 |
| **P3** | 关 Classic 减 Flash；量产安全策略 | 发布固件 |

当前仓库 **`ble_test_app`** 可作为 P0 的连接/广播/GATT 骨架，逐步替换为 Companion `0xFF10` 与 FS 命令。

---

## 11. 相关文档

| 文档 | 内容 |
|------|------|
| [ble/README.md](README.md) | BLE 设计文档索引 |
| [bluetooth_guide.md](../bluetooth_guide.md) | 驱动、HCI、`bttool` 联调、FAQ |
| [build_guide.md](../build_guide.md) | 编译烧录 |
| [开发计划.md](../开发计划.md) | 产品模块与里程碑 |
| [mtp_simple/README.md](../../boards/sf32lb52/helmone/services/mtp_simple/README.md) | USB MTP 与 catalog 行为 |

---

## 12. 修订记录

| 版本 | 日期 | 说明 |
|------|------|------|
| v0.1 | 2026-06-15 | 初稿：纯 BLE、Companion 0xFF10、FS over BLE、Flutter、传感器 |
