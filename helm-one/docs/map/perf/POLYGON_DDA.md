# 多边形扫描填充：去掉"逐行 64 位软除"（方案 + 测试结果）

> **一句话**：`fill_polygon_fp` / `fill_polygon` 原来每个扫描行、每条活动边都做一次
> `int64 / int64`（这颗 M33 上走 `__aeabi_ldivmod`，150~300 周期）；改成**每条边只在
> 被激活时算一次斜率与余数、之后逐行增量推进**。像素**逐位不变**（宿主差分 14 种配置
> ≈30 万例全 IDENTICAL），上板定点自测 A/B：多边形光栅化 **−7.6%**（折算整帧约 −3%）。
>
> 落地：`vendor/HelmOne` 提交 `69e64984`（分支 `retire-eval`）。
>
> 配套原始数据：[MEASUREMENTS.md §9](./MEASUREMENTS.md)；验证怎么做：
> [TESTS.md](./TESTS.md) §2.1；优化记录一览：[README.md](./README.md) §4.6。

---

## 1. 先量：这条值多少

`vmap_render.c` 加了图元级周期埋点（`VMAP_RENDER_PRIM`，**只读 CYCCNT、只加计数**），
`vmap_view.c` 每帧跟着 `[vperf] frame` 打一行：

```text
[vperf] prim polyfp=<cyc> npfp=<n> poly=<cyc> np=<n> disc=<cyc> nd=<n> \
               cross=<n> scan=<n> h=<6 桶> cn=<6 桶>
```

| 字段 | 含义 |
|---|---|
| `polyfp` / `poly` | `fill_polygon_fp`（AA：折线四边形 + AA 多边形）/ `fill_polygon`（面要素填充）的**周期数** |
| `disc` | `fill_disc_aa_fp`（圆盘 / 折线拐点）的周期数 |
| `cross` | **逐行软除次数**（活动边 × 行）—— 改动前的成本主体 |
| `scan` | **（行 × 边）扫描次数** —— "没有活动边表"的代价（这次没动） |
| `h` / `cn` | 点数 n 的直方图 / 按 n 分桶的周期数（分桶：≤4 / ≤8 / ≤16 / ≤32 / ≤256 / >256） |

`ctl dvfs 240` 钉频、GPX 回放 + 导航，7 帧（z=14，`ms = cyc / 240e3`）：

| 帧 | `cyc_tile` | `polyfp` | `poly` | `disc` | 三者合计 | `cross` | `scan` |
|---|---|---|---|---|---|---|---|
| 全量 `dirty=0 tiles=16 warms=4` | 235 ms | 48.6 | 69.9 | 47.9 | **70.7%** | 23,120 | 272,356 |
| 补边 `dirty=6 tiles=4` | 152 ms | 43.3 | 29.2 | 38.5 | 73.2% | 26,872 | 247,161 |
| 补边 `dirty=4` | 135 ms | 37.2 | 23.8 | 35.2 | 71.2% | 23,686 | 188,097 |
| 补边 `dirty=5` | 96 ms | 25.7 | 8.6 | 23.0 | 59.7% | 17,332 | 135,080 |
| 全量 `warms=0` | 185 ms | 42.5 | 34.2 | 36.6 | 61.1% | 20,968 | 146,662 |
| 补边 `dirty=3` | 78 ms | 15.6 | 22.2 | 8.7 | 59.5% | 11,614 | 90,491 |

**判据**：三个图元占 `cyc_tile` 的 60~73%（多边形约 40~50%），一帧 1.2~2.7 万次逐行软除
⇒ 值得做；天花板 = `cross × 每次软除成本`，**不含** `scan`（那是活动边表的事，另算）。

`cn` 分桶（累计、4 帧）显示**没有任何一次多边形调用的 n > 256**：

| n 桶 | ≤4 | ≤8 | ≤16 | ≤32 | ≤256 | >256 |
|---|---|---|---|---|---|---|
| 周期占比 | 20~22% | 6~8% | 20~23% | 22~32% | 20~27% | **0** |

⇒ 状态数组开 256 条边就能覆盖真实负载的全部边（见 §2.3）。

---

## 2. 方案

### 2.1 原式与增量式

改动前（`fill_polygon_fp`；`fill_polygon` 是同一形状的整数版）：

```c
x_fp = (int32_t)((int64_t)(y_scan_fp - yi_fp) * (xj_fp - xi_fp)
                 / (yj_fp - yi_fp) + xi_fp);
```

