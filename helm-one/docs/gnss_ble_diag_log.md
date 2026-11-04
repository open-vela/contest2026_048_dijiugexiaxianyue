# GNSS / BLE 诊断日志速查

这份文档只描述 **my_vendor 新增的诊断插桩**，不涉及行为改动。
所有插桩都符合两个约束：**只写日志**，不改变阈值、控制流、返回值、恢复策略。

> 唯一的例外是 §2.2 的 `poll()` 硬钳位 —— 那条确实改了行为（限制超时上界），
> 但实测**没有解决问题**，它的推断已被证伪，详见该节。留在这里是为了不让
> 下一个人再沿着"调超时值"的方向走一遍。

排查时按两类问题取日志：

1. **为什么会异常**
2. **异常为什么恢复得这么慢**

相关的既有机制（diag 线程、supervisor、watchdog）见 [diag.md](diag.md)。
日志的持久化（落盘、级别过滤、目录）见 [diag_log.md](diag_log.md)。

---

## 1. 为什么异常

### 1.1 星历注入为什么 0 帧（GPS 长时间无定位的直接原因）

`gnss_eph_inject()` 的调用方 `gnss_eph_run()` **无条件**把 `g_eph_injected`
置真，所以一次 0 帧的失败注入会让本轮开机不再重试。原来这条路径完全不可见，
现在每次注入都有完整的取证行。

| 日志 | 说明 |
|------|------|
| `gnss: eph inject enter` | 进入注入，同时打一个 `rt` 时间线打点 |
| `gnss: eph src <path> size=N mtime=T stat_ok=1` | 开读前的文件属性 |
| `gnss: eph pre-read n=… first=…` | 预读 6 字节的结果。**`n=0` 即文件为空的瞬间** |
| `gnss: eph first frame cls=… id=… mode=MGA\|DBD` | 首帧决定按 AssistNow 还是 DBD 处理 |
| `gnss: eph short payload want=… got=…` | 半截帧，文件在写入中被读到 |
| `gnss: eph short cksum want=2 got=…` | 同上，连校验位都读不到 |
| `gnss: eph done frames=N skip=S badck=B bytes=X brk=R mode=… size=A->B mtime=C->D changed=0\|1 cost=…` | **汇总行**。`brk` 是循环退出原因，`changed=1` 说明文件在注入期间被并发改写 |
| `gnss: eph ZERO FRAMES brk=… bytes=… changed=…` | **最关键的一行**，0 帧时打印 |
| `gnss: eph run enter auto=… injected_before=…` / `eph run exit injected=…(was=…) frames=…` | 标志位前后对比，直接暴露"0 帧仍被标记为已注入" |

`brk` 取值：`eof` / `not-ubx-mid` / `short-payload` / `short-cksum` / `bad-checksum`
/ `payload-too-big` / `send-mga-fail` / `send-dbd-fail` / `abort-diag` /
`pre-read-not-ubx` / `open-fail` / `lseek-fail`。

`skip` 计数是"被过滤规则丢掉的帧"（AssistNow 文件里的 `0x13/0x80` 导航库帧）。
**`frames=0` 且 `skip>0` 说明整个文件都被过滤了**，是 `0x13/0x80` 之外的另一条
"0 帧但无报错"路径。

`gnss: eph boot_try skip fd=… pending=… injected=…` 说明"待注入"被静默丢弃
（10 s 限频）。

### 1.2 读线程为什么退出 / 为什么被重启

| 日志 | 说明 |
|------|------|
| `gnss: reader enter` | 新会话开始，同时清空活跃度统计 |
| `gnss: reader exit stop=1 sess=… rd=… byte=… nmea=… bad=… ubx=… pvt=… junk=… pub=… gap_max=… loop_max=… stall1s=…` | **退出画像**。`stop=1` 是被 reap 请求退出；`stop=0` 是外部 cancel |
| `gnss: restart begin why=boot\|diag-thread-stall hb_age=… sess=…` | 谁要求重启、当时心跳多旧 |
| `gnss: reap enter pid=… alive=… has_thread=… wait_ms=… hb_age=…` | `alive=1` = TCB 查得到（`nxsched_get_tcb`），说明线程还在、只是不干活；`alive=0` 才算已退出。`pid` 是**线程自己的 tid**（入口 `gettid()`，与 `ps`/`schedmon` 里那个 pid 对得上；`getpid()` 给的是 task group 的 pid，本线程 group=0，历史上一直读到 0） |
| `gnss: diag tcb pid=… st=… lock=… wait=… wd=…` | **卡死那一刻的 TCB 现场**，与 `diag not ok` 同一时刻打出。`wd=1` = 有 watchdog armed（真在带超时的 `nxsem_wait_slow` 里，问题在超时没到期）；`st=Ready` 且 `wd=0` = 不在信号量等待上（卡在驱动/自旋那一类）。`pid=0/0` 时这条会退化成 `diag tcb unknown` —— 那说明 pid 又丢了，此时没有现场可看 |
| `gnss: diag tcb gone pid=…` | 查得到 pid 但 TCB 已经不在（线程已退），与 `alive=0` 同义 |
| `gnss: reap wait1 … alive=…` / `reap wait2 … alive=…` | 等待与 cancel 各花了多久、是否仍然活着 |
| `gnss: reap ok in N ms` / `reap EBUSY after N ms` | 关键耗时点。`EBUSY` 会放弃 join 并继续处理 |
| `gnss: restart done ret=… in N ms (eph_cleared)` | 整个重启的总耗时 |
| `gnss: vcc recover why=silence\|diag-kick\|read-err force=… fails=… sess=… gap_max=… stall1s=… poll_close=…` | **恢复触发者**，把"谁把它判死了"写明确 |

### 1.3 看门狗为什么判定模组死了

`g_nmea_life_ms` 只在 `gnss_publish()` 里更新，所以"丢了几百字节"会被升级成
"模组故障"。触发瞬间现在会把全部判定输入打出来：

```
gnss: silence trig silent=… lim=… hold=… eph=… dbd=… grace=… kick=… fail=…
      sess=… gap_max=… stall1s=… win_byte=… win_nmea=… win_bad=…
      win_ubx=… win_pvt=… win_junk=… win_rd=… win_poll_to=… win_pub=…
```

- `win_byte` 正常（每窗口约 4000 字节）但 `win_pub=0` → 是**解析**没产出 publish，不是模组静默。
- `win_byte` 也很小 → UART/模组层面确实没数据。

读线程活跃度（`g_rd_*`）反映 **1024 字节接收环的溢出风险**：

- 模组约 800 B/s，环 1024 字节 ⇒ **1.28 秒**就会写满，写满即静默丢字节。
- `gap_max` 是相邻两次成功 `read()` 的最大间隔；`stall1s` 是间隔 ≥1 s 的次数。
- 每次出现 ≥1 s 的间隔会立刻打一行 `gnss: reader gap N ms (rx buf 1024 B, ~800 B/s)`。

### 1.4 蓝牙：协商了什么、丢没丢包

`sf32lb52_bth4.c` 现在解码 LE Meta 子事件。偏移约定：`data[0]`=H4 类型，
`data[1]`=事件码，`data[2]`=plen，`data[3+n]`=payload[n]。

| 日志 | 说明 |
|------|------|
| `sf32lb52 bth4 conn st=… hdl=… role=… peer=… itv=… lat=… to=…` | LE Connection Complete，含**协商到的** interval/latency/timeout |
| `sf32lb52 bth4 conn_upd#N st=… itv=…(x.xx ms) lat=… to=…(… ms)` | 每次 Connection Update。原来只打两个字节，看不出参数，现在能看到反复重协商的过程 |
| `sf32lb52 bth4 rmt_feat … feat=…` | 对端特性位 |
| `sf32lb52 bth4 dlen hdl=… tx=…/… rx=…/…` | 数据长度变更 |
| `sf32lb52 bth4 fwd fail n=N type=… len=… ret=… (pkt dropped)` | **HCI 包被永久丢弃的唯一出口**。`ret=-12`(`-ENOMEM`) 就是 uart_bth4 环满。丢断连事件会让上层以为链路还在 |
| `sf32lb52 bth4 sess hdl=… conn#… dur=… acl_in=P/B acl_out=P/B evt_rx=… fwd_fail=… conn_upd=…` | 断开时的**会话画像**。`dur` 很短 + `acl_*` 很小 + `reason=0x13` ⇒ 对端在重试 |

