# 外设诊断线程（diag）

开机后单独跑一条 **`diag` 线程**，按注册表轮询外设是否健康，不健康则调用该外设自己的 `restart`。  
它不跑 HCI、不读 UART、不进 HPWORK。Companion 堵在同步命令上时，这条线程仍能判定并重启。

HCI RX `-ENOMEM` 的因果见 [ble/companion_impl_plan.md](ble/companion_impl_plan.md) §7.4；串口 FAQ 见 [bluetooth_guide.md](bluetooth_guide.md)。本文只写 **diag 本身**。

---

## 1. 为什么要单独一条线程

`ble_supervisor` 写在 `ble_companion` 主循环里。HCI 一堵：

1. companion 卡在 `bt_adapter_disable` / `start_adv` 等 Command Complete；
2. `companion_supervisor_pump()` 进不去，adapter cycle **请求不出来**；
3. `sf32lb52_bt_controller_disable()` 若 `work_cancel_sync(HPWORK)` 永久等 RX worker，连 cycle 自己也钉死；
4. HPWORK 空转 `-ENOMEM` 时还会饿死 GNSS UART。

GNSS 自己的「无 NMEA 则重开 UART」跑在 GNSS 读线程里。读线程若堵在 dump / 注入 / `poll` 之外的路径，这条自恢复也到不了。

diag 的职责：用 **独立优先级、独立栈** 做看门，业务线程死了也能踢一脚。

```text
                    ok() / restart()
  ┌─────────┐     ┌──────────────────┐
  │  diag   │────►│  ble  注册表槽   │──► companion cycle / LCPU 复位
  │  500 ms │     ├──────────────────┤
  │  prio 9 │     │  gnss 注册表槽   │──► PA43 断电 + 读线程 kick
  └─────────┘     ├──────────────────┤
                  │  dvfs 注册表槽   │──► gov worker 心跳超时则 kick 回来
                  ├──────────────────┤
                  │  fs   注册表槽   │──► 重挂坏掉的 LittleFS
                  ├──────────────────┤
                  │  （最多 8 槽）   │──► 其它外设自行 register
                  └──────────────────┘
                         ▲
                         │ companion 仍在跑时
                         └── 只投递 cycle，真正 disable/enable 仍在 companion
```

当前注册了 **4 个槽**（`ble` / `gnss` / `dvfs` / `fs`），`myvendor_diag_register`
被调用的次数可用反汇编确认（`myvendor_diag_start` 里应看到 4 次调用）——
**曾经在替换相邻代码块时误删过 GNSS 的注册**，所以这是个值得复查的点。

| 槽 | `ok()` | `restart()` | 超时 / 冷却 | 详见 |
| --- | --- | --- | --- | --- |
| `ble` | companion 线程是否还在推进 | 复位 LCPU / 投递 cycle | — | [ble/companion_impl_plan.md](ble/companion_impl_plan.md) |
| `gnss` | 读线程心跳 | PA43 断电 + 读线程 kick | 12000 / 30000 ms | [gnss_ble_diag_log.md](gnss_ble_diag_log.md) |
| `dvfs` | `sf32lb_dvfs_gov_stat()` 的 `age` 是否小于 `SF32LB_DVFS_GOV_STALE_MS` | `sf32lb_dvfs_gov_kick()` | 2000 / 10000 ms | [dvfs.md](dvfs.md) §4.4 |
| `fs` | `sf32lb_sd_fs_ok()`（拿 FAT 当"卡还活着"的证据） | `sf32lb_sd_fs_heal()`（重挂） | 3000 / 15000 ms | [sd_recovery.md](sd_recovery.md) §5 |

IWDT（`iwdg_feed`，优先级 180）管的是整机死锁复位，**不管**「蓝牙还能跑、只是 HCI 环满了」。diag 不管整机复位。

---

## 2. 代码与启动

| 路径 | 作用 |
|------|------|
| `boards/sf32lb52/helmone/include/myvendor_diag.h` | `myvendor_diag_ops_t`、`register` / `start` |
| `boards/sf32lb52/helmone/services/myvendor_diag.c` | 线程、默认注册 `ble` / `gnss` / `dvfs` / `fs` |
| `boards/sf32lb52/helmone/bsp/bringup.c` | `g_bringup[]` 最后一步 `"diag"` |
| `boards/sf32lb52/helmone/services/CMakeLists.txt` | 编进 `libboard` |

