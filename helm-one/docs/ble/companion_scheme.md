# 自行车码表 BLE 方案（初稿）

> **已 supersede**：当前以 [companion_impl_plan.md](companion_impl_plan.md) +
> [pairing_and_radio.md](pairing_and_radio.md) 为准；本文与
> [companion_design.md](companion_design.md) 均为历史参考。§9 的
> “bondable=0 / 无加密”已过时。

> 适用：SF32LB52 + OpenVela（`my_vendor`）+ Flutter 手机 App  
> 原则：**全部走 BLE**（放弃 Classic SPP）；USB MTP 仅作插线维护可选保留  
> 更新：2026-06

---

## 1. 目标与范围

### 1.1 产品需求

| 方向 | 能力 |
|------|------|
| **手机 ↔ 码表** | 推送 GNSS/海拔；轨迹 GPX 上传/下发；类文件系统操作；可选通知 |
| **外设 ↔ 码表** | 心率（已规划）；后续踏频、功率、车灯等 |
| **维护** | USB MTP（现有 `mtp_simple`）可选保留，不参与 App 主路径 |

### 1.2 设计原则

1. **Flutter 双端一致**：Android / iOS 共用同一套 BLE GATT 协议，不依赖 Classic SPP。  
2. **传输与语义分离**：文件能力复用 `mtp_simple` 的 **catalog + 路径/handle + 读写语义**；传输层为 BLE 自定义帧，不照搬 USB bulk。  
3. **配对从简**：联调期 Companion 特征不要求加密；不引导 App 写 GAP Device Name（0x2A00）。  
4. **模块解耦**：GATT 回调薄；业务进 DataBroker / 消息队列；与外设 Central 独立。

---

## 2. 平台与栈

### 2.1 硬件 / 固件

| 项目 | 说明 |
|------|------|
| 芯片 | SF32LB52，HCPU 应用 + LCPU 蓝牙 ROM |
| HCI | `/dev/ttyHCI0`（`CONFIG_UART_BTH4=y`） |
| 协议栈 | zblue + OpenVela Bluetooth Framework（`CONFIG_BLUETOOTH_FRAMEWORK_LOCAL=y`） |
| 存储 | LittleFS（默认 `/mnt/lfs`），后续可扩展 SD |

### 2.2 必开配置（defconfig 参考）

```kconfig
CONFIG_UART_BTH4=y
CONFIG_BLUETOOTH=y
CONFIG_BLUETOOTH_FRAMEWORK=y
CONFIG_BLUETOOTH_FRAMEWORK_LOCAL=y
CONFIG_BLUETOOTH_BLE_SUPPORT=y
CONFIG_BLUETOOTH_BLE_ADV=y
CONFIG_LIBUV=y
CONFIG_LIBUV_EXTENSION=y
```

联调期建议：

- Device Name **只读**（勿开 `DEVICE_NAME_GATT_WRITABLE_ENCRYPT` 给 App 写）  
- 评估关闭 `CONFIG_BT_SMP`（需注意与 `BT_PRIVACY` 编译约束）  
- `set bondable 0`（应用层）

### 2.3 Rev B 芯片注意

SF32LB52-N16 等 **rev_id ≥ A4** 须使用 **LCPU2HCPU_MB_CH1_BUF_REV_B** 邮箱地址；已在 `sf32lb52_bt_adapter.c` 按 revision 选择 RX ring 地址，并定义 `SF32LB52X_REV_AUTO`。

---

## 3. 总体架构

```text
┌──────────────── Flutter App (Central) ────────────────┐
│  flutter_blue_plus · GNSS · FS 客户端 · UI            │
└────────────────────────┬────────────────────────────┘
                         │ BLE 连接
┌────────────────────────▼────────────────────────────┐
│                    码表 (SF32LB52)                   │
│  ┌─────────────────┐    ┌─────────────────────┐  │
│  │ ble_companion     │    │ ble_sensor          │  │
│  │ Peripheral        │    │ Central             │  │
│  │ GATT 0xFF10       │    │ HR/CSC/CPS…         │  │
│  └────────┬──────────┘    └──────────┬──────────┘  │
│           │                          │             │
│  ┌────────▼──────────────────────────▼──────────┐  │
│  │ transfer_backend（catalog + fs_ops）           │  │
│  │ 与 mtp_simple 共用 LittleFS 语义               │  │
│  └────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────┘
         │                    │
         ▼                    ▼
    心率带/踏频/功率         USB mtp_simple（可选，维护）
```