`reason` 速查：`0x08` 监督超时（链路问题）、`0x13` 对端主动断开（手机 App 决定）、
`0x16` 本机主机要求断开。

### 1.5 蓝牙：上层是不是"以为还连着"

`g_ctx.connected` 只能由 `gatts_disconnect_callback` 清除；peripheral 角色的
`le_disconnected` 只记 reason。事件一丢，`ble_supervisor` 的
`expected = !companion_phone_connected() && …` 恒为 false，广播停摆看门狗
就此永不触发。现在两侧事件并排可见：

| 日志 | 说明 |
|------|------|
| `ble_companion: hci_disc role=peripheral reason=… peer=… ctx_connected=… ccc=… mtu=… adv_active=… adv_restart=… hold=… fs_busy=…` | HCI 侧断开时的上层状态。**`ctx_*` 只描述手机那条链路，所以只在 peripheral 角色下打**（2026-09-18 起） |
| `ble_companion: hci_disc role=central reason=… peer=…` | central 角色（心率/速度传感器）的断开，只给对端地址。之前两种角色共用一行，18.454 那次心率带掉线被读成了"手机 ctx_connected=1 ccc=1 mtu=244" |
| `ble_companion: gatts_disc <addr> reason=… hci_first_ms=…` | GATTS 侧断开，`hci_first_ms` 是相对 HCI 断开的延迟 |

**判读方法**：出现 `hci_disc role=peripheral ctx_connected=1` 之后
**没有** `phone disconnected` / `gatts_disc` ⇒ 卡死已发生。

### 1.6 蓝牙：通知是不是真的发出去了

`bt_gatts_notify()` 的返回值不可信（Vela SAL 丢掉了 `bt_gatt_notify_cb` 的结果），
所以"已计入 `seq` 但从未到达"的页会被当成已发。

| 日志 | 说明 |
|------|------|
| `ble_companion: fs_disc state=… seq=… sent=… done=… fail=… credit_to=… credit_lost=… inflight=… gap=…ms mtu=… ccc=…` | **通知对账**。`sent - done` 就是从没被确认的页数；`credit_lost` 是信用超时强行归零时吞掉的页数 |

`sent > done` 且手机随即 `reason=0x13` ⇒ 手机是按序号/CRC 断裂主动断开重试。

### 1.7 非持有者解锁互斥量

`sched: mon …` 每 15 s 一行，原来只有计数。现在计数非 0 时会紧跟一行现场：

```
sched: mutexbad last n=… holder=… tid=… run=… this=<线程> st=… runner=<线程> st=…
```

上游此处是 `DEBUGASSERT(holder == tid)`，本 vendor 把它降级成计数。
非持有者 post 是未定义行为，可能留下卡死的 blocking 位——典型的
"随机卡一下又自己好了"。

### 1.8 UBX-NAV-PVT 探针（模组发不发 PVT）

给 `gSpeed`/`sAcc`（NMEA 拿不到）做的前置探针，方案见
[gnss_speed_filter.md](gnss_speed_filter.md) §步骤 2。**只看帧、不解析**：

| 日志 | 说明 |
|------|------|
| `gnss: cfg ack dyn=… elev=… cno=… spd=… aid=… pvt=…` | **配置侧**。`pvt=1` = `CFG-MSGOUT-UBX_NAV-PVT_UART1` 被 ACK；`-1` = 模组 NAK（键/口不支持）；`0` = 250 ms 没等到 ACK |
| `gnss: pvt out uart1=… uart2=…` | 只在 UART1 失败时打，说明退回 UART2 试的结果 |
| `gnss: first pvt len=92` | **接收侧**：本会话第一帧 PVT 到了。M10 定长 92 字节，别的长度说明协议版本不同 |
| `gnss: pvt raw <hex>` | 紧随 `first pvt` 的十六进制原文（分 3 行），用来核对解码 |
| `gnss: pvt fix=… gnss=… sv=… gs=… vn=… ve=… sacc=… hacc=… pdop=…` | 解码后的值，1 Hz（`CONFIG_MYVENDOR_GNSS_LOG`）。`gs`/`vn`/`ve` 是 centi-km/h（与 `pub … spd=` 同单位），`sacc` mm/s、`hacc` mm、`pdop` 0.01。**未过任何 sanity/包络** |
| `gnss: pub … spd=… hz=… pvt=%d gs=%u sacc=%u pacc=%u` | `spd` = NMEA RMC 的**未滤波** SOG，`gs` = PVT 的**滤波过** `gSpeed`，`sacc` = 它的 1σ 精度（mm/s），`pacc` = PVT hAcc（**米**，位置精度）；`pvt=0` 表示这一份快照没贴上 PVT（没开/太旧），此时其余为 0 |
| `gnss: link … ubx=… pvt=… junk=…` | `pvt` 只统计**整帧且校验通过**的 `cls=0x01 id=0x07`；`ubx` 是所有 UBX 整帧 |

`pvt=` 在涨就说明模组确实在发；恒 0 而 `cfg ack … pvt=1` 是"配置被收下但收不到帧"，
按上面那张表往 legacy `CFG-MSG` 退。`hz=` 实测不变（PVT 与 NMEA 同一个历元突发），
只有倒库窗口会到 2；判模组输出率始终看 `byte`/`nmea`。

**想看原始字节**：有 `test gnss raw [sec]`（可打印字符原样、其余 `<xx>`），但它自己
`open()` 同一个 `/dev/ttyS0` 逐字节读，**和常驻的读线程共用同一个 RX FIFO、没有任何仲裁**：
读线程 prio 50 比它高（nsh 100），基本抢不到几个字节，而且被抢走的字节会让读线程 5 s
没有 publish → 触发 `silence trig` 的 VCC 循环。要 raw 就用上面那两行（`first pvt` +
`pvt raw`）或 `test gnss eph dump`，别用 `raw`。

### 1.9 读线程的 housekeeping 节奏（`loop≈800/s` 是什么）

读线程的状态机**就写在主循环体里**：每收一个字节、处理完 `continue` 回循环顶。
所以 `link cpu` 行的 `loop=` 稳态就是**字节率**（NMEA ~800 B/s ⇒ 800~1000/s）——
这个量本身不是异常。

但 `rx_run` 那一段（housekeeping）——

```
eph_boot_try → dbd_try_start → dbd_tick → dyn_apply → link_report → silence-check
```

——自 2026-09-17 起有**结构闸**（`GNSS_HOUSEKEEP_MS = 50 ms`）：从"每收一个字节
跑一遍"改成 20 Hz。在那之前，这一段每个分支都要过 mutex 和几个定时器判断，而且
**任何漏了限频的日志都会按字节率刷屏**（现场：`dbd dump skip (mtp busy: …)` 一行
刷满屏、持续二十多秒）。段内各项都是时间驱动的 —— `link_report` 15/5 s、
`dbd_tick` 400 ms/10 s、静默判定 5/15 s、`dyn_apply` 重试 2 s —— 50 ms 的节奏
对它们的时间精度没有影响；新会话（开机 / VCC 恢复 / 休眠唤醒）会立刻跑一次。

**判据**：闸门之后，housekeeping 里的日志最多 20 行/秒；同一行 1 秒内出现远多于
这个数，就说明它挂在某条**逐字节**路径上、漏了限频。要看"模组输出率"始终看
`byte`/`nmea`。

**倒库（DBD dump）刻意不抢**（2026-09-17 定策）：它只值"把导航库留给下次开机"，
没有实时性要求，所以 LFS 被 App/UI 占着（或 hold 还在保鲜期内）就安静等下一轮 ——
`GNSS_DBD_RETRY_MS = 30 s` 既是它的重试节奏，也是这类日志的上限。真正要快的是
**星历注入**（决定本次开机多久有定位），它有自己的退避 2~30 s；泄漏的 hold 由
`quiesce()` 在源头回收，那是为所有消费方（注入首当其冲），不是给倒库插队。

### 1.10 模组侧配置：读回了什么、什么时候切模型（2026-09-17）

