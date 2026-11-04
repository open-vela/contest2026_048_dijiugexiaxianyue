# LiveMap 的 GPU/搬运加速：已落地的层次、航向朝上（旋转）、跨 MCU 移植

> 状态：**本文是"下一步"的记录**。已落地的部分在
> [perf/MEASUREMENTS.md §8](./perf/MEASUREMENTS.md)；本文只写**还没做的两件事**：
> ① 航向朝上（地图整体旋转）；② 把搬运/加速抽象成可移植的 API 面。
>
> 起因：2026-09-25 给 scroll 的 468 KB `memmove` 上了 EPIC（2D DMA），
> 滚动帧 `scr` 从 122~208 ms 降到 40~46 ms（两跳）/24 ms（单跳）。用户随后追问
> "EPIC 能不能做旋转加速、箭头朝上"，以及"要不要为移植写一个通用 API 文件"。

---

## 1. 现状（已落地，供下文引用）

| 层 | 文件 | 职责 |
|---|---|---|
| 策略/兜底 | `boards/.../vmap/vmap_blit.c`（+ `.h`） | surface + 矩形 API、CPU 兜底、**开机自检**与准入位、两跳、运行计数 |
| 板级封装 | `services/myvendor_epic_blit.c`（+ `include/myvendor_epic_blit.h`） | EPIC 那半：句柄复用、层数组配置、坐标/偏移检查、RGB565↔RGB888 约定 |
| 调用点 | `vmap_render.c`（`vmap_render_scroll` / `vmap_render_clear`） | "一块矩形 + 两条边" |

关键判据（复跑见 `[vblit]` 行）：EPIC 拷贝 = CPU 的 **12%** 周期；**只有"目的地址
低于源"的方向逐位可信**（其余走两跳，中转缓冲进程级、325 KB）。

---

## 2. 航向朝上（地图旋转）：EPIC 支持，但要先回答"画质能不能接受"

### 2.1 EPIC 的变换能力（都是现成的）

- `EPIC_TransformCfgTypeDef`：`angle`（**0.1° 单位**）、`h_mirror`/`v_mirror`、
  `pivot_x/y`、`scale_x/y`（10 位小数）；配套 `HAL_EPIC_Rotate()`（同步）/
  `HAL_EPIC_Rotate_IT()`。
- **52X 上连混合都走它**：`HAL_EPIC_BlendStart()` 的 `SF32LB52X` 分支直接
  `return HAL_EPIC_Rotate(epic, 单位变换, ...)`（`bf0_hal_epic.c:3730-3735`）
  ⇒ 旋转是这条流水的主干，不是旁路功能。
- **树里已有出货级用法**：`lv_draw_sifli_epic_img.c:109-113` 把 LVGL 的
  `lv_image_set_rotation/scale` 翻成 `transform_cfg.angle/pivot/scale`
  ⇒ "图层按任意 0.1° 旋转"在这块板上验证过。
- 更猛的一条：`HAL_EPIC_TransStart(..., hor_path, ver_path, user_data)` 收
  **逐行回调** `EPIC_TransPath`，那已经不是仿射（可做弯曲/鱼眼式重映射）。

### 2.2 方案 A：显示期整体旋转（小时级、可逆，**先试这个**）

把地图画布当成 `lv_image`，喂 `lv_image_set_rotation(canvas, heading)` —— 走 2.1
那条已验证的通路。箭头是**另一个 LVGL 对象**（`view->arrow_layer`），不跟着转，
天然"地面转、箭头朝上"。

- 渲染/瓦片泵/脏区/跟车数学**一行不用动**（全在北向上的画布坐标里）。
- 代价：一次 EPIC 变换 ≈ 可见窗 240×232 = 55.7k px，按实测速率折算 **~6 ms/帧**
  （当前帧 430~500 ms ⇒ ~1.5%）。
- 画布留白够不够：可见窗旋转后外接框最大 ~334×334；画布 496×472 的半对角线
  236（对 167 的旋转窗半径）⇒ 装得下，但**有效留白变小 ⇒ rebase 更勤**。
