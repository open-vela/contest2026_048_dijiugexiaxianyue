# GNSS 星历下发：计算方法 / 验证方法 / 注意事项

> 2026-09-27。适用对象：MAX-M10S-00B-01（ROM SPG 5.10）。
> 两块代码：**App 侧**把 RINEX 广播星历打成 UBX-MGA-* 帧；**固件侧**把文件灌进模组。
> 本文是这两块的字段表、四条验证判据、以及踩过的坑。工具都在 `zcode/analysis/` 与 `zcode/`。

---

## 0. 一句话把链路说清

```
IGS/BKG MIXED BRDC (igs.bkg.bund.de)
   └─ App: rinex_mga.dart / RinexMga.convertBytes()
        └─ 一组 UBX-MGA-*-EPH + 两帧辅助（INI-TIME / INI-POS）
             └─ BLE companion FS 写到 /mnt/lfs/eph/mga_<unix>.ubx
                  └─ 固件 gnss_eph_inject() 逐帧灌进模组 UART（115200/1M 定长帧）
                       └─ 模组回 UBX-MGA-ACK（0x13/0x60），固件统计成 `mga ack ...`
```

**另一条完全不同的源**：模组自己的导航库倒库（`0x13/0x80` MGA-DBD），文件名同样叫 `mga_<utc>.ubx`，
**只能靠第一帧的 id 分辨**（`0x40`=App 的 INI 头，`0x80`=DBD）。名字和大小都分不出来。

---

## 1. 计算方法（App 侧打包）

### 1.1 三个"抄错就完"的地方（先看这个）

1. **角度字段是"半圆"，不是弧度**。GPS/GAL/BDS 的 `m0/i0/omega/omega0/omegaDot/deltaN/idot`
   在 MGA 里都以 **半圆** 为单位（RINEX 给的是弧度）⇒ 打包前要 `/π`。
   **写反了 `crc` 与 `i0` 这种偏移错更凶**：模组照样回 `accepted`，但存进去的是垃圾，
   倾角偏 0.3°、卫星位置偏上百公里，**静默劣化定位**（App 注释里记过一次）。
2. **toe/toc 的时间单位三个星座各不相同**：GPS/QZSS **×16 s**、GAL **×60 s**、**BDS ×8 s**。
   三个值都不能按"秒"读。**这是最容易看错的一栏。**
3. **谐波项的宽度不同**：GPS/GAL 的 `cuc/cus/cic/cis` 是 **I2**、`crc/crs` 是 I2；
   **BDS 那六个全是 I4**。把 GPS 那段抄到 BDS 上会全错。

### 1.2 MGA-GPS-EPH / MGA-QZSS-EPH（`0x13 0x00` / `0x13 0x05`，载荷 **68** B，`type=0x01`）

| off | 字段 | 类型 | 标度 / 说明 |
|---|---|---|---|
| 0 | type | U1 | `0x01` |
| 1 | version | U1 | 0 |
| 2 | svId | U1 | PRN |
| 3 | reserved0 | U1 | 0 |
| 4 | fitInterval | U1 | 0 = 2h；>4h 置 1 |
| 5 | uraIndex | U1 | 由 URA(米) 查表得到 |
| 6 | svHealth | U1 | 低 6 位 |
| 7 | tgd | I1 | 2⁻³¹ s |
| 8 | iodc | U2 | — |
| 10 | toc | U2 | **×16 s** |
| 12 | reserved1 | U1 | 0 |
| 13 | af2 | I1 | 2⁻⁵⁵ s/s² |
| 14 | af1 | I2 | 2⁻⁴³ s/s |
| 16 | af0 | I4 | 2⁻³¹ s |
| 20 | crs | I2 | 2⁻⁵ m |
| 22 | deltaN | I2 | 2⁻⁴³ 半圆/s |
| 24 | m0 | I4 | 2⁻³¹ 半圆 |
| 28 | cuc | I2 | 2⁻²⁹ rad |
| 30 | cus | I2 | 2⁻²⁹ rad |
| 32 | e | U4 | 2⁻³³ |
| 36 | sqrtA | U4 | 2⁻¹⁹ √m |
| 40 | toe | U2 | **×16 s** |
| 42 | cic | I2 | 2⁻²⁹ rad |
| 44 | omega0 | I4 | 2⁻³¹ 半圆 |
| 48 | cis | I2 | 2⁻²⁹ rad |
| 50 | crc | I2 | 2⁻⁵ m |
| 52 | i0 | I4 | 2⁻³¹ 半圆 |
| 56 | omega | I4 | 2⁻³¹ 半圆 |
| 60 | omegaDot | I4 | 2⁻⁴³ 半圆/s |
| 64 | idot | I2 | 2⁻⁴³ 半圆/s |
| 66 | **reserved2** | U2 | **0。不是 `week`**（见 §3.4） |

### 1.3 MGA-GAL-EPH（`0x13 0x02`，**76** B）