| 日志 | 说明 |
|------|------|
| `gnss: sig gps=… qzss=… gal=… sbas=… bds=… glo=… fixmode=…` | 每次会话开头用 `UBX-CFG-VALGET` 读回的**实际**配置（这些键我们没下发过，是出厂默认，光猜不行）。`-1` = 没读到（键不支持或超时），**不是"关着"** |
| `gnss: motion still (stationary model)` / `gnss: motion resume` | 停车/运动切换：停稳（<3 km/h 持续 20 s）**且**解算质量差时，把 `CFG-NAVSPG-DYNMODEL` 切到 `stationary(2)`（零速约束，室内维持解最有用）；速度起来 3 s 就切回。这颗固件若不认 stationary，NAK 一次后本会话不再试 |
| `gnss: still switch on\|off` | 上面那套切换的**运行时开关**（`ctl gnss still on\|off`）。**默认关**（2026-09-17 骑行结论：零速约束会压低速度、拖住自己的解除条件 → "拐弯记成直行 + 速度归零"）；关着时 `gnss_dyn_feed()` 直接清零返回，`g_dyn_still` 恒假，也就不会再出现 `motion still/resume` 这两行 |
| `gnss: saw advertiser` / `ble_sensor: HR saw advertiser, connecting` | 已降级为 `LOGD`（默认编译掉）：退避窗口里它每个 tick 都成立，`LOGI` 时是按轮询周期刷屏（0x3e 的 3 s 退避刷 ~90 行）。真正发起连接那次由 `connecting <kind> <addr> rssi=` 记录 |
| `gnss: assist pos resend (phone, no fix yet)` | 自己的解算还不行、且手机位置有更新时补发一次 `MGA-INI-POS_LLH`（最小间隔 60 s）。位置辅助只在注入时发一次的话，手机晚连上就白丢了 |

**辅助位置的精度按真实值发**：有 PVT 就用 `hAcc` 打底，再按年龄外推（停车 2 m/s、
运动 15 m/s），下限 100 m。原先本机那份固定报 **20 km** —— 那等于"在地球上"，
对模组几乎没有约束力。

**`mtp busy:` 的四项怎么读**（`gnss: dbd dump skip (mtp busy: …)` /
`gnss: eph inject skip (mtp busy: …)`）：

| 字段 | 含义 |
| --- | --- |
| `xfer=1` | 真正在传（USB MTP）→ 让路是对的 |
| `sess=1` | 有会话开着 → 让路是对的 |
| `plug>=1` | 主机选了 MTP 配置（哪怕一次传输都没发生）→ 可疑点 |
| `hold=1` + 其余全 0 | **泄漏的 lfs hold**：一次预约没人还。超过 30 s 保鲜期会被 `quiesce()` **在源头回收**并打 `mtp: leaked lfs_hold reclaimed` |
| `hold_by[a+1200ms b+3400ms]` | hold 命中时附带的**最近取用记录**（谁、多久之前）。hold 是裸计数、没有 owner 字段，这一串是它唯一的来源线索 |

---

## 2. 异常为什么恢复得慢

一轮恢复用一条时间线串起来，前缀 `gnss: rt`：

```
gnss: rt begin #N why=silence|diag-kick|read-err|reader-restart|boot
gnss: rt 1 reader-spawn   +0   tot=0
gnss: rt 2 vcc-off        +12  tot=12
gnss: rt 3 vcc-power-down +3   tot=15      <- 这里是 usleep(off 2500)
gnss: rt 4 vcc-settle     +2503 tot=2518   <- 这里是 usleep(settle 1000)
gnss: rt 5 uart-open      +1004 tot=3522
gnss: rt 6 cfg-flush      +3   tot=3525
gnss: rt 7 cfg-zda        +9   tot=3534
gnss: rt 8 eph-enter      +41  tot=3575
gnss: rt 9 eph-done       +3710 tot=7285
gnss: rt 10 cfg-nav       +18  tot=7303
gnss: rt 11 cfg-done      +4   tot=7307
gnss: rt 12 first-nmea    +640 tot=7947
gnss: rt 13 first-fix     +…   tot=…
gnss: rt end why=fix total=… nmea_ok=1 nmea_ms=… fix_ok=1 fix_ms=…
```

每个 `+N` 是**该阶段单独耗时**，`tot` 是**距本轮恢复起点的累计**。
按 `+` 列排序即可看出恢复时间花在哪一段：

- `vcc-power-down` = `CONFIG_BOARD_L96_GNSS_PWR_OFF_MS`（默认 2500 ms）
- `vcc-settle` = `CONFIG_BOARD_L96_GNSS_PWR_DELAY_MS`（默认 1000 ms）
- `eph-done` = AssistNow 注入（`GNSS_EPH_GAP_US` 每帧 + UART 发送时间）
- `first-nmea` / `first-fix` = 模组重新收数 / 重新定位

`first-fix` 迟迟不出现时，`gnss: pub valid=… sats=… hear=… view=…` 里的
`hear` 才是关键：`hear` 是 GSV 里 `snr>0` 的卫星数，**`hear≤3` 就不可能定位**
（需要 ≥4）。注意 `view` 是各条 GSV 的 "in view" 字段累加值，是放大的，别按
卫星数读。

`gnss: rt end why=superseded` 表示上一轮还没等到修复，新一轮恢复就开始了
——这就是"恢复自己套恢复"的振荡。

### 2.1 星历阶段内部耗时（`eph-*` 面包屑）

时间线里 `eph-done` 一项就可能占好几秒（实测 3710 ms），而它是"一整段"，
看不出里面卡在哪。所以在 `gnss_eph_inject()` 内部再打一层 `rt` 打点：

```
gnss: rt N eph-psram-done +1
gnss: rt N eph-fs-begin   +0
gnss: rt N eph-prune      +…
gnss: rt N eph-src        +…      <- 扫星历目录
gnss: rt N eph-open       +…
```

**看 `+` 最大的那一个就知道卡在哪一段**，不用去猜。

同时 `gnss_eph_src()`（取星历文件路径的那一步）有**逐次调用**的计时，
超过 `GNSS_EPH_SLOW_MS`(20 ms) 就单独报一条：

```
gnss: eph-src slow opendir/readdir/stat N ms
gnss: eph-src 1234 ms opendir=1 readdir=57/max=210 stat=8/max=12 slow=4 hit=/mnt/lfs/…
```

结论值得记住：**LittleFS 的目录扫描本身就是慢的**，而且慢在 `readdir` 上。
`hit=` 表明目录里有多个候选文件时它会一个个 `stat` 过去，遇到大目录更慢。
这不是 bug，是设计上的固有成本 —— 星历目录里的文件数要控制住。

### 2.2 读线程的 `poll()` 超时：不是"超时太长"，而是计时基准会被打断

读线程曾经永久卡死（`hb_age` 八万毫秒，`reader not exited … skip join`），
当时的推断是"`timeout_ms` 没有上界"，于是加了一个硬钳位：

```c
if (timeout_ms <= 0 || timeout_ms > 1000) timeout_ms = 200;
```

**但钳位没有解决问题**：带钳位的那版日志里，卡死依旧（`hb_age=80840`、
`stop_before=1`、`busy=1`）。所以推断是错的，钳位本身无害但也没用。

正确的机制是：**`poll()` 的超时是一个由 tick 驱动的看门狗，而阻塞式 `read()`
靠的是 ISR 唤醒信号量（不依赖 tick）**。所有基于 tick 的超时
（`poll`、`usleep`、`nxsem_tickwait`）在时间基准被打断时都会一起出问题。
`sifli_systick_reclock()` 的注释里就点名了 `up_timer_set_lowerhalf()`
是已知的 `poll()` 看门狗杀手。

结论：**别再往"超时值"方向调**。真正的修法是换掉依赖 tick 的等待方式
（DWT 周期计数器做非阻塞读循环 + 预分配读线程栈轮换，
**绝不对可能持锁的线程用 `task_delete`**）。

**已修（`myvendor_gnss.c`）**：读线程的稳态等待是**阻塞 `poll()` + 信号唤醒**。

- `gnss_read_some()` 的等待回到**阻塞** `poll(…, timeout_ms)`：线程真正睡在
  `nxsem_tickwait()` 上，由 UART RX 中断（或超时到点）唤醒，稳态 CPU 归零。
