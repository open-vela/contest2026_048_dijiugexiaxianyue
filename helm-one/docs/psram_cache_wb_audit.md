# PSRAM 写回缓存（`PSRAM_CACHE_WB`）审计：这棵树要改什么

2026-09-27。配套阅读：`zcode/analysis/MPU_WB_ATTEMPT.md`（为什么走这条路、官方 2.4 的用法调查）。

## 0. 一句话结论

画布 store 慢的真因是 **PSRAM 走 write-through**（核内 D-cache 开着，但 WT 不吸收 store：实测
88 周期/字 ≈ 11 MB/s）。官方 SDK 2.4 让 PSRAM 变快的办法是**全局** `CONFIG_PSRAM_CACHE_WB=y`
（`eh-lb525` 等出货板就这么配）。**审计下来改动面比预想的小**：我们这棵树里把 PSRAM 交给别的
master 的只有 **EPIC（画布 + LVGL 图层）** 和 **USB-MTP**，而 USB 那侧官方已经带了 PSRAM 判据；
真正的缺口只有 **两条**：

1. **EPIC 合成输出侧没有 invalidate**（画布会读到旧像素）——最要紧的一条。
2. **通用 DMA 目的侧没有 invalidate**（今天目的缓冲几乎都是 SRAM，属结构性隐患 + 现存潜在陈旧行）。

## 1. 一致性模型（现状）

| 主体 | 配置 | 谁缓存 PSRAM |
|---|---|---|
| HCPU（本核） | I/D 缓存**开**（`mpu_config()` 尾部）；PSRAM SBUS `0x60000000-0x61FFFFFF` = **WT cacheable**；SRAM = Normal **non-cacheable**；外设 = Device | 只有 HCPU，且是写通 |
| LCPU（BLE 核） | 官方 `LPSYS mpu_config()` 把 MPI1/MPI2（`0x60000000-0x9FFFFFFF`）标成 `ATTR_RAM`（non-cacheable） | **不缓存** |
| 其它 master（EPIC/LCDC/SDIO/MPI/USB/UART/I2C/SPI） | 直接读写物理内存，绕过 CPU 缓存 | — |

- **HCPU↔LCPU 的共享数据全在 SRAM**（BT TX/RX ring `0x2007FE00`、RX ring `0x2040FExx`、NVDS、
  mailbox），且 `sf32lb52_bt_adapter.c` 两侧都已有 clean/invalidate（`:883/898/912/1033/1095`）。
- 其它 master 的缓冲也几乎都在 SRAM：SD bounce（`g_sd_mblkbuf`）、UART DMA bounce
  （`g_uart_dma_rx`）、LCD 条（`g_lcd_sram_buf_a/b`）、I2C/SPI 栈上小缓冲、BT ring。
  **SRAM 不在缓存范围内 ⇒ WB 不影响它们**（这是本次审计最大的"减负"发现）。
- WT → WB 的差别：WT 下 CPU 缓存里的行**总是干净的**（store 直写内存），所以 master 随时读都
  能拿到最新数据；WB 下会积累**脏行** ⇒ ① master 读之前必须 **clean**；② master 写之后 CPU
  必须 **invalidate**，否则读到旧行。

## 2. 谁碰 PSRAM（盘点）

