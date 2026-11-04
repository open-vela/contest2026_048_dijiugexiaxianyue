# BLE 传感器 Central（`ble_sensor`）

码表对手机是 **GATT Server（Peripheral）**，对外设心率/踏频/功率是 **GATT Client（Central）**。两条链路共享 SF32 LCPU 一条 HCI，但 **地址、MTU、连接参数、GATT 回调必须分开**。混用会拆传感器链路，或把 Companion 文件传输打挂。

协议与手机侧仍以 [companion_impl_plan.md](companion_impl_plan.md) + `companion_proto.h` 为准。本文只写传感器与双角色约束。

**代码**：`boards/sf32lb52/helmone/services/ble_companion/ble_sensor.c`

**NSH**：`test sensor …`（跑在 `ble_companion` 线程，不启 bicycle UI）

**更新**：2026-10-18（定向回连见 [directed_reconnect.md](directed_reconnect.md)；恢复阶梯见 `companion_impl_plan.md` §7.4）

---

## 1. 产品意图

三台独立外设，不是一台设备上的三个服务：

| 槽位 | 广播/主服务 | 测量特征 | 附加 |
|------|-------------|----------|------|
| HR | `0x180D` | `0x2A37` Notify | 电量 `0x180F`、佩戴位置 `0x2A38` |
| CSC | `0x1816` | `0x2A5B` Notify | 电量（若有） |
| CPS | `0x1818` | `0x2A63` Notify | 电量（若有） |

`test sensor connect <idx>` **按扫描表地址连**，不按名字。同一地址只占一个 GATTC 槽（HR → CSC → CPS 先匹配者）。

连接数：`CONFIG_BT_MAX_CONN=4` → 手机 1 + 三个传感器槽。

---

## 2. NSH

```text
test sensor scan [sec]     # 默认 30s observer，然后打 HR/CSC/CPS 表
test sensor scan stop      # 只停扫描，已连的 GATT 保持
test sensor list           # 重打扫描表
test sensor connect <idx>  # 按表行地址连接（不要写 stop，要用 scan stop）
test sensor read           # 重打已连接汇总
test sensor disconnect     # 断开 GATT，不自动重连
test sensor off            # 停扫描并断开
```

扫描时会 `companion_hold_le_adv(true)`（停广播，避免与 observer 抢控制器）。仅限时搜表和 HCI 建链期间 hold；CSC 停车后的重连观察者**不再**占广播，否则手机扫不到码表。`scan stop` / 超时 / 连上后释放。手机已连着时广播本就会停，不影响已有 Companion 连接。

---

## 3. 硬约束（真机踩过）

### 3.1 不要 `discover all`

`bt_gattc_discover_service(NULL)` 会走完整 GATT：GAP、GATT、DIS、媒体、电话等。框架表很小：

| Kconfig | 值 | 含义 |
|---------|----|------|
| `GATT_CLIENT_SERVICE_MAX` | 8 | 每个连接记下的主服务数 |
| `GATT_CLIENT_ELEMENT_MAX` | 40 | 属性总数（含声明/特征/描述符） |
| `GATT_CLIENT_CHAR_PER_SERVICE_MAX` | 16 | 单服务特征数 |

安卓心率模拟器服务很多。表溢出会在应用看到任何 `svc` 回调之前被 SAL **HCI 拆链**。不要改上游 SAL：只按 UUID 发现。板卡 `vela_override/bluetooth/sal_gatt_client_interface.c` 在溢出时保持连接，第二次 UUID 发现 **追加** 属性表（`keep_db`），不冲掉已发现的 BAS/CCC。

先发现 `0x180F` 记下电量 handle，再发现槽位主服务。无线侧一条 ACL 上可以有很多服务，限制在固件表，不在电台。

电量：`0x2A19` 带 Notify 则订 CCC；否则 READY 后每 2 s ATT Read。没有 BAS 则 `bat=NA`。

### 3.2 先发现完，再订 CCC

错误顺序（会停心率通知，约 2s 后 `notify stale`，HCI `reason=0x16` 本机拆链）：

1. 发现 `0x180D` → 订阅心率 → 再发现 `0x180F`（覆盖 SAL 表，心率 CCC 找不到）
2. 订阅后立刻 `read` 位置，再开第二次 discover（静态 `read_params` 与 discover 重叠）

正确顺序：

