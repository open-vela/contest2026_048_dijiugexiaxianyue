# BLE 配对模型与射频节奏（准入 / 广播三档 / 退避 / 并发纪律）

本文是 **2026-09-20** 那轮改动（配对准入、广播策略、射频退避、跨线程纪律）的权威说明，
**2026-10-18** 起窗口外已绑定手机优先走定向广播（细节见 [directed_reconnect.md](directed_reconnect.md)）。
配套代码：

| 内容 | 代码 |
|---|---|
| 准入、手机记录、配对窗口、`ctl pair …` | `boards/sf32lb52/helmone/services/ble_companion/myvendor_bt_pair.c` |
| 广播三档、GATT 回调、`security_changed` 钩子 | `boards/sf32lb52/helmone/services/ble_companion/ble_companion.c` |
| 传感器 Central、射频时隙状态机、空轮退避 | `boards/sf32lb52/helmone/services/ble_companion/ble_sensor.c` |
| 设备 UI 的「手机蓝牙」子页 | `boards/sf32lb52/helmone/ui/bicycle/src/lvgl_page/helm_menu.c` |
| App 侧（Flutter，仓库在 `~/Desktop_My/work/my_work/sifli/02_sw/flutter`） | `lib/ble/companion_client.dart`、`lib/ui/device/device_page.dart` |

背景与协议见 [companion_impl_plan.md](companion_impl_plan.md)；
传感器 Central 见 [ble_sensor.md](ble_sensor.md)。

---

## 1. 配对模型：没配对就不接受连接

改动前的问题（现场实测）：广播无定向可连接、特征权限全是裸的 `GATT_PERM_READ/WRITE`，
于是**手机上 app 已关、配对已删，仍有一条 `sec=1 bonded=0` 的连接在读写**控制 / 导航 /
FS 通道。现在有三层：

| 层 | 规则 | 代码 |
|---|---|---|
| ① 连接准入 | **窗口内**放行任何人；**窗口外**仅放行记录里的手机（身份地址）。有记录但地址对不上（RPA 回连）给宽限，**未加密则踢**；没记录直接拒。新配对由 `pair_confirm` / `bondable_le` 在窗口外拒绝 | `myvendor_pair_admit()` / `pair_confirm` |
| ② 权限兜底 | 所有特征 / CCCD 都是 `GATT_PERM_READ\|WRITE + ENCRYPT_REQUIRED` | `ble_companion.c` 的 attr table |
| ③ 广播 | 窗口内 = 快广播 + 本地名 + bondable；有记录窗口外 = 定向 `ADV_DIRECT_IND`（有新鲜 RPA 时）或无定向无名 `ADV_IND` 回退，均非 bondable；没记录 = 不广播 | `companion_start_advertising()` / `companion_sync_bondable()` |

**IRK / 绑定只服务手机这条线**（用户 2026-09-20 明确）：传感器（HR/CSC/CPS）一律
**按地址连** —— 它们不是我们配对的设备，没有密钥、也没有身份地址可解析。所以所有
「加密 / 绑定 / 补记录」的判断都要用 `ble_sensor_owns_addr()` 把传感器链路排除掉。

### 1.1 「已绑定手机」记录

文件：`/mnt/kv/bt_phone.tsv`，最多 **3 行** `ADDR<TAB>TYPE`（`TYPE` 是 HCI 地址类型，0 public / 1 random）；满时 FIFO 挤掉最老一条。新记录延迟 **2 s** 落盘（`PAIR_SETTLE_MS`），开机按密钥池修剪陈旧 RPA。它是**主机准入允许名单**，不是 zblue 绑定表；绑定密钥在 `/mnt/kv/bt_keys.bin`，见 §5。

| 事件 | 谁做 | 代码 |
|---|---|---|
| 写 / 更新 | `security_changed` 事件对账（**主力**）、控制帧路径、bond 回调 | `myvendor_pair_peer_poll()` / `myvendor_pair_note_peer_if_secure()` / `myvendor_pair_note_bond()` |
| 删 | 解绑（`bt_unpair` + 删文件 + 关窗） | `myvendor_pair_unbind_poll()` |

要点：

