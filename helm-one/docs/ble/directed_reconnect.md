# 已绑定手机定向回连（RPA / RAL / advtest）

**版本**：v1.0 · **日期**：2026-10-18 · **固件验证**：`build_date=202610181832`
配套代码：`services/ble_companion/ble_companion.c`、`vela_override/zblue/patch_hci_core.py`、
`vela_override/bluetooth/sal_le_advertise_interface.c`、`nsh/ble_test_app/ble_test_app.c`
相关：[pairing_and_radio.md](pairing_and_radio.md)、[companion_impl_plan.md](companion_impl_plan.md) §7.4。

---

## 1. 最终设计速查

窗口外、已绑定手机存在时，码表优先发**定向广播**：

| 项 | 当前实现 |
|---|---|
| 广播类型 | `BT_LE_LEGACY_ADV_DIRECT_IND`，低占空比（HCI `type=0x04`） |
| own 地址 | `BT_LE_ADDR_TYPE_PUBLIC`（码表 public 身份地址，HCI `own=0x00`） |
| TargetA | 手机最近一次连接时的**原始 InitA/RPA**（`bt_conn_get_info().le.remote`） |
| TargetA 类型 | `BT_LE_ADDR_TYPE_RANDOM`（HCI `peer_type=0x01`） |
| `DIR_ADDR_RPA` | **不置**（own 不是 RANDOM） |
| AdvData / ScanRsp | 定向时不带（传 `NULL/0`；legacy 定向 PDU 本身没有数据） |
| RPA 缓存 | RAM 单槽 `g_last_peer_rpa`，`g_rpa_lock` 保护 |
| RPA 最大年龄 | **10 min**（`RPA_MAX_AGE_MS=600000`） |
| 定向窗口 | **60 s**（`DIRECTED_ADV_WINDOW_MS=60000`） |
| 失败回退 | 无定向 `ADV_IND`、200 ms、无本地名 |
| 冷却 | **10 min**（`DIRECTED_ADV_RETRY_MS=600000`） |
| 首次开机/重启后 | 无 RPA 缓存 → 第一次回连走无定向 |
| 配对窗口 | **始终无定向、可发现、带本地名、bondable** |
| 无记录且窗口外 | 完全不广播 |

### 1.1 状态机

```text
配对窗口 active                  -> ADV_IND，可发现，200 ms，bondable
else 无手机记录                   -> OFF
else 有新鲜 RPA 且不在冷却         -> ADV_DIRECT_IND，own=PUBLIC，TargetA=RPA
else                              -> ADV_IND，无名，200 ms
```

- 手机连接时只暂存真正的 RPA；链路达到 L2 且解析出的身份仍在手机绑定记录中，
  companion 线程才把它提升为 `g_last_peer_rpa`；**不清失败冷却** —— 冷却只由
  定向失败/超时设置，10 min 后自然重试。无定向回连成功只刷新 RPA，不能把
  冷却清掉（R6 实测：清了会在 17 s 后再次发定向）。
- 定向启动回调失败：`directed adv failed status=N; fallback to undirected for 600000 ms`。
- 定向被 SAL/控制器提前停：`directed adv stopped early; fallback ...`。
- 60 s 内没有连接：`directed adv no connection in 60000 ms; fallback ...`。
- `bt_le_start_advertising()` 返回 NULL：同样设 10 min 冷却，下一拍回退无定向。

---

## 2. 为什么必须是 own=PUBLIC + TargetA=当前 RPA

2026-10-18 用同一台手机做了 A/V1/V2/V3 实机对照（测试固件 `ble_test advtest`）：