**码表双角色**

| 链路 | 码表 GAP 角色 | 说明 |
|------|---------------|------|
| 手机 | Peripheral | 广播 `Helm One-XXXX`，App 连接 Companion |
| 传感器 | Central | 扫描/绑定/订阅标准服务 |

---

## 4. 手机链路 — GATT 服务定义

### 4.1 Companion Service `0xFF10`

广播 Scan Response 中带 Complete Local Name + Service UUID `0xFF10`，便于 Flutter 过滤。

| 特征 | UUID | 属性 | 方向 | 说明 |
|------|------|------|------|------|
| Protocol Version | 0xFF11 | Read | 设备→App | `uint16` 协议版本，当前 `0x0001` |
| Device Status | 0xFF12 | Read / Notify | 设备→App | 状态位：录制中、GPS 源、存储满、同步中等 |
| Control | 0xFF13 | Write | App→设备 | 控制命令（见 4.3） |
| GNSS Fix | 0xFF14 | Write（No Response） | App→设备 | 手机 GNSS 下行（见 4.4） |
| FS Command | 0xFF15 | Write | App→设备 | 文件系统命令（见 5.1） |
| FS Data | 0xFF16 | Write / Notify | 双向 | 文件系统数据与响应（见 5.2） |
| Notification | 0xFF17 | Write | App→设备 | 可选，简化通知 TLV |

权限：**Read/Write 均不要求配对/加密**（联调期）。

### 4.2 标准服务（栈自带，只读展示）

| 服务 | UUID | 注意 |
|------|------|------|
| Generic Access | 0x1800 | **勿让 App 写 Device Name**，避免触发 SMP |
| Device Information | 0x180A | 型号、固件版本、序列号 |
| Battery（可选） | 0x180F | 码表电量 |

业务文件 **不要** 走 0x2A00 / 标准 DIS，统一走 **0xFF15/0xFF16**。

### 4.3 Control 命令（0xFF13）

```text
[opcode:1][param…]

0x01  START_RECORD
0x02  STOP_RECORD
0x03  START_FS_SYNC    进入文件同步态（可选，Device Status 置位）
0x04  ABORT_FS_SYNC
0x10  REBOOT_FS        请求 catalog 刷新（设备本地 rescan）
```

### 4.4 GNSS Fix（0xFF14）

手机以 **Write Without Response** 推送，建议 1 Hz（导航 5 Hz）。

```c
/* 小端，32 B 固定长度 */
struct phone_gnss_fix {
  int32_t  lat_e7;           /* 纬度 × 1e7 */
  int32_t  lon_e7;
  int32_t  alt_mm;           /* 海拔 mm */
  uint16_t speed_centi_kmh;  /* 0.01 km/h */
  uint16_t course_deg;       /* 0..359 */
  uint32_t utc_sec;
  uint8_t  fix_quality;      /* 0=无效 1=2D 2=3D */
  uint8_t  sats;
  uint8_t  hdop_x10;
  uint8_t  reserved;
};
```

设备侧写入 telemetry / 导航模块；Device Status 可置 `GPS_SOURCE_PHONE`。

### 4.5 Device Status（0xFF12）Notify 示例

```text
[flags:2][free_mb:4][err:1]

flags bit0  recording
      bit1  gps_phone_valid
      bit2  storage_low
      bit3  fs_sync_busy
```

---

## 5. 文件系统 over BLE（类 MTP 语义）

### 5.1 设计说明

- **不** 在 BLE 上跑 USB PTP 容器；**复用** `mtp_simple` 的 object handle、路径、catalog 与读写逻辑。  
- 新模块 `ble_fs_dispatch.c` 实现命令；底层调用抽出的 `transfer_backend`（与 USB MTP 共享）。  
- Flutter 实现对称 `BleFileSystemClient`，API 类似远程目录。