- 关键在于这个超时**不再是唯一的退出途径**：`gnss_reader_wake()` 给读线程发
  `SIGUSR1`，把 `nxsem_tickwait()` 打断成 `-EINTR`。**信号不经过 tick**，
  所以上面那条"计时基准一被打断就永远醒不来"的路被堵死了 —— 这正是 §2.4 里
  一直缺的那颗"牙齿"。`gnss_thread_reap()` 置位 `g_gnss_stop` 之后、
  `myvendor_gnss_diag_restart()` 置位 `g_diag_kick` 之后，都必须调它一次。
- `sa_flags = 0` 是硬要求：**不能设 `SA_RESTART`**，否则
  `nxsem_tickwait()` / `nxsem_wait()` 会自己重启等待，信号白发一场。
  另外信号掩码是从父线程继承的，所以入口显式 `pthread_sigmask(SIG_UNBLOCK)`。
- 用 `SIGUSR1` 是因为本配置没开 `CONFIG_SIG_SIGUSR1_ACTION`，
  `nxsig_default()`（`sched/signal/sig_default.c`）对表外信号的默认动作是
  `SIG_IGN` —— 即使 handler 没装上，这个信号也只是被忽略，**不会把读线程
  打死**。若哪天开了那个 Kconfig，默认动作会变成异常终止，就必须换个信号。
- 读线程入口仍然显式 `pthread_setcancelstate(PTHREAD_CANCEL_DISABLE)`：
  `poll()` 本身是 cancellation point，reap 那次 `pthread_cancel()` 真的能撕到
  它 —— 而它会持有 LittleFS 占用和 UART 锁，"持锁的尸体"正是文件头禁止的。
  退出只走 `g_gnss_stop` 优雅路径。
- 读线程路径上的 `usleep()`（星历帧间隙、UART 写退避、VCC 稳定时间、打开失败
  重试）仍统一走 `gnss_sleep_us()` / `gnss_sleep_ms()`：DWT 预算 +
  `sched_yield()`，不挂 wdog；`gnss_wait_pid_gone()` 的重试间隔也一样，免得
  reap 自己挂在 `usleep()` 上。
- **自旋仍必须用 `gnss_yield_all()`，不能用裸 `sched_yield()`**。本 fork 里
  **数值越大优先级越高**（`SCHED_PRIORITY_MAX`=255 最高、`MIN`=1 最低；
  佐证：`hpwork`=224 > `lpwork`=100，`"BT LW WQ"`=10，`diag`=9）。自旋会饿住
  `diag`(9) 和 `BT LW WQ`(10)：diag 是 GNSS 的自愈入口，BT LW WQ 是 zblue 的
  延迟工作队列。`sched_yield()` 只在同优先级内轮转
  （`nuttx/sched/sched_yield.c` 的注释写得很明确），所以 `gnss_yield_all()`
  临时把本线程降到最低优先级再让出，每 `GNSS_SPIN_YIELD_EVERY` 次做一次。
- **原优先级必须由调用方在进入自旋前捕获一次**（`gnss_sleep_us()` 入口），
  **不能在自旋循环里用 `pthread_getschedparam()` 读** —— 那时读到的已经是我们
  刚降下去的最低优先级，会被当成原值固化，线程从此常年 `pri=1`。
- `GNSS_SLEEP_SPIN_MAX` 是 `gnss_sleep_us()` 的最后一道自旋上限；DWT 自检不过
  （CYCCNT 不动）或 HCLK 未知时整体退回 `usleep()` 老路径。

**未做：预分配读线程栈轮换**。`.bss` 已占 346.8 KB / 95.9%（可用约 15 KB），
再备一块 10 KB 的 `g_gnss_stack` 会挤压 SRAM，所以没有做。理由是先去掉
压栈的成因：上面的改动让读线程**一定能**在 200 ms 内回到循环顶看到
`g_gnss_stop`，于是 reap 的 `g_gnss_stack_busy` 不再会永久为 1、spawn 不再
会被永久拒绝 —— 轮换要解决的问题本身消失了。如果以后 `read wait spin bound
hit` 或 `reader not exited` 再现，再补轮换（栈放 PSRAM，走 `board_malloc.h`）。

### 2.3 已排除：SysTick 时间基准是健康的

为了排除"`sifli_systick_reclock()` 把计时搞坏了"，给每次重装加了取证行：

```
CLK: SysTick rearm #N ticks A->B ctrl=…(en=1 int=1 src=1 flg=0) val=… load=…
```

实测 `en=1 int=1 src=1` 恒成立，`load` 始终等于**上一档 HCLK / 100**
（也就是恒定的 100 Hz），`val < load`。

**所以重装逻辑本身没问题**，它不是读线程卡死的原因 —— 但 §2.2 说的
"tick 驱动的超时会被打断"依然成立，只是打断源不是这里。

### 2.4 "异常无法恢复"的机制：恢复路径本身没有牙齿

这一节回答的是"为什么一旦坏掉就再也起不来"。**它和 §2.2 是两个独立的问题**：
§2.2 是"为什么会卡住"，这里是"卡住之后为什么救不回来"。就算 §2.2 的根因被
修掉，只要恢复路径还是这个样子，下一个别的原因造成的卡死依旧会变成永久失效。

结论：读线程的 cancellation 被显式关闭（为了不留"持锁的尸体"，合理），于是
**唯一**的退出途径是它自己走到循环顶看到 `g_gnss_stop`。而恢复路径里所有
"夺回控制权"的手段其实都不起作用：

| 手段 | 实际行为 | 依据 |
| --- | --- | --- |
| `pthread_cancel()` | **静默空操作**。目标 `NONCANCELABLE` 时 `nxnotify_cancellation()` 只置 `CANCEL_FLAG_CANCEL_PENDING` 就返回 `true`，`pthread_cancel` 跟着返回 OK —— 看起来成功，实际什么都没做 | `sched/task/task_cancelpt.c:111`（`ret` 初值 `true`，只有末尾 `else` 分支才置 false）|
| `close(fd)` 唤醒卡住的 poll | **不行**。`poll_notify(POLLERR\|POLLHUP)` 只在 `uart_connected()` 里，且要 `CONFIG_SERIAL_REMOVABLE`；close 路径没有 | `drivers/serial/serial.c:2245` |
| `pthread_join()` | 无超时。线程卡在系统调用里就是永久阻塞 —— 历史上把 diag 钉了 468 s | 本 fork 没有 `pthread_tryjoin_np` / `pthread_timedjoin_np` |
| `g_gnss_stack_busy` | **过期后无人清**。只有读线程正常跑完退出尾部、或成功 join 才会清它；被 cancel / 强杀 / 异常终止时永远停在 1，于是 `spawn` 永远被拒 | `myvendor_gnss.c` 退出尾段 + `gnss_thread_reap()` |

最后一行是"永久失效"的真正闩锁：旧代码在 `reap()` 里**先看 `busy` 再验存活**，
所以"线程早就死了但 `busy=1`"被误判成"还活着" → 永远 `-EBUSY`。

**已做的修复**：用 `pthread_cancel()` 的返回值当**权威存活性判据**
（`sched/pthread/pthread_cancel.c`：`ESRCH` = "does not correspond to any known
thread … probably already exited"，且它**不看 `g_gnss_pid`**，不会被
`gnss_pid_alive(0)` 那种假阴性骗到）。拿到 `ESRCH` 就接管：对象已不可调度，
`join` 立即返回，顺手清掉 `busy` / `has_thread` / `pid` 三个陈旧状态。
另加 `RECOVERY FAILED after N ms` 一次性升级日志，让"GNSS 已经没了"不再静默。

**`g_gnss_pid == 0` 已结案（2026-09-17）**：入口原来用 `getpid()`，而 NuttX 的
getpid() 返回 **task group 的 pid**（`task_getpid.c` → `task_get_info()->ta_pid`）；
读线程从 bringup 上下文建出来，group 就是 0（`ps` 那行的 GROUP 列即 0），所以它
从入口第一行起就是 0 —— 没有任何地方写过 0。入口已改 `gettid()`（线程自己的
tid，`ps` 里的 9），于是按 pid 走的判据（`gnss_pid_alive()`、`gnss_diag_why()`
的 TCB 现场）第一次真正生效；`gnss_pid_alive()` 也改成 `nxsched_get_tcb()` 探测
（`kill(pid,0)` 在 NuttX 没有"探测存在"的语义，见该函数注释）。