| off | 字段 | 类型 | 标度 |
|---|---|---|---|
| 0/2 | type / svId | U1 | `0x01` / PRN |
| 4 | iodNav | U2 | — |
| 6 | deltaN | I2 | 2⁻⁴³ 半圆/s |
| 8 | m0 | I4 | 2⁻³¹ 半圆 |
| 12 | e | U4 | 2⁻³³ |
| 16 | sqrtA | U4 | 2⁻¹⁹ |
| 20 | omega0 | I4 | 2⁻³¹ 半圆 |
| 24 | i0 | I4 | 2⁻³¹ 半圆 |
| 28 | omega | I4 | 2⁻³¹ 半圆 |
| 32 | omegaDot | I4 | 2⁻⁴³ 半圆/s |
| 36 | iDot | I2 | 2⁻⁴³ 半圆/s |
| 38/40 | cuc / cus | I2 | 2⁻²⁹ rad |
| 42/44 | crc / crs | I2 | 2⁻⁵ m |
| 46/48 | cic / cis | I2 | 2⁻²⁹ rad |
| 50 | toe | U2 | **×60 s** |
| 52 | af0 | I4 | 2⁻³⁴ s |
| 56 | af1 | I4 | 2⁻⁴⁶ s/s |
| 60 | af2 | I1 | 2⁻⁵⁹ s/s² |
| 61 | sisaIndexE1E5b | U1 | — |
| 62 | toc | U2 | **×60 s** |
| 64 | bgdE1E5b | I2 | 2⁻³² s |
| 66-71 | reserved1 + health/validity ×4 | U2+U1×4 | 0 |
| 72 | reserved2 | U4 | 0 |

### 1.4 MGA-BDS-EPH（`0x13 0x03`，**88** B）

| off | 字段 | 类型 | 标度 |
|---|---|---|---|
| 0/2 | type / svId | U1 | `0x01` / PRN |
| 4 | SatH1 | U1 | 健康 |
| 5 | IODC | U1 | — |
| 6 | a2 | I2 | 2⁻⁶⁶ s/s² |
| 8 | a1 | I4 | 2⁻⁵⁰ s/s |
| 12 | a0 | I4 | 2⁻³³ s |
| 16 | toc | U4 | **×8 s** |
| 20 | TGD1 | I2 | **0.1 ns**（秒值 ×1e9 再 ×0.1） |
| 22 | URAI | U1 | — |
| 23 | IODE | U1 | — |
| 24 | toe | U4 | **×8 s** |
| 28 | sqrtA | U4 | 2⁻¹⁹ |
| 32 | e | U4 | 2⁻³³ |
| 36 | omega | I4 | 2⁻³¹ 半圆 |
| 40 | Deltan | I2 | 2⁻⁴³ 半圆/s |
| 42 | IDOT | I2 | 2⁻⁴³ 半圆/s |
| 44 | M0 | I4 | 2⁻³¹ 半圆 |
| 48 | Omega0 | I4 | 2⁻³¹ 半圆 |
| 52 | OmegaDot | I4 | 2⁻⁴³ 半圆/s |
| 56 | i0 | I4 | 2⁻³¹ 半圆 |
| 60 | Cuc | **I4** | 2⁻³¹ |
| 64 | Cus | **I4** | 2⁻³¹ |
| 68 | Crc | **I4** | 2⁻⁶ |
| 72 | Crs | **I4** | 2⁻⁶ |
| 76 | Cic | **I4** | 2⁻³¹ |
| 80 | Cis | **I4** | 2⁻³¹ |
| 84 | reserved1 | U4 | 0 |

**另一个只属于 BDS 的偏移**：BDS 用 **BDT**，比 GPST 慢 14 s ⇒ 从 RINEX 的 BDS 记录
换算到"GPS 周内秒"时要 **`(sow − 14) mod 604800`**，否则 `toc` 差 14 秒。

### 1.5 两帧辅助（`0x13 0x40`，靠 `type` 分）

| type | 帧 | 载荷 | 关键字段 |
|---|---|---|---|
| `0x10` | INI-TIME_UTC | 24 B | `leapS`@3（当前 **18**）、`year..sec`@4-10、`tAccS`@16（U2，秒） |
| `0x01` | INI-POS_LLH | 20 B | `lat`@4 / `lon`@8（I4，×1e-7°）、`alt`@12（I4，cm）、**`posAcc`@16（U4，cm）** |

`posAcc` 是**我们自己声称的精度**，模组无法验证，却拿它决定搜索窗口宽窄 —— 见 §3.1。

---

## 2. 验证方法（四条，按成本从低到高）

### 2.1 离线字段级：`zcode/analysis/ubx_eph_dump.py <文件>`

不看板子、不看模组，直接把文件解到字段级。四条判据：

1. **分帧完整性**：每帧 `cksum=ok`、末行 `tail=0`（无截断/无多余字节）。
2. **星座清点**：全 BRDC 应给出 GPS 31~32 + GAL 30 + BDS 32 + QZSS 5（PV 数随天空可见性浮动。
   实测见过 GPS 缺 sv=6 的一次）。
3. **GPS 轨道自检**（抓标度因子错的最强判据）：半长轴 A = **26560 ± 5 km**、
   倾角 i = **55 ± 2°**、`omegaDot ≈ −8×10⁻⁹ rad/s`、半径 r 落在 26.2~27.0 千公里。
   **这些数值对了，就说明那 30 多个标度因子没写错** —— 模组是**不会**替我们检查的。