| 路径 | master | 方向 | 缓冲位置 | 维护现状 |
|---|---|---|---|---|
| EPIC 合成输出（**画布 0x60E00000**、LVGL 图层） | EPIC | 写 PSRAM | PSRAM | ❌ **只有 clean，无 invalidate** |
| EPIC 合成输入（图层 / 图片 / 字形位图） | EPIC | 读 PSRAM | PSRAM（LVGL 堆） | ✅ clean 已有（`lv_epic_flush_layers_cache()` 输入侧） |
| LVGL draw-buf | EPIC/CPU | 双向 | PSRAM | ✅ `lv_draw_buf_sifli_epic.c:88/111` flush+invalidate |
| USB MTP（TX/RX、`g_io_buf`/`g_upload_ram`） | USB PCD | 双向 | PSRAM | ✅ `bf0_hal_pcd.c:761`(clean) `:1543`(invalidate)，`IS_DCACHED_RAM` 判据 |
| NAND 编程（LFS/GPX/MTP 落盘） | MPI DMA | 读 PSRAM | PSRAM（LFS 缓存/用户缓冲） | ✅ **WB 下官方把 DMA 整段编译掉**（`bf0_hal_mpi_ex.c:997` `#ifndef PSRAM_CACHE_WB`），退回 FIFO 轮询 ⇒ 天然一致 |
| MPI/NOR 读（页读） | MPI | 写 | MPI cache window `0x62000000`（**非 PSRAM**） | ✅ `sf32lb_nand.c:123` 已有 invalidate |
| LCD 刷屏条 | LCDC | 读 | **SRAM** 静态数组 | ✅ 已 clean（`myvendor_lcd_disp.c:200`） |
| SD 卡数据 | SDIO DMA | 双向 | **SRAM** bounce | ✅ `sf32lb_sdio.c:2062/2067/2169` |
| GNSS/串口 RX | UART DMA | 写 | **SRAM** bounce | ✅ `sifli_uart.c:1010/782-793` |
| 传感器 I2C/SPI | I2C/SPI DMA | 写 | **SRAM**（栈上小缓冲） | ✅（SRAM 非缓存） |
| BT HCI / LCPU | LCPU（另一核） | 双向 | **SRAM** ring/NVDS | ✅ 两侧都有 |
| PSRAM 上那些大缓冲（BLE FS 129 KiB+256 KiB+16 KiB、MTP 2 MiB staging、TTF 字体、tile cache、vmap arenas、GNSS `s_ubx_pl`/`s_dbd_buf`） | **无 master**（纯 CPU） | — | PSRAM | ✅ 不需要维护 |

## 3. 缺口清单（要改的）

**G1（最关键）EPIC 输出侧缺 invalidate。**
`lv_sifli_epic_cfg.c:574 lv_epic_flush_layers_cache()` 对输入**和**输出都只调
`lv_epic_flush_cache_range()`（clean），合成完成后没有对输出层 invalidate。
输出层就是 vmap 画布（`0x60E00000`，走 `myvendor_epic_copy`）与 LVGL 的图层。
现成的 `lv_epic_invalidate_cache_range()`（`:347`）就在同一个文件里，只是这条路径没调。
> 顺带：这条在 **WT 下也该修** —— master 写过的区域，CPU 缓存里的旧行同样陈旧（WT 只保证
> CPU 自己的 store 出去，不保证别人写的能被 CPU 看到）。

**G2（结构性）通用 DMA 目的侧缺 invalidate。**
`bf0_hal_dma.c` 三条启动路径（`DMA_Start` / `HAL_DMA_Start` / `HAL_DMA_Start_IT`，
`:432 / :790 / :869`）都只 `mpu_dcache_clean(SrcAddress, …)`，**没有任何 DstAddress 的
invalidate**。今天我们的 DMA 目的缓冲几乎都是 SRAM，所以不流血；但任何"以后把 PSRAM 缓冲交给
DMA 写"的改动都会踩，而且它也是"CPU 读到 master 写的东西"这类陈旧行的通用来源。

**不必改的（有证据）**：NAND 编程（G3，官方在 WB 下删掉了 DMA 路径）、MPI 读（G4，DMA 调用点
全是 write=1）、USB/MTP（G5，PCD 自带判据）、LVGL 图层输入（G6，clean 已有）、SRAM 各路径
（G7，不在缓存范围）。

## 4. 改动计划（增量：先中性、后开关）