1. 连上后等 **500 ms**（避开 LE Read Remote Features / 自动 PHY）
2. 发现 `0x180F`，缓存 `0x2A19` handle（没有 BAS 就跳过）
3. 发现 `0x180D`（或 CSC/CPS）；SAL keep_db 使 BAS 仍在表里
4. 订测量 CCC；若电量带 Notify 再订 `0x2A19` CCC，最后读位置
5. `SENSOR_READY`：Notify 电量靠 CCC；仅 Read 的电量每 2 s `bt_gattc_read(bat_h)`；缺电量则 `bat=NA`

### 3.3 Central 不要压连接参数

手机（我们是 Peripheral）仍可请求 7.5 ms + 2M PHY + DLE。  
传感器（我们是 Central）必须用 `BT_LE_CONN_PARAM_DEFAULT`（约 30–50 ms、监督 4 s）。

对传感器强制 7.5 ms / 1 s 监督 + PHY + DLE 会与安卓 45 ms 更新撞车，HCI 连接更新 `status=0x16`，发现未完成就断。

关机 / 丢链：

- **HR**：8 s 无测量 notify（`SENSOR_NOTIFY_STALE_MS`）视为关机，本机 HCI `0x16` 拆链再重连。不要靠 1 s HCI 监督超时。
- **CSC / CPS**：真踏频器停车常 **停发** 测量 Notify。同样 8 s 只标 `measurement idle`，保持 GATT 并用 ATT 探活，**不因停车主动开 observer 或拆链**。若 HCI/GATTC 已确认断链，才进入低占空比重连扫描；距离导致的 `0x08` 走同一路径。

### 3.4 Companion 文件传输 vs 传感器 MTU

zblue 的 `att_mtu_updated` 是 **全局** 的：心率连上默认 ATT 23 也会进 **GATTS** 回调。原先无条件 `companion_fs_set_mtu(20)`，文件通道按 20 字节分包，传输失败或极慢。日志特征：连 HR 时出现 `ble_companion: MTU -> 20 (payload 17)`。

处理：

1. `companion_is_phone()` / `ble_sensor_owns_addr()`：传感器地址上的 connect / disconnect / MTU / notify-complete / 特征写 / CCC **不得**改 `g_ctx.peer`、不得 `companion_fs_on_disconnect()`、不得改 FS MTU。
2. `companion_fs_set_mtu(addr, mtu)` **必须**带对端地址，且等于当前 FS peer、且已 `companion_fs_on_connect`。新 GATTS 回调若漏判 `companion_is_phone`，这一层仍挡住传感器 ATT 23。
3. **手机 RPA ↔ 身份地址（2026-09-21）**：连接回调记 RPA、加密后 ATT/CCC 用身份地址时，
   `companion_is_phone()` 会刷新 peer 并同步 FS（否则 `fs ccc/cmd DROPPED (not phone)`，
   App「0 帧超时」）。权威说明见 [pairing_and_radio.md](pairing_and_radio.md) §1.3。

### 3.5 同一台 BLE-HR 不要再开调试助手

心率模拟器通常只允许一个 Central。蓝牙调试助手连着 `7B:1A:…` 时，码表会收不到 notify，随后 stale 重连。测码表时先在助手上断开。

### 3.6 SF32 扫描 / 广播走框架 API，SAL 在板卡抽换

`CONFIG_BT_EXT_ADV_LEGACY_SUPPORT=y`，LE features 由 `sf32lb52_bth4.c` stub（无 Ext Adv 位），不要改 zblue `hci_core.h`。手机链路存在时启动传感器观察者仍可能遇到 `LE Set Random Address = -EACCES`，因此保留 zblue `id.c` 的 identity 回退，见 [required_patches.md](../required_patches.md)、[pitch/ble.md](../pitch/ble.md)。

应用层只用框架 API，不直接调 zblue GAP：