### 5.2 FS Command（0xFF15）

App 写入一帧命令（长度 ≤ MTU-3）：

```text
[cmd:1][seq:1][payload…]
```

| cmd | 名称 | payload 概要 | 对应 mtp_simple 语义 |
|-----|------|--------------|----------------------|
| 0x01 | ListStorage | 无 | GetStorageInfo |
| 0x02 | ListObjects | parent_handle u32 | GetObjectHandles |
| 0x03 | GetObjectInfo | handle u32 | GetObjectInfo |
| 0x04 | ReadOpen | handle u32, offset u32 | GetObject / Partial |
| 0x05 | ReadClose | session u8 | 结束读 |
| 0x06 | WriteOpen | parent_handle, name_len, name_utf8[], size u32 | SendObjectInfo |
| 0x07 | WriteData | session u8, chunk… | SendObject 数据阶段 |
| 0x08 | WriteClose | session u8, crc32 | 提交写入 |
| 0x09 | Delete | handle u32 | DeleteObject |
| 0x0A | Mkdir | parent_handle, name | 建目录 |

响应经 **FS Data Notify** 返回，不占用 Command 特征的 Read。

### 5.3 FS Data（0xFF16）帧格式

```text
[cmd:1][seq:1][flags:1][len:2][payload…]

flags: 0x01 ACK   0x02 LAST   0x80 ERR
```

**读文件流程**

```text
App  Write  FS Command  ReadOpen(handle, offset)
Dev  Notify FS Data    ACK + session_id + total_size
Dev  Notify FS Data    DATA chunks (seq++, LAST on final)
App  Write  FS Command  ReadClose(session)
```

**写文件（GPX 下发）**

```text
App  Write  WriteOpen → Notify ACK + session
App  Write  WriteData × N → 设备写 LittleFS 缓冲/直写
App  Write  WriteClose + crc32 → Notify OK / ERR
```

**列目录**

```text
App  Write  ListObjects(parent)
Dev  Notify 多条：{ handle, name, size, is_dir } 直到 LAST
```

### 5.4 性能与参数

| 项 | 建议 |
|----|------|
| MTU | 协商 247+（`CONFIG_BT_L2CAP_TX_MTU`） |
| PHY | 支持则 2M PHY |
| 连接间隔 | 同步时 15–30 ms；骑行 GNSS 可 30–50 ms |
| 吞吐 | 实测约 15 KB/s（小米 15 ms interval）；1 MB GPX 约 70 s。大文件走 USB |
| 断点续传 | ReadOpen / WriteOpen 带 offset |

同步期间 App 保持前台；Device Status 置 `fs_sync_busy`。

---

## 6. 外设链路 — 传感器 Central

码表作 **Central**，与手机 Companion **独立连接**。

| 外设 | 标准服务 | UUID | 数据 |
|------|----------|------|------|
| 心率 | Heart Rate | 0x180D | HR Measurement 0x2A37 Notify |
| 踏频/速度 | CSC | 0x1816 | CSC Measurement 0x2A5B |
| 功率 | Cycling Power | 0x1818 | CP Measurement 0x2A63 |
| 车灯等 | 厂商 / 自定义 | 0xFF20+ | 后续扩展 |

**统一 telemetry 结构**（内部总线）：

```c
struct bike_telemetry {
  uint32_t ts_ms;
  uint16_t hr_bpm;
  uint16_t cadence_rpm;
  uint16_t speed_centi_kmh;
  int16_t  power_w;
};
```

注意 `CONFIG_BT_MAX_CONN`：至少 **1（手机）+ 传感器数量**。

---

## 7. Flutter App 架构

```text
lib/
  core/
    ble_fs/           # FS 协议编解码（与本文 cmd 对齐）
    gnss_codec/
    gpx/
  ble/
    companion_client.dart   # 扫描、连接、GNSS、Control
    fs_client.dart          # List / Read / Write / Delete
  features/
    ride/               # 骑行：推 GNSS
    sync/               # 轨迹同步 UI
    sensors/            # 传感器管理（码表侧，App 仅配置/展示可选）
```

**依赖**

