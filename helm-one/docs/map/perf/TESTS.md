# LiveMap 渲染：测试与复核怎么做（结果在哪、缺口在哪）

> 本文是**测试侧的索引**：每一类改动该用哪种验证、命令怎么敲、基线怎么选、
> 现在哪里还没覆盖。产品侧的优化记录在 [README.md](./README.md)，实测数字在
> [MEASUREMENTS.md](./MEASUREMENTS.md)，搬运/航向的设计在 [../GPU_ACCEL.md](../GPU_ACCEL.md)。

---

## 1. 三层验证，各管一段

| 层 | 回答什么问题 | 判据 | 代价 |
|---|---|---|---|
| ① **宿主差分**（旧 vs 新，同一 TU） | "这次改动是不是**逐位**没变" | 两个二进制输出 `diff` 为空 | 秒级，但只覆盖纯函数/像素原语 |
| ② **设备自检**（`[vblit]` 开机一次） | "这块**硬件**在这个方向上可不可信" | 整块缓冲 EPIC vs CPU 逐位比对 + 基准周期 | 一次 flash，~1 s 开机开销 |
| ③ **端到端实测**（`[vperf] frame`） | "收益有多少、哪条路在跑" | `scr / cyc_begin / cyc_tile` + `hop1/hop2/cpuc` | 一次 flash + 造负载 |

**原则**：能"逐位相同"证明的改动（重构、优化）只认①；新硬件路径必须②先判定
**能不能用**，再③量收益。任何"看起来对"都不算证据。

---

## 2. 套件清单

### 2.1 `verify/harness.c`（仓库自带，像素原语）

- 覆盖：`cover_span_fp` / `fill_polygon(_fp)` / `fill_disc_aa_fp` / `blend_span` /
  `fill_run_u16` / `vmap_render_clear` / `vmap_render_scroll`，含**裁剪矩形互相重叠**
  （专抓"像素被混合两次"）与**奇数宽画布**（成对写的奇偶分支）。
- 原理：拼在 `vmap_render.c` **之后**编成同一 TU，直接调它的 static；
  `-ffunction-sections -Wl,--gc-sections` 把不需要的外部符号整段丢掉。
- ⚠ **2026-09-25 起基线有约束**（见 §3）：文件头的用法注释已写明要把
  `vmap_blit.c` 与宿主桩一起编、并挡掉两个板级头。
- 状态：**已跑通、可照抄命令复跑**。桩与两个假头已入库：
  `verify/stubs_host.c`（EPIC 桩 + `vmap_malloc` 走 malloc）、`verify/fake/`
  （`myvendor_epic_blit.h`、`vmap_alloc.h`）。文件头有完整命令（含
  `-DVMAP_RENDER_PRIM=0`：图元级周期埋点在宿主机上要关掉，DWT 不存在）。
- **2026-09-25 实测**：14 种配置（`-O0/-O1/-O2` × `AA=1/0` × 两个种子，
  外加 101×301 高画布两档）× 2 万随机用例 + 每组 2000~3000 个多边形专项
  ≈ 30 万例 **全部 IDENTICAL**。
- 另含一组**多边形专项**（`T_POLY_CASES`）：随机用例的 y 量程最多盖 8 行，
  压不到"跨几十行"的边，专项补齐了跨满画布的边 / 超容量的大 n /
  半开区间 `[yi, yj)` 的边界。

### 2.2 scroll/clear 专属差分（**在 /tmp，未入库**）

- 覆盖：`vmap_render_scroll` / `vmap_render_clear` 与老的"逐行 memmove + 逐行补边"
  的逐位等价；算例含越界（走 clear）、|dx| 到 w-1、真机 496 宽、连续多次 scroll。
- 结果（2026-09-25）：**600 例 -O1 → 1000 例 -O1 → 2000 例 × {-O0,-O2} × 2 种子
  全部 IDENTICAL**（累加器 `88e7a834`）。
- 位置：`/tmp/vmb_verify/scroll_harness.c`（+ `stubs2.inc` + 两个假头 + `fake/`）。
  **待搬进本目录**（见 §4 第 2 条）。

### 2.3 设备自检 `[vblit]`（EPIC 搬运）

- 开机在 `vmap_view_create()` 里跑一次（`vmap_blit_probe()`），分配 2×186 KB
  自检缓冲，用完即释放。
- 输出：`probe begin` / 每方向一行 `case …`（`hw= busy= dif= pat= zero= bbox= …`）/
  两个 `fill` 用例 / `bench` / `probe done`（`ovl_bits= fill= nonovl=`）。
- **判据**：`ovl_bits` 是逐方向准入位图（`bit = dirx*3+diry`，0=负 1=零 2=正）；
  `nonovl=1` 是"不相交拷贝逐位可信"的总闸；两个 fill 用例验颜色回读。
- 当前实测：`ovl_bits=0x04b fill=1 nonovl=1`，基准 `cpu=21.45M vs ep=2.65M`（12%）。
- `bench` 的 `ep=0` 表示**没跑成**（不是"很快"）。