4. **文件名 ↔ 帧内 toe**：两者相差应 ≲ 2 h。**必须从 GPS 历元起算**：
   `tow = (unix + LEAP − 315964800) mod 604800`。
   ⚠ **直接写 `unix % 604800` 会差 3 天**（259200 s，因为 GPS 历元不是 604800 的整数倍），
   会把好数据判成"不一致"—— 这个坑踩过（2026-09-27）。
   这一个判据能同时抓住：旧内容配新名字、单位写错、选错周、下到的 BRDC 不是当天。

> **同一条判据现在固件里也常驻**（2026-09-27 起）：`gnss_eph_inject()` 会拿**第一帧星历的
> `toe`**（三套格式的偏移/单位不同，见 `gnss_ubx_eph_toe_s()`）和**文件名里的 UTC** 互校，
> 差 > 2 h 就打 `gnss: eph toe mismatch <路径>: name_tow=… toe=… diff=… s`（**只告警、不丢帧**）。
> 所以第 4 条判据不只在你手动跑校验器时生效 —— 每次注入都会看一眼。
> 阈值 2 h 的余量：`toe` 在 2 h 网格上、生成时刻离最近网格点最多 1 h；同一份文件里
> 不同卫星可以落在相邻网格（实测见过 08:00 与 10:00 混在一份里，差 1.7 h）⇒
> **不要把阈值收到 2 h 以下**。

### 2.2 传输完整性：整文件 CRC32 + 逐帧校验和

- 主机 ↔ 板子：`ctl fs get/put` 自带**整文件 CRC32**（`zcode/ephfile.py`）。
- 板子 → 模组：注入日志 `eph done frames=N skip=S stale=X tdrop=T badck=B bytes=… brk=eof changed=0`
  里 **`badck=0` + `brk=eof` + `changed=0`** 表示"文件每帧校验和对、读到 EOF、注入期间没人改过它"。

> **注意口径**：以上**只证明"我们完整发出去了"，不证明"模组收下了"**。

### 2.3 模组到底收没收：`gnss: mga ack …`（2026-09-27 落地）

模组对每一帧 MGA 回 `UBX-MGA-ACK`（`0x13/0x60`）。`CFG-NAVSPG-ACKAIDING` 已经是开的
（`myvendor_gnss.c` 的 `GNSS_CFG_ACKAIDING`），固件注入循环现在把这些回执统计成两行：

```
gnss: mga ack gps=23/6 qzss=5/0 gal=27/0 bds=29/3 other=1 (ok/bad)
gnss: mga ack rejected gps=0/6 qzss=0/0 gal=0/0 bds=0/6 (type/info)
```

第二行只在有拒收时打，每个星座给**最后一个被拒的 `type/info` 对**，其中
`type`：**1 = `MGA-ACK-DATA0`（受理）、0 = `MGA-NAK-DATA0`（不受理）**。

> **实测结论（2026-09-27）**：那几条"infoCode=6"的**其实是 NAK**（`type=0`），
> 不是"回 accepted 但带 code 6 的 ACK" —— 这个歧义就是靠补记 `pl[0]` 解掉的。
> 量级：一次注入 84 收下 / 9 NAK / 93 条回执。
> **仍待接口手册（UBX-21035062）确认**：NAK 的 `infoCode` 是否与 ACK 同一套语义
> （两者字段布局相同，见附录 B.1，但"6=unknown-type"对一帧合法星历说不通）。

**ACK/NAK 布局**：`pl[0]=type(1=ACK,0=NAK)`、`pl[2]=infoCode`、`pl[3]=msgId`、`pl[4..7]=payloadStart`。

**`infoCode` 取值**（`nsh/test/test_gnss.c` 的 `gnss_mga_info()`；括号里是参考实现的叫法）：

| code | 含义 | 参考实现 |
|---|---|---|
| 0 | accepted | accepted |
| 1 | no-time（要先发 MGA-INI-TIME） | no time |
| 2 | bad-version | **type not supported** |
| 3 | size-mismatch | size mismatch |
| 4 | not-stored | **database store failed** |
| 5 | not-ready | not ready |
| 6 | unknown-type | unknown type |

> code 2/4 两处叫法不一致（固件 vs 参考实现），值得对着接口手册核一遍；
> **code 4 才是"没存进去"那个判据。**（参考实现那张表与固件那张表的原文并列，见 **附录 B.2**。）

**两个已知的统计局限**（别把 ACK 计数当帧数）：

- **INI 帧不回执**：连发 100 帧、INI 两帧总是 `other=0`；`test_gnss.c` 里
  `TIME_UTC no MGA-ACK` 那条容错路径说明这是模组的正常行为。
- **覆盖不全**：实测 154 帧 DBD 只回了 40 个 ACK；98 帧星历回 92 个。

### 2.4 模组库里有没有：DBD 倒库 + 逐字节搜索

`ctl gnss dump_out` → `mga_<utc>.ubx`（`0x13/0x80`），再用
`zcode/analysis/ubx_eph_in_dbd.py <灌进去的文件> <倒库文件>` 做**精确子串搜索**：

- 参数区是高熵的，随机连续对上 12 字节的概率约 2⁻⁹⁶；脚本内置**随机窗口反向对照**（应为 0 命中）。
- 命中要**看匹配到的字节本身**（不能是零串/重复模式），否则是低熵假阳性。
- 实测结论（2026-09-27，注入后 5.1 s 就倒库、无覆盖时间）：**GAL 30/30、BDS 32/32 命中；
  GPS 0/31、QZSS 0/5**。而 LNAV 子帧流也不存在（`ubx_dbd_bitprobe.py`：前导码 0x8B 命中
  41 次 vs 随机基线 43）⇒ **倒库认不出我们的 GPS/QZSS 字节，是接收机侧的存储表示问题，
  不是打包错**（打包已逐字段核对，见 §2.1 与 ACK 的 26/31 收下）。