| 路径 | 框架 API | 板卡 SAL override |
|------|----------|-------------------|
| 手机广播 | `bt_le_start_advertising`（窗口内/回退：`ADV_IND`；已绑定有新鲜 RPA：`ADV_DIRECT_IND`，own=PUBLIC、TargetA=当前 RPA） | `vela_override/bluetooth/sal_le_advertise_interface.c`：legacy `bt_le_adv_start`；定向目标/RPA 见 [directed_reconnect.md](directed_reconnect.md) |
| 搜表观察者 | `bt_le_start_scan_settings`（`LOW_LATENCY`、legacy、被动、1M） | `vela_override/bluetooth/sal_le_scan_interface.c`：`options=0`，`-EALREADY` 先 stop 再 start |
| 重连观察者 | 同上，但 `BALANCED`（已有 ≥1 路 READY 则 `LOW_POWER`） | 等到**已保存地址**再广播（不是 HCI 定向扫描）。HCI 建链飞行中停观察者 |

手机未连接且 sensor 待重连时不再长期同时打开广播和 observer，而由 companion 线程做互斥时间片：

1. 手机掉线后先 **15 s phone-first 只广播**（`SENSOR_PHONE_FIRST_MS`）；
2. 之后稳态 **4 s 广播** → 请求 hold 并先停广播 → **0.8 s 短扫**；
3. 先停 observer，再 release hold 恢复广播。

主循环先消费 hold/release，再启动下一角色；板级 scan / advertise / GATTC connect SAL 共用 ticket 队列，把 libuv worker 中的实际 HCI 操作串行化，保持 `stop scan/adv → start adv/scan/initiate` 顺序。日志为 `[radio] phone-first ADV 15000 ms`、`[radio] stop phone ADV for short scan`、`[radio] scan window 800 ms`、`[radio] resume steady ADV 4000 ms`、`[radio] resume steady quiet 4000 ms`。手机已连接时无需广播，但也不是连续扫：4 s 静默 + 0.8 s 短扫；已经建立的手机和 sensor GATT 链路可同时保持，不参与时间片。

`id.c` 与时间片解决的是两个不同问题：前者保证已有手机链路时 observer 不因地址更新失败而起不来；后者保证手机断开后，重连 observer 不会一直占住单射频，手机每个周期都有可发现窗口。

满载射频（手机 + 三传感器 + 重连 observer）时，HCI `0x3e`（Connection Failed to be Established）和 `0x08`（监督超时）仍可能出现。缓解：同时只飞一条 `bt_gattc_connect`；建链期间停 observer；已有 READY 链路时重连冷却 1500 ms。

HCI 广播报告在 `sf32lb52_bth4.c` 静音，由 `ble_sensor` 每 2 s 汇总。健康日志：`advertising started id=1 status=0`。

断链看 `sf32lb52 bth4 disconn … reason=0x??`：

| reason | 含义 |
|--------|------|
| `0x13` | 对端断开 |
| `0x16` | **本机**断开（HR stale、溢出拆链、应用 `disconnect`） |
| `0x08` | 监督超时 |
| `0x3e` | Connection Failed to be Established（建链与扫描/广播抢射频） |

### 3.7 其它

- GATT 回调：拷贝 + 标志；connect / discover / subscribe / 扫描启停只在 companion 线程。
- 不要 `statfs()`（会饿死 `BT LW WQ`）。
- 蓝牙 enable 之后不要写 GAP Device Name。
- 改 `defconfig` 后必须 `cmake --build cmake_out/helmone_nsh --target resetconfig`。
- 安卓模拟器常用 RPA，重启 MAC 会变；真带子多为稳定 public/static random。未做 IRK 绑定（有 SMP/PRIVACY，无 `BT_SETTINGS`）。
- 意外掉线：保留地址，低占空比 observer 等到 **该 MAC** 再广播再连；冷却 800 ms（已有其它 READY 链路或建链失败 0x3e 时 1500 ms）。HCI 建链/发现未完成时不并飞第二条。`test sensor scan` 只禁止连**新**设备，已 `connect <idx>` 绑定的槽位仍会重连。`connect <idx>` 不得清掉其它已绑定槽位。

### 3.8 健康监控（假「已连接」）

手机 App 与传感器同时连时，HCI `0x13`/`0x08` 可能到了 zblue 却进不了 GATTC 回调。槽位留在 `SENSOR_READY`，菜单显示「已连接」，外设认为自己空闲、继续可连接广播。

`sensor_health_pump`（companion 线程）：