**仍未做（需要决策）**：真正的"最后一招"是内核自己在用的强制取消 ——
`sched/group/group_killchildren.c:107` 先 `atomic_or(&rtcb->flags,
TCB_FLAG_FORCED_CANCEL)` 再 `pthread_cancel(pid)`，这个 flag 恰好绕过
`NONCANCELABLE` 保护，并且能把阻塞在 `TSTATE_WAIT_SEM` 的线程用
`nxsem_wait_irq(tcb, ECANCELED)` 直接唤醒（`poll()` / LittleFS I/O / `g_lock`
阻塞时都是这个状态）。代价是线程会在取消点上退出、**不跑协作式清理尾部**，
它那一刻持有的锁会留在原地 —— 正是文件头禁令说的"持锁的尸体"。在 §2.2 修好
之后这条路径应当不可达，所以它值不值得加是个取舍，不是纯收益。

### 2.5 读线程的 CPU 占用：`poll(…, 0)` 自旋是忙等，稳态烧掉一半 CPU

§2.2 把读等待换成"非阻塞探测 + DWT 自旋"之后，卡死问题解决了，但**代价没有
算过**：那是彻头彻尾的忙等。

现场（`ps`）：

```
  PID GROUP PRI POLICY   TYPE    NPX STATE    EVENT          CPU COMMAND
    0     0   0 FIFO     Kthread   - Ready                15.8%   0.0% CPU0 IDLE
    9     0   1 RR       pthread   - Ready                16.2%  51.8% gnss
    8     8 100 RR       Task      - Ready                66.8%  21.3% bicycle_ui
   14     0   9 RR       pthread   - Ready                45.7%  18.2% diag
```

两件事要分开看：

**1）`pri=1` 是 `gnss_yield_all()` 的现场。** 线程 prio 本来是 50，为了不饿死
`diag`(9) / `BT LW WQ`(10)，它定期把自己降到 `SCHED_PRIORITY_MIN`(1)、让出、
再恢复。`ps` 抓到的就是窗口里的 1。

**2）`51.8%` 是忙等本身，不是意外。** `gnss_read_some()` 每次调用带 200 ms 预算，
而 GNSS 的 NMEA 是 **1 Hz** —— 一个 200 ms 窗口里绝大多数时候没有数据，于是每次
都把预算自旋满。拿 `gnss: link` 行验算：行间隔 5 s（`785.73` → `790.75`），窗口里
`rd=23`、`poll_to=16`：

```
16 × 200 ms / 5 s ≈ 64%        ← 与 ps 的 51.8% 吻合
```

**而且这笔买卖是亏的**：`CPU0 IDLE` 是 `0.0%` —— 自旋一次都没让 IDLE 进去。
降到 pri=1 之后 `diag`(9)、`ble_companion`(8)、`btn_poll`(50)、`bicycle_ui`(100)
全排在它前面，IDLE(0) 永远轮不上。所以 `gnss_yield_all()` 只付出了优先级在
50↔1 之间反复震荡的调度开销，没换来任何睡眠。

**为什么会写错**：当时在 `gnss_read_some()` / `gnss_yield_all()` 的注释里写的是
"数据正常流动时根本走不到这里"。**对 GNSS 这句话是错的** —— 1 Hz 输出就是"正常
流动"，所以读线程**永远**在等数据的空转里。同理，§2.2 结尾那句"读线程一定能
在 200 ms 内回到循环顶"只论证了"一定能醒"，没论证这个"一定能醒"要烧多少 CPU。

**修法**：稳态等待改回**阻塞** `poll(…, timeout_ms)`（线程真正睡下去，由 UART RX
中断或超时唤醒），再用 `gnss_reader_wake()` 的 `SIGUSR1` 补齐"一定能醒"这个性质
—— 见 §2.2"已修"一节。超时从"唯一的退出途径"退化成"记账节奏"之后，阻塞就安全了。
同时删掉读等待里的 DWT 自旋（`GNSS_RXSPIN_MAX` 随之移除），`gnss_yield_all()`
只剩 `gnss_sleep_us()` 一条低频调用路径，顺带把它的优先级自我引用也修掉了。

**仍在烧 CPU 的地方（已知，未改）**：`gnss_sleep_us()` 本身还是自旋实现，调用点
都是低频路径（写退避、星历帧间隙、VCC 稳定时间），但
`gnss_sleep_ms(CONFIG_BOARD_L96_GNSS_PWR_DELAY_MS)`（可达 1 s）和
`gnss_sleep_ms(GNSS_RETRY_MS)`（2 s）是实打实的整秒 CPU，出现在空闲唤醒和打开
失败重试上。同样的信号兜底思路对它们也适用 —— 如果实测发现这两条路径占比可观，
照着 §2.2 再处理一次即可。

### 2.6 `diag` 的占用：`lfs_fs_size()` 的全树遍历，把心跳探针变成全盘扫描

`ps` 里 `diag` 常占 10~20% CPU（关掉 INFO/WARN **之后**现场仍是 22.3%），而它
只是个 500 ms 一跳的健康检查线程。

**本节第一版结论是错的**（当时归因给"日志落盘的 SD 忙等"）。下面保留纠正
过程 —— 因为"排除法排除错了"这件事本身比结论更值得记。

`diag_thread()` 主循环：

```c
/* myvendor_diag.c 的 diag_thread() 主循环 */
      diag_tick_one(&g_slot[i], now);   /* 4 个槽：gnss/dvfs/fs/ble */
      myvendor_schedmon_tick();
      (void)myvendor_diaglog_flush();   /* 每 tick 一次，已节流 */
      usleep(500000);
```

**量化闭合**：`DIAG_TICK_MS = 500u`，现场 diag 占 22.3% → 每 tick ≈ **111 ms**
纯 CPU。这个量级既不可能是"写几个块"（那是 I/O 等待，不该记成 CPU），也不
可能是 `nxsched_foreach`（微秒级）。它需要的是"**每 tick 都发生一次的大循环**"。

真正的元凶在 `diag_fs_ok()` → `sf32lb_sd_fs_ok()`：

```c
/* nuttx/fs/littlefs/lfs_vfs.c: littlefs_statfs() */
  ret = littlefs_convert_result(lfs_fs_size(&fs->lfs));   /* ← 遍历整棵元数据树 */
```

`lfs_fs_size()` 要**走遍所有元数据对**才能数出已用块数，**且完全不缓存**。
而 `sf32lb_sd_fs_ok()` 一次判**两个** LFS 挂载点（`/mnt/lfs`、`/mnt/kv`），
每 500 ms 一次 —— 两次全盘元数据遍历/秒 ≈ 2 × ~55 ms = 那 111 ms。

纠正后的排除表：

| 项目 | 实测开销 | 结论 |
| --- | --- | --- |
| `myvendor_schedmon_tick()` | `nxsched_foreach` 遍历 19 个任务 + 拷贝，**微秒级**（`MYVENDOR_SCHEDMON_TASKS`=24 的那个结构有 ~1.3 KB，只解释了 diag 的 `USED=1836` 栈占用） | 不是成本 |
| `diag_gnss_ok()` / `diag_dvfs_ok()` / `diag_ble_ok()` | 读变量 / 心跳 + 一个 mutex，微秒级 | 不是成本 |
| **`diag_fs_ok()`** | **两个 `littlefs_statfs()`，每个都是一次全树 `lfs_fs_size()`，不缓存 → 每 tick ~110 ms** | **就是这个** |
| `usleep(500000)` | 走 `clock_nanosleep`；`pthread_kill` 走 `tkill` 只打单线程，也打不到 diag，不会提前返回 | 节奏是准的 |
| `myvendor_diaglog_flush()` | `open` + `write` + `close`，**已节流且环常为空** | 次要 |

**第一版为什么会错**：原表里那句"`diag_fs_ok()` 是 3 次 `statfs`（基本走
RAM）"是个**没验证的假设**。只看 FAT 它是对的 —— `fat_nfreeclusters()` 算过
一次就把结果存进 `fs_fsifreecount`，之后 O(1)。但 LittleFS 没有这个缓存，
`lfs_fs_size()` 每次都重算。**同一个 API 名字，两个文件系统的复杂度差了一个
数量级**；凭"API 叫什么"推代价，就是这么翻车的。