| 方案 | HCI 参数 | 结果 |
|---|---|---|
| A | `type=0x04 own=0x00 peer_type=0x00 peer=78:d8:...`（TargetA=身份地址） | 90 s 无人连；回退无定向后 **0.36 s** 连上 |
| V1 | `type=0x04 own=0x00 peer_type=0x01 peer=<当前RPA>` | **手机连上**（5–7 ms） |
| V2 | `type=0x04 own=0x01 peer_type=0x01 peer=<当前RPA>` | 30 s 内无连接尝试 |
| V3 | `type=0x04 own=0x03 peer_type=0x01 peer=<当前RPA>`（+`DIR_ADDR_RPA`） | 30 s 内无连接尝试 |
| B3 反例 | `own=0x00` + `DIR_ADDR_RPA` | zblue `valid_adv_param()` 因 `USE_IDENTITY`+`DIR_ADDR_RPA` 返回 `-EINVAL`，无 HCI |

结论：

1. **TargetA 必须是手机当前 RPA**，身份地址不行；本板控制器不会自动把身份地址
   解析成 RPA（Zephyr 的 `direct_adv` 样例语义在这颗控制器上不成立）。
2. **own 必须是码表 public 身份地址**。手机后台 `connectGatt(autoConnect:true)`
   只关联它缓存的码表 public AdvA；`own=RANDOM`（NRPA/RPA）的定向广播手机
   完全不尝试连接。
3. `DIR_ADDR_RPA` 对本手机没有帮助；产品路径不使用。
4. 手机 RPA 会轮换（实测每次回连可能变化，典型 15 min），所以 TargetA
   只能来自“最近一次连接”，不能是固定身份地址。

### 2.1 另一个真 bug：pending conn 按身份地址查

V1 第一次实机连接虽然建立，但 host 立刻断开：

```text
<err> No pending conn for peer 62:EB:C6:EE:2B:D7 (random)
disconn ... reason=0x16
```

原因：`le_adv_start_add_conn()` 按 `TargetA`（RPA）登记 pending conn；
`bt_hci_le_enh_conn_complete()` 调 `find_pending_connect(hdev, role, &id_addr)`
时用的是 `translate_addrs()` 解析后的**身份地址**，匹配不上。

修复在 `vela_override/zblue/patch_hci_core.py`（构建期改写 zblue 副本）：

```c
if (IS_ENABLED(CONFIG_BT_PERIPHERAL) &&
    evt->role == BT_HCI_ROLE_PERIPHERAL) {
    conn = find_pending_connect(hdev, evt->role, &peer_addr); /* 原始 RPA */
    if (!conn && bt_addr_le_cmp(&peer_addr, &id_addr) != 0)
        conn = find_pending_connect(hdev, evt->role, &id_addr);
} else {
    conn = find_pending_connect(hdev, evt->role, &id_addr);
}
```

无定向广播仍走 `BT_ADDR_LE_NONE` 分支，不受影响。

---

## 3. RPA 捕获、RAL 与两套“记录”的分工

三个概念不要混：

1. **原始 RPA（定向目标）**
   - `le_connected()` 里 `bt_conn_get_info(conn, &info).le.remote`，
     这是 `CONNECT_IND` 的原始 `InitA`。
   - 只 stage 到 `g_pending_peer_rpa`；L2 加密成功、且身份仍在手机绑定记录中后，
     才提升为 `g_last_peer_rpa`。3 秒 Admission grace 不算已验证。
   - `bt_conn_get_dst()` / GATTS 回调给的是 `bt_lookup_id_addr()` 解析后的
     **身份地址**，不能拿来做 TargetA。
2. **主机准入允许名单**：`/mnt/kv/bt_phone.tsv`
   - 最多 3 行（`PAIR_PHONE_MAX=CONFIG_BT_MAX_PAIRED=3`），`ADDR<TAB>TYPE`；
     满时 FIFO 挤掉最老一条。
   - 写盘前 2 s settle（`PAIR_SETTLE_MS`）；开机按密钥池修剪陈旧 RPA。
   - 供 `myvendor_pair_admit()` 使用，**不是 zblue 绑定表**。
3. **zblue 绑定表 / 控制器 RAL**
   - `/mnt/kv/bt_keys.bin` 存 IRK/LTK；`CONFIG_SETTINGS` 关闭，由
     `vela_override/zblue/patch_keys_kv.py` 落盘。
   - 开机 `myvendor_keys_kv_load_all()` 恢复，`bt_id_add()` 把 IRK 写进控制器 RAL。
   - RAL 容量 **3**（`SF32LB52_HCI_RL_SIZE=3`，LCPU `ble_max_ral=3`）。
   - 日志：`bt_keys kv: <addr> -> RAL rl_size=3 entries=N`。