1. **HCI 备份**：zblue `disconnected`（central）→ `ble_sensor_on_hci_disconnected`（只置标志）。
2. **禁止假 READY**：订阅失败/超时拆链，不得 `slot_finish_ready`。`notify_ms` 只在真正的测量 Notify 上刷新。
3. **僵死 ACL**：已连接却再次看到该地址的可连接 ADV（且 ATT 已判定不活）才拆。CSC 停车**不开**重连观察者；只 ATT 探活。观察者仅给 IDLE 槽位等广播。看到 ADV **不得**清掉 0x3e/双连接冷却，否则会立刻再建链失败。
4. **HR Notify 过期**：先 ATT 探活，失败再本机 0x16。不要一沉默就拆，否则观察者一抢射频就会误杀心率。
4. **断链看门狗**：`SENSOR_DISCONNECTING` 超过 4 s 无回调 → 重建 GATTC。
5. UI：一拆链就离开「已连接」（`DISCONNECTING` + `want_link` 显示「连接中」）。

日志：`ble_sensor: health HR ready ntf=…` 以及 `health stats zombie=… hci=…`。

### 3.9 一个地址只占一个槽位（双模传感器 / 踏频器被当成功率计）

便宜的双模传感器（部分踏频器同理）会在 ADV 里**同时**广播 `0x1816`(CSC) 与
`0x1818`(CPS)。`walk_ad()` 是按 UUID 逐条置 `SENSOR_FEAT_*` 的，于是
`svc_mask` 两位都有，一台设备天然同时出现在「踏频器」和「功率计」两个列表里
—— 这是对端 ADV 的事实，不是解析串了（UUID → kind 的映射没有任何交叉）。

会出问题的是**绑定**。现在的规则是 `ble_sensor_connect_index_kind()` 里那条：
**一个地址只开一条 GATTC**，该地址已被别的槽位占用（`want_link` 或
`state >= SENSOR_CONNECTING`）就拒绝，菜单侧（`helm_scan_connect_idx()`）用
同一判据先弹「已被踏频占用」，否则用户会白等一场「正在连接」。

#### 3.9.1 扫描回调**不挑设备**（2026-09-17 改正）

`on_scan_result()` 现在只做两件事：`found_upsert()` 维护 UI 的发现列表、刷新
**已武装**槽位的 `rssi`/`adv_seen`。**它不再替任何槽位挑设备。**

这里原先有一段「保留每个 kind 里信号最强的广播者」的候选绑定，是**两个现场
故障的共同根**：

1. **删了还回连**。它给 `slot->addr` 填上一台用户从没配对过的设备，随后
   `slot_try_connect()` 借 `g_allow_connect` 把 `want_link` 一置，这台设备就
   成了「回连目标」。而 `g_allow_connect` 在 sensor 策略开着时开机即为真
   （`companion_ready()` → `companion_apply_sensor_policy()` →
   `companion_sensor_autorc_all()` → `ble_sensor_set_connect(true)`）。于是
   **回连依据根本不是 KV 里的记录**：菜单里删掉记录后，ADV 还在，下次开机
   又被挑中 —— 表现就是"删了，重启后它自己又连上了"。
2. **功率计连不上**。一台 CSC+CPS 的表会被 CSC 槽位先挑走（取第一种匹配，
   HR > CSC > CPS），功率计页再点它就只能被上面那条「一个地址一条 GATTC」
   挡掉。

现在的规则只有一条：**只有配对过的地址才回连**。

- 武装：`companion_sensor_autorc_all()` 按 `/mnt/kv/bicycle_sensors.tsv` 里
  `autorc` 的记录调 `ble_sensor_connect_addr()`；记录删了就不再武装。
- 点选：`ble_sensor_connect_index_kind()`（带页面 kind）。
- 两者都自己置 `want_link`，再由扫描回调的 want_link 循环把 `adv_seen` 打开
  → `slot_try_connect()` 才建链。
- **删除还必须清地址**：`slot_drop_want()` 除了清 `want_link`，对
  `state < SENSOR_CONNECTING` 的槽位连 `addr`/`name` 一起清。否则
  `slot_try_connect()` 里 `if (g_allow_connect) slot->want_link = true;`
  会在下一次 pump 把 `want_link` 拉回来，等于没删。正在连/已连的槽位不动
  地址，交给 `slot_handle_disconnected()` 的 `!keep` 分支收尾。