**已做的修复**（一条主修 + 两条次要）：

1. **主修：常态探针从 `statfs` 换成 `stat`**。`sf32lb_sd_fs_ok()` 改成两级：
   常态只 `stat()` 三个挂载点根（LFS 命中根 inode、FAT 读根目录簇，都 O(1)
   且吃块缓存），**一个 tick 零全盘遍历**；只有 `stat` 已经读不出来时才升到
   `df` 口径（`statfs`）确认，用来把"卡坏了"和"LFS 实例坏了"分开。这正是
   mount 之后那次回读验证用的手法。**判据没变，代价降了一个数量级。**
2. **次要：落盘节流**。`diag_thread()` 不再每 tick 都刷，改成 2 s 一次或环内
   积压到 `DIAGLOG_FLUSH_HIWATER`（1 KB）时提前刷（新增
   `myvendor_diaglog_pending()`，纯内存读、无 I/O）。环是 2048 B，INFO/WARN
   关闭后正常顶不到环满；**这是省 SD 写和卡寿命的做法，不是 CPU 的解**。
3. **次要：SD 忙等补 yield**。`sd1_wait_write()` / `sd1_wait_card_idle()` 原先
   **一句 yield 都没有**，而 `sd1_wait_dat0()` 每 1 ms 会 `sched_yield()` + 喂狗。
   三者现在统一走新增的 `sd1_spin_yield()`。这不降低 CPU 数字（轮询本身还是
   CPU），但消除了一个真实隐患：diag 是 pri=9，卡一慢它就在纯自旋里最长待
   2 s，**优先级低于 9 的任务全被饿住 —— 包括 `ble_companion`(pri=8)**，
   这正是「连上了但未连接」那类问题的同源形态。

**注意**：`CONFIG_SYSLOG_DEFAULT_MASK=0xf` 只关掉 syslog 那条路，对 diag 占用
**没有决定性作用** —— 关掉之后实测仍是 22.3%。（`LOGI` 就展开成
`syslog(LOG_INFO,…)`；而 `printf` 那条路压根没进 syslog：
`CONFIG_SYSLOG_CONSOLE` / `CONFIG_SYSLOG_CHAR` / `CONFIG_SYSLOG_REDIRECT`
一个都没开。）**别再把这两个数混在一起看。**

**补充：运行时的档位现在会记忆。** `CONFIG_SYSLOG_DEFAULT_MASK` 只是"从没调过"
时的起点；`ctl log err|warn|notice|info|all|silent|<0xNN>` 设的档位会写进
`persist.log.mask`（`/mnt/kv/db/persist.log.mask`，十进制存的八位屏蔽位），
开机由 `myvendor_devctl_load()` 在 apply 其它 persist 之前 `nx_setlogmask()`
装回去 —— 不用再每次上电重敲。

两条边界要知道：

1. **`storage` 那一步之前的十几步仍按编译期默认走**（watchdog/vfs/rtc/lcd…）。
   KV 那时还没挂，读不到 `persist.log.mask`。
2. **工厂固件不读也不写这个 key**。产品固件里用户压到 `0xf`，工厂要全开，
   读 KV 会把产测的日志一起压掉；反过来工厂若回写 `0xff`，会跟着 KV 带到
   产品上变成刷屏。跟 `persist.usb.mtp` / `persist.bt.radio` 是同一条理由。

`ctl log` 不带参数打印当前档位和它所在的 KV key（NuttX 没有 `nx_getlogmask()`，
打印的是开机载入值或本进程最后一次 set 的快照）。`persist.log.mask` 写失败
**不回滚**已生效的档位，只在 `ctl log` 输出里多一行 warning。

#### 2.6.1 同一处病的第二个病例：UI 系统页的磁盘栏

修完 diag 后全仓搜 `statfs(` 又抓到一个，性质相同但**处置不同**：

- `ui/bicycle/src/lvgl_page/helm_menu.c` 的 `helm_sys_refresh_disks()`
  原先**每 2000 ms** 对 `/mnt/lfs`、`/mnt/fat`、`/mnt/kv` 各调一次 `statfs()`
  —— 又是两个 LittleFS 卷，即每 2 秒在 **UI 线程**上白烧约 110 ms
  （现场 `bicycle_ui` 占 22.0%，这是其中一大块）。
- `helm_toolbox.c` 的 `sys_disk_refresh_one()` 也调 `statfs()`，但它是**一次性**的
  （`sys_disk_refresh_all()` 只填 `!have` 的槽，页面显示时才复位），**不用改**。

**为什么这里不能照搬 diag 的改法**：diag 要的是"还能不能用"这个布尔量，`stat`
就够；而这一页要显示的就是**容量**，`statfs` 是必须的。所以改的是**频率**而不是
探针 —— 刷新周期从 2 s 放宽到 `HELM_SYS_DISK_MS`（30 s）。容量本来就变得慢，
且进页面的路径用的是 `force=true`，首屏数字仍是即时的。

**这条要记住的反面**：周期性的 `statfs` 在本卷上是个"定时炸弹"式的写法 ——
写的人以为它是个便宜的系统调用，实际是每 tick 一次全盘遍历。**任何周期性
路径要用容量之前，先想清楚它多久跑一次。**

### 2.7 长写盘被当成"线程停摆"：心跳只在主循环顶上喂一次

现场（一次 388 KB 的 GPX 经 BLE 导入）：

```
ble_companion: fs write close acked …/20260529户外骑行.gpx 387918 bytes (flush deferred wbuf=260942)
ble_companion: fs write flushed 131072/387918
…
[  451.487955] [ ERROR] diag: ble not ok (thread) hb=15247 alive=1 skip=0 rxstall=0 busy=0 phase=fs phone=0
[  455.933458] [ ERROR] diag: ble not ok (thread) hb=19693 …
[  455.936199] [ ERROR] diag: ble restart (unhealthy 4340 ms, n=1)
[  455.937108] [ ERROR] diag: ble companion restart (thread stalled)
[  457.590102] [ ERROR] ble_companion: SIGKILL pid=31
ble_companion: fs write commit …/20260529户外骑行.gpx 387918 bytes leftover=260942 18751ms
[  457.810057] [ ERROR] sf32lb52 bt force reset (LCPU)
[  471.730279] [ ERROR] ble_companion: adapter not ready, state=0 (expect ON or BLE_ON)
[  471.730279] [ ERROR] ble_companion: bt_gatts_register_service failed: 2
[  479.840106] [ ERROR] ble_companion: still alive pid=50, skip respawn
```

**"stopped" 的其实是"在忙"。** 判据是 companion 主循环的心跳
（`companion_bridge_heartbeat()`），而它**只在 `ble_companion.c` 主循环顶上喂
一次**。CLOSE 已应答之后的延迟补写 `fs_deferred_commit()` 跑在同一个线程上，
它调的 `wbuf_flush()` 是个**同步排空**循环（`while (g_fs.wbuf_len > 0)`），
中间不回主循环。

**量化闭合**：`FS_WBUF_SIZE` = 256 KiB，`FS_FLUSH_CHUNK` = 4 KiB → 64 块；
`fs write commit … 18751ms` ÷ 64 ≈ **293 ms/块**（`wbuf_yield()` 里的
`usleep(2000)` 只占 2 ms，其余是 LittleFS 在 SD 上的写）。而
`DIAG_BLE_THREAD_MS` = 15000 —— **任何超过写缓存（256 KiB）的文件都必然触发**，
`388 KB` 只是把它撞出来了。时间线也正好对上：最后一次心跳 ≈ 438.8 s
（451.49 − 15.25），kill 在 457.59，差 18.8 s ≈ 那次 commit。

| 判据 | 值 | 说明 |
| --- | --- | --- |
| `DIAG_BLE_THREAD_MS` | 15000 | `diag_ble_ok()` 的心跳年龄上限 |
| `g_ble_ops.fail_ms` | 4000 | 连续判坏 4 s 才动手（现场 `unhealthy 4340 ms`） |
| 实测排空耗时 | ~19 s | 15000 + 4000 = 19 s，正好在 commit 结束前后越线 |