| 增量 | 内容 | 行为影响 |
|---|---|---|
| **I1 铺线** | Kconfig `MYVENDOR_PSRAM_CACHE_WB`（default n）+ `chips/sf32lb52/CMakeLists.txt` 里 `if(CONFIG_MYVENDOR_PSRAM_CACHE_WB) target_compile_definitions(arch PRIVATE PSRAM_CACHE_WB)` | **零**（宏不定义） |
| **I2** | 补 G2：DMA 目的侧 invalidate（`IS_DCACHED_RAM(Dst)` + 方向判据） | WT 下无脏行 ⇒ 中性（修掉潜在陈旧行） |
| **I3** | 补 G1：EPIC 合成后对输出层 invalidate（走现成的 `lv_epic_invalidate_cache_range()`） | 同上 |
| **I4** | 新建 `configs/nsh-wb/defconfig`（= nsh + 该选项 =y）并烧一版，跑一致性探针 + 回归 + 性能 | **开关真正打开** |
| **I5** | 绿了才考虑设为产品默认 | — |

> 注意（老坑）：NuttX 这边的 cmake **只有 defconfig 路径变化时**才会重读它 ⇒ 就地改
> `configs/nsh/defconfig` 不生效，所以用**独立配置目录** `configs/nsh-wb`（顺带让两条配置并存、
> 回退只要重编 `nsh`）。

## 5. 验证计划

1. **一致性探针（要新增 `test coh`）**：CPU 写 PSRAM 图案 → DMA `MEMORY_TO_MEMORY` 搬到另一块
   PSRAM → 逐字节比对；反向：DMA 写 PSRAM → CPU 读比对。**这条直接测 G2/C2**，在 WT 与 WB 下
   都能跑（WT 下应当也 PASS，除非命中现存陈旧行）。
2. **画布**：滚屏/缩放/标签肉眼 + `[vperf] frame`（期望 store 成本明显下降）+ 与 WT 版做像素
   哈希对比（同一路线、同一缩放）。
3. **存储**：`test lfs`（CRC 校验）、GPX 记录→回放、SD 读写真值。
4. **外设**：BLE companion 传文件（走 BLE FS 的 256 KiB 写缓存）、GNSS 记录、主题切换/UI 交互。
5. **长测**：至少一轮 soak（看门狗/读线程/DVFS 那几族指标不恶化）。

## 6. 反悔路径

`configs/nsh/defconfig` 不带该宏 ⇒ 重编即回到 WT；I2/I3 的改动在 WT 下是空操作，不需要回滚。

---

# 2026-09-27 晚：I2/I3 已落地，WB 首版已上板（附实测数字）

## 落地（commit 见 git log）

- **I2** `bf0_hal_dma.c`：三条启动路径都补了**目的侧 clean**（新增 `DMA_GetDstBytes()` 与源侧对称）
  —— 防"设备写内存时 CPU 脏行被逐出、把设备的数据盖回去"。**"设备写完后 CPU 读到旧行"
  那一半仍然必须由驱动在传输完成后 invalidate**（HAL 没有能覆盖 polling 路径的完成回调），
  已写在 `HAL_DMA_Start` 的注释里。
- **I3** `myvendor_epic_blit.c`：新增 `epic_invalidate_rect_rows()`，在 EPIC 搬运**同步返回后**
  只作废**被写到的那些行**（整层作废会把矩形外 CPU 的脏行丢掉 = 丢数据；矩形外的像素 EPIC
  没碰）。靠的是既有的"包装返回时硬件一定已停"约定（换 `_IT` 版就要挪进回调）。
- **I4（部分）** 新增 `configs/nsh-wb/defconfig`（= nsh + `CONFIG_MYVENDOR_PSRAM_CACHE_WB=y`，
  唯一差别就是那一行）；WB 版**编译干净**（0 error），ELF 里出现 `mpu_dcache_clean` /
  `DMA_GetSrcBytes` / `DMA_GetDstBytes` 真符号（WT 版为 0）。

## 两条口径修正（原 §2/§3 的补充）

1. **`MYVENDOR_BLIT_EPIC` 默认关**（用户 09-26 定，隔离 WDT 变量）⇒ **vmap 自己的 EPIC 搬运
   今天不走**，"EPIC 写画布"这条缺口只在把那个开关打开时才成立 —— I3 是给那一刻准备的。