排查这类问题看 ADV 调试日志：`test sensor adv`（或 `ctl` 里的对应项）打开后
`adv_debug_print()` 会打 `match: CSC+CPS`，一眼能看出对端到底广播了几个服务。
另外 `ui.found[].linked` 一旦为真（任一槽位持有该地址且 `CONNECTING..READY`），
**各 kind 的列表都会隐藏它** —— 所以按踏频器连上之后，它不该再出现在功率计
的扫描列表里。

**点选按页面类型绑定。** 菜单是分类型页的（`s_scan_kind`），列表也按该类型过滤，
所以「点哪页就该连哪页」：`CONNECT` 命令现在带上页面 kind
（`myvendor_devctl_sensor_connect_kind()` → bridge 的一次性 `sensor_connect_kind`
→ `ble_sensor_connect_index_kind()`）。同一个入口以前一律「取第一种匹配」，
于是 HR+CSC 的组合带子在「踏频器」页被点中时绑的却是**心率**槽位。

- 对端 ADV 里没有该服务 → 直接 `LOGE` 拒绝，不偷偷换一个类型连。
  列表本来就按 `kind_mask`（= `found->svc_mask`）过滤，两者口径一致。
- 该地址已被别的类型占着（`want_link` 或已连）→ 拒绝，**保持一地址一槽位**。
  组合带子一次只能当一个角色用：两条 GATTC 连同一对端，回调归属与健康监控
  （该拆谁）都会互相打架。**要换角色得先删掉原来那条记录**，否则会一直被
  「已被…占用」挡着。
- 不带 kind（`COMPANION_SENSOR_KIND_AUTO`）＝旧的「取第一种匹配」，NSH
  `test sensor connect` 没有页面上下文，仍走这条。

### 3.10 保存与开机回连（「连过但开机不回连」怎么查）

**绑定是什么时候落盘的**：`slot_finish_ready()`（发现 + 订阅都成功，即菜单上的
「已连接」）才调 `myvendor_devctl_sensor_remember()`。它把记录写进内存表并
**同类型只留一条 `autorc`**（`remember()` 里对同 kind 的其它记录清 autorc），
然后落盘到 `/mnt/kv/bicycle_sensors.tsv`（临时文件 + `rename`，KV_REGION 在
SD 上，bringup 里挂载）。

开机时 `companion_sensor_autorc_all()` 读回记录，对 `autorc` 的记录调
`ble_sensor_connect_addr()`（按**保存的 MAC**，不按名字）→ 进
`SENSOR_RADIO_RECONNECT` 低占空比观察者，等到该地址再广播才连。

**查三次，一次定位**（NSH）：

```text
sys hr            # 或 sys；看末尾 sys rec 段
```

- `sys rec  /mnt/kv/bicycle_sensors.tsv  missing errno=…` → **没保存成功**
  （文件读不出来）。errno 是 2(ENOENT)= 从没写过；其它值 = FS 层问题。
  连接那次如果保存失败，日志里会有
  `ble_sensor: CSC record save failed N (will not autorc)`（ERROR 级，默认可见）。
- `sys rec` 有记录但 `autorc 0` → 记录在，但没被选为回连目标（菜单里那一行没有 ★）。
- `sys rec` 有记录、`autorc 1`，但重启后 `sys slot 1 idle` 且不再变 `conn` →
  记录没问题，是**回连没等到对端**。这时对比 `sys rec` 的地址和 `sys scan` 里
  `sys found` 的地址：**同一个设备地址不一样 = 对端用的是可解析私有地址
  （RPA），MAC 会轮换**，按保存地址回连永远等不到（`docs/ble/ble_sensor.md`
  §3.7 那条「未做 IRK 绑定」的已知限制）。菜单侧表现为扫描列表里能看到它，
  但它不显示「回连」。

`sys slot` / `sys found` 是**本次开机**的链路态，`sys rec` 是**持久化绑定表**；
判断「保存下来了没有」只看 `sys rec`。

菜单侧同一件事：`已保存设备` 行带 ★ 且值为「回连」= 该记录就是开机回连目标
（`helm_fill_scan_recs()`；★ 由 `recs[i].autorc` 决定）。

---

### 3.11 对端用轮换 RPA 时，「按地址回连」不可能成立（2026-09-17 归档）

**现象**：手机模拟的踏频/功率计**每次连接、每次复位后地址都不一样**
（`47:08:D7:18:AD:F4` → `7B:EB:BA:12:F7:BD` → `6D:A6:C3:AF:79:9C` → `57:E0:E1:EC:89:5A`），
`autorc` 永远匹配不上；而**手机 App 里显示的 MAC 一直没变**。

