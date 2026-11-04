# companion/ — 手机端与台架工具

码表端在车上完全独立（无网、无触摸），但有三类事天然属于手机：**要手指的**（点地图、
选途经点）、**要网络的**（星历、OTA、天气）、**要系统的**（通知转发）。本目录就是这三类
工作的实现，外加一个用来"假装是传感器"的台架工具。

| 子目录 | 内容 | 规模 |
| --- | --- | --- |
| [`flutter/`](flutter/) | Helm One 伴侣 App（Android / iOS） | 111 个 `.dart` / 约 3.5 万行（`lib/`），22 个测试文件 |
| [`esp32/csc_sim/`](esp32/csc_sim/) | ESP32 台架工具：模拟 BLE 骑行速度/踏频传感器（CSC） | 9 个 `.c/.h` |

---

## 1. flutter/ — Helm One 伴侣 App

包名 `sifli_companion`，应用 ID `top.jinsc.helm_one`，版本 `1.0.0`。

### 三栏结构

| 栏 | 做什么 |
| --- | --- |
| **骑行** | 会话列表与详情、GPX 回放、图表（速度/心率/海拔/踏频）、圈段表、轨迹地图 |
| **设备** | BLE 配对与连接、实时状态、记录管理、文件传输、固件 OTA、星历注入 |
| **设置** | 单位/语言/主题、保活、离线城市地图下载与清理、缓存与日志导出、App 自更新 |

### 与码表的协议

- **BLE**：对码表是 GATT Client；固件侧 `companion_proto.h` ↔ App 侧
  `lib/ble/companion_proto.dart` **必须同版本**（改协议时两端一起升）
- **文件传输**：分块 + CRC16 + RLE，落 `/mnt/lfs/mtp/`（MTP 可见卷）
- **星历**：拉当天广播星历 → 转 UBX-MGA → 写入码表并立即注入模组
- **通知转发**：`NotificationListenerService` 按应用白名单与锁屏/骑行条件过滤
- **OTA**：下载后校验 SHA-256 与长度，版本号 + 硬件型号双重校验才允许写入升级槽

协议细节见 [`helm-one/docs/ble/`](../helm-one/docs/ble/)（`companion_design.md`、
`companion_recording.md`、`companion_scheme.md`）。

### 构建

```bash
cd companion/flutter
flutter pub get
./scripts/build.sh apk            # 同步版本号 → flutter build → 自动 adb install
./scripts/build.sh appbundle      # 出 .aab（不分发安装）
flutter test                      # 22 个测试文件
```

版本号约定：手写 `version.name`（如 `1.0.0`）+ 构建号取日期 `YYYYMMDD`，
由 `scripts/sync_version.sh` 合成 `1.0.0+20260920` 写进 `pubspec.yaml`。

### 目录

```text
lib/
├── app/        应用外壳、路由、主题
├── ble/        BLE 连接、协议编解码、文件传输
├── db/         本地库（sqflite）：会话、轨迹、设备
├── gnss/       星历、UBX-MGA 组包、注入流程
├── map/        离线城市地图下载与缓存
├── ota/        固件下载、校验、上传升级槽
├── ride/       骑行数据模型、GPX 读写、图表序列
├── transfer/   通道复用与重传
├── ui/         页面（骑行 / 设备 / 设置）
├── i18n/       中英文
└── util/ utils/ 日志、格式化、平台通道
```

> **未随仓内容**：`build/`、`.dart_tool/`、`android/.gradle/`、`android/local.properties`、
> `linux/flutter/ephemeral/` 均为构建产物或本机路径，按 Flutter 惯例不入库。

## 2. esp32/csc_sim/ — 台架用 BLE CSC 模拟器

实车之外要复现「踏频 0→120 rpm 跳变」「断连重连」这类场景，靠真传感器不好控。这个 ESP32
固件对外扮演一个 **BLE Cycling Speed and Cadence（仅踏频）** 从设备：

- 按真实曲柄转动节奏发 `(累计圈数, 事件时间 1/1024 s)`，让客户端算出的踏频与真表一致
  （而不是按通知频率发假数据）；
- 串口控制台（与日志共用 UART0，一根烧录线即可驱动）：
  `mode` / `rpm <n>` / `start` / `stop` / `status`；
- 附 `tools/csc_central.py` 作为 PC 端中央设备，做无人值守回归。

```bash
cd companion/esp32/csc_sim
idf.py set-target esp32c3      # 或其他 ESP32 型号
idf.py build flash monitor
```

> **未随仓内容**：`build/`（ESP-IDF 构建目录，开发仓里曾误提交，此处不入库）。