### 3.1 多手机/多绑定

RPA 缓存是**单槽**，只服务“最近一次完成 L2 加密且身份仍在绑定记录里的手机”。
最多 3 台已绑定手机时，其他手机依赖无定向回退；如果需要多台都自动回连，
要改成按 `last_seen` 的多槽缓存，或回退到 `ADV_IND` + 主机准入。

### 3.2 首次开机/适配器 cycle

RPA 缓存只在 RAM；重启或 adapter cycle 后缓存为空，第一次回连必须走无定向。
无定向连上后抓到当前 RPA，下一次断开才开始定向。这是“只对已绑定手机定向”
与“重启后仍能回连”的取舍。

---

## 4. 绑定核对判据

### 4.1 板端

```text
ctl pair phone
  pair: phones 1/3 window=0
    [0] 78D8404BF520 type=0

cat /mnt/kv/bt_phone.tsv
  78D8404BF520	0

bt_keys kv: 78:D8:40:4B:F5:20 (public) -> RAL rl_size=3 entries=1
bt_keys kv: restored 1/1 record(s)
le secure lvl=2
```

- 记录地址应是手机**身份地址**（type=0），不是轮换 RPA。
- `entries` 必须 ≥1；`rl_size=0` 或 entries 不涨说明 IRK 没进控制器 RAL。
- `ctl pair list` 的 `sec=2 bonded=1` 表示当前链路已加密且对端在绑定名单里。
- `ctl nvds` 只是原始 NVDS dump，不能单独作为 RAL 生效的证据。

### 4.2 手机侧（Android）

```text
Bonded devices:
  XX:XX:XX:XX:09:84 [ LE ] Helm One-0984
shim::record ... le_linkkey_known:T ble_enc_key_size:16 le_encrypted:T
settings get secure bluetooth_address -> 78:D8:40:4B:F5:20
```

- 手机绑定的是码表 public 地址 `55:F7:0A:21:09:84`。
- 手机身份地址 `78:D8:40:4B:F5:20` 与板端记录一致。
- 手机侧拿不到当前 RPA；RPA 只能从码表连接事件里取。

---

## 5. 测试入口：`nsh-btadv` + `ble_test advtest`

产品 `configs/nsh` **不编**测试 app；测试用独立配置：

```bash
cd /home/jinsc/SDK/vela/openvela
./build.sh ./vendor/HelmOne/boards/sf32lb52/helmone/configs/nsh-btadv/ \
  --cmake -b cmake_out/helmone_nsh-btadv -j8
vendor/HelmOne/scripts/wrap_nuttx_image.sh cmake_out/helmone_nsh-btadv
```

配置要点：`CONFIG_MYVENDOR_BLE_TEST_APP=y`、`CONFIG_MYVENDOR_BLE_DIRECTED_ADV=y`。

测试前让出射频：

```text
ctl radio off
ctl sensor off
```

语法：

```text
ble_test advtest ind [repeat]
ble_test advtest dir <peer_type> <mac> <rpa 0|1> <low|high> <interval> <repeat> [own_type] [hold_ms]
```

参数：

- `peer_type`：TargetA 的 HCI 类型，0 public / 1 random；
- `mac`：显示序，可带冒号或 12 位无冒号；
- `rpa`：强制 `DIR_ADDR_RPA`，0 清、1 置；
- `low|high`：低/高占空比；
- `interval`：HCI 单位 `0x20..0x4000`（320 = 200 ms）；
- `repeat`：1..100；
- `own_type`：0 public / 1 random，默认 0；
- `hold_ms`：起播后保持时间，0..30000，便于手机发起连接。

示例：