**一眼判据：看地址首字节的高两位**

| 高两位 | 类型 | 会变吗 |
| --- | --- | --- |
| `11` | 静态随机 / public | **不变** —— 真实设备基本都在这一档（小米手环 `DF:…`，真实功率计/踏频计） |
| `01` | 可解析私有地址（RPA） | **按规范轮换**（本板 `CONFIG_BT_RPA_TIMEOUT=900`），只有拿到对端 IRK 才能解析回身份地址 |
| `00` | 不可解析私有地址 | 轮换，且**拿到 IRK 也救不了** |
| `10` | 保留位 | 不是合法随机地址 —— App 界面显示的多半是这个（身份/public 地址） |

**机制**：Android 的 `BluetoothLeAdvertiser` **固定使用随机地址，App 指定不了** ——
App 里填的 MAC 只显示在它自己的界面上，**上不了空**。所以「手机显示不变、MCU 每次变」
不是我们的 bug，是平台行为。真实功率计用自己的 BLE 芯片，出厂就设静态/public 地址，
所以「存地址、按地址回连」对真实设备天然成立。

**我们这边是干净的（三段证据，逐条排除）**：

- **落盘/读取**：`srec_mac_fmt`（按 `addr[5]..addr[0]` 打印）与 `srec_mac_parse`
  （`sscanf` 回填 `v[5]..v[0]`）严格互逆；
- **匹配**：`addr_eq()` 只比 6 字节、不比 type，所以 `type` 字段不是干扰项；
- **一致性**：`sys link csc` 的 `rec *` 与 `slot` 值**永远同步**（每次连接后 `sys rec 1`
  都忠实地跟着 `sys slot 1` 变）—— 变的是**源**，不是编解码。

**曾经走错的两条路（别再走）**：

1. 「保存的 MAC 存错了」—— 编解码互逆，排除；
2. 「配对密钥没落盘（`CONFIG_SETTINGS` 没开）」—— 听着很像，但**我们从来没和它配对过**：
   当天 5 个观察值全是 RPA、连接事件 `atype=1`，**从未出现身份地址** `94:7B…`。
   而且平台的 bond 存储**早就编进来了**：`BLUETOOTH_STORAGE_PROPERTY_SUPPORT=y`、
   `KVDB=y`、`BLUETOOTH_STORAGE_UPDATE=y` —— 不是缺机制，是**没有密钥可存**。

**结论与决定**：

- **保持「地址 = 唯一标识」的模型**（对真实设备正确，不做改动）；
- **不为这台模拟器改产品逻辑** ——「按服务自动连最强广播者」会把踏频/功率计混在一起
  （两台都归成 `kinds 0x2`，名字又都是 `-`，没有第二区分依据）；
- **改用 ESP32 做模拟器**：把地址设成**静态随机/public**（高两位 `11`），现有
  `autorc` 逻辑立刻正确工作，一行固件都不用改。

**如果哪天产品上真要支持隐私型外设**，唯一的路是**由我们主动发起配对**：
连接后调 `bt_conn_set_security(conn, BT_SECURITY_L2)`，手机侧弹一次系统配对框，
拿到 IRK 后控制器会把 RPA 解析成身份地址（连接事件里 `atype` 变成 0、地址变成
`94:7b…`），平台自带的 bond 存储负责落盘，复位后仍能解析。代价是手机上要点一次 +
bond 管理。**注意**：zblue 的 `LOG_*` 在本 build 里是被整个编译掉的（见
[gnss_ble_diag_log.md §2.8](../gnss_ble_diag_log.md)），SMP 事件**不会**打印任何日志，
必须自己加 `syslog` 才看得见。

**现场陷阱**：拿**已经轮换掉的旧 RPA** 去连必然失败 ——

```
sf32lb52 bth4 conn st=02 hdl=0x0000 role=0 atype=0 peer=00:00:00:00:00:00 itv=0 lat=0 to=0(0 ms)
ble_sensor: CSC HCI disc reason=0x00
ble_sensor: CSC connect failed 57:E0:E1:EC:89:5A, scanning
```

`st=02` 且 `peer=` 全 0 就是这一类（控制器连不上那个地址）。所以扫描到之后要**尽快连**；
对 RPA 对端，隔十几秒再点就可能已经失效。