**主循环里还有一件事**：每拍调一次 `myvendor_diaglog_flush()`，把诊断日志环刷到
`/mnt/kv/diag/`。位置刻意放在**主循环体**里，而不是下面那个被 25 s 宽限门控的槽位遍历里 ——
否则开机头 25 秒的日志永远不会落盘。也刻意**只在 diag 线程**刷：出错点往往正持着
`g_sd_lock`，在那里刷会自锁。详见 [diag_log.md](diag_log.md)。

`bringup` 顺序：`… → gnss → … → bth4 → ble → diag`。  
`myvendor_diag_start()` 可重复调用；已经起来则直接返回 0。启动失败只打日志，**不中止** bringup。

---

## 3. 线程参数

| 项 | 值 | 说明 |
|----|-----|------|
| 名字 | `diag` | `pthread_setname_np` |
| 栈 | 3072 | 静态数组，16 字节对齐 |
| 优先级 | **9** | 高于 `ble_companion` 默认 8，低于 BT long WQ（10）和 IWDT（180） |
| 节拍 | 500 ms | `usleep`，不占 CPU |
| 开机宽限 | **25 s** | 从线程起来算；过完才开始 `ok()` |
| 槽位数 | 8 | 同名再注册则覆盖 ops |

优先级可用 `CONFIG_MYVENDOR_DIAG_PRIORITY` 覆盖（当前未进 Kconfig，改宏或以后加配置项）。  
HPWORK 仍高于 9：RX worker 必须自己 `usleep` 让出，diag 才能在 `-ENOMEM` 风暴里跑起来。

`ok()` / `restart()` 在 diag 线程里直接调用。**不要**在这两个函数里做长时间阻塞（GNSS 断电约 1.5 s 是已知例外；期间其它槽要等这一拍结束）。

---

## 4. 注册表

```c
typedef struct myvendor_diag_ops_s
{
  const char *name;     /* 日志名，唯一键 */
  bool (*ok)(void);     /* true = 健康 */
  int  (*restart)(void);/* 恢复；可含断电 */
  int  (*start)(void);  /* 可选，tick 不用 */
  int  (*stop)(void);   /* 可选，tick 不用 */
  uint32_t fail_ms;     /* 连续不健康多久才 restart；0 → 5000 */
  uint32_t cooldown_ms;/* 两次 restart 最短间隔；0 → 15000 */
} myvendor_diag_ops_t;

int myvendor_diag_register(const myvendor_diag_ops_t *ops);
int myvendor_diag_start(void);
```

`ok` 和 `restart` 必填，`name` 必填。同名覆盖，便于工厂/调试替换实现。  
`start` / `stop` 留在表里是为了外设自己持有一套完整方法（例如将来 NSH `diag ble stop`）；**当前 tick 只调 `ok` / `restart`**。

每拍对每个槽：

1. `ok() == true` → 清 `fail_since`，本槽结束。
2. 第一次失败 → 记下 `fail_since`，打 `diag: <name> unhealthy`。
3. 连续失败未满 `fail_ms` → 等。
4. 距上次 `restart` 未满 `cooldown_ms` → 等。
5. 否则打 `diag: <name> restart` 并调 `restart()`，重置 `fail_since`（仍不健康则会在 cooldown 后再来一次）。

宽限期内不调用 `ok()`，避免开机搜星、开广播当成故障。

---

## 4.1 诊断时基（watch）：把**卡在等待里**的线程踢回来（2026-09-19 落地）

上面那套 `ok()`/`restart()` 管的是"不健康就重启" —— 代价是重启本身（GNSS 那条要断电重启模组，
十几秒没有定位）。而现场更常见的是**线程还活着、只是卡在某次等待里**：这时最便宜的恢复不是重启，
而是**踢它一脚**。diag 是唯一一条只做判定、不跑业务循环的线程（`DIAG_TICK_MS` 500 ms），
所以由它来提供这个时基。

```c
/* 客户端：注册一次，给一个"我在动"的进度时间戳 */
myvendor_diag_watch("gnss-reader", pthread_self(), GNSS_WAKE_SIG,
                    MYVENDOR_DIAG_WATCH_MS_DEFAULT, gnss_reader_progress_ms);
myvendor_diag_watch_hold(pthread_self(), true);   /* 已知会长时间不更新进度时 */
...
myvendor_diag_watch_hold(pthread_self(), false);
myvendor_diag_unwatch(pthread_self());            /* 线程退出前 */
```