```text
ble_test advtest dir 0 78D8404BF520 0 low  320 1       # B1 public target
ble_test advtest dir 1 4A1B2C3D4E5F 0 low  320 1       # B2 static random
ble_test advtest dir 0 78D8404BF520 1 low  320 1       # B3 预期 -EINVAL
ble_test advtest dir 1 4A1B2C3D4E5F 1 low  320 1 1     # B3b own=RPA
ble_test advtest dir 0 78D8404BF520 0 high 320 1       # B4 高占空比
ble_test advtest dir 0 78D8404BF520 0 low  320 10      # B5 重复 10 次
ble_test advtest dir 1 58D506A7C29A 0 low  320 1 0 30000  # V1：own=public + 当前 RPA
```

判据：

- `ADVTEST begin ...` 参数回显正确；
- `ADVTEST n=... start_cb=SUCCESS elapsed=...`；
- 手机在 `hold_ms` 内发起连接；
- `ADVTEST summary total=... started=... failed=... timeout=...`；
  退出码 0 仅当无 failed/timeout；
- 失败看 `[adver] err:` 与 `adv start failed status=`。

---

## 6. 日志判据

| 日志 | 含义 |
|---|---|
| `adv directed (paired; cached RPA <RPA> age=<ms> ms)` | 定向起播，目标/RPA 年龄 |
| `adv connectable unnamed (paired; no fresh RPA; 200 ms)` | 无缓存/过期，回退无定向 |
| `adv connectable unnamed (paired; directed cooldown; 200 ms)` | 定向失败冷却中 |
| `phone RPA cached after secure identity check` | L2 加密及身份记录核对通过，RPA 已缓存 |
| `directed target removed from phone records` | 目标身份已解绑，清缓存并走无定向 |
| `directed adv start rejected; fallback to undirected for 600000 ms` | 同步起播失败，进入冷却 |
| `directed adv failed status=<n>; fallback to undirected for 600000 ms` | 启动回调失败 |
| `directed adv stopped early; fallback ...` | SAL/控制器提前停 |
| `directed adv no connection in 60000 ms; fallback ...` | 60 s 未连 |
| `sf32lb52 bth4 adv_param itv=... type=0x04 own=0x00 peer_type=0x01 peer=<RPA>` | HCI 参数正确 |
| `adv_cc op=0x2006 status=0x00` / `adv_enable=1` | 控制器接受 |
| `No pending conn for peer ...` / `reason=0x16` | pending conn 查找失败（修复后不应再出现） |
| `bt_keys kv: ... RAL rl_size=3 entries=N` | IRK 是否进 RAL |

---

## 7. 已知边界

- **首次开机/cycle 后无 RPA**：第一次回连走无定向。
- **RPA 轮换**：旧 RPA 的定向会 60 s 无人连，回退无定向；连上后刷新 RPA。
  60 s 是手机后台重连延迟（实测约 30 s）与回连速度的折中。
- **多绑定手机**：只有最近完成 L2 且身份在记录里的那台能定向；其他走无定向回退。
- **手机侧“忽略设备”** 不会通知码表；`bt_phone.tsv`/密钥池可能残留，
  需要配对窗口重配或 `ctl pair unbind`。
- **配对窗口内始终无定向**：新手机必须能扫到名字。
- 定向广播没有 AdvData/ScanRsp；依赖扫描 Service UUID/名字的调试工具
  看不到它是正常的；“手机不连”要与“工具扫不到”区分。
- 控制器复位限频/升级请求的保留见 [companion_impl_plan.md](companion_impl_plan.md) §7.4。

---

## 8. 修复后上板回归清单（交给 DeepSeek）

测试产品 `nsh` 固件；每项记录固件 `build_date`、手机/测试 Central 的身份地址和
地址类型、完整串口日志、`ctl pair list` 前后结果、手机端连接时间。先确认
`CONFIG_MYVENDOR_BLE_DIRECTED_ADV=y`。不要把扫描器看不到定向 PDU 的名字当作
起播失败，应以 HCI `type=0x04`、起播回调和真实连接判断。

