# 第三方组件与数据来源

本仓以 **Apache-2.0** 发布（见 [`LICENSE`](LICENSE)）。下表是本仓**随仓分发**的第三方
内容及其许可。各组件目录内均保留原始许可文件，冲突时以组件自带为准。

## 一、随仓分发的代码组件

| 组件 | 位置 | 许可 |
| --- | --- | --- |
| openvela / NuttX（内核与 apps，经 `repo` 拉取，不随本仓） | 工作区 `nuttx/`、`apps/` | Apache-2.0 |
| **LVGL 9.5**（`helm-one/` 内自带一份，运行时用的是这份） | `helm-one/apps/graphics/lvgl/lvgl/` | MIT |
| **FreeType**（矢量字体渲染） | 由工作区 `external/freetype` 提供 | FTL / GPLv2 二选一 |
| **zblue**（BLE 主机栈，经 `vela_override/` 就地打补丁） | 工作区 `external/zblue` | Apache-2.0 |
| **SiFli SF32LB52 SDK**（chips / CMSIS / DSP） | `helm-one/chips/` | Apache-2.0（CMSIS 为 Apache-2.0） |
| **littlefs**（KV/LFS 卷） | `helm-one/.cache/littlefs-v2.5.1/` | BSD-3-Clause |
| **Flutter / Dart 运行时与插件** | `companion/flutter/`（依赖见 `pubspec.yaml`） | BSD-3-Clause（各插件见其 pub 页） |
| **ESP-IDF**（`csc_sim` 依赖，不随仓） | — | Apache-2.0 |

字体文件（`helm-one/boards/sf32lb52/helmone/ui/bicycle/fonts/`、`mkfs/fat/fonts/`）随附的
思源黑体 / 阿里巴巴普惠体 / MiSans 等，版权归各自权利人，按各自许可（OFL / 阿里普惠体
许可 / MiSans 许可）使用，**仅做子集嵌入**，不单独再分发完整字库。

## 二、随仓分发的数据

| 数据 | 位置 | 许可与义务 |
| --- | --- | --- |
| **OpenStreetMap 地图数据**（含样本瓦片 `mkfs/fat/map/` 与 `docs/osm/osm/chuzhou.osm`） | `helm-one/` | **ODbL 1.0** —— 使用时须署名「© OpenStreetMap contributors」；对外分发派生产物同样受 ODbL 约束 |
| **SRTM / Terrarium 高程**（生成坡度与爬坡剖面） | 不随仓（按需下载） | SRTM 公有领域；Terrarium 见 Mapzen 托管条款 |
| 实机照片 | `docs/photos/` | 本队原创，随本仓 Apache-2.0 |

> 本作品**不含语音唤醒功能**，因此不涉及「你好，openvela」唤醒词及其相关许可。

## 三、上游改动

开发期未改动 openvela 上游仓库；板级差异通过
`helm-one/boards/sf32lb52/helmone/vela_override/` 抽换机制实现。对上游的改进以补丁形式
单独提交（zblue `id.c` 随机地址、`scan.c` 空指针保护、HCI H4 接收重开有界重试等），
补丁与说明见 [`helm-one/docs/pitch/`](helm-one/docs/pitch/README.md)。

## 四、AI 工具

开发期使用 ZCode（Z.ai）、Cursor 等 AI 工具，其中生成的代码同样以 Apache-2.0 发布；
具体使用口径见技术报告 3.6 节与 [`logs/README.md`](logs/README.md)。