- **写盘前先比对**（地址 + 类型一致就直接返回）：`/mnt/kv` 是 flash，早期版本每次控制帧、
  每次对账都重写一次，现场「手机一连上就 `pair: phone record …` 刷屏」，纯耗寿命。
- 「手机连上只读不写」也必须能落记录 —— 这就是 `security_changed` 事件存在的原因，
  见 §4.1。
- 手机侧「忽略设备」**不会**通知设备，所以记录可能过期：这时靠配对窗口重新配
  （设备 UI「配对新手机」，或 `ctl pair open [秒]`）。

### 1.2 配对窗口

| 项 | 值 / 行为 |
|---|---|
| 打开方式 | 设备 UI：进「蓝牙设备 → 手机蓝牙」→ 点「配对新手机」→ 弹窗上单击勾号确认；或 `ctl pair open [秒]`；或 app 的控制帧 |
| 长度 | 默认 60 s（`ctl pair open` 省略秒数时；app 走 30 s） |
| 开机自动开窗 | **不允许**（用户 2026-09-20）：只有 UI / NSH / app 显式操作才开 |
| 关闭 | 配对成功、解绑都会立刻关窗（剩下的秒数不该继续允许「再配一次」） |
| UI 呈现 | 三态：未配对没开窗 =「配对新手机 / 按勾号开始配对」；窗口内 =「配对中」+ `Ns` 倒计时；已配对 = 地址 + 已连接/未连接 |

窗口状态是跨线程共享资源（UI 读倒计时、ctl 开窗、companion 取重启请求、广播决策读
「在不在窗口内」），统一由 `g_window_lock` 保护。

### 1.3 RPA ↔ 身份地址与 FS「0 帧超时」（2026-09-21）

**现象（App）**：`CCCD=01`、照常发 `0xFF15`，一直「0 帧超时」。  
**现象（MCU）**：`fs ccc DROPPED (not phone)` / `fs cmd DROPPED (not phone)` ——
命令在设备侧被直接丢掉，不是 App 没发。

**根因**：回连时 `gatts_connect_callback` 记下的是手机 **RPA**（例
`52:05:28:3E:8A:2F`），加密后 ATT/CCC 回调换成 **身份地址**（例
`78:D8:40:4B:F5:20`）。`companion_is_phone()` 原先严格 `memcmp(g_ctx.peer)`，
比对失败就把 FS CCC/命令当「非手机」丢弃。

**处置**（`ble_companion.c` · `companion_is_phone()`）：

1. 传感器地址（`ble_sensor_owns_addr`）仍一律返回 false。
2. 已有手机链路且对端非传感器、地址不一致 ⇒ **单手机槽下认作同一台**，刷新
   `g_ctx.peer`，并 `companion_fs_on_connect()` 同步 FS peer（否则 notify 仍打旧地址）。
3. 日志：`ble_companion: phone peer refresh <old> -> <new> (RPA/identity)`。
4. 仍 DROPPED 时日志带 `addr=` / `peer=` / `sensor=`，便于和连接地址对拍。

**验收**：回连后要么出现 `peer refresh`，要么直接 `fs ccc … -> 1` / `fs cmd in`；
App 不应再 0 帧超时。细节注释见函数上的 Doxygen（`@note` / `@warning`）。

### 1.4 ATT 写 `0100` 但 `fs ccc … -> 0`（SAL CCC 下标，2026-09-21）

**现象（MCU）**：驱动 `gatt: ccc write h=0x001c raw=0100`，companion 却
`ble_companion: fs ccc h=0x0009 raw=0000 -> 0`（`h=0x0009` 是应用侧
`COMP_FS_DATA_CCC_ID`，与 ATT handle `0x001c` 不同属正常）。  
**现象（App）**：`CCCD=01`、双写 CCC 仍「0 帧超时」——**单靠 App 改不了**。

**根因**：上游 `bt_sal_on_ccc_written` 用 `ccc->cfg[bt_conn_index(conn)].value`
上报。CCC `cfg[]` 是**按对端地址找的池子**，不是连接下标；错槽常为 0，
GATTS 就把 Notify=1 报成关。

**处置**：板级抽换 `vela_override/bluetooth/sal_gatt_server_interface.c` ——
写成功后优先用 `buf` 的 LE16，否则按 `bt_conn_get_dst` 扫 `cfg[]`。