### 2.4 两跳/准入的复合正确性（宿主，**在 /tmp，未入库**）

- 用一个"还原上板实测行为"的 EPIC 模拟器（光栅顺序逐像素读写 ⇒ 目的地址高于源时
  会盖掉未读源）先**校验模型**（九个方向的通过/失败集合必须与上板一致），再在
  "安全方向单跳、其余两跳"的准入下跑 3000 例随机矩形/位移：
  **九个方向全部与 CPU 逐位相同**。
- 位置：`/tmp/vmb_verify/blit_harness.c` + `epic_model.inc`。

### 2.5 端到端造负载（`[vperf] frame`）

- 顺序：`ctl dvfs 240` →（等 1 s）`bicycle_nsh gnss sim <gpx>` →（等 3 s）
  `bicycle_nsh nav <lon> <lat>`。
- **换算只在主机侧**：`ms = cyc / 240e3`，且 240 MHz 要用 `cyc_tot / (tickd×10ms)`
  现场复核（`hz=` 是假的 `SystemCoreClock`，见 MEASUREMENTS §2）。
- 日志跨会话追加 ⇒ 任何"最近 N 秒"统计只看**最后一个启动横幅之后**；`grep -a`。

---

## 3. ★ 基线的选择：搬运接入（`0be501d0`）是一个分水岭

`vmap_render_scroll/clear` 现在改走 `vmap_blit_*`（`vmap_render.c` 那次改了 122 行）。
于是：

- **差分基线必须选在 `0be501d0` 之后**（含 `vmap_blit_*` 调用的那版），
  两边都要挂 `vmap_blit.c` + 宿主桩；
- 选更早的基线（`0be501d0` 之前）也能跑，但它比的是**两件事**（scroll 重构 +
  搬运接入），要能接受这一点才用；
- 判据仍是**产物**：两个二进制 `diff` 为空，不是"构建成功"、不是"有 diff 就重跑"。

当前 HEAD 的 `vmap_render.c` 与"搬运接入后被测的那一版"**逐字节相同**
（`git status` 里它是干净的）⇒ 2026-09-25 那次"逐位相同"的结论**继续有效**。

---

## 4. 已知缺口（按优先级）

1. **瓦片/矢量那条路没有任何宿主覆盖**：`vmap_render_draw_tile` 及其 5 个层函数
   只能靠端到端看画面 —— 航向朝上第一次尝试（见 [../GPU_ACCEL.md](../GPU_ACCEL.md) §5）
   就是栽在这条路上"看着不对"才发现方向的。
2. **§2.2 / §2.4 两套 harness 还在 `/tmp`**，没进仓库 ⇒ 结论不可复跑。
   待办：搬成 `verify/scroll_harness.c` + `verify/stubs_host.c`（EPIC 桩 + `vmap_malloc`
   走 malloc）+ 两个假头（挡掉 `<nuttx/config.h>` 与 EPIC 头），并把已跑出的结果
   写进文件头。
3. ~~**`verify/harness.c` 的修复版没跑通**（缺上面那套桩）~~ —— **已解决**
   （桩与假头入库，见 §2.1 的状态；这次"逐行增量推进"就是用它在 14 种配置下
   证明了像素逐位不变）。
4. 自检只覆盖**矩形拷贝/填充**这一档；将来若加仿射（`_rotate`）要按同样口径补用例。

---

## 5. 2026-09-25 这次改动，各自的证据

| 改动 | 证据 | 备注 |
|---|---|---|
| scroll/clear 改"矩形+两条边"并接 `vmap_blit_*`（`vmap_render.c` 122 行） | 宿主差分 §2.2：约 9600 例 × 三种 -O × 两种子 **IDENTICAL** | 采样口径见 §3 |
| CPU 兜底的成对写 / 跨行方向规则（`vmap_blit.c`） | 同上（scroll harness 走 CPU 兜底那条路） | |
| EPIC 单跳（安全方向） | 设备自检 §2.3：4 个方向 `dif=0` | 其余 4 个方向坏 10752~12800 px |
| 两跳（其余方向经中转缓冲） | §2.4 复合对拍 3000 例九方向全同 + 上板 `hop2=1 cpuc=0` | 中转缓冲进程级 |
| 收益 | §2.5 端到端：`scr` 122~208 ms → 两跳 39.8/41.8/45.5 ms、单跳 24.1 ms | 单跳只有 1 帧样本 |
| 航向朝上（点级旋转变换 + 全量重画） | **已回退** | 失败机制见 [../GPU_ACCEL.md](../GPU_ACCEL.md) §5 |
| 多边形填充去掉"逐行 64 位软除"（`fill_polygon*` 换成逐行增量推进，`vmap_render.c` ~90 行） | 宿主差分 §2.1：14 种配置 × 约 30 万例 **IDENTICAL**；设备侧定点自测 A/B：多边形光栅化 **−7.6%**（`[vperf] polyself`，确定性输入） | 方案与全部结果：[POLYGON_DDA.md](./POLYGON_DDA.md)；原始数据：[MEASUREMENTS.md](./MEASUREMENTS.md) §9 |