以 `dy` 为除数、`dx` 为斜率、每行 `y_scan` 涨一个常量（fp 路径 = `VMAP_FP_ONE`，
整像素路径 = 1），于是"分子"每行只加一个常量 `step`。难点是必须**逐位**复现 C 的
**向零截断**除法。最终形式（细节见 `vmap_render.c` 的注释）：

```text
M   = |dy|                                  > 0
sgn = sign(dx)                              （商的方向；dx == 0 时取 +1）
P_k = |dx| * |y_scan_k - yi|                ≥ 0，每行只加常量 G
A_k = floor(P_k / M),  a_k = P_k - A_k*M     （0 ≤ a_k < M）
x_k = xi + sgn * A_k                        （= 原式）
```

每行的推进只剩（`gm = sgn*floor(G/M)`、`gr = G mod M`，装配时算一次）：

```c
a += gr;  if (a >= M) { a -= M; x += sgn; }  x += gm;
```

一次加法、一次比较、最多一次 ±1，没有乘法、没有除法、没有累积误差
（维护的是 floor 分解的恒等式本身）。

### 2.2 为什么不能用"朴素余数增量"（踩过的坑）

最自然的写法是维护"商 + 余数、余数超界就补一次 ±1"（`|余数| < |dy|`）。**它必错**：
截断除法的**余数符号约定**也是解的一部分 —— `n = q*dy + r` 在 `|r| < |dy|` 之下
有两个解，只有"`sign(r) == sign(n)`"那个才是 `trunc` 的那一个。只保证 `|r| < |dy|`
会让商漂一位；实测 `dy < 0, dx > 0` 的边**每行偏 1**（宿主差分当场抓到，定位脚本
逐行打印"新实现 vs 原式"的交叉点）。

另一条已经排除的实现细节：**每条边的首行**仍按定义算一次（`A = floor(|dx|*|Δy|/|dy|)`），
所以每条边只有这一次 64 位除法，之后全靠推进 —— 这也是"逐位相同"的锚点。

### 2.3 实现边界

| 项 | 取值 | 依据 |
|---|---|---|
| 状态槽上限 `VMAP_POLY_DDA_CAP` | **256** | `cn` 分桶实测：真实负载没有 n > 256 的调用（§1）；超过的边**照旧逐行算**（像素不变） |
| 状态放哪 | `.bss`（256 × 24 B = 6 KB） | 实测 `bicycle_ui` 栈只剩 ~10 KB（`ps`：24.3/34.7 KB），不放栈 |
| 槽号 | `= 边号 i` | 见 §2.4：取模复用被实测否掉 |
| 残留识别 | `gen`（每次调用递增） | 静态数组的"残留"只可能来自更早的调用，`gen` 单调递增 ⇒ 可证 |

改动只落在 `fill_polygon_fp` / `fill_polygon` 内部（含它们共用的 4 个 static helper），
`vmap_render_draw_tile` 与各层函数**一行没动** —— 那条路没有宿主覆盖（[TESTS.md](./TESTS.md) §4.1）。

### 2.4 被实测否掉的方案：槽按 `i & 63` 取模复用

动机是省内存（128 KB 的多边形也不可能开 4096 个槽）。看着很美：槽数固定、与 n 无关，
撞车时用"`idx` 对不上就重算"兜底。

**实测反而更慢**：大面要素同时激活的边有一二十条，下标相差 64 的整数倍会**成对**撞在
同一个槽上，于是每行都要重新装配（**两次** 除法/条）⇒ `poly` 段 **+13%**，比不改还慢。
已回退，理由写进了代码注释（免得后人再试一遍）。

### 2.5 上限在哪：软除换成了"状态回写"

定点自测（§3.3）显示**每次交叉点只省下 12~36 周期**，而软除本身是 ~200 周期。
机制：增量推进每行要把状态（`x`、`a` 两个 int32）写回内存，而这块 PSRAM 是
**write-through**（同一目录 MEASUREMENTS.md §2.5 实测：**4 字节 store ≈ 88 周期**），
2 次 ≈ 176 周期 ≈ 正好抵消掉那次软除。

⇒ 想把这 200 周期全拿到，只有两条路（都留作后续，不在这次范围内）：

1. **让状态留在寄存器**：把循环反过来 —— 外边逐条边走完它自己的行、把交叉点按行
   累积起来，最后逐行排序出 span。这样每条边只在激活时碰一次内存。
2. **先降 PSRAM 的 store 成本**：画布/状态专用的 write-back MPU region
   （MEASUREMENTS.md §2.5 结论与"还没做的"清单）。

另外这次**没有**活动边表：`scan`（行 × 边）一帧 9~27 万次仍在。对 n 大的面要素，
那是比软除更大的一块（虽然每次迭代便宜）；要做就得接受"交叉点顺序"与 64 条上限的
语义边界（见 §3.1 的覆盖说明）。