- 进度回调**必须无锁、无阻塞**（一次 `volatile` 读即可），由 diag 每拍调用；超过 `stall_ms`
  没动就 `pthread_kill(tid, sig)`。踢的日志：`diag: kick <name> (no progress <ms> ms, kicks=N) rc=…`。
- 同一个"没进展"窗口最多踢一次（踢完等它动），日志再节流到 5 s；表 4 项，同一 `tid` 重复注册 =
  更新，表满 `-ENOSPC`；**不受 25 s 宽限期门控**（踢是便宜的，开机也可能需要）。
- **只发信号能解决的才有用**：收方要自己处理 `-EINTR`（handler 要装、且**不能**设 `SA_RESTART`）。
  卡在 `nxmutex_lock()` 那种信号免疫的等待里，仍然只能靠重启路径。
- 为什么需要它（**不是**"超时设太长"）：本 fork 里 `poll()`/`usleep`/`nxsem_tickwait` 这类超时都挂
  在**调用线程 TCB 的 `waitdog`** 上（`nuttx/sched/semaphore/sem_tickwait.c:87`），而
  `g_wdactivelist` 会断链、板级抽换件 `vela_override/sched/wd_start.c` 的策略是"断链不崩、剪环保命"
  —— 被剪掉的节点就是**永不触发的超时**。2026-09-19 实测：GNSS 读线程因此卡在 `poll()` 里 20 s
  （diag 行 `wd=0` + `in_poll` 一直涨 + 心跳停），最后靠 12 s 的线程重启救回来。详见
  [gnss_ble_diag_log.md](gnss_ble_diag_log.md) §2.9。
- **已知会长时间不更新心跳的地方必须 `hold`**，否则会被踢（踢了不崩，但会让 `gnss_sleep_ms()` 提前
  返回，模组上电稳定时间就不够了）：GNSS 读线程现在是"上电/等模块/开串口"整段、VCC 周期、
  重配（等 7 个 cfg ACK + 灌注星历）、开串口失败的重试间隔。**开机那次实测**：没 hold 时
  2.19 s→3.73 s 的上电段被踢了一次；补上 hold 后同一次开机 0 次 kick。
- 目前用户：GNSS 读线程（`stall=1500 ms`，即 3× 它的 200 ms poll 预算，远小于 8 s 的停顿阈值）。
  以后别的线程（BLE companion、MTP/传输、UI flush）照这个注册即可。

---

## 5. 已注册：BLE

条件：`CONFIG_BLUETOOTH`。

| | |
|--|--|
| `ok` | companion 心跳 15 s 内新鲜，且 `sf32lb52_bt_hci_rx_stalled(3000)` 为假，且 `sf32lb52_bt_hci_cmd_stalled(4000)` 为假。成功投递会清 stall 窗口。心跳门槛长于 zblue 10 s HCI 超时，避免一条同步命令被当成线程死掉。 |
| `fail_ms` | 4000 |
| `cooldown_ms` | 20000 |
| 第一次 `restart` | 进程还在、主循环也还在：`companion_bridge_diag_cycle_post()`。进程还在但心跳已停：`sf32lb52_bt_controller_force_reset()`（先解开堵在 Command Complete 上的 worker），并同样 post cycle，companion 醒过来后走正规 adapter cycle。 |
| 第二次（仍不健康） | PID 已没了，或硬复位后心跳仍停：`board_restart_ble_companion()`。先 skip-HCI + LCPU 复位再 SIGTERM；**不要**在 HCI 飞行中 `task_delete`。 |
| 禁止 | 心跳超时就立刻 SIGTERM。那会再排队 `bt_le_adv_stop`，10 s 后 `z_fatal_error` assert，之后扫描/广播都起不来。 |

两条 stall 判据是互补的，别只留一条：

| 判据 | 看的是 | 现场特征 |
|--|--|--|
| `rxstall`（`hci_rx_stalled`） | RX 回调**失败**（`-ENOMEM` 等）已持续 3 s | `rxf` 在涨、`sf32lb52 bt rx callback:` 在刷 |
| `cmdsilent`（`hci_cmd_stalled`） | 命令**成功发出**后 4 s 内**一个事件都没回来** | `rxf=0`、`rxage` 飞涨、`oktxage` 也跟着涨 —— 控制器彻底静默 |