**验收**：刷机后（会短暂复位）同一次写应出现 `fs ccc … raw=0100 -> 1`，
随后有 `fs` 数据帧；镜像里应有
`strings -a nuttx.bin | grep sal_gatt_server_interface`。

---

## 2. 广播分档（定向优先，失败回退无定向）

| 档 | 条件 | 类型 | 间隔 | 本地名 | 日志 |
|---|---|---|---|---|---|
| ① 窗口内 | `myvendor_pair_window_active()` | `ADV_IND` + bondable | 200 ms | 有 | `adv discoverable (pair window; 200 ms)` |
| ② 已绑定 + 窗口外 | `myvendor_pair_has_phone_record()` | 有新鲜 RPA 且不在冷却：`ADV_DIRECT_IND`（own=PUBLIC，TargetA=当前 RPA）；否则无名 `ADV_IND` | 200 ms | 无 | `adv directed (paired; cached RPA …)` / `adv connectable unnamed (paired; directed cooldown|no fresh RPA; 200 ms)` |
| ③ 都没有 | — | **不广播** | — | — | `adv off (unpaired, no window; …)` |

**定向回连的完整规则**（RPA 缓存 10 min、定向窗口 60 s、失败冷却 10 min、
A/V1/V2/V3 实机对照、zblue pending conn 修复、`advtest` 测试入口）见
[directed_reconnect.md](directed_reconnect.md)。一句话：

- TargetA 必须是手机**当前 RPA**，身份地址不行；
- own 必须是码表 **public** 地址，`own=RANDOM`（含 `DIR_ADDR_RPA`）手机不认；
- RPA 会轮换，所以只缓存最近一次连接的 RPA；无缓存/过期/失败就回退无定向。

定向广播不节省空口时长，换来的是“只有目标手机能连”；产品仍用 200 ms 快广播，
回连速度优先。旧文档里的 1280 ms 空闲档已于 2026-09-21 移除。

**开关与持久化**：设置页的「手机蓝牙」开关就是 `ctl radio on|off`，**状态落 KV**，
重启后保持。关掉它 = 设备完全不广播，手机当然找不到 —— 排查「扫不到」时先看它。

---

## 3. 射频节奏（本控制器广播与扫描不能同时开）

`ble_sensor.c` 的 `sensor_reconnect_radio_pump()` 是一个相位机，把射频在
**手机广播**与**外设重连扫描**之间分时：

```
IDLE → PHONE_FIRST_ADV(15000 ms) → [steady ADV(4000 ms) → ADV_STOP_WAIT → SHORT_SCAN(800 ms)] → …
```

- 每一轮扫描都要先把手机广播停掉（硬件不支持同时开），所以**扫描越勤，手机被找到的机会越少**。
- 手机刚掉线先给 **15 s phone-first**（只广播，不扫描）；之后稳态 **4 s ADV / 0.8 s 短扫**。
- 手机连着时不是连续扫：同样走 **4 s 静默 + 0.8 s 短扫**（`[radio] steady quiet window 4000 ms`）。
- `SENSOR_ADV_STOP_ACK_MS` 是「等让位确认」的**有界**等待 —— 等不到也照常起扫，
  否则会像 2026-09-18 那样空等 14 s 把射频全空掉。

### 3.1 空轮回连退避与手机 grace（2026-09-21）

一台**绑定过、但此刻不在附近**的外设，不该让射频一直空转；但旧的指数退避
（`+2 → +4 → … → +32 s`）会把手机 ADV 饿到十几秒才露一次脸——手机回连与
心率回连**一起变慢**（现场 `empty=4/5`、`+8/+16 idle backoff`）。

| 项 | 行为 |
|---|---|
| 常态 | ADV **4 s** ↔ 短扫 **0.8 s**；空扫 idle 线性 **+0/+1/+2/+4 s**，封顶 **4 s** |
| 手机刚掉线 | **15 s phone-first**：只广播、不扫描，不算 idle（偏等人；窗口结束再进稳态） |
| 清零 | 连上外设 / 用户重扫重绑 / `ble_sensor_reconnect_backoff_reset` / 手机再连上 |
| 日志 | `[radio] phone-first ADV 15000 ms`；`[radio] resume steady ADV 4000 ms (+N idle, empty=M)`；手机已连时 `resume steady quiet 4000 ms` |