---

## 3. 注意事项

### 3.1 精度绝不能报得比实际好

`tAcc` / `posAcc` 是**随数据一起写的自报值，模组无法验证**。报紧了，模组把搜索窗收得比误差还窄，
真卫星落在窗外就永远找不到 —— **"错误的辅助比不发更糟"**。
固件的补救：`gnss_eph_assist_fixup()` 按年龄放大（`posAcc = max(原值, age×50 m/s)`、
`tAcc = max(原值, age)`），年龄算不出来时**直接丢这一帧**。
u-blox 的乐观启动要求 `pacc ≤ 100 km`。

### 3.2 绝不重标 TOE

星历过期就**连着旧 TOE 一起发、或者干脆不发**。把 TOE 改成"当前时刻"是唯一能把
"可被识别为旧数据（模组能降级处理）"变成"看起来新鲜但是错的"的操作。

### 3.3 年龄门限只在 RTC 已对时时才生效

`eph age utc=… now=… stale=…` 里 `now=0` 表示时基无效（未对时）⇒ `eph_stale` 恒 false
⇒ **EPH 帧不会因超龄被丢**。实测 237 行里 ~194 行是 `now=0`。且门限用的是**文件名/mtime**，
不是帧内 TOE ⇒ "时间戳新、内容旧"的组合会被放行（§2.1 第 4 条就是补这个洞）。

### 3.4 GPS 的 offset 66-67 是 `reserved2`，不是 `week`

两个独立来源一致：pyubx2 的 `ubxtypes_set.py`（`"idot": (I2,P2_N43), "reserved2": U2`）与
`jkivilin/ubx-mga-gnss-rinex-ephemeris-converter` 的 `build_mga_gps_eph()`（`0x0000, # reserved2`）。
**u-blox 8 手册在这一点上有误**（那份参考实现的 README 专门警告过"有些 MGA 字段描述是错的"）——
别再把它当"没填的 week"去改。

### 3.5 位置辅助必须来自当下的定位

App 侧 `_assistHead()` 用的是 `getLastKnownPosition() ?? getCoarsePosition()`：
Android 的 last-known 缓存几乎不返回 null ⇒ **永远在用陈旧坐标**（实测四次推送跨 9 分钟，
`INI-POS` 帧**逐字节相同**），而且把 Android 记录的**原始那次**定位精度原样透传，
于是"1.25 km 外的旧坐标"配着"±48.9 m"送进模组。**加年龄门槛（超 60~120 s 就强制取新定位）
+ 按年龄加漂移量**。

### 3.6 倒库相关

- `ctl gnss dump_out` **要求当前有 2D/3D 定位**（`need a live 2D/3D fix first` 是代码策略，
  见 `myvendor_gnss.c` 的 `myvendor_gnss_dump_request()`；工厂模式与 idle park 也会拒）。
- ~~**它会误报失败**~~ **已修**（2026-09-27）：原来只以 `!dump_busy()` 判完成，而它在倒库
  进行中会**短暂为 false** ⇒ ctl 打 `finished without file` 就返回，而文件 5 秒后才正常出现
  （现场：t=2899.88 报失败、t=2905.00 打出 `dumped 154 DBD messages …`）。
  现在改为"**连续 0.5 s 空闲且已拿到文件路径**或整体超时（60 s）"才收场 ⇒
  看到 `dump_out: done -> <路径>` 就是真的成了。**旧固件才有那个现象**。
- **倒库里比两份 dump 不要按位置对齐**：每条记录带自增序号，整条流会错位一格，
  看起来"全变了"。有效判据是**内容集合**（payload 哈希后比集合），工具 `ubx_dbd_cmp.py`。
- **MGA-DBD 的载荷是不透明的**：按 pyubx2 是 `reserved1(U12)` + 一串 `data(U1)`，
  库/文档层都不解它 ⇒ 逐字节搜索（§2.4）是唯一可行的内容判据。

### 3.7 App ↔ 板子传文件（`ctl fs`）

- **板子 tty 输入缓冲只有 `CONFIG_UART_RXBUFSIZE`（本工程 256）字节** ⇒ 主机每批**只能发 1 行**
  base64（77 B）。发多了丢字节、板子读到坏行。
- 主机发送期间**必须自己排空 rx**：hub 是给所有客户端扇出的，任何一个客户端不读都会
  把 hub 的发送堵住，堵住期间数据一个字节都进不了串口（board 15 s 超时 FAIL）。
- 写入先落 `<完整文件名>.part` 再 `rename`：`gnss_eph_src()` 选源只要求"文件名能解出 UTC +
  头 6 字节是 UBX + 大小 > 64"，半截文件完全满足、会被当星历灌进模组。

### 3.8 文件命名

必须是 `mga_<unix>.ubx` 或 `mga_YYYYMMDDTHHMMSSZ.ubx`（`gnss_eph_parse_utc()` 只认这两种）。
注入器按**文件名的 UTC** 排序取最新那份。窗口只保留 5 份（`GNSS_EPH_KEEP`）。