`cmdsilent` 用的时刻是 `host_send()` **成功返回**才盖章的那一个，所以 TX ring
满这种发送失败不会被算成控制器哑；空闲（没连接、没扫描）时不会有命令，也就
不会误报。2026-09-17 那次控制器静默只有第二条能看见 —— 见
[gnss_ble_diag_log.md](gnss_ble_diag_log.md) §2.8。

Companion 侧：`companion_supervisor_pump()` 在 `ensure_advertising` **之前**就跑；若 `sf32lb52_bt_hci_rx_stalled(2500)` 或 `companion_bridge_diag_cycle_take()`，立刻 `ble_supervisor_request_cycle(ADAPTER)`。HCI stall **不走** 30 s/60 s cooldown。

zblue `bt_hci_cmd_send_sync` 10 s 超时在板级 CMake 里改成返回 `-ETIMEDOUT`，不再 `BT_ASSERT`。

时间线（companion 已卡在同步 HCI、stall 持续）：

```text
t=0          companion 堵在 Command Complete；心跳停
t≈15 s       ok() 变假 → unhealthy（宽限 25 s 过后才开始）
t≈19 s       第一次 restart：force LCPU reset + post cycle
             （companion 若仍能跑则会自己 disable/enable）
t≈39 s       cooldown 满，仍无心跳 → SIGTERM 后 respawn（拆除跳过 HCI）
```

不要一上来就 SIGTERM：host 栈还活着时，正规 cycle（`h4_close`/`h4_open`）才能把 H4 RX 线程拉起来。LCPU 硬复位是后手。进程确实没了才 spawn。

相关文件：

- `chips/sf32lb52/sf32lb52_bt_adapter.c` — stall 窗口、`force_reset`、disable 超时
- `nuttx/drivers/serial/uart_bth4.c` — 环满时丢掉广播/ACL，给 Command Complete 留位置
- `services/ble_companion/ble_supervisor.c` / `ble_companion.c`
- `include/companion_bridge.h` — `diag_cycle_post` / `take` / `pending`

---

## 6. 已注册：GNSS

条件：`CONFIG_BOARD_L96_GNSS`。

| | |
|--|--|
| `ok` | 读线程心跳 8 s 内新鲜；再看最近一次 **NMEA 行**（`$…\n`）是否在 8 s 内。无定位但有 GSV 算健康。kick / 断电 / 灌星历 45 s 内只看心跳。从未收到 NMEA 且不在恢复窗则不健康。未 `myvendor_gnss_start` 则视为健康（不归 diag 管） |
| `fail_ms` | 12000 |
| `cooldown_ms` | 30000 |
| `restart` | 心跳还在：abort dump → 清快照 → 置 `g_diag_kick`（必要时 close UART 打断 poll）；读线程关串口后 PA43 断电再开。心跳超时：同一块静态栈 `task_delete` 再 `pthread_create` |

GNSS 读线程自己还有「静默 5 s 重开 UART，连续两次再断电」。diag 是后手：读线程若还在转，通常 5 s 内自己恢复，`g_nmea_life_ms` 会刷新，`ok()` 变真，diag 不会再断电。  
`g_nmea_life_ms` **不**在 UART drop 时清掉，避免正常重开窗口被当成「立刻不健康」。恢复窗避免灌星历期间二次 kick。

室内搜不到星、但仍有 NMEA，diag **不会**重启模组。

---

## 7. 再挂一个外设

在该外设的 `.c` 里准备一套静态 `myvendor_diag_ops_t`，`start()` 成功后（或 bringup 里）调用 `myvendor_diag_register()`。线程已起来也可以注册，下一拍就会扫到。

```c
#include "myvendor_diag.h"

static bool baro_ok(void)
{
  return /* 近期有采样 */;
}

static int baro_restart(void)
{
  myvendor_board_sensor_request_reopen();
  return 0;
}

static const myvendor_diag_ops_t g_baro_ops =
{
  .name = "baro",
  .ok = baro_ok,
  .restart = baro_restart,
  .fail_ms = 8000u,
  .cooldown_ms = 30000u,
};

/* 驱动起来之后 */
(void)myvendor_diag_register(&g_baro_ops);
```

约定：

- `ok()` 必须短、无锁死、可重入。不要在里面 `open()` 设备或发 HCI。
- `restart()` 只投递/置位，能让**该外设自己的线程**去关断重开最好（BLE 第一次就是这样做的）。必须在 diag 线程断电时，接受其它槽本拍被推迟。
- `fail_ms` 要大于外设自己的恢复窗口，避免和自恢复抢着重启。
- 名字不要和 `ble` / `gnss` 冲突，除非有意覆盖。