| 编号 | 操作 | 必须满足的判据 |
|---|---|---|
| R1 正常回连 | 配对手机 A，等 `le secure lvl=2`，断开后让 App 后台自动回连，重复 10 次 | 每轮先见 `phone RPA cached after secure identity check`；下一次起播 `type=0x04 own=0x00 peer_type=0x01`，TargetA 与上次连接的原始 InitA 相同；手机能连接；没有 `No pending conn`/`reason=0x16` |
| R2 未加密陌生连接 | A 已绑定；重启使 RPA 缓存为空，配对窗口关闭；陌生 Central B 用 RPA 连无定向广播，不完成加密，等待 grace 超时并断开 | B 不触发 `phone RPA cached`；`ctl pair list` 不增加 B；后续没有指向 B 的 `type=0x04`；A 仍可经无定向广播回连 |
| R3 错误连接归属 | A 已绑定；在 B 的 grace 期间让 A 建立连接并完成 L2（如设备允许并发） | 缓存只能归属于完成 L2 且在绑定记录中的那条连接；不能把 B 的 InitA 与 A 的身份拼在一起。若硬件不允许并发，在 host mock 中按 B connected → A connected → A secure 顺序验证 |
| R4 非 RPA 地址 | 使用 public 地址或 static random 地址的已绑定 Central 连接并达到 L2，随后断开 | 不出现 `phone RPA cached`；后续走无定向 `ADV_IND`，没有把 public/static random 地址强填成 `peer_type=0x01` 的定向广播 |
| R5 解绑目标 | 绑定 A、B 两台；让 A 最后连接并达到 L2，然后只解绑 A（`ctl pair list` 确认 B 保留），触发下一次广播 | 见 `directed target removed from phone records`；没有指向 A 的定向 HCI 参数；B 能在无定向广播中回连 |
| R6 同步启动失败 | 在测试构建中对一次定向 `bt_le_start_advertising()` 注入 `NULL` 返回；保留其余路径原样 | 立即见 `directed adv start rejected; fallback to undirected for 600000 ms`；下一拍是 `ADV_IND`；10 min 内不重复发定向；无持续 `adv` stall/cycle |
| R7 异步启动失败 | 在测试构建中令一次定向起播回调返回非成功状态 | 见原有 `directed adv failed status=...` 和 10 min 回退；下一拍无定向；无反复定向起播 |
| R8 定向超时与过期 | A 的 RPA 缓存有效但手机不回连；另测缓存超过 10 min | 定向 60 s 后回退无定向并冷却；过期缓存直接走无定向，不使用旧 TargetA |
| R9 双角色压力 | A 回连过程中开启传感器扫描/连接，重复 10 次 | 手机和传感器链路均能恢复；无 `cmdsilent`、持续 `status=2` 或 supervisor 反复 cycle |

R6/R7 的故障注入只放在临时测试构建，测试后移除并重新构建产品镜像。每项报告
实际 HCI 参数、回调状态、连接结果与耗时；失败时附首个异常前后至少 5 秒日志。

### 8.1 实测结果（2026-10-18）

产品 `202610182006` / `202610182026`，故障注入用 `nsh-btadv` 测试镜像。
四轮修复后补测：R1/R2/R3/R5/R6/R7/R8 通过或部分通过；R4 受主机 BlueZ 配对限制。