- **唯一真风险 = 画质**：结构体里没有插值/滤波位，只有亚像素偏移
  （`x_offset_frac/y_offset_frac`）⇒ 采样是"带亚像素的最近邻"；画布里烘着的
  **道路名标签会跟着转**，12~15 px 的字斜过来会糊，1~3 px 细线有锯齿。

**第一个实验**（半小时，建议先做）：固定喂一个角度（如 30°）给
`lv_image_set_rotation`，上板拍一张截图，看三件事 ——
① 瓦片底图可接受度；② 标签文字糊到什么程度；③ 旋转后有没有露边/没画到的区域。
这三条决定要不要投 2.3。

### 2.3 方案 B：只转底图，矢量与文字按旋转后坐标重画（"Garmin 式"，天级）

底图（瓦片位图，反正要重采样）用 EPIC 转；nav 线/标签/REC 线按同一个旋转变换
**重新光栅化**，**文字保持正立**。质量最好，也是产品上常见的样子。

代价（要提前知道）：
- 画布要从"北向上大 overscan"改成"屏幕对齐底图 + 上层矢量"两层；
- 标签/导航/轨迹**所有取像素坐标的地方**都要过一遍旋转，裁剪与脏区要换坐标系；
- 与 §1 那套"改一处、宿主机逐位对拍"的验证节奏比，这是一次实打实的重构。

### 2.4 不能省的物理事实

瓦片是**烘好的位图** ⇒ 任何"航向朝上"都必须在某处重采样它。EPIC 是这块芯片上
做这件事最便宜的地方（§2.1）。所以"矢量全部重画、底图不采样"这条路不存在。

---

## 3. 跨 MCU 移植：API 面怎么写

**结论：需要，但不必新造 —— 现在这个分层已经是"可移植 API + 芯片 shim"，
只是名字还锁在 EPIC 上。** 移植友好化要做三件事：

### 3.1 把 shim 改名成厂商中立的接口（唯一必须的改动）

现在：`include/myvendor_epic_blit.h` / `services/myvendor_epic_blit.c`
建议：**`include/myvendor_blit.h` + 每颗芯片一份实现**（`myvendor_blit_epic.c` /
`myvendor_blit_dmac.c` / `myvendor_blit_cpu.c` …），头文件里的名字去掉 `epic`：

```c
bool myvendor_blit_ready(void);
bool myvendor_blit_busy(void);
bool myvendor_blit_copy(uint16_t *dst, uint32_t dst_stride, int32_t dx, int32_t dy,
                        const uint16_t *src, uint32_t src_stride, int32_t sx, int32_t sy,
                        int32_t w, int32_t h);
bool myvendor_blit_fill(uint16_t *dst, uint32_t dst_stride, int32_t x, int32_t y,
                        int32_t w, int32_t h, uint16_t color565);
void myvendor_blit_wait(void);
```

**已经具备的可移植性**（这轮白拿的）：
- `vmap_blit.c`（策略层）**不引任何芯片头**，只用 `uint16_t *` + 步长 + 矩形；
- 运行期准入（方向表/两跳/自检）**不依赖具体引擎** —— 换引擎只需**重跑一次自检**，
  它会自己把新引擎的重叠行为测出来（本轮的九组方向用例就是干这个的）；
- 自检的"参照答案"是 CPU 实现 ⇒ 不需要任何引擎的文档或先验知识。

### 3.2 移植一颗新 MCU 时要满足的契约（写进新 shim 的注释）

1. **同步语义**：`copy/fill` **返回时硬件已停**（本树是 polling 版 HAL；若新引擎只有
   中断版，shim 必须自己等到完成 —— 不能把未完成的活留给 LVGL 的下一步绘制）。
2. **不猜、不静默错**：越界/尺寸超限/对齐不满足/句柄没初始化 ⇒ **返回 false**，
   让上层退回 CPU。**绝不"尽力而为"地画错像素** —— 上层的正确性靠自检，不靠 shim。
3. **自报约束**：新引擎如果要求地址/跨度对齐、坐标上限、只支持某些格式，
   在 shim 里检查并返回 false（本树 EPIC 是"偏移非负 + 坐标 ≤ 1010"）。