2. **在跑的是 LVGL 自己的 EPIC 合成**（`CONFIG_LV_USE_SIFLI_EPIC=y`）：它是**读**画布/图层
   去合成 LCD 条带（条带缓冲在 **SRAM**）⇒ 只需要 clean，而 `lv_epic_flush_layers_cache()` 的
   输入侧 clean 已经有了。**⇒ 当前配置下，G1 的"现实暴露面"主要是"EPIC 读 PSRAM"这一半，
   已被覆盖；G2 仍然只是结构性隐患（今天没有 PSRAM 目的缓冲）。**

## WB 首测（板上 `202609270224`，`configs/nsh-wb`）

MPU 实读判据：psram 区间 `rlar=61ffffe7` ⇒ AttrIndx=**3**=`ATTR_PSRAM_WB`（WT 版是 `…e9`=idx4）。

| 量 | WT | WB | Δ |
|---|---|---|---|
| `[vperf] psram` 128 KiB 逐字写 | 2,880,282 cyc | **1,935,521 cyc** | **−33%** |
| 同上 32 B 块写 | 2,883,578 | 1,848,853 | −36% |
| 同上 逐字读 | 831,618 | 965,566 | **+16%** |
| 健康 | — | 无断言/`feeding=1`；`test lfs` **PASS**（CRC 校验）；GNSS/BLE 正常 | — |

**怎么解读（这是这条路的真正结论）**：D-cache 只有 **16 KB**、画布 **457 KB** ⇒ 顺序写几乎
每行都是"填充 + 回写"，总线流量不降反升；CPU 少等，所以 store 快 33%，但**读因为要夹着回写
脏行而慢 16%**。代入已知占比（写入约占 `cyc_tile` 23%、CPU 搬运段 ≈ 164 ms/762 ms 帧）⇒
**帧级净收益大概只有 ±5%（可能被读变慢抵消成"打平"）**，绝没有"缓存吸收 store"那种量级。

### ⚠ 上面那个 ±5% 的估计**被实测否决**了（boot 帧 A/B，2026-09-27 深夜）

boot 帧是**内容确定**的负载（同一位置、`z=14 dirty=0`），两次 WT boot 只差 **0.04%**：

| boot 帧（`[vperf] frame`，z=14） | `cyc_begin` | `cyc_warm` | `cyc_tile` | `cyc_label` |
|---|---|---|---|---|
| WT `202609270221` 第 1 次 | 10,710,820 | 791,525,265 | **44,935,701** | 16,443,708 |
| WT `202609270221` 第 2 次 | 10,757,727 | 705,021,520 | **44,951,923** | 16,478,789 |
| **WB `202609270224`** | **7,590,172** | **219,175,068** | **35,993,171** | **9,672,918** |
| WB 相对 WT | −29.4% | −69% | **−19.9%** | **−41.3%** |

配置差异已用 `diff` 钉死：两个 `.config` **只差 `CONFIG_MYVENDOR_PSRAM_CACHE_WB=y` 一行** ⇒
单变量。两次 WT 的可重复性 <0.5% ⇒ **WB 的 −20%/−29%/−41% 远超噪声，是真的**。

**为什么模型估低了**：那个 128 KB 微基准是**流式**访问（4096 行 > 16 KB 缓存 ⇒ 必然抖动），
而真实渲染有**局部性**（瓦片数据被反复读、标签候选反复扫）⇒ 缓存真的命中。微基准只该用来
判断"缓存能不能吸收顺序写"（不能），不该用来外推整帧。

**⇒ 修正后的口径**：**冷/开机帧的渲染段实测快 20~41%**（这条已被两次可重复的 WT 对照证实）；
**交互（平移/缩放）稳态帧还没量** —— 那正是用户抱怨的那一类，也是决定"要不要设为产品默认"
的唯一缺口。测法：同一路线/缩放下人工平移 ~20 s，取 `[vperf] frame` 的中位数，WT/WB 各一次。

⇒ **建议：WB 保持为实验配置（`nsh-wb`），先不做产品默认**；要用帧级数字定论，需要一次
可重复的滚动负载 A/B（`ctl` 没有 UI/负载注入，得靠人工或 GPX 模拟）。Revert = 重编 `nsh` 并
`flash`（一条命令）。