---

## 3. 测试结果

### 3.1 宿主差分：像素逐位不变

`vendor/HelmOne/docs/map/perf/verify/harness.c`，拼在 `vmap_render.c` 之后编成同一
TU，两个二进制（`120d2af1` 改动前 / 工作区改动后）吃同一批输入、比对整块画布与逐用例哈希。

**这次为它补了一组"多边形专项"**（`T_POLY_CASES`）：随机用例的 y 量程最多盖 8 行
（AA 那条只有 ±3.5 px），压不到"跨几十行"的边，而这类改动的错**只在跨行时才露**。
专项用确定性构造补齐三件事：跨满画布的边、n 超过状态容量的多边形（65~96 点）、
半开区间 `[yi, yj)` 的边界（顶点正好落在扫描行**下界**与**中心**上），外加水平边 /
重复点 / 零长度边 / `dy = ±1` 的细长条。

| 配置 | 用例 | 结果 |
|---|---|---|
| `-O0` / `-O1` / `-O2` × `AA=1` / `AA=0` × 两个种子 = 12 组 | 每组 20000 随机 + 2000 专项 | **全部 IDENTICAL** |
| `-O0/-O1/-O2` 的另一档：`-DT_CANVAS_W=101 -DT_CANVAS_H=301`（跨 300 行） | 每组 20000 + 3000 | **全部 IDENTICAL** |
| UBSan（`-fsanitize=undefined`） | 3000 + 1000 | 改动前后报告**完全相同**（都是 `fill_disc_aa` 里 `cx << 8` 的老报告，与多边形这条路无关，没动它） |

合计约 **30 万例**。命令（桩与两个假头已入库，可照抄）：

```bash
V=vendor/HelmOne/boards/sf32lb52/helmone/ui/bicycle/src/vmap
ST=vendor/HelmOne/docs/map/perf/verify/stubs_host.c
FA=vendor/HelmOne/docs/map/perf/verify/fake
H=vendor/HelmOne/docs/map/perf/verify/harness.c
FLAGS="-x c - -I$FA -I$V -O1 -ffunction-sections -fdata-sections -Wl,--gc-sections -lm \
       -DVMAP_RENDER_PRIM=0 -DT_CASES=20000 -DT_POLY_CASES=3000"
( git -C vendor/HelmOne show 120d2af1:boards/sf32lb52/helmone/ui/bicycle/src/vmap/vmap_render.c; \
  cat $V/vmap_blit.c; cat $ST; cat $H ) | gcc $FLAGS -o /tmp/vmap_old
( cat $V/vmap_render.c; cat $V/vmap_blit.c; cat $ST; cat $H ) | gcc $FLAGS -o /tmp/vmap_new
diff <(/tmp/vmap_old) <(/tmp/vmap_new) && echo IDENTICAL
```

**覆盖边界（说清楚没证到什么）**：只覆盖像素原语。瓦片/矢量那条路
（`vmap_render_draw_tile` 及其层函数）没有宿主覆盖 —— 这次改动也没碰它们；
另外"顶点恰好落在扫描行上"之外的所有退化情形（自交、共线、巨大坐标）只有随机覆盖。

### 3.2 静态证据：软除调用点从"行循环里"挪到了"每条边一次"

用**设备侧真实编译参数**（`arm-none-eabi-gcc -O1 -mcpu=cortex-m33`，取自
`cmake_out/helmone_nsh/compile_commands.json`）编出两份 `.o`，数 `R_ARM_THM_CALL __aeabi_ldivmod`：

| 函数 | 改动前 | 改动后 |
|---|---|---|
| `fill_polygon_fp` | 141 条指令，**1 处**（在行循环里） | 331 条指令，2 处：seed（每条边**激活时**一次）+ "n > 256" 的兜底原式（实测一帧 **0 次**） |
| `fill_polygon` | 133 条指令，**1 处**（在行循环里） | 318 条指令，2 处（同上） |
| `vmap_poly_dda_arm` | — | 63 条指令，2 处（**每条边一次**；本工具链不把 `/` 与 `%` 合成一次 libcall —— 单独编一个 `q=a/b; r=a%b;` 也是两次，所以这不是写法问题） |

即：**行循环的常规路径里不再有除法**，剩下的调用都按"每条边一次"计。

### 3.3 上板定点自测 A/B（主证据）

