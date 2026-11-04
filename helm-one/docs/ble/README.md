# BLE 方案设计

自行车码表 **SF32LB52 / OpenVela** 的蓝牙通信设计文档目录。与板级驱动、联调命令分离，便于 App 与固件协议对齐。

| 文档 | 说明 |
|------|------|
| [companion_impl_plan.md](companion_impl_plan.md) | **实施权威**（v0.9）：四项架构决定、协议规格 v0.3、P0 范围与验收结果、已知约束（含 HCI RX `-ENOMEM` 两级自恢复） |
| [ble_sensor.md](ble_sensor.md) | **传感器 Central**：双角色、NSH `test sensor`、只发现 UUID、与 Companion FS/MTU 隔离、绑定记录与回连（含 `ctl sensor bind` 恢复、空轮回连退避） |
| [pairing_and_radio.md](pairing_and_radio.md) | **配对模型与射频节奏**：连接准入 / 手机记录（IRK 只服务手机）/ 广播分档（定向/无定向）/ 空轮回连退避 / 跨线程纪律 / `ctl` 运维工具箱 / 现场判据速查；**§1.3** RPA↔身份地址与 FS 0 帧 |
| [directed_reconnect.md](directed_reconnect.md) | **已绑定手机定向回连**（2026-10-18）：`ADV_DIRECT_IND` + own=PUBLIC、TargetA=最近 RPA、RPA 缓存/过期、60 s 窗口、失败回退、A/V1/V2/V3 实机对照、zblue pending conn 修复、RAL/绑定核对、`nsh-btadv` + `ble_test advtest` |
| [companion_bringup_notes.md](companion_bringup_notes.md) | **Bring-up 手记**：设计取舍的理由、真机调试踩的坑（构建/烧录/串口/主机工具）、排查方法论 |
| [companion_design.md](companion_design.md) | 总体设计（v0.1）：背景、架构、FS over BLE、传感器、实施阶段 |
| [companion_recording.md](companion_recording.md) | **Phase 1 记录**：App 写 GPX、与展示分离、协议与清单 |
| [companion_scheme.md](companion_scheme.md) | 早期初稿，内容已被 `companion_design.md` 覆盖，保留作参考 |

协议细节以 **`companion_impl_plan.md` + `companion_proto.h`** 为准；`companion_design.md` 中 `0xFF17` 通知的 `[type:1][len:1]` 格式与 Status 位域已被前者修正。

## 相关文档（docs 根目录）

| 文档 | 内容 |
|------|------|
| [required_patches.md](../required_patches.md) | **干净 OpenVela 树上必须改的 zblue `id.c`**（否则广播开着扫不了传感器）；构建期补丁 `patch_hci_core.py`（定向 pending conn 按原始 RPA 查）与 `patch_keys_kv.py`（IRK/LTK 落盘 + RAL） |
| [bluetooth_guide.md](../bluetooth_guide.md) | HCI、驱动、`bttool` 联调、FAQ |
| [diag.md](../diag.md) | **外设诊断线程**：独立于 companion 监管 BLE / GNSS，可按函数表注册 |
| [build_guide.md](../build_guide.md) | 编译烧录 |
| [开发计划.md](../开发计划.md) | 产品模块与里程碑 |

## 代码参考

| 路径 | 说明 |
|------|------|
| `boards/sf32lb52/helmone/domain/gpx/` | GPX 编解码 + 记录线程（纯 C，`gpx decode/record`） |
| `boards/sf32lb52/helmone/include/companion_proto.h` | BLE Companion 协议常量（固件 / App 共用） |
| `boards/sf32lb52/helmone/services/ble_companion/` | Companion `0xFF10` + `ble_sensor` Central（P3 NSH）+ `ble_supervisor`（adapter cycle → LCPU 复位，限频） |
| `boards/sf32lb52/helmone/nsh/ble_test_app/` | 底层 BT 冒烟测试（`0xFF00`，保留）；`nsh-btadv` 构建下含 `ble_test advtest` 定向矩阵 |
| `boards/sf32lb52/helmone/services/mtp_simple/` | USB MTP 与 catalog 语义 |

**版本**：v1.0 · **更新**：2026-10-18 · 传感器 Central 见 `ble_sensor.md`；配对/广播/射频节奏见 `pairing_and_radio.md`（含 §1.3 FS RPA）；**已绑定手机定向回连/RPA/RAL/advtest 见 [directed_reconnect.md](directed_reconnect.md)**；HCI `-12` 两级恢复见 `companion_impl_plan.md` §7.4；companion 卡住时的后手见 [diag.md](../diag.md)