**用户视觉确认（2026-09-27 深夜）**：WB 版上板后用户实测 **"地图没花"** ⇒ WB 唯一的
"静默损坏"检测项通过（画布路径在 WB 下没有可见错像素）。**⇒ WB 实验结论闭环**：
功能正确（`test lfs` PASS + 视觉正常 + 无断言），但**性能收益只有 ±5%、可能打平**，
所以**不作为产品默认**。

**板上状态（2026-09-27 终态）**：**WB 已转正为产品默认** —— `configs/nsh/defconfig` 带
`CONFIG_MYVENDOR_PSRAM_CACHE_WB=y`，板上跑 `202609270249`（AttrIndx=3、store 2.03M 周期、
无断言、`test lfs` PASS、用户确认地图无花）。实验配置 `configs/nsh-wb` 已删除（与 nsh 重复）。

**要拿 WT 基线做 A/B**：把 `configs/nsh/defconfig` 里那一行删掉，然后**强制重配**
（defconfig 内容改了 cmake 不重读）：`rm -rf cmake_out/helmone_nsh` +
`export PATH=$PWD/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH` +
`python3 vela_my_vendor_tools.py build`（≈10 min），烧完就是 WT 基线。

## 还没做的（按价值排序，留着）

### ✅ 2026-09-27 深夜：vmap 的 EPIC 搬运**重新打开**（`MYVENDOR_BLIT_EPIC=1`）并量了

用户决定重新打开（09-26 曾为隔离 WDT 变量关掉）。开关在 `vmap_blit.h`；打开后开机自检会跑
九方向与 CPU 逐位比对，实测 `ovl_bits=0x04b fill=1 nonovl=1` **通过** ✓，且 `[vblit] bench`
显示 EPIC 在 186 KB 那块上是 CPU 的 **63%**（不是早先记的 8×——bench 与真实滚动段不是一回事）。

同一 hop 负载、WB 恒定、唯一变量是 EPIC（`ctl idle stay` + `ctl dvfs 240` + `gnss hop 3000 20`）：

| 量（中位数） | EPIC 关 (n=15) | EPIC 开 (n=14) | 判读 |
|---|---|---|---|
| `cyc_begin`（CPU 搬运/滚动段） | 7,800,913 | **3,068,492** | **−61%**（两边分布都紧）|
| `cyc_tile`（瓦片光栅化） | 118,732,280 | 119,282,445 | +0.5% ⇒ **不变** |
| 同帧四段总和 | 390,418,804 | 358,512,492 | −8.2%（该负载冷瓦片占 2/3，滚动段本来就小）|
| `SLOW pump gap` / EPIC 超时 / `wdog clip` / `stop feed` | 25 / 0 / 0 / 0 | 25 / 0 / 0 / 0 | **没有恶化** ⇒ 09-25 那次"与 WDT 饥饿相关"的嫌疑这次没复现 |

**换算**：这 8.2% 是"冷瓦片为主"的极端负载；真实**平移帧**（瓦片热、无冷加载）里
`cyc_begin` 与 `cyc_tile` 各占一半（09-25 实测 164 ms vs 162 ms）⇒ EPIC 的 −61% 落在
一半上 ⇒ **平移帧估计 −30%**（待用"高速 GPX 回放"这类热瓦片负载确认；hop 负载量不到）。

**⚠ 依赖关系（改一处必须看另一处）**：EPIC 是绕过 CPU 缓存写内存的 master，而 PSRAM 现在
默认 WB ⇒ **打开 EPIC 必须同时保留 `myvendor_epic_blit.c::epic_invalidate_rect_rows()`**
（按被写到的行逐行作废）。两件事要么都开、要么都关；写进了 `vmap_blit.h` 的注释里。

### ✅ 帧级 A/B 已做（2026-09-27 深夜）——新增"全国跳"调试命令