---

## 4. 官方参考

| 文档 | 编号 | 用途 |
|---|---|---|
| MAX-M10S Data sheet | **UBX-20035208** | 产品/电气/引脚；**cold start 23~28 s、hot start 1 s**；`V_BCKP` 备份电源脚、backup 模式条件 |
| **u-blox M10 SPG 5.10 Interface description** | **UBX-21035062** | ★ **UBX 消息字段表在这本**（MGA-*-EPH / MGA-DBD / MGA-ACK / CFG 项） |
| MAX-M10S Integration manual | **UBX-20053088** | AssistNow 用法、"乐观启动"条件、精度声称口径 |
| u-blox M10 SPG 5.10 Release notes | UBX-22001426 | 行为变更 |
| IS-GPS-200 | — | toe/toc 的 16 s 网格、LNAV 子帧位布局的最终出处 |

**已落本地的第三方参考**（不用去网上找）：

- `/tmp/ubxconv` — `jkivilin/ubx-mga-gnss-rinex-ephemeris-converter`：RINEX→MGA 的独立实现
  （含 ICD 子帧位解码器 `decode_gps_subframes`）。README 第 215 行警告"u-blox 8 手册里
  有些 MGA 字段描述是错的，本工具靠 pyubx2 + 接收机实测行为交叉校正过"。
- `/tmp/pyubx2pkg/src` — pyubx2 的 `ubxtypes_set.py`，有全部 `MGA-*-EPH` 字段表。
  **核任何 UBX 偏移优先查这两个。**