真帧逐帧对比**测不出**这条：地图帧与帧之间的内容差异有 ~10%，而效应是几个百分点
—— 拿真帧比是"噪声盖信号"。所以加了一个**开机一次的多边形定点自测**
（`-DVMAP_RENDER_PRIM_SELF=1` 打开，**默认关**：一次 ~0.5 s），确定性输入 ⇒ 两个固件的
多边形逐点相同。形状照搬真实负载：fp 路径 = 32 条"道路带"四边形 + 8 个 12 边形 ×8 遍；
整像素路径 = 8×16 + 4×64 + 2×256 点闭合多边形 ×8 遍。

| 路径 | 改动前（周期） | 改动后（周期） | 差 | 交叉点 | 周期/交叉点 |
|---|---|---|---|---|---|
| fp（四边形 + 12 边形） | 9,842,831 | 9,571,632 | **−2.8%** | 21,776 | 452.0 → 439.6 |
| 整像素（16/64/256 点） | 118,073,558 | 108,680,471 | **−8.0%** | 263,968 | 447.3 → 411.7 |
| **合计** | 127,916,389 | 118,252,103 | **−7.6%** | 285,744 | 447.7 → 413.8 |

- 折算整帧：多边形约占 `cyc_tile` 的 40~50% ⇒ **约 −3%（≈7 ms / 235 ms 帧）**。
- 两个路径差这么多是因为它们的"交叉点/跨度填充"配比不同；fp 那条的 `cover_span_fp`
  本身占比更大，所以同样的绝对收益摊薄成更小的百分比。
- **为什么只有 −7.6%**：见 §2.5（软除换成了状态回写）。

### 3.4 上板行为烟测

最终固件（`build_date 202609252316`）跑 `ctl dvfs 240` + GPX 回放 + 导航：
`[vperf] frame` / `[vperf] prim` 正常，本会话 `[ ERROR]` **0** 行、无 `HardFault` /
`assert` / coredump。`polyself` 行**不出现**（默认关，符合预期）。

### 3.5 口径与样本量提醒

- **真帧对比的结论是"测不出"，不是"没变"**：别用某一帧的 `cyc_tile` 差来引用这次收益。
- 定点自测是**同输入**对比，分辨率够；但它只覆盖多边形图元（不含 span 填充之外的
  层逻辑、特征迭代、bbox 早退）。
- 换算：`ms = cyc / 240e3`，240 MHz 要用 `cyc_tot / (tickd × 10ms)` 现场复核
  （MEASUREMENTS.md §2；`hz=` 字段是假值）。
- `cross` 是**工作量**计数（活动边 × 行），改动后它还在数，但常规路径已不再除 ——
  别把它读成"还有这么多次软除"。

---

## 4. 怎么复跑

```bash
# ① 像素不变（宿主，秒级）
#    命令见 verify/harness.c 文件头；基线用最终提交之前的那个 commit。
# ② 图元拆分（板上，常开）
python3 vendor/HelmOne/build_board.py build flash
#    ctl dvfs 240 → bicycle_nsh gnss sim <gpx> → bicycle_nsh nav <lon> <lat>
grep -a -o 'vperf] prim.*'  <console.log 的本次上电段>   # 只取最后一个启动横幅之后
# ③ 定点 A/B（板上，两版各烧一次；自测默认关，要显式打开）
python3 vendor/HelmOne/build_board.py build flash   # 加 -DVMAP_RENDER_PRIM_SELF=1
grep -a -o 'vperf] polyself.*' <console.log>           # 两个固件的数字直接对减
```

---

## 5. 这次改了哪些文件

| 文件 | 改了什么 |
|---|---|
| `ui/bicycle/src/vmap/vmap_render.c` | 增量推进的 4 个 helper（`arm`/`seed`/`step`/`live`）+ 两个 `fill_polygon*` 的行循环；图元级周期埋点（常开）；定点自测（默认关） |
| `ui/bicycle/src/vmap/vmap_render.h` | 埋点/自测的结构体与原型 |
| `ui/bicycle/src/vmap/vmap_view.c` | `[vperf] prim`（每帧）与 `[vperf] polyself`（开机一次，默认关）各一行 |
| `docs/map/perf/verify/harness.c` | 多边形专项（`t_poly_suite`）、画布尺寸可覆盖、用法注释重写 |
| `docs/map/perf/verify/stubs_host.c`、`verify/fake/*.h` | **新入库**：宿主桩与两个假头 ⇒ harness 从此一句话可复跑 |
| `docs/map/perf/MEASUREMENTS.md` | §9 原始数据（拆分表、A/B、复跑） |
| `docs/map/perf/README.md`、`TESTS.md` | 优化记录条目、验收表、缺口状态 |