小幅平移**不触发瓦片重载**，`[vperf] frame` 因此几乎不出帧（实测：GPX 回放 45 s 只出 1 帧）。
按用户建议加了 **`bicycle_nsh gnss hop [period_ms] [count]`**：在**固定城市序列**
（北京/上海/广州/成都/乌鲁木齐/哈尔滨/拉萨/三亚/昆明/西安，间隔上千公里）之间跳，
每次跳都让瓦片整批作废 ⇒ 每帧都是"冷瓦片 + 全屏重绘"。实现不建线程（
`bicycle_gpx_sim.c::bicycle_gnss_hop_get_fix()` 在 getter 里按时间换点，优先于 GPX 回放）。

采法：`ctl idle stay`（挡自动静止，否则屏幕一黑就没人画）→ `ctl dvfs 240`（钉频）→
`gnss hop 3000 20`，取跳期 `[vperf] frame` 的**中位数**（同一序列，WT/WB 各跑一次）。

| 中位数（hop 负载，每帧 14 瓦片） | WT (n=8) | WB (n=10) | 判读 |
|---|---|---|---|
| `cyc_begin`（CPU 搬运/滚动段） | 12,271,671 | **7,419,009** | **−40%**，两次分布都很紧（WT 10.6~13.4M / WB 7.36~9.20M）⇒ **可信** |
| `cyc_warm`（冷瓦片加载） | 359,615,467 | 259,964,337 | −28%，但离散大（各城市内容不同） |
| `cyc_tile`（瓦片光栅化） | 141,375,539 | 133,826,307 | −5%，离散大 |
| `cyc_label` | 26,114,793 | 31,243,888 | +20%，但两次分布重叠 ⇒ **不足为据** |

⇒ **修正后的最终口径**：WB **不是打平** —— 在**可重复的极端负载**（每帧冷瓦片）上，
`cyc_begin` 稳定快 40%、冷加载约 −28%，光栅化变化很小。它最擅长**CPU 顺序搬运/滚动**
那条路（正是 [[myvendor-vmap-blit-layer]] 里 CPU 兜底最贵的段）。`cyc_label +20%` 那条
**没能证实**（n 太小、分布重叠），要下结论得再加样本。

**⇒ 用户 2026-09-27 决定：WB 转正为产品默认**（见下面"板上状态"）。理由：实测 `cyc_begin`
稳定 −40%、功能验证全过（`test lfs` PASS、地图无花、无断言），而成本只是"多一层需要维护的
一致性约定"—— 那层约定已经按 §3 的缺口清单补齐并落了注释。

**旁证一条（未归因）**：WB 那一跑出现一次
`gnss_uart: ERROR rx ring overflow, dropped 667 byte(s)`（WT 那跑没有）；GNSS 缓冲在
SRAM、与 WB 无直接关系，但值得在后续 soak 里盯一眼。

1. **`test coh` 探针**：直接测"CPU 写 PSRAM → DMA 搬 → 比对"与"DMA 写 PSRAM → CPU 读 → 比对"，
   即 I2 那条缺口是否真的被补上。**今天产品里没有任何 PSRAM DMA 目的缓冲**
   （EPIC 搬运关着、LFS 在 WB 下走 FIFO、LCD 条带在 SRAM、USB 关着）⇒ 它保护的是"以后
   重新打开 EPIC 搬运或用 USB/MTP"那一刻。
2. **`test coh` 探针**：直接测"CPU 写 PSRAM → DMA 搬 → 比对"与"DMA 写 PSRAM → CPU 读 → 比对"，
   即 I2 那条缺口是否真的被补上。**今天产品里没有任何 PSRAM DMA 目的缓冲**
   （EPIC 搬运关着、LFS 在 WB 下走 FIFO、LCD 条带在 SRAM、USB 关着）⇒ 它保护的是"以后
   重新打开 EPIC 搬运或用 USB/MTP"那一刻。
3. **`PSRAM_CACHE_WB` 下的 NAND 编程吞吐**：WB 会把 MPI 的 DMA 编程路径整段编译掉、退成
   FIFO 轮询（官方如此）；`test lfs` 在 WB 下写 163 KB/s、读 1016 KB/s，**这两个数没有 WT
   对照**（要 A/B 得再切一次配置）。