4. **不要把策略塞进 shim**：方向准入、两跳（中转缓冲）、CPU 兜底属于
   `vmap_blit.c` 这一层；shim 只回答"这一条命令能不能做、做完没有"。
5. **非 2D 引擎**（只有 mem2mem 的 DMA）也要能落在同一个接口后面：由 shim 自己
   逐行发（stride ≠ 矩形宽）、自己处理方向。这正是 `VMAP_BLIT_DMAC` 那个空位的形状。

### 3.3 上层还需要"旋转/缩放"那一档吗（与 §2 的关系）

如果做 §2 的航向朝上，接口要从"矩形拷贝/填充"扩一档**仿射**：
`myvendor_blit_rotate(dst..., src..., angle_0p1deg, pivot, scale, mirror)`。
建议**单独成函数**、并且**允许返回 false**（不是每颗 MCU 的 2D 引擎都能旋转）；
策略层照样可以兜底（CPU 旋转、或直接放弃该效果）。

---

## 4. 待办（**已按 §5 的实测失败改过顺序**）

1. ☐ **先做"旋转滚动"**（§5 的教训）：航向变化时用一条 EPIC 变换把上一帧整幅转过去、
   只重画露出的楔形边；**不要**用"全量重画"当骨架。
2. ☐ 点级旋转（§2.2）留作**底图与矢量两侧都改完**之后的一步，且必须先把标签收集器、
   投影 API、跟随位移、可见瓦片范围四处一起转（第一次只接了前面几处 ⇒ 标签只能先关掉）。
3. ☐ `ctl nav heading <deg>` 手调开关：没有它，旋转只能靠骑车/回放去撞。
4. ☐ 移植友好化：shim 改名 `myvendor_blit.*`（纯改名 + 一处调用点，无行为变化）。
5. ☐ 真需要旋转时再加 `myvendor_blit_rotate()`（§3.3），并让它可失败。

---

## 5. ★ 回退记录：航向朝上的第一次尝试（2026-09-25，**失败、已全部撤掉**）

做了什么：把"世界→画布"的点级变换加了旋转（新增 `vmap_place.h`：`bx/by/u + rot/cos/
sin/cx/cy`，`rot=false` 时逐位等于原表达式 —— 这一条有宿主性质测试 20 万例背书），
瓦片 5 个层函数 + 投影 API + 跟随位移 + 可见瓦片范围都接上；瓦片背景另做了
`fill_quad_fast()`（逐行边交点 + `fill_run_u16`，接缝外扩 1 px），避免 187k px 走通用
AA 填充。航向接的是 `course_deg`，5° 死区。

**现象**：`build_date 202609252205` 烧上去后 —— **底图整个没绘制出来**。

**机制（两条，缺一个都不成立）**：

1. **"航向变了就全量重画"这个骨架是错的**：一帧全量重画要靠瓦片泵分十几拍才铺完，
   而 GNSS 每 200 ms 就可能再超死区一次 ⇒ 画布被反复清空、**永远铺不完**。
   事后补的两道闸门（等 `pump_phase == 0` + `VMAP_HEADING_MIN_INTERVAL_MS=2000`）能缓解，
   但那是"把一个错骨架按住"，不是修好。
2. **标签只能先关掉**：收集器（`vmap_label_collector_add_tile`）当时还没跟着转，
   不关就会把标签贴到错的路上 —— 所以即使 1 修好，画面也是"有底图、没标签"。

**结论**：`docs/map/perf/TESTS.md` §4 第 1 条记的就是这次暴露的缺口 —— **瓦片/矢量
那条路没有任何宿主覆盖**，只能靠上板看画面，而画面坏掉时很难从日志定位。下次先做
"旋转滚动"（一帧一条 EPIC 命令 + 补楔形边），点级旋转放到两侧都改完之后。

回退范围：`vmap_render.c/.h`、`vmap_view.c/.h`、`vmap_config.h` 全部回到 `0be501d0`
之后的那版，新增的 `vmap_place.h` 删除；产品代码里**没有留下任何旋转相关代码**。
唯一保留的是 `perf/verify/harness.c` 的修复（与旋转无关，见 TESTS.md §2.1）。