**不能**「预知有人要连再广播」——只能在最可能有人连的窗口把 ADV 留够。
Vela SAL 另有 CCC 用 `bt_conn_index` 读错槽的坑，见 §1.4（板级已抽换）。

---

## 4. 并发纪律（跨线程）

协议栈（`bt_service_*`）、companion、NSH/`ctl`、LVGL 四类上下文都在碰同一批状态，
以下是硬规则（都是现场 panic 换来的）：

### 4.1 不要在非协议栈线程里遍历连接链表

`bt_conn_foreach()` 会在回调前 `bt_conn_ref()`，而链表正被协议栈增删时就会踩到野指针：

```
coredump n574_20260920_121424:
  kind=assert  pc=bt_conn_ref→stlex  cfsr=0x82  mmfar=0x124
  pid=35 ble_companion  irq=1
```
同一个签名更早还出现在 `ctl pair list`（pid=ctl）——**结论是「谁都不行」**，不是
「换个线程就行」。

替代做法（现在的实现）：

| 需求 | 做法 |
|---|---|
| 「哪条链路已加密」 | 注册 `bt_conn_cb.security_changed`（协议栈线程、回调带合法 `conn`）→ 只把地址+类型存进暂存并置标志；companion 每拍取走落盘（`myvendor_pair_peer_poll()`） |
| 兜底 | `le_param_updated`（手机连上后必做参数更新）里再判一次；协议栈万一不报 Encrypt Change 也能补上 |
| 「手机侧链路还在不在」 | `myvendor_pair_phone_link_up()`（低频调用，仍走 foreach，但只在状态对账时用） |
| `ctl pair list` | 投 `COMPANION_TEST_PAIR_LIST`，由 companion 线程填快照，ctl 侧只打印 |

### 4.2 框架调用只置标志，由 companion 线程执行

在 ctl/NSH 任务里直接调 `bt_le_stop_advertising()` 之类会撞 `libuv/src/unix/thread.c:358`
断言（板子当场 panic）。所以：

- `ctl pair open` → `myvendor_pair_window_open()` 置 `g_window_restart_req`
  → companion 取走再重播广播（`myvendor_pair_window_take_restart()`）；
- `ctl pair unbind` / UI 解绑 → `myvendor_pair_unbind_request()` 置标志
  → `myvendor_pair_unbind_poll()` 在 companion 线程做 `bt_unpair`；
- `ctl radio on|off`、`ctl sensor on|off` 走 `companion_bridge` 的同类投递。

### 4.3 共享暂存一律上锁

| 暂存 | 谁写 | 谁读 | 锁 |
|---|---|---|---|
| 配对窗口截止时刻 / 重播请求 | UI、ctl、companion | UI、companion、广播决策 | `g_window_lock` |
| 「刚加密的那条链路」地址+类型 | 协议栈线程（`security_changed`） | companion 每拍 | `g_secure_lock`（取走时整份拷贝，锁外做 tsv/flash） |

---

## 5. 运维 / 恢复工具箱

| 命令 | 用途 |
|---|---|
| `ctl pair list` | 当前 LE 连接（下标 / 地址 / 角色 hr\|csc\|cps\|? / 安全等级 / 是否绑定）+ `conns=` |
| `ctl pair connect [idx\|addr前缀]` | 主动发起配对（`bt_conn_set_security(L2)`）；无参数 = 挑非传感器那条 |
| `ctl pair accept on\|off` | 是否自动同意配对请求（`on` 会注册 auth 回调） |
| `ctl pair open [秒]` | 开配对窗口（另见 §1.2） |
| `ctl pair phone` | 打印 `/mnt/kv/bt_phone.tsv` 记录（地址 + 类型）与窗口剩余 |
| `ctl pair unbind` / `forget` | 解绑记录里那台（`bt_unpair` + 删记录）；`forget` 为零参强制清除路径 |
| `ctl radio on\|off` | 手机侧广播（= 设置页「手机蓝牙」开关，**落 KV**） |
| `ctl sensor on\|off` | 外设 Central 总开关（= 设置页「外设蓝牙」开关） |
| `ctl sensor bind <0HR\|1CSC\|2CPS> <mac> [addr_type]` | **按 MAC 重绑一台传感器**：记录被删后从串口恢复现场（见 [ble_sensor.md](ble_sensor.md)） |
| `ctl nvds` | dump 控制器 NVDS 原始十六进制；**不能单独证明 RAL 生效**。RAL 判据看 `bt_keys kv: <addr> -> RAL rl_size=3 entries=N`；绑定密钥在 `/mnt/kv/bt_keys.bin`，`bt_phone.tsv` 只是主机准入允许名单 |