**已做的修复**：心跳在 `wbuf_yield()` 里续上。它是三个排空循环
（`wbuf_flush()` 的延迟补写、`wbuf_append()` 的缓存溢出与边收边写）共用的
"让出点"，一处覆盖三处。

**为什么这不算削弱 diag**：`diag_ble_restart()` 那段注释写明它要抓的是
"companion 堵在不可中断 HCI 等待上"。那种情况下这个循环根本不在跑，也就没人
喂心跳 —— 判据该响的时候照样响。**"忙" 与 "停摆" 的区别，就是循环在不在推进**。

**这条要记住的**：`companion_bridge_heartbeat()` 的语义是"主循环在推进"，
不是"主循环在转"。**任何会把主循环一次占用到接近 `DIAG_BLE_THREAD_MS` 的
同步工作，都要在自己的让出点上续心跳**，否则 diag 会把它当成 §2.4 那种卡死
而 SIGKILL —— 杀在一个正在写盘的线程上。

**这一节的边界（同日已补）**：被 kill 之后的**恢复路径本身不自愈**，是另一个
独立问题，与 GNSS 的 §2.4 同形。两处都已修：

- **`skip_sync` 闩锁（`chips/sf32lb52/sf32lb52_bt_adapter.c`）**。旧写法
  `if (en >= 0 && !keep_skip)` 把清 skip 挂在 enable 的返回值上，于是
  「enable **压根没被调用**」这条路（`status` 既不是 ENABLED 也不是 INITED
  —— 已被 `sf32lb52_bt_controller_deinit()` 降到 IDLE，`en` 保持初值
  -EIO）会把 skip 永远留着。现场判据正是这个形状：reset 之后每一行都是
  `skip=1 open=0 status=0`，而 reset 前一行（455.93）还是 `status=2`。
  现在**无条件**还回去（除非调用方明确要求保持），并明确报出
  `status=N (not INITED), enable skipped` —— 「enable 失败」和「根本没人可
  enable」不再是同一行日志。挡住误发的本来就是 `host_send_packet()` 里的
  `status != ENABLED || !queue_open` 两道判据，不需要 skip 兜底。
- **"skip respawn" 不再永久放弃（`bsp/bringup.c`）**。僵尸任务卡在不可中断
  等待上时，`SIGTERM`/`SIGKILL`/`task_delete` 都要等它回到取消点才生效，
  而原先是纯 `return -EBUSY`：僵尸不退出 → 下次照样卡同一处 → BLE 永久停在
  「skip=1 + 没有 companion」，手机再也搜不到码表，且没有任何东西会再试。
  现在这条路上会 spawn 一个**独立的回收任务** `ble_reclaim`
  （`CONFIG_MYVENDOR_BLE_COMPANION_AUTOSTART` 内，static 2 KiB 栈），
  每 `BLE_COMPANION_RETRY_MS`(30 s) 重走一次三连杀 + spawn；`stuck_n` 归零
  （真的起来了）就退出。刻意不走 LPWORK：`board_restart_ble_companion()`
  自己最多阻塞 3 s 等三连杀，压在共享工作队列上会连累别的延迟工作；也刻意
  不依赖 diag 那条 20 s 路径 —— 它在，但没人保证它一定会被调到。
  另外每 `BLE_COMPANION_RETRY_LOG_N` 次打一行
  `stuck pid=… N attempts; BLE down until it exits`，不再"静默失败"。
  计数只在 `board_start_ble_companion_async()` 真正起来一个任务时清零，
  所以日志里的次数是可信的。

还没查清的一点（留作线索）：现场 `status` 从 2 掉到 0 是在 companion 被
SIGKILL + teardown 那 1.9 s 内发生的，唯一会写 IDLE 的地方是
`sf32lb52_bt_controller_deinit()`，但它的调用者还没定位 —— 手里有
`deinit` 被谁调的日志时，这条可以结掉。

### 2.8 notify 卡死：一条"杀不掉"的不可中断等待（2026-09-17 现场）

一次"连续异常"的完整形态：`rxage` 一路涨到 100 s+、`rxf=0`（收包一次都没
失败）、`skip=1`、`still alive pid=47, skip respawn` 每 30 s 循环，直到重启。

```
[  620.345] diag: ble not ok (thread) hb=15495 alive=1 skip=0 rxstall=0 busy=0 phase=notify phone=0
[  624.459] ble_companion: stall dump pid=47 phase=notify
  #0 nxsched_switch
  #1 nxsem_wait_slow / #2 nxsem_wait / #3 k_poll / #4 k_queue_poll
  #5 net_buf_alloc_len / #6 net_buf_alloc_fixed / #7 net_buf_alloc
  #0 bt_l2cap_create_pdu_timeout / #1 bt_att_chan_create_pdu / #2 bt_att_create_pdu
  #3 gatt_notify / #4 bt_gatt_notify_cb / #5 gatt_send_notification
  #3 companion_tick at ble_companion.c:2214
[  626.670] ble_companion: task_delete pid=47
[  627.785] ble_companion: still alive pid=47, skip respawn (attempt 1)
```

逐环（都能在代码里对上）：

1. **562 s 控制器停止回事件**：`rxevt=0x13` 停住，`0x13` 正是
   Number Of Completed Packets（还 buffer 的那个事件）。
2. **没有事件 → 没有 `le_disconnected()` → `g_ctx.connected` 一直是 1**。
   注意 `companion_bridge_phone_get()` 是**按心跳**判活的（见它的注释），
   所以它报 `phone=0` 而 `companion_tick` 仍然进了 notify 分支 —— 两者不是
   一回事，排查时别把 `phone=0` 当成"链路已断"。
3. **状态心跳照发**：每 `COMPANION_STATUS_HEARTBEAT_SEC` 一次 notify，每次从
   L2CAP/ATT 池借一个 buffer；buffer 只有控制器回 Number Of Completed
   Packets 才会归还 → 池子被抽干。
4. **最后一次 notify 永久阻塞**：`att.c` 的 default 分支给的是
   `timeout = K_FOREVER`，`l2cap_internal.h` 里 `bt_l2cap_create_pdu` 这个宏
   展开就是 `bt_l2cap_create_pdu_timeout(..., K_FOREVER)`，一路到
   `k_sem_take(K_FOREVER)` → `nxsem_wait_uninterruptible()`。
5. **这个等待杀不掉**（不是"没杀干净"，是根本杀不动）：
   - `task_delete()` 先走 `nxnotify_cancellation()`；因为阻塞的是内部
     `nxsem_wait` 而不是 libc 的 `sem_wait`，`tl_cpcount == 0`，函数只挂一个
     pending cancel 就返回 **true** → `nxtask_terminate()` 根本没被调用，
     而 `task_delete` **返回 0（成功）**。日志里那行 `task_delete pid=47`
     是"成功"的，什么也没干。
   - 本 build 没开 `CONFIG_SIG_DEFAULT`，SIGKILL 没有默认终止动作。
   - `nxsem_wait_uninterruptible()` 对 EINTR 是 `do { } while (ret == -EINTR)`，
     就算被唤醒也会立刻睡回去。
6. **于是 LCPU 永远不复位**：`board_restart_ble_companion()` 里
   `sf32lb52_bt_controller_force_reset()` 在**成功分支**（杀干净之后），走不到；
   而 `skip_sync` 是杀之前置的 → `host_send()` 对一切命令返回 `-ENODEV`，
   BLE 直到重启都回不来，`ble_reclaim` 每 30 s 是空转。

   旁证：`ble_sensor` 在 `578.79 scanning to reconnect` 之后 **90 秒没有任何
   日志**。`bt_hci_cmd_create()` 用的是同一个
   `net_buf_alloc(&hci_cmd_pool, K_FOREVER)`（`hci_core.c:386`，注释里已经
   承认过这个坑），所以它是同一个坑里的第二个受害者，不是"扫不到设备"。

对策：