- `flutter_blue_plus` — BLE  
- `geolocator` — 手机 GNSS  
- `permission_handler` — 蓝牙 + 定位  

**无需** Classic SPP / Platform Channel（Android/iOS 同一套 Dart）。

---

## 8. 设备端软件模块

| 模块 | 路径（规划） | 说明 |
|------|--------------|------|
| `transfer_backend` | `boards/.../transfer/` | 从 `mtp_simple` 抽 catalog + fs_ops |
| `ble_companion` | `boards/.../ble_companion/` | GATT 0xFF10、`ble_fs_dispatch`、GNSS |
| `ble_sensor` | `boards/.../ble_sensor/` | Central、HR/CSC/CPS |
| `ble_test` | 已有 | 可演进为 companion 骨架 |
| `mtp_simple` | 已有 | USB 维护，与 backend 共享 |

**参考实现**

- GATT Server：`frameworks/.../tools/gatt_server.c`（custom 0xFF00 示例）  
- 现有 demo：`boards/.../ble_test_app/ble_test_app.c`  
- 文档：`docs/bluetooth_guide.md`

---

## 9. 配对、安全与排障

| 场景 | 建议 |
|------|------|
| 联调 | `bondable=0`；Companion 无加密；用 nRF Connect / 自研 App，少用系统蓝牙设置连接 |
| 写 0x2A00 Device Name | **禁止** App 暴露；会触发 SMP 配对 |
| 配对取消/超时 | 连接断开；可能需 `rm -rf /data/misc/bt` + `reboot` |
| 崩溃 | 查 unqlite 写 `/data/misc/bt`；见 `bluetooth_guide.md` §8 |
| rx ring buf=0 | Rev B 地址；确认 `sf32lb52_bt_adapter.c` revision 逻辑已编入 |

---

## 10. 实施阶段

| 阶段 | 内容 | 交付 |
|------|------|------|
| **P0** | `ble_companion`：连接、GNSS 0xFF14、Control、Status | Flutter 能推 GPS |
| **P1** | `transfer_backend` 抽取 + FS List/Read/Write/Delete | GPX 双向同步 |
| **P2** | MTU/PHY 优化、断点续传、Mkdir | 大文件稳定 |
| **P3** | `ble_sensor` HR；CSC/CPS | 外设数据上屏/录制 |
| **P4** | Notification 0xFF17；车灯 0xFF20 | 可选 |
| **维护** | 关 Classic 减 Flash（可选） | 仅 BLE 产品化 |

---

## 11. 与 USB MTP 的关系

| 通道 | 用途 | 协议 |
|------|------|------|
| **BLE Companion** | App 日常：GNSS、GPX、目录 | 0xFF10 + FS 命令 |
| **USB MTP** | 开发/插线维护 | 现有 `mtp_simple` PTP |

两者共用 **transfer_backend** 时，catalog 与文件视图一致。

---

## 12. 相关文档

| 文档 | 内容 |
|------|------|
| [bluetooth_guide.md](../bluetooth_guide.md) | 驱动、bttool、排障 |
| [build_guide.md](../build_guide.md) | 编译烧录 |
| [companion_design.md](companion_design.md) | 当前主方案 |
| `mtp_simple/README.md` | USB MTP 与 catalog 模型 |

---

## 附录 A：UUID 汇总

| 名称 | 16-bit UUID |
|------|-------------|
| Bike Companion Service | 0xFF10 |
| Protocol Version | 0xFF11 |
| Device Status | 0xFF12 |
| Control | 0xFF13 |
| GNSS Fix | 0xFF14 |
| FS Command | 0xFF15 |
| FS Data | 0xFF16 |
| Notification | 0xFF17 |
| Vendor Sensor/Light（预留） | 0xFF20 |

## 附录 B：广播建议

```text
ADV:  Flags 0x06 (LE General, BR/EDR not supported)
      Complete UUID16 0xFF10
RSP:  Complete Name "Helm One-XXXX"
```

Legacy 广播模式；名称与 `ble_test` 的 `Vela-BT` 联调名可并存，产品化用 `CONFIG_MYVENDOR_PRODUCT_NAME` + MAC 后缀。