| 编号 | 结果 | 关键证据 / 限制 |
|---|---|---|
| R1 | **通过 10/10** | 每轮 `type=0x04 own=0x00 peer_type=0x01 peer=<上一轮 RPA>`；起播→连接 0.014–0.265 s；`No pending conn` 0、`0x16` 0 |
| R2 | **通过（B=public）** | B 未加密连接 → `grace 3s` → `reason=0x05` 踢；无 `phone RPA cached`；`ctl pair phone` 不增 B；无指向 B 的 `type=0x04`；A 随后无定向回连。B 用 RPA 的变体未测 |
| R3 | **串行通过；真正并发未出现** | B 先连、A 在 grace 内启动；板端停 ADV，A 在 B 被踢后才建链；缓存/定向 TargetA 是 A 的 RPA，不是 B。硬件/单广播状态未出现双链路并发 |
| R4 | **未测/受阻** | 主机 BlueZ 对 B（public）多次 `AuthenticationFailed`，B 未达 L2；“绑定 public 断开后无定向”未执行。public 非 RPA 不缓存已由 R2 + `bt_addr_le_is_rpa()` 代码证明 |
| R5 | **通过（合成 B 记录）** | 写入 A+B 两条 tsv；A 最后 L2 → 定向；`ctl pair unbind 0` → `pair: phone record removed slot 0 (1 left)` → `directed target removed from phone records` → `no fresh RPA` 无定向；B 记录保留。B 实际回连未测（合成记录，无 B 密钥） |
| R6 | **通过（修复后）** | 首次实测冷却被“无定向回连刷新 RPA”清零（17 s 后又定向）；已删除该清零。复测：`injected synchronous directed start failure` → `directed adv start rejected; fallback ...` → 20 s 后仍 `directed cooldown`，无新 `type=0x04` |
| R7 | **通过（延迟注入后）** | 立即注入 STOPPED 会撞 framework“先 bt_sal_le_start_adv、后 list_add_tail”窗口，事件被丢；测试注入改为 100 ms 后发 STOPPED：`injected async directed failure` → `directed adv stopped early; fallback ...` → 无定向；A 回连 |
| R8a | **通过** | 定向 59.997 s 后 `directed adv no connection in 60000 ms`；fallback 后 2.088 s A 无定向连上 |
| R8b | **通过** | `injected expired RPA cache` → `no fresh RPA` → 无 `type=0x04`；A 无定向回连 |
| R9 | **部分** | 手机回连 5/5；严格定向 3/5（1 次 setup cooldown、1 次瞬时 `status=2` 后 fallback）；`cmdsilent`/`No pending conn`/`0x16`/supervisor cycle 均 0。现场无真实传感器，只验证了 sensor central 使能/扫描侧 |

**本轮额外修复**：

1. `companion_directed_peer_poll()` 不再清 `g_adv_directed_retry_after_ms`；
   无定向回连只刷新 RPA，失败冷却保留到 10 min 到期（R6）。
2. `ctl pair unbind <idx>` 暴露单槽解绑（R5）；无参 `unbind` 仍是全部解绑。
3. 测试构建的异步注入延迟 100 ms，避开 framework 登记窗口（仅测试代码）。

### 8.2 已连接时切换 radio 策略（2026-10-18 已上板，build `202610182036`）

修复 `ctl radio on` 在手机已连接时仍调用定向起播的路径后，需要单独验证：

1. 让 A 连接并确认 `le secure lvl=2`，保持连接；执行 `ctl radio off`、`ctl radio on`。
2. `ctl radio on` 后保持连接至少 30 s：不得出现新的 `adv directed`、HCI `0x2006/0x200a` 起播、`err:-22` 或手机/FS 断链。
3. 主动断开 A：应按现有 RPA 年龄和冷却状态恢复定向或无定向广播，并能重新连接。
4. 手机保持连接时再切换一次 `ctl sensor off/on`，核对同样的“不起播、不断链”判据。

**验收结果（2026-10-18，build `202610182036`）**：

- 连接态 `ctl radio off/on`：30 s 窗口内无 `adv directed`、无 `type=0x04`、
  无 `0x2006/0x200a`、无 `zblue_start_adv ... err:-22`、无断链；
  `[radio] tick ... phone=1` 持续。
- 主动断开后：`adv directed (paired; cached RPA ...)`，HCI
  `type=0x04 own=0x00 peer_type=0x01` + `0x2006/0x200a status=0x00`；
  手机约 **5 s** 回连并 `le secure lvl=2` + `phone RPA cached`。
- 连接态 `ctl sensor off/on`：30 s 窗口内同样无起播/`-22`/断链；
  `sensor central off/on` 正常，FS 命令前后不断。
- 残余噪声：`sensor off` 时 3 行 `zblue_conn_disconnect, conn null`
  （历史累计 344 次，启动阶段也有），不影响手机链路，不是本轮回归。