当前板卡传感器线程（BMI270 / BMP388 / MMC5983 / VBATS ADC）已有自己的采样与 I2C reopen，**没有**挂到 diag。需要时按上表加。

---

## 8. 日志（串口）

全部走 `syslog`，前缀 `diag:`。

| 日志 | 含义 |
|------|------|
| `diag: register ble fail=4000 cooldown=20000` | 槽位挂上 |
| `diag: started prio=9` | `myvendor_diag_start` 成功 |
| `diag: thread up, grace 25000 ms` | 线程进循环 |
| `diag: ble unhealthy` | `ok()` 刚变假 |
| `diag: ble restart (unhealthy … ms, n=1)` | 即将调 `restart` |
| `diag: ble request companion adapter cycle` | 软恢复，等 companion 取走 |
| `diag: ble force LCPU reset (thread stalled)` | 心跳停了但进程还在：先扳 LCPU，不 SIGTERM |
| `ble_supervisor: request adapter cycle reason=adapter (immediate)` | companion 已接手 |
| `diag: ble force LCPU reset` | 第二次仍不健康且循环还在 |
| `diag: ble companion thread respawn` | 进程没了，或硬复位后仍无心跳 |
| `ble_companion: stop skipped HCI (recovering)` | respawn 拆除跳过 ADV/adapter HCI |
| `sf32lb52 bt force reset (LCPU)` | adapter 真正断电 |
| `sf32lb52 bt rx worker still busy, async cancel` | disable 等 HPWORK 超时 |
| `diag: gnss restart` / `gnss: diag restart (power-cycle PA43)` | 模组断电 |
| `gnss: diag kick, drop uart` | 读线程看到踢腿，重开串口 |

开机后约 25 s 内不应出现 `unhealthy`。若一过宽限就 `gnss unhealthy`，看模组是否一直没吐 NMEA（供电、波特率、UART2）。

`sf32lb52 bt rx callback: -12 (n=… stall=…)` 仍可能刷：那是 adapter 在报投递失败。要看随后有没有 `diag:` / `ble_supervisor:` cycle。若 stall 涨到几十万毫秒且没有 `diag:`，线程没起来或被更高优先级饿死。

---

## 9. 和其它恢复路径的关系

| 路径 | 跑在哪 | 看什么 | 何时够用 |
|------|--------|--------|----------|
| H4 丢可丢包 | `uart_bth4_receive` | 环满 | 扫描报告灌环，host 还在读 |
| H4 `get_rx` 超时 | `vela_override/zblue/patch_net_buf_timeout.py`（构建期补丁） | ACL/事件拿不到 buf | host RX 线程还活着 |
| adapter `usleep(3 ms)` | HPWORK RX worker | `notify_host < 0` | 避免空转饿死 GNSS |
| GNSS 静默重开 | GNSS 读线程 | 5 s 无 NMEA | 读循环还在转 |
| `ble_supervisor` | companion 主循环 | ADV / SCAN / GATT / HCI stall 2.5 s | companion **没**堵在同步 HCI 上 |
| **diag** | `diag` 线程 | 注册表 `ok()` | 上面都动不了时的后手 |

不要指望 diag 替代 supervisor：正规 BLE 恢复是两级阶梯 —— adapter cycle（关开 host）失败后升级为 **LCPU 复位 + companion task 重建**（5 分钟限频，升级请求由 supervisor 保留）。diag 只保证 **请求一定能发出来**；companion 死了才自己扳 LCPU。

---

## 10. 已知边界

- 宽限 25 s 内 HCI 已经死了，要等到宽限结束才第一次 `ok()`。
- `force_reset` 与 companion 同时 `bt_adapter_disable` 有竞态；只在软 cycle 失败后使用。复位后 host 可能仍需一次正规 cycle 才能重新 ADV。
- skip-HCI respawn 后立刻 `bluetooth_delete_instance` 会和 uv worker 上的 `bt_disable` 抢 `manager_cleanup` 链表（MemFault）。启动失败走 skip-HCI 拆除；GATTS `NOT_READY`(2) 先短等再 recycle adapter。
- GNSS `restart` 里 `gnss_module_power_cycle()` 会挡住 diag 约 1.5 s。
- 槽位没有 unregister；同名覆盖即可。
- 未对 LCD / I2C / SD 注册。那些已有各自的 reopen / IWDT。

---

**更新**：2026-09-15