### 3.12 射频时隙：要等别人「确认」的判据必须自己带时限（2026-09-18）

手机未连接、又有已保存的传感器时，`ble_sensor` 的时间片状态机给两者轮流分配射频：

```
IDLE ──► PHONE_FIRST_ADV(15000 ms) ──► STEADY_ADV(4000 ms) ──► ADV_STOP_WAIT ──► SHORT_SCAN(800 ms)
                    ▲                                                              │
                    └──────────────────────────────────────────────────────────────┘
```

`ADV_STOP_WAIT` 原来要等 companion 把「停广播请求」消费掉、并通过
`ble_sensor_set_phone_state(connected, adv_held)` 回执（`g_phone_adv_held`）才起扫。
**这个回执来自 `g_hold_le_adv`，而 `g_hold_le_adv` 在 drain 里被派生量 `transient` 覆盖；
`transient` 的定义却是「正在扫描 / 正在建链」** —— 于是形成环：

> 要起扫 ⇒ 先要 `held` ⇒ `held` 要 `transient` ⇒ `transient` 要「已经在扫描」

实机表现（2026-09-18，build 202609181700）：状态机停在 `ADV_STOP_WAIT`，**射频全空**
（不广播、不扫描、HCI 一条命令都不发），14 s 后被 diag 的 `cmdsilent` 拆适配器，
22.6 s 一轮的死循环，手机永远扫不到设备、传感器也永远扫不到。同日 `cmdsilent` 还有第二个
误报源：它按「最后一条命令的时间戳」判控制器哑，而「发送成功」是 `wait_tx_idle()`
`usleep(1000)` 轮询确认 LCPU 取走字节后才盖章，**控制器的 Command Complete 常比这个盖章早
几毫秒**，于是一条已被回答的命令被判成「发了没人应」。

现在三条约束（改这块时请一并守住）：

| 约束 | 落地 |
|------|------|
| 「**要**射频」与「**正在用**射频」分开 | `g_hold_req_latch`（显式 request/release 边沿闩锁）与 `transient` 取并，drain 不再覆盖闩锁 |
| 等别人回执必须**有界** | `SENSOR_ADV_STOP_ACK_MS = 1500 ms`，超时照样起扫（那时广播的 HCI stop 已经回完，射频本来就空） |
| 「控制器哑」按**命令配对**判，不按时间戳 | `sf32lb52_bt_adapter.c` 的 `g_hci_pend_op`：发出时置位、收到同 opcode 的 Command Complete/Status 清零；空闲不算哑 |

修好后实测：`stop phone ADV` → `reconnect scan window 1000 ms (held=1)` 间隔 **30 ms**，
每轮 3 s（2 s 广播 + 1 s 扫描）无空档，`adapter cycle`/`cmdsilent` 均为 0，手机在开机
1.5 s 内连上。

排查入口（都已提到 INFO，默认可见）：

```
ble_companion: hold le adv on|off                 # 让位闩锁的边沿
ble_sensor:    [radio] stop phone ADV …           # 进入 ADV_STOP_WAIT
ble_sensor:    [radio] reconnect scan window … (held=?)   # held=0 说明是超时兜底
ble_sensor:    [radio] tick phase=? held=? want=? scanner=? want_scan=? duty=?
hci tx op=0x200a p=01|00 …                        # 广播开关；0x200b/0x200c = 扫描参数/开扫描
sf32lb52 bt stall … pendop=0x???? pendage=?       # pendop 才是"发了没人应"的判据
sal adv|scan ticket n=? serving=?                 # 跨角色 FIFO 票据（>2 s 会强制推进并报错）
```

`hci tx` / `hci rx` 是逐条命令级追踪，只在 bring-up 期间需要；要静音就把
`sf32lb52_bt_adapter.c` 里那两处 `syslog(LOG_INFO, "hci tx rx` 注释掉。

---

## 4. 日志

缺的字段打 **`NA`**，不省略：

```text
ble_sensor: HR 71 bpm loc=foot bat=99% contact=yes energy=1385 kJ
ble_sensor: HR 71 bpm loc=foot bat=NA contact=yes energy=NA
```

健康连上顺序：

