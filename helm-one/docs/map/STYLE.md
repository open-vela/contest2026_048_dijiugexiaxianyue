# vmap 地图样式（设计口径 + 端到端落地）

面向 240×400 / RGB565 / 半透半反屏的离线矢量底图。本文是**唯一口径**：
HTML 预览、`lvgl_sim` 模拟器、板端 `vmap_style.c` 三处数值都从这里抄。

- HTML 实现：`docs/osm/web/static/vmap_style.js`（`VmapPainter`）
- 模拟器参考实现：`/home/jinsc/SDK/vela/lvgl_sim/src/scene_vmap.c` + `vmap_raster.c`
- 板端：`bicycle/src/vmap/vmap_style.c` / `vmap_render.c` / `vmap_label.c`

---

## 1. 三条原则

1. **底图安静**：用地色块只做 3~8% 的色差，颜色预算全给水系和道路。
   （z14 的 `landuse=residential` 是大面积矩形，一旦上色重就变成"色块拼图"。）
2. **道路靠"壳"分层**：每条路 = 深色壳 + 亮色芯；等级用**宽度阶梯**表达，
   不是七种颜色。放大只加宽、不换色。
3. **文字压在路上**：路名沿路方向旋转、白描边、按等级排优先级；
   同一个名字一屏只出一次。

---

## 2. 调色板

四套预设，`paper` 是默认（对应设备 `classic`），`bright ← nav`，
`terrain ← outdoor`（白天户外），`night ← night`（夜间）。设备四档一一对应
（`vmap_style_id_t`：classic / nav / outdoor / night）。

> **2026-11-01 板端压深**：用户反馈"日间配色在屏幕上有些淡"。本表的 hex 仍是
> **设计稿原值**（下面的表格不改），板端在 `vmap_style.c` 取用时对**三个日间盘**
> 统一做 `压深 10% + 色度 +8%`（道路芯色只压 6%，避免把"路比地亮"压平），
> 夜间盘不处理。要整体调深浅只改 `VMAP_DAY_DEEPEN_PCT` / `VMAP_DAY_SAT_PCT`。

| token | paper（纸面） | bright（冷白） | terrain（户外） | night（夜间） |
|---|---|---|---|---|
| bg 画布底 | `#F4F1E8` | `#F6F7F9` | `#F3F0E2` | `#14161A` |
| land 1 住宅 | `#EFEADE` | `#EEF1F4` | `#EDE8D6` | `#1A1D22` |
| land 2 商务 | `#F5ECE4` | `#F6EFF0` | `#F2E7DA` | `#1E1A1E` |
| land 3 工业 | `#EFECE4` | `#EEF1F3` | `#E9E7DA` | `#191C20` |
| land 4 学校 | `#F9F2DA` | `#FBF3DE` | `#F7EFD0` | `#232019` |
| land 5 公园 | `#D8E8CA` | `#DFEDD6` | `#D2E5BE` | `#17251C` |
| forest 林地 | `#CBE0BC` | `#D3E7C4` | `#C2DCAE` | `#16241A` |
| water 水面 | `#A9D0E8` | `#A9D4F0` | `#A6CFE4` | `#16303F` |
| water 岸线 | `#7FB6D4` | `#7BB8DD` | `#79B3CE` | `#20465A` |
| waterway 水系 | `#93C2DA` | `#8FC4E4` | `#88BBD4` | `#2A556C` |
| 路名文字 | `#514C45` | `#4A5560` | `#4E493F` | `#C3C9D2` |
| 水名（斜体） | `#3D7EA6` | `#2F7BA8` | `#33708F` | `#7FB6D8` |
| 公园/POI | `#3F7048` | `#3C7048` | `#3A6B3C` | `#8FC79A` |
| 地名 | `#2E2C29` | `#1F2933` | `#2B2A24` | `#E6EAF0` |
| 描边 halo | `#FFFFFF` 92% | `#FFFFFF` 94% | `#FFFFFF` 90% | `#0C0E12` 90% |

### 道路