| # | 位置 | 做了什么 |
| --- | --- | --- |
| ① | `vela_override/zblue/patch_net_buf_timeout.py`（configure 期改写成副本再编译，**上游 zblue git 不动**，同 `patch_hci_core.py`） | 池空且调用方要 `K_FOREVER` 时改成 `K_NO_WAIT` 轮询 + 2 s 上限；`skip_sync` 置位期间立即放弃。返回 NULL → `bt_gatts_notify` 报错、`companion_tick` 继续跑、心跳不断 |
| ② | `bringup.c` 的失败分支 | 僵尸收不回来**也**执行 `sf32lb52_bt_controller_force_reset()`：它等的 buffer 要靠控制器回 Number Of Completed Packets 才归还，拉 LCPU 是唯一能让它走回取消点、自己退出的动作（`nxnotify_cancellation()` 已经把 pending cancel 挂在它身上） |
| ③ | `bringup.c` 的失败分支 | `BLE_COMPANION_GIVEUP_SKIP_N`(3) 轮之后撤回 `skip_sync`（和 bridge 的 teardown skip）：僵尸该卡还是卡（它卡的不是这个），但 `ble_sensor` 重连、手机重连这些**与 companion 无关**的通路还能自己救回来 |
| ④ | `sf32lb52_bt_adapter.c` / `myvendor_diag.c` | stall dump 多打一行 `sf32lb52 bt ring wr= rd= pend= size= oktxage=`；diag 健康判据多一条 `cmdsilent`（命令**成功发出**后 `DIAG_BLE_CMD_MS`(4 s) 内没有任何事件回来），空闲安全 |

新那行 ring 是**区分两种起点**的唯一判据：

- `wr` 在走、`rd` 不动 → LCPU 在写，是主机侧 RX 抽取链断了
  （`rx_worker_running` 闩死 / HPWORK 被占）；
- `wr` 不动 → LCPU 根本没往环里写，控制器/IPC 侧静默。

`oktxage` 与 `txage` 的区别：后者在"尝试发送"时就盖章（失败了也算），前者
只在 `host_send()` 成功返回后盖章，`cmdsilent` 用后者，避免 TX ring 满这种
发送失败被误判成控制器哑。

① 的两条诊断**走 `syslog`，不是 zblue 的 `LOG_ERR`**：

```
myvendor net_buf: pool %p abandoned (recovering)     # skip_sync 置位期间放弃
myvendor net_buf: pool %p empty for %u ms, giving up # 池空到上限
```

本 build 没开 `CONFIG_LOG`，zblue 的 `LOG_*` 会被整个折叠掉 —— 打完补丁的固件
里连格式串都搜不到（`strings nuttx | grep 'myvendor net_buf'` 可验），
`patch_hci_core.py` 里那些 `LOG_ERR` 同理一直是哑的。所以这两条用 NuttX 的
`syslog()`（`extern` 声明，不 include `<syslog.h>`，避免它的 `LOG_*` 宏和
zephyr 的打架）。

还没查清的（线索）：**562 s 那个起点**。同一秒附近有

```
[  563.710] wdog: clipped corrupt g_wdactivelist (1) bad=2006e740 n=20000520 p=200084c8 walk=0 rescued=7
```

那是 `vela_override/sched/wd_start.c` 在剪环修复，而 `patch_hci_core.py` 的
注释里早就写过"看门狗链被剪会丢超时"。下次复现先看上面那行 ring 分清 LCPU 哑
还是主机断，再对 `wdog: clipped` 与 `rxn` 冻结的先后。

### 2.9 读线程卡 20 s：`wd=0` + `in_poll` 一直涨 = 超时看门狗丢了（2026-09-19 结案）

现场（2069~2096 s 那段）：

```
gnss: diag not ok (nmea-age) extra=8287 phase=read-poll fd=7 … hb_age=7650
gnss: diag tcb pid=52 st=5 lock=0 wait=0x20021ed4 wd=0 on_rdy=0 flink=0x20064518
gnss: diag uart tx_hold=-1 rx_hold=-1 open_cnt=2 setup_n=1 shutdown_n=0 last_shutdown_pid=0
…
gnss: reap rd self=34 entered=1 seq=264886 poll_n=33401 in_poll=20138 last=205 ms
gnss: reader exit stop=1 … poll_to=1 poll_err=0 wr_fail=0 gap_max=883 loop_max=20140 stall1s=0 poll_close=863
```

判读（与 §2.2 是同一件事，这次拿到了直接证据）：

| 证据 | 说明 |
|---|---|
| `in_poll=20138`、`loop_max=20140` | **一次 `poll()` 进去 20 s 没出来**（不是"多轮慢"） |
| `last=205 ms` | 之前每一轮 poll 都是正常 205 ms 到期 —— 超时机制平时是好的，**只有这一次失效** |
| `st=5` + `wait=0x20021ed4` | 线程在等一个信号量（`poll()` 里那个栈上的 `sem`，见 `fs/vfs/fs_poll.c:457`） |
| **`wd=0`** | `WDOG_ISACTIVE(&tcb->waitdog)` = 假 —— **超时看门狗没在跑** |
| `tx_hold/rx_hold=-1` | 串口驱动那两把 `nxmutex` 没人持有 ⇒ **不是** `uart_poll()` 被锁住那条路 |
| reap 后 1.6 ms 就 `reader exit` | **信号这条路是通的** —— 叫得醒，只是超时不会自己到 |

机制：`poll(…, 200)` 的超时在本 fork 里就是"在调用线程 TCB 的 `waitdog` 上挂一个定时器"
（`nuttx/sched/semaphore/sem_tickwait.c:87` `wd_start(&rtcb->waitdog, …)`）。`wd=0` 说明这个
看门狗**没挂上/已被剪掉** —— 而 `g_wdactivelist` 断链正是本板已知问题（`vela_override/sched/wd_start.c`
的"断链不崩、剪环保命"，见上面那行 `wdog: clipped`）。⇒ **超时永远不会到点**，线程只能等数据或信号。

后果链：poll 不出来 ⇒ 心跳不更新 ⇒ diag 8 s 判"停顿" ⇒ 12 s 走线程重启（`diag-thread-stall`）⇒
VCC 周期 + 重新配置（那次重启后第一轮 `cfg-nav` 只等了 185 ms、模组还没起来，`cfg ack … 全 -1`，
于是 `fails=2` 又触发一次 kick 才成功 —— 属于重启时序问题，不是新故障）。

**已落地的止损（2026-09-19）**：把读线程接到 [diag.md](diag.md) §4.1 的**诊断时基**上 ——
diag 每 500 ms 看一次读线程心跳，超过 `1500 ms` 没动就发一次 `SIGUSR1`（就是 `gnss_reader_wake()`
那条路），线程 `-EINTR` 回主循环重新 poll，**不重启线程、不断电、没有十几秒断档**：

```
diag: watch gnss-reader on (stall=1500 ms sig=10)
diag: kick gnss-reader (no progress 1543 ms, kicks=1) rc=0     # 卡住时才会出现
```

读线程侧对"已知在等"的段落（上电/开串口整段、VCC 周期、重配 + 灌注、开串口失败重试）用
`myvendor_diag_watch_hold()` 明说"我在忙"，避免被误踢（被踢不会崩，但 `gnss_sleep_ms()` 会按
EINTR 提前返回，模组上电稳定时间就不够了）。**开机实测**：没 hold 时上电段被踢过一次
（2.19 s→3.73 s），补上之后同一次开机 0 次 kick。

**没解决的（真正的地基）**：`tcb->waitdog` 的泄露/被剪本身 —— 也就是
`nuttx/sched/task_exit.c` 不清 `tcb->waitdog` 那笔账（见 `required_patches.md`）。时基只是把它
从"卡 20 s 再重启"压成"卡 1.5 s 踢一脚"。

---

## 3. 建议的取日志方式

```bash
# 只看恢复时间线
grep -E 'gnss: rt' log

# 只看注入取证
grep -E 'gnss: eph' log

# 只看判死原因
grep -E 'gnss: silence trig|gnss: vcc recover' log

# 蓝牙协商 + 丢包 + 会话画像
grep -E 'bth4 (conn|conn_upd|sess|fwd fail|dlen)' log

# 断连配对（看有没有丢事件）
grep -E 'hci_disc|gatts_disc|phone disconnected' log

# 通知对账
grep -E 'fs_disc|fs notify complete' log

# 非持有者解锁
grep -E 'mutexbad' log

# 控制器静默 / notify 卡死（§2.8）
grep -E 'bt stall|bt ring|cmdsilent|skip respawn|not reapable|still alive' log
```
