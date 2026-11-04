# GPX 记录与展示分离 — Phase 1：App 侧记录

本文定义 **轨迹记录（Recording）** 与 **码表展示（Display）** 的职责边界，并给出 Phase 1 落地步骤。展示侧改造（地图、速度、轨迹线 UI）在 Phase 2 进行。

- BLE 总方案：[companion_design.md](companion_design.md)
- 协议常量：`boards/sf32lb52/helmone/include/companion_proto.h`
- 固件路径：`boards/.../my_vendor/domain/gpx/`（`gpx record` / `gpx decode`）
- bicycle `DP_Recorder` 保持 X-TRACK 原样，后续 GNSS 接入优先走 `gpx` 组件

**版本**：v0.1  
**更新**：2026-06-16

---

## 1. 为什么要分离

| 维度 | 码表写 GPX（X-TRACK 旧行为） | App 写 GPX（推荐） |
|------|------------------------------|---------------------|
| 存储 | 占 SD/LFS，长骑文件大 | 手机存储充裕 |
| 同步 | 骑完 BLE 传文件，慢 | 骑完已在手机，可直传 Strava |
| 可靠性 | 关机/写盘与 UI 耦合 | App 前台服务 + 系统定位 |
| 码表职责 | 显示 + 存储 + 协议 | **只显示 + 传感器** |

**原则**：产品级 GPX 记录优先在 **`gpx/` 组件**（C、批写、队列层）演进；bicycle UI 与 `DP_Recorder` 暂不改动。

---

## 2. 模块边界

```text
┌─────────────────────────────────────────────────────────┐
│  Phase 1 — Recording（本文）                             │
│  ┌──────────────────┐         ┌──────────────────────┐  │
│  │ Companion App    │  BLE    │ ble_companion (P0)   │  │
│  │ · geolocator     │◄───────►│ · CTRL START/STOP    │  │
│  │ · 写 GPX 文件     │         │ · Device Status      │  │
│  │ · 上传 Strava    │         │ · (P1b) Track Notify │  │
│  └──────────────────┘         └──────────────────────┘  │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│  Phase 2 — Display（后续）                               │
│  bicycle UI · DP_GNSS · LiveMap · SportStatus           │
│  可选：App 推 GNSS Fix (0xFF14) 供码表显示               │
└─────────────────────────────────────────────────────────┘
```

| 模块 | Phase | 职责 |
|------|-------|------|
| **App GPX Writer** | 1 | 采点、写 GPX、导出/上传 |
| **ble_companion Control/Status** | 1 | 会话同步（REC 指示、可选状态位） |
| **码表 DP_Recorder** | — | 默认关闭；仅调试可开 Kconfig |
| **码表 UI / 地图** | 2 | 速度、里程、轨迹线（与 GPX 文件无关） |

---

## 3. Phase 1a：手机 GPS 记录（优先实现）

无需码表向 App 推轨迹点；App 使用系统定位即可。

### 3.1 App 流程

1. 扫描连接 `Helm One-*`（过滤 UUID `0xFF10`），发现 Service `0xFF10`。
2. 用户点「开始骑行」：
   - App 启动前台定位服务（Android FGS / iOS Always 按审核策略）。
   - 创建 GPX 文件，写 `<gpx>` / `<trk>` 头。
   - （可选）Write `COMPANION_CTRL_START_RECORD` → 码表 Status 置 `RECORDING`。
3. 定位回调（建议 **1 Hz**，导航场景 **5 Hz**）：
   - App 本地追加 `<trkpt lat="…" lon="…"><ele>…</ele><time>…</time></trkpt>`。
   - （Phase 2）可同时 WWR 写 `companion_gnss_fix` 给码表显示。
4. 用户点「结束」：
   - 写 GPX footer，`close` 文件。
   - Write `COMPANION_CTRL_STOP_RECORD`。
5. 导出：`/rides/YYYY-MM/TRK_*.gpx`，或分享至 Strava / 文件。

### 3.2 GPX 格式要求

与 X-TRACK / Strava 兼容即可：