| 等级 | paper 芯 / 壳 | bright | terrain | night |
|---|---|---|---|---|
| 1 motorway | `#F7C873` / `#CE8F33` | `#F8C556` / `#D09425` | `#F2C070` / `#C08E38` | `#C08A3A` / `#6E4E1E` |
| 2 primary | `#FBDC9C` / `#C6A054` | `#FBD98A` / `#C4A050` | `#F7DC9C` / `#C0A45C` | `#9C7B3C` / `#5E4A22` |
| 3 secondary | `#FEFDF9` / `#B7B0A1` | `#FFF` / `#BCC4CF` | `#FDFBF4` / `#B5AF9C` | `#4A5058` / `#262A30` |
| 4 tertiary | `#FEFDF9` / `#DFD9CB` | `#FFF` / `#C6CDD7` | `#FDFBF4` / `#DCD6C4` | `#434950` / `#24282D` |
| 5 residential | `#FEFDF9` / `#E4DFD2` | `#FFF` / `#CFD5DE` | `#FDFBF4` / `#E0DBCB` | `#3C4249` / `#21252A` |
| 6 service | `#FEFDF9` / `#E9E5DA` | `#FFF` / `#DBE0E6` | `#FDFBF4` / `#E5E1D3` | `#343A41` / `#1E2227` |
| 7 path/cycle | `#A97C4A`（虚线，无壳） | `#8FA0AE` | `#9E6E42` | `#5A6B52` |

> **芯色不是纯白**：`#FEFDF9`。纯白在 RGB565 里和背景的差只有 1~2 级，
> 大面积白路会"糊"成一片；带一点暖灰反而清楚。
>
> **支路壳要"很浅"**（WP-FIX3 ②，2026-10-29）：tertiary/residential/service 的
> 壳取 `#DFD9CB / #E4DFD2 / #E9E5DA`（paper），只比底色深 10~20 级。之前的
> `#C1BAAA / #CAC4B6 / #D5CFC2` 在 1× 下把支路糊成"米色带白线"，被否掉。

---

## 3. 线宽阶梯（设备 1×，z14 基准）

| 等级 | 芯宽 px | 壳 = 芯 + 1.9 | 从哪个 zoom 开始画 |
|---|---|---|---|
| 1 motorway | 4.2 | 6.1 | z6 |
| 2 primary | 3.6 | 5.5 | z9 |
| 3 secondary | 3.0 | 4.9 | z11 |
| 4 tertiary | 2.6 | 4.5 | z12.5 |
| 5 residential | 2.2 | 4.1 | z13.5 |
| 6 service | 1.7 | 3.6 | z14.5 |
| 7 path | 1.4（虚线 5/4） | — | **z13.5** |

- **缩放**：`width = base × 2^((z − 14) × 0.5)`，夹在 `0.62 ~ 2.4`。
  设备 `setScale()` 0.25×~8× 对应同一条曲线（z14 的 1.0 是基准）。
- **path 比 service 先出**：这是骑行地图，绿道/步道比小区内部路重要。
- **支路壳宽固定 +2px**（芯 <=3px 时，1px/边）：`road_case_px()` 对
  tertiary/residential/service 不走 `1.9×ws`（那会把 3px 芯糊成 5px 壳）。
  芯 1px 时不画壳（缩到线状时套壳会变"米色带白线"）。
- **画序**：低等级先画、高等级后画（`cls 7 → 1`）；**同一级内先画完全部壳、
  再画完全部芯**（两遍 pass，`draw_road_layer()`）。这样同级路口处后画的芯
  能盖住先画的壳，不会被同级壳切断。
- **路口补芯**（WP-FIX3 ①）：同级两条路在共享顶点处只是相切、芯不重叠，
  两遍 pass 之后壳仍会从楔形里露出来（12× 下是一条贯穿路口的 V 形缝）。
  每级芯画完后，对同 class、不同要素、相距 <=3px 的顶点对，用两条芯的边
  围成的三角形（长 4~12px，按壳宽/夹角算）补上芯色。开关：
  `vmap_render.c` 的 `VMAP_JUNCTION_FILL`。
- **瓦片裁断处壳用平头**（WP-FIX3 ④）：一条路跨瓦片被切成两段时，两段的
  圆头壳帽会在接缝处重叠；后画的壳帽比芯帽宽 1px，会盖到相邻瓦片的芯上，
  在很直的路面上留一道 1~2px 横带。判据：端点局部坐标落在 `[0, extent]`
  边界/外侧 = 被裁断 ⇒ 该端平头；真端点仍圆帽。芯帽同色，保持圆帽。

---

## 4. 标签

| 类型 | 字号 | 字重/字形 | 颜色 | 优先级 |
|---|---|---|---|---|
| 地名 place | 12.5 | 600 | `lbl.place` | 最高（+90） |
| 水名 water | 10.5 | 斜体 | `lbl.water` | +40 |
| 路名 cls1-2 | 9.5 | 常规 | `lbl.road` | 按等级 |
| 路名 cls3-7 | 9.0 | 常规 | `lbl.road` | 按等级 |
| POI/公园 | 9.5 | 常规 | `lbl.poi` | 最低 |