> **原文摘录见 [附录 A](#附录-a官方文档原文摘录)**（datasheet 的 TTFF / 备份模式 / A-GNSS 表，
> 每条注明它支撑本文哪条结论）；**两个第三方参考的代码片段见 [附录 B](#附录-b参考工程代码片段)**
> （pyubx2 的三张 EPH 字段表与 MGA-DBD 定义、参考实现的 `reserved2`/`infoCode`/倒库协议/子帧位布局）。
> 附录是本文所有字段表与判据的**原文出处**，表里有疑问先回附录比对，不要凭记忆重打一遍。

---

## 5. 工具清单

| 工具 | 用途 |
|---|---|
| `zcode/analysis/ubx_eph_dump.py <文件>` | 离线字段级校验（§2.1 四条判据） |
| `zcode/analysis/ubx_eph_in_dbd.py <灌入> <倒库>` | 在倒库里逐字节搜"我们灌进去的参数" |
| `zcode/analysis/ubx_dbd_bitprobe.py <倒库>` | 找位打包的导航子帧流（前导码周期） |
| `zcode/analysis/ubx_dbd_cmp.py <旧> <新>` | 两份倒库的内容集合对比（**不要**按位置对齐） |
| `zcode/ephfile.py ls\|get\|put` | 主机 ↔ 板子传文件（经 hub，不抢 tty） |
| `ctl fs ls\|get\|put` | 同上，板子侧 |
| `ctl gnss dump_in` / `dump_out` | 手动注入 / 手动倒库 |
| `sys gnss` / `sys onboard` | 模组状态：**看有没有 `search` 这个词**判断是否有效定位 |

---

## 6. 现状与未结（2026-09-27）

**已核实**：

- App 四个星座的打包偏移与标度**逐字段核对无误**（对照 pyubx2 字段表）⇒ **星历计算方式不用改**。
- 传输侧 `badck=0 brk=eof changed=0`，注入 `frames=100`。
- 模组**收下了**：`mga ack gps=26/2 qzss=5/0 gal=27/1 bds=28/3`；
  补记 `pl[0]` 后确认**少数拒收是 NAK**（`type=0`），不是"accepted 带非 0 code"。
- **App 唯一要改的是位置辅助**（坐标冻结 + 原样透传旧精度）—— **已改**（`eph_service.dart`：
  `_posMaxAge` 2 分钟年龄界 + `_posDriftMps` 15 m/s 外推进 `posAcc`）。

**未结**：

1. **倒库里认不出 GPS/QZSS 字节**（GAL/BDS 认得出）⇒ 接收机侧存储表示问题，不影响功能，
   但它意味着**倒库不能当内容判据**。
2. **TTFF 没有日志判据**：全日志没有任何 GNSS `valid=1`（`first publish` / `snapshot live`
   每次开机各只打一次且都是 `valid=0`）⇒ 量 TTFF 只能每 10 s 轮询 `sys onboard`，
   第一条不含 `search` 的即首次定位。**建议补一行"首次有效 publish 打一条日志"。**
   已知参照：datasheet **cold start 23~28 s / hot start 1 s**，而实测一次是 23.4~26.5 min
   （但不知何时拿到户外，**不能归因给注入**）。
3. **NAK 的 `infoCode=6` 语义**待接口手册确认（见 §2.3 的注）。
4. **V_BCKP 是否接、接哪儿**（原理图待查）：datasheet §3 写明 hardware backup mode 的条件是
   `VCC=0 **且** V_IO=0`、且 **PIO 不得被驱动**；而板子是 PA43 直接切 VCC、且注释写着
   "host UART stays open"。若备份域是活的，**hot start 1 s 是模组白送的**，整套注入管线在
   架构上就是可省的；若没接，注入就是必须的。

---

# 附录 A：官方文档原文摘录

> 出处：**MAX-M10S Data sheet, UBX-20035208 - R08**（本机 `/home/jinsc/esp/Desktop/MAX-M10S_DataSheet_UBX-20035208.pdf`；
> 另有一份内容同源的中文规格书 `C24834155_…_WJ1195077.PDF`，在 `/home/jinsc/esp/Desktop/`）。
> 抽取方式 `pdftotext -q <pdf> -`，下面按原文引，**方括号里是本项目用它支撑哪条结论**。

## A.1 相关文档（Data sheet "Related documents"）

```
[1] MAX-M10S Integration manual, UBX-20053088
[2] u-blox M10 SPG 5.10 Interface description, UBX-21035062
[3] u-blox M10 SPG 5.10 Release notes, UBX-22001426
[4] Product packaging reference guide, UBX-14001652
[5] Joint IPC/JEDEC standard, www.jedec.org
```

**[用途]** 本文 §1 的全部字段表**不在 datasheet 里**——字段表的官方出处是 **[2] UBX-21035062**；
AssistNow 的设计口径与"精度声称"指导在 **[1] UBX-20053088**。
我们现在手上只有 datasheet，所以字段表目前是拿两个第三方来源交叉核的（见附录 B）。

## A.2 启动时间（Table 2 "MAX-M10S typical performance in multi-constellation GNSS modes"）

```
Time To First Fix (TTFF) 5, 7, 8
    Cold start    28 s   23 s   27 s   28 s
    Hot start      1 s     1 s     1 s     1 s
```
（列顺序 = GPS / GLONASS / BDS B1I / GALILEO；另一档表里 cold start 为 29/27/30/41 s。）

**[用途]** 这是 §6 里"实测 23.4~26.5 min" 的参照系：**比 datasheet 的冷启动还慢约 50 倍，
hot start 是 1 s 量级**。也就是说"定位慢"这件事的量级不是几何条件能解释的，
它把矛头指向"注入到底有没有起作用"（§2.3/§2.4）。

## A.3 备份模式与引脚状态（§3 Pin definition, Table 11 Pin state）

```
In reset mode (RESET_N = low), all PIOs are configured as input pull-up.
In hardware backup mode (VCC = 0 V and V_IO = 0 V), PIOs must not be driven.
```
引脚表里有 **`V_BCKP` — Backup voltage supply**（绝对最大 3.6 V）；特性表列
**`Backup modes: Hardware backup mode and software standby mode`**。

**[用途]** §6 未结第 4 条。板子是 **PA43 直接切 VCC**（`bsp_pinmux.c` 注释
"official low-power cut; host UART stays open"）——**"UART 常开"和"PIOs must not be driven"
是冲突的**，值得对着原理图确认（也决定 hot start 1 s 是不是白送）。

## A.4 协议与固件版本

```
ROM SPG 5.10
UBX
NMEA versions 2.1, 2.3, 4.0, 4.10 and 4.11 (default)
```

**[用途]** 与板上 `mon ver sw=[ROM SPG 5.10 (7b202e)]` / `PROTVER=34.10` 对上，
确认我们对着的是 **M10 SPG 5.10** 这一代（消息集与 M8 有差异，别照 M8 的手册做）。

## A.5 支持的 A-GNSS 服务（Table 5）

```
AssistNow Live Orbits        GPS L1C/A, Galileo E1, QZSS L1C/A, GLONASS L1OF, BeiDou B1I
AssistNow Predictive Orbits  GPS L1C/A, Galileo E1, GLONASS L1OF
AssistNow Autonomous         GPS L1C/A, Galileo E1, GLONASS L1OF, QZSS L1C/A, BeiDou B1I
```

**[用途]** 说明四星座（含 QZSS）都在 AssistNow 覆盖内 ⇒ **我们往四个星座都灌星历是设计内行为**，
不是"灌了它用不到的东西"。（顺带：本板 GLONASS 在 `CFG-SIGNAL` 上是 NAK 的，见
`myvendor_gnss.c` 里"不再写 GLONASS"那段注释。）

---

# 附录 B：参考工程代码片段

两个来源，都已落本地（不用联网）：
**B1** = `pyubx2` 1.3.7（`/tmp/pyubx2pkg/src/pyubx2/`）；
**B2** = `jkivilin/ubx-mga-gnss-rinex-ephemeris-converter`（`/tmp/ubxconv/`）。

## B.1 pyubx2 —— 字段表的第三方权威（用于核 §1 的每一行）

`ubxtypes_set.py`：

```python
    "MGA-GPS-EPH": {
        "type": U1, "version": U1, "svId": U1, "reserved0": U1,
        "fitInterval": U1, "uraIndex": U1, "svHealth": U1,
        "tgd": (I1, P2_N31), "iodc": U2, "toc": (U2, P2_P4),
        "reserved1": U1, "af2": (I1, P2_N55), "af1": (I2, P2_N43), "af0": (I4, P2_N31),
        "crs": (I2, P2_N5), "deltaN": (I2, P2_N43), "m0": (I4, P2_N31),
        "cuc": (I2, P2_N29), "cus": (I2, P2_N29), "e": (U4, P2_N33), "sqrtA": (U4, P2_N19),
        "toe": (U2, P2_P4), "cic": (I2, P2_N29), "omega0": (I4, P2_N31),
        "cis": (I2, P2_N29), "crc": (I2, P2_N5), "i0": (I4, P2_N31),
        "omega": (I4, P2_N31), "omegaDot": (I4, P2_N43), "idot": (I2, P2_N43),
        "reserved2": U2,          # ← offset 66-67：保留，不是 week
    },
```

```python
    "MGA-GAL-EPH": {
        "type": U1, "version": U1, "svId": U1, "reserved0": U1,
        "iodNav": U2, "deltaN": (I2, P2_N43), "m0": (I4, P2_N31), "e": (U4, P2_N33),
        "sqrtA": (U4, P2_N19), "omega0": (I4, P2_N31), "i0": (I4, P2_N31),
        "omega": (I4, P2_N31), "omegaDot": (I4, P2_N43), "iDot": (I2, P2_N43),
        "cuc": (I2, P2_N29), "cus": (I2, P2_N29), "crc": (I2, P2_N5), "crs": (I2, P2_N5),
        "cic": (I2, P2_N29), "cis": (I2, P2_N29),
        "toe": (U2, 60), "af0": (I4, P2_N34), "af1": (I4, P2_N46), "af2": (I1, P2_N59),
        "sisaIndexE1E5b": U1, "toc": (U2, 60), "bgdE1E5b": I2,
        "reserved1": U2, "healthE1B": U1, "dataValidityE1B": U1,
        "healthE5b": U1, "dataValidityE5b": U1, "reserved2": U4,
    },
```
（`toe`/`toc` 的 `60` 就是 **×60 s** 那个单位；GPS 的 `P2_P4` = **×16 s**。）

```python
    "MGA-BDS-EPH": {
        "type": U1, "version": U1, "svId": U1, "reserved0": U1,
        "SatH1": U1, "IODC": U1, "a2": (I2, P2_N66), "a1": (I4, P2_N50), "a0": (I4, P2_N33),
        "toc": (U4, P2_P3), "TGD1": (I2, E1_N1), "URAI": U1, "IODE": U1,
        "toe": (U4, P2_P3), "sqrtA": (U4, P2_N19), "e": (U4, P2_N33),
        "omega": (I4, P2_N31), "Deltan": (I2, P2_N43), "IDOT": (I2, P2_N43),
        "M0": (I4, P2_N31), "Omega0": (I4, P2_N31), "OmegaDot": (I4, P2_N43),
        "i0": (I4, P2_N31), "Cuc": (I4, P2_N31), "Cus": (I4, P2_N31),
        "Crc": (I4, P2_N6), "Crs": (I4, P2_N6),
        "Cic": (I4, P2_N31), "Cis": (I4, P2_N31), "reserved1": U4,
    },
```
（`P2_P3` = **×8 s**；`Cuc/Cus/Crc/Crs/Cic/Cis` 是 **I4**，与 GPS/GAL 的 I2 不同。）

```python
    "MGA-DBD": {
        "reserved1": U12,
        "data_grp": ("None", {"data": U1}),
    },  # repeating group
```
**[用途]** 这是 §3.6 那条"**DBD 载荷不透明**"的出处：**库层面也不解它的内容**，
前 12 字节只标 `reserved1`，其后是一串 `U1` —— 所以 §2.4 的逐字节搜索是唯一可行的内容判据。

`ubxtypes_core.py` 的消息标识表（**注意 MGA 用三字节、末字节是载荷 `type`**）：

```python
    b"\x13\x60\x01": "MGA-ACK-DATA0",
    b"\x13\x60\x00": "MGA-NAK-DATA0",     # ← 首字节 pl[0] 区分 ACK / NAK
    b"\x13\x00\x01": "MGA-GPS-EPH",
    b"\x13\x02\x01": "MGA-GAL-EPH",
    b"\x13\x03\x01": "MGA-BDS-EPH",
    b"\x13\x05\x01": "MGA-QZSS-EPH",
    b"\x13\x80":    "MGA-DBD",            # ← 倒库，只有两字节
```
**[用途]** §2.3 里"ACK/NAK 靠 `pl[0]` 区分、我们目前没记它"的出处，也是 §6 未结第 1 条的下一步。

## B.2 ubx-mga 参考实现 —— 打包与倒库的实操参照

`ubx_tool.py`（`build_mga_gps_eph()` 尾部，**`reserved2` 写 0 的直接证据**）：

```python
    payload += struct.pack('<hH',
        raw['idot'],            # idot (I2) -- 14-bit sign-extended to 16
        0x0000,                 # reserved2 (U2)
    )

    assert len(payload) == 68, f"MGA-GPS-EPH payload must be 68 bytes, got {len(payload)}"

    # Build complete UBX frame
    header = struct.pack('<2B2BH', 0xB5, 0x62, mga_class, mga_id, len(payload))
    frame_data = header[2:] + payload  # class + id + length + payload (for checksum)
    ck_a, ck_b = 0, 0
    for b in frame_data:
        ck_a = (ck_a + b) & 0xFF
        ck_b = (ck_b + ck_a) & 0xFF
    return header + payload + bytes([ck_a, ck_b])
```

`ubx_tool.py`（`infoCode` 表）：

```python
MGA_ACK_INFO_CODES = {
    0: 'accepted',
    1: 'no time (send MGA-INI-TIME first)',
    2: 'type not supported',
    3: 'size mismatch',
    4: 'database store failed',
    5: 'not ready',
    6: 'unknown type',
}
```
**[用途]** 与固件 `gnss_mga_info()` 对比：**code 2 应为 `type not supported`（不是 `bad-version`）、
code 4 应为 `database store failed`（比 `not-stored` 更明确）**，见 §2.3 的注。

`ubx_tool.py`（开 ACK，老一代走 `CFG-NAVX5`；本板走 `CFG-NAVSPG-ACKAIDING`）：

```python
def enable_mga_ack(ser):
    """Enable MGA-ACK flow control via CFG-NAVX5."""
    # mask1 bit 10 = ackAid
    msg = UBXMessage('CFG', 'CFG-NAVX5', SET,
        version=0, mask1=0x0400, mask2=0, ackAiding=1)
    send_ubx(ser, msg)
    # Wait for CFG ACK
    ack = wait_for_ack(ser, 0x06, 0x23, timeout=1.0)
    return ack is True
```

`ubx_tool.py`（`cmd_dump_dbd()`，**倒库协议与我们固件一致、且同样要求有定位**）：

```python
    poll_msg = UBXMessage('MGA', 'MGA-DBD', POLL)
    ...
    # MGA-DBD response: multiple messages, terminated by empty payload MGA-DBD.
    # Problem: NMEA keeps flowing, so serial timeout never fires.
    # Use deadline: if no new MGA-DBD received within gap_timeout, assume done.
    gap_timeout = 3.0  # seconds with no MGA-DBD = done
    ...
                if payload_len <= 0:
                    # Empty MGA-DBD = end of dump
                    break
    ...
    if not mga_messages:
        print("No MGA-DBD data received. Is the receiver running and has a fix?",
              file=sys.stderr)
        sys.exit(1)
    ...
    # Write to file
    with open(output, 'wb') as f:
        for msg in mga_messages:
            f.write(msg)
    ...
    print(f"Saved {len(mga_messages)} MGA-DBD messages ({total_bytes} bytes) to {output}")
```
**[用途]** ① **poll 用空载荷、以一个空载荷 MGA-DBD 结束**——与我们 `gnss_dbd_try_start()` 的
`gnss_ubx_send(fd, 0x13, 0x80, NULL, 0)` 一致；② 它把消息**原样写回文件**（"for backup/comparison"）
⇒ 印证 §3.6"DBD 是原样存/原样回灌"；③ **"Is the receiver running and has a fix?"** 独立佐证
§3.6 那条"倒库要有定位"。注意它的 `gap_timeout` 逻辑解释了我们的 `ack=0` 那次为何也能完成倒库。

`ubx_tool.py`（ICD 子帧位布局，`decode_gps_subframes()` 节选 + `URA_TABLE`）：

```python
    # Subframe 1: Clock and health parameters
    # Word 3 (sf1_words[0]): WN, L2 code, URA, SV health, IODC MSBs
    wn = extract_bits(sf1_words[0], 1, 10)
    ura_index = extract_bits(sf1_words[0], 13, 4)
    sv_health = extract_bits(sf1_words[0], 17, 6)
    iodc_msb = extract_bits(sf1_words[0], 23, 2)
    # Word 7 (sf1_words[4]): TGD (bits 17-24)
    tgd_raw = twos_complement(extract_bits(sf1_words[4], 17, 8), 8)
    tgd = tgd_raw * (2**-31)  # seconds
    # Word 8 (sf1_words[5]): IODC LSBs (bits 1-8), toc (bits 9-24)
    iodc = (iodc_msb << 8) | iodc_lsb
    toc_raw = extract_bits(sf1_words[5], 9, 16)
    toc = toc_raw * 16  # seconds        # ← ×16 s，第三方再证
```
```python
URA_TABLE = [2.4, 3.4, 4.85, 6.85, 9.65, 13.65, 24, 48,
             96, 192, 384, 768, 1536, 3072, 6144, 6145]
```
**[用途]** `toc = toc_raw * 16` 是 **toe/toc 单位 = 16 s** 的第三方独立确认（本文 §1.1 第 2 条）；
`URA_TABLE` 是 `uraIndex` 那一栏（§1.2 的 offset 5）的查表依据；子帧位布局用于
`ubx_dbd_bitprobe.py` 判断"库里有没有位打包的 LNAV 流"。

`README.md`（两句关键论断）：

```
By injecting current ephemeris data, the receiver can skip downloading navigation data
from satellites (which takes 12.5+ minutes for a full almanac cycle) and achieve a fix
within seconds — particularly useful for receivers with limited sky visibility.
```
```
- **u-blox documentation errors**: Some MGA message field descriptions in the u-blox 8
  receiver description contain errors that were identified and corrected during
  development by cross-validating against pyubx2 source code and the receiver's actual
  behavior.
```
**[用途]** 第一句是"注入能省掉什么、省多少"的量级依据（**不注入 → 光解导航数据就要 12.5+ 分钟**，
这与我们实测的 23.4~26.5 min 是同一量级——**反过来说明那次注入很可能没起作用**，见 §6 未结第 3 条）；
第二句是 §3.4"**别再信 u-blox 8 手册里的 MGA 字段名**"的出处（`week` 那个坑就是这么来的）。