```xml
<?xml version="1.0" encoding="UTF-8"?>
<gpx version="1.1" creator="Helm One App"
 xmlns="http://www.topografix.com/GPX/1/1">
  <metadata>
    <name>2026-06-16 Ride</name>
    <time>2026-06-16T08:00:00Z</time>
  </metadata>
  <trk>
    <name>Untitled</name>
    <trkseg>
      <trkpt lat="39.907415" lon="116.391332">
        <ele>45.2</ele>
        <time>2026-06-16T08:00:01Z</time>
      </trkpt>
      <!-- … -->
    </trkseg>
  </trk>
</gpx>
```

- 时间：**ISO 8601 UTC**（`…Z`）。
- 坐标：WGS84，纬度 `lat`、经度 `lon`，至少 6 位小数。
- 海拔：米（`<ele>`），来自 GPS 或 baro（App 侧融合）。

### 3.3 Flutter 参考模块

```text
lib/features/ride/
  ride_session.dart      # 开/停、状态机
  gpx_writer.dart        # 流式写 trkpt
  ride_repository.dart   # 文件路径、历史列表
lib/ble/
  companion_client.dart  # 连接 0xFF10
  companion_control.dart # START/STOP_RECORD
```

依赖：`geolocator`、`path_provider`；权限 `permission_handler`。

### 3.4 码表固件（当前）

- **`gpx/` 组件**：NSH `gpx record` / `gpx decode`；GNSS 接入 `gpx_record_push`（待做）。
- **bicycle `DP_Recorder`**：保持 X-TRACK 原行为，与 `gpx` 组件独立。
- `ble_companion` P0 实现 Control/Status 后，App 可同步 REC 状态（Phase 2）。

---

## 4. Phase 1b：码表 GPS → App 记录（可选）

当内置 GNSS（L96）优于手机时，由码表 **Notify `0xFF18` Track Point**（结构见 `companion_track_point`），App 仍负责 **组装 GPX 文件**。

前提：

- GNSS 驱动对 `ble_companion` 可见（独立 `/dev/gnss` 或进程内桥接）。
- App 已 `CTRL_START_RECORD`，设备以 1–5 Hz Notify。

此阶段 **仍不在码表写 GPX 文件**。

---

## 5. 路线文件 vs 骑行记录

| 类型 | 方向 | 通道 | 用途 |
|------|------|------|------|
| **设备侧记录 GPX** | GNSS → `gpx/` | 本地 SD/LFS | `gpx record`、后续常驻服务 |
| **bicycle UI 记录** | 码表 UI | `DP_Recorder` + LVGL FS | X-TRACK 原路径，暂不改动 |
| **导航路线 GPX** | App → 码表 | `FS_WRITE_*` 或 USB MTP | 地图/导航 |

日常骑行 **不应** 依赖 `FS_READ` 拉整段 GPX。

---

## 6. 实施清单

### Phase 1（记录）

- [x] **`gpx/` 组件**：`gpx_queue` + `gpx_writer/reader` + `gpx_record` 批写线程
- [x] `companion_proto.h` 协议常量
- [ ] GNSS → `gpx_record_push`
- [ ] Flutter `gpx_writer` + `ride_session`
- [ ] `ble_companion`：Control + Device Status（P0）
- [ ] （可选 P1b）Track Point Notify + GNSS 桥接

### Phase 2（展示，后续）

- [ ] App WWR 推 `0xFF14` → 码表 `DP_GNSS` 注入
- [ ] `TrackFilter` 与 Recorder 解耦，骑行会话驱动地图轨迹线
- [ ] Dashboard 录制开关改为「App 记录中」只读状态

---

## 7. 相关文档

| 文档 | 内容 |
|------|------|
| [companion_design.md](companion_design.md) | GATT 全量定义、FS、传感器 |
| [gpx/README.md](../../boards/sf32lb52/helmone/domain/gpx/README.md) | GPX 组件 API、队列、批写 |
| [dp_recorder_guide.md](../../boards/sf32lb52/helmone/ui/bicycle/doc/dp_recorder_guide.md) | bicycle `DP_Recorder` 数据流（X-TRACK） |
| [开发计划.md](../开发计划.md) | 阶段 4 存储与记录 |