设备 UI 的操作映射：

| 动作 | 结果 |
|---|---|
| 蓝牙设备 → 手机蓝牙 | 未配对 = 提示配对；已配对 = 设备状态（地址 + 已连接/未连接） |
| 点「配对新手机」→ 弹窗单击勾号 | 开 30 s 配对窗口（行变「配对中 + 倒计时」） |
| 手机蓝牙页长按右键 → 弹窗单击 | 解除绑定 |
| 心率带/踏频器/功率计页长按右键 → 弹窗单击 | **删除该外设的记录**（删了就不会再回连，见 ble_sensor.md） |

---

## 6. 现场判据速查

| 日志行 | 含义 |
|---|---|
| `adv discoverable (pair window; 200 ms)` | 窗口内快广播（新手机可扫到） |
| `adv directed (paired; cached RPA <RPA> age=<ms> ms)` | 有记录且 RPA 新鲜：定向广播（own=PUBLIC，TargetA=RPA） |
| `adv connectable unnamed (paired; directed cooldown; 200 ms)` | 定向失败冷却中，回退无定向 |
| `adv connectable unnamed (paired; no fresh RPA; 200 ms)` | 无缓存/RPA 过期，回退无定向 |
| `adv off (unpaired, no window; …)` | 没记录不广播（设计如此） |
| `adv start failed status=<码> id=<id>` | 广播启动失败；status=2 是框架 **1 s 超时**（射频/扫描占用，或旧 SAL 参数错误都会表现成 2）；定向失败另有 `directed adv failed/stopped/no connection` 行 |
| `pair: secured peer <ADDR> -> record` | `security_changed` 对账补上了手机记录 |
| `pair: phone record <ADDR> type=<t>` | 记录**内容变化**（正常不应刷屏） |
| `pair: <ADDR> not in list -> grace 3s (await L2 for bonded RPA)` | 有记录但地址对不上（RPA 回连）给 **3 s** 宽限等 L2；超时后拒绝并冷却 60 s |
| `pair: reject unpaired <ADDR> (grace over)` | 宽限到期仍没绑定 → 断开 |
| `le secure lvl=2` | 手机链路已加密（配对完成的**实证**） |
| `[radio] resume steady ADV 4000 ms (+N idle, empty=M)` | 稳态 4 s 广播，空扫 idle 在涨（见 §3.1） |
| `[radio] resume steady quiet 4000 ms` | 手机已连：4 s 静默 + 0.8 s 短扫 |
| `phone RPA cached <RPA> (directed ready)` | Admission 通过，原始 RPA 已缓存 |
| `directed adv no connection in 60000 ms; fallback ...` | 定向 60 s 未连，回退无定向 |
| `bt_keys kv: <addr> -> RAL rl_size=3 entries=N` | 手机 IRK 是否进控制器 RAL |
| `ble_sensor: HR connected <ADDR>` | 外设连上（记录会因此写回 autorc=1） |
| `phone peer refresh <A> -> <B> (RPA/identity)` | 手机 RPA/身份地址切换，peer 已刷新（见 §1.3） |
| `fs ccc DROPPED (not phone) … addr=… peer=…` | FS CCC 被拒（刷新后仍见此行才是真异常） |
| `fs cmd DROPPED (not phone) …` | FS 命令被拒（同上） |

**版本**：v1.2 · **更新**：2026-10-18（窗口外已绑定手机定向回连 + 60 s 窗口/10 min RPA 缓存；直连细节见 [directed_reconnect.md](directed_reconnect.md)）