```text
ble_sensor: HR connected, discover BAS then 0x180d
ble_sensor: HR svc 0x180f ...
ble_sensor: HR bat handle 0x.... (notify)
ble_sensor: HR discover 0x180d
ble_sensor: HR svc 0x180d ...
ble_sensor: HR notify on handle ...
ble_sensor: HR ready feat=HR+BAT
ble_sensor: HR 70 bpm loc=... bat=... contact=... energy=...
```

没有 BAS 则跳过电量，`bat=NA`，心率 notify 仍应工作。

连传感器时 **不应**再出现 `ble_companion: MTU -> 20`（那是手机链路的 MTU 日志）。

假连接恢复：

```text
ble_companion: le disc role=central reason=0x08
ble_sensor: HR HCI disc reason=0x08
ble_sensor: HR disconnected AA:BB:…, scanning to reconnect
```

或（HCI 也丢了，靠广播）：

```text
ble_sensor: HR advertised while linked (zombie), reconnect
ble_sensor: HR disconnecting
```

---

## 4.1 绑定记录被删 = 「外设连不上了」（2026-09-20 现场）

设备**只回连绑定过的地址**（`/mnt/kv/bicycle_sensors.tsv` 里 `autorc=1` 的行）。
现场的「心率计突然连不上」就是这条记录**被删了** —— 文件里只剩 CSC/CPS，表本身没事
（工作站扫描还能看到那台表）。删记录只有一条路：

> **设备 UI 上 KEY2 长按那行「已保存的设备」→ 弹确认 → 单击勾号**
> （`helm_sensor_do_del()`；全树唯一调用 `myvendor_devctl_sensor_delete()` 的地方）

翻菜单、试长按（手机那一行是「长按解绑」，手感很像）时很容易误删。三种恢复方式：

| 方式 | 操作 |
|---|---|
| 菜单 | 心率带/踏频器/功率计页 → 扫描 → 选设备（画面上的这一行） |
| **串口 / monitor** | `ctl sensor bind <0HR\|1CSC\|2CPS> <mac> [addr_type]`（2026-09-20 为此加的） |
| app | 无（app 侧不做传感器管理） |

`ctl sensor bind` 的两个坑：

- **字节序**：命令行给的是显示序（`AA:BB:…`），而框架 / `ui.found[].addr` 是
  **最低位在前** ⇒ 内部翻一次，否则日志里会打出反的地址（`AC:7B:F3:04:F6:DF`）且永远连不上；
- 记录**由「连接成功」那条路写回**（autorc=1）：`bind` 之后等几秒，看到
  `ble_sensor: HR connected <ADDR>` 再 `cat /mnt/kv/bicycle_sensors.tsv` 确认新行。

顺带：`test sensor scan/connect` 那套是**独立外设探针**，从 NSH 调过一次后板子随后重启过
（无 coredump），远端恢复请用 `ctl sensor bind`。

## 4.2 空轮回连退避（2026-09-20）

绑定过但不在附近的外设会让射频空转。现在一轮 **0.8 s 短扫**没连上任何东西，就把
下一轮之前的静默窗按 **+0/+1/+2/+4 s** 线性增加（封顶 **4 s**）；连接成功 / 用户点选 /
`ctl sensor bind` / 恢复流程都清零。日志判据：

```text
ble_sensor: [radio] resume steady ADV 4000 ms (+4000 idle, empty=3)
ble_sensor: [radio] resume steady quiet 4000 ms
```

完整规则（含射频相位机与广播三档）见 [pairing_and_radio.md](pairing_and_radio.md) §3。

---

## 5. 相关文件

| 路径 | 角色 |
|------|------|
| `ble_companion/ble_sensor.c` | observer、扫描表、GATTC、重连 |
| `ble_companion/ble_companion.c` | `0xFF10`；Central 不改 conn param；GATTS 忽略传感器地址 |
| `ble_companion/ble_sensor.h` | `ble_sensor_owns_addr()` |
| `test/test_companion.c` | NSH `test sensor` |
| `ble_companion/ble_supervisor.c` | ADV/SCAN/GATT/HCI stall → adapter cycle → LCPU 复位（5 分钟限频，请求可保留） |
| `chips/sf32lb52/sf32lb52_bth4.c` | 静音 ADV 报告；打印 HCI 断链 reason |
| `configs/nsh/defconfig` | `BT_MAX_CONN=6`、GATTC 表尺寸、`LEGACY_SUPPORT` |