规则（`VmapPainter.placeLabels`）：

1. 描边：白 halo 3px（地名 4px），`lineJoin=round`。
2. **沿路排版**：取该 feature 最长的线段中点，按线段角度旋转
   （角度归一到 ±90°，避免倒字）。
3. **同名去重**：数据是按瓦片切的，一条路在相邻瓦片里各有一份；
   同一个名字**一屏只画一次**，取离屏幕中心最近的那份锚点。
4. **碰撞**：所有标签用旋转后的 AABB 做碰撞，padding 2~3px；
   贴边时把文字拉回视口内（和设备的 clamp 一致）。
5. 只有几何可见（`vmapRoadVisible`）的路才出名字 —— 否则会出现
   "有路名没路"的鬼影。

---

## 5. 板端要改的地方（按顺序）

### 5.1 `vmap_style.h` / `vmap_style.c`

现在只有一个 `road_case_r/g/b` + `road_case_extra`，**表达不了"主干道黄壳、
支路灰壳"**。要加 per-class 壳色：

```c
typedef struct {
    uint8_t r, g, b;
    uint8_t w;        /* 芯宽 */
    uint8_t cr, cg, cb; /* 壳色（新增） */
} vmap_style_road_t;
```

然后 `vmap_style.c` 四张表按本文第 2 节的数值填（`classic ← paper`，
`nav ← bright`，`outdoor ← terrain`，`night ← night`）。

### 5.2 `vmap_render.c`

- 画序：**低等级先、高等级后**；同一级内**先全部壳、再全部芯**（两遍 pass）。
  同级路口再加"路口补芯"（见 §3），瓦片裁断处的壳用平头（见 §3）。
- 线宽从 `style->road[cls].w` 取，按 `setScale()` 乘同一个缩放曲线。
- `cls 7` 走虚线：沿折线按弧长切 5/4（参考 `lvgl_sim/src/scene_vmap.c`
  的 `dashed_line()`，注意**不能按 1px 递进**，会被线宽撑成锯齿）。

### 5.3 `vmap_label.c`

- 字号/颜色/优先级按第 4 节做一张表，别写死在调用处。
- 同名去重从"距离 22px"改成"一屏一个名字 + 取离中心最近"。
- 路名沿最长线段旋转（现有 `pathMid` 已经算了角度，直接复用）。

### 5.4 用地可见性

`land 2/3`（商务/工业）在 z14 就是一堆灰块，**z15 之前不画**；
`land 1/4/5` 从 z12.5 开始。设备固定 z14，所以商务/工业默认不出现。

---

## 6. 验证链路

```
HTML 预览（docs/osm/web）        ← 调色板/线宽/排版的快速试验台
      ↓ 抄同一组数值
lvgl_sim（/home/jinsc/SDK/vela/lvgl_sim）
      ↓ 同一份 vmap_style.c / vmap_render.c
板子（SF32LB52 + EPIC）
```

- **HTML**：`./view_map.sh` → 右下角勾「设备视图 240×400」看 1× 真实尺寸；
  左侧「地图样式」切四套预设。
- **模拟器**：`lvgl_sim --scene vmap`（现在是 mock 几何，验证光栅化 + 中文标注）；
  接真实瓦片见 `lvgl_sim/README.md` 的"下一步"。
- **板子**：只看两件事 —— 阳光下能不能读、骑行时能不能一眼分出主干道。

每个环节**只看这一层能看的东西**：HTML 看配色和排版；模拟器看 1× 下的
线宽/字号/字模；板子看对比度和刷新。

---

## 7. 已知差异与坑

| 坑 | 说明 |
|---|---|
| RGB565 量化 | `#FEFDF9` 和 `#FFFFFF` 在 565 下都是 `0xFFBF/0xFFFF` 级别，差 1~2 级；调色板别用 1~2 级的差做区分 |
| 无抗锯齿（板端） | 板端 EPIC 画线有 AA，但 1px 的虚线/细线仍会"跳"；小于 1.4px 的线宽不要用 |
| 字体 | 设备是定制点阵字库，HTML/模拟器用 TTF 近似；**字号要按点阵字模的实际字面高验证**，别按 TTF 行高 |
| z14 单层数据 | 瓦片只有 z14，放大只是把 z14 几何拉粗；z16 以上会出现"路口变胖"的观感，这是数据限制不是渲染问题 |
| 数据空洞 | `osm_data/map`（5 km 旧包）在滁州主城 118.29~118.34 有一段**没有瓦片**（x=13575~13577 缺失），换区域时先用 HTML 预览确认覆盖率 |
