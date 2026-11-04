# 主界面数据面板：App 自定义 8 格布局（设计稿）

> 目标：主界面下半部分的数据（现在那块 228×150 信息卡）由 App 配置——选哪些
> 指标、每项占几格。设备只存一份「格子坐标 + 指标 ID」的紧凑布局，不在 MCU 上
> 做通用布局引擎，也不解析 JSON。
>
> 本文只做设计，不改代码。所有几何数字都来自当前源码（2026-11-02）：
> `helm_shell.c` 的 `helm_build_standby_card()`、`helm_palette.h`、`fonts/`。
> 配套文档：[`DASHBOARD_WIDGETS_AND_MIGRATION.md`](DASHBOARD_WIDGETS_AND_MIGRATION.md)
> ——控件目录、每种跨度怎么画、P01…P05/状态栏/横幅如何迁移到这套格子。

---

## 1. 范围与非目标

**做（v1）**

- 主界面信息卡变成 2 列 × 4 行 = **8 格**的可配置面板。
- 每格可放一个控件；控件跨度只允许 **1 格、2 格、4 格**（矩形：1×1、2×1、
  1×2、2×2），正好对应「有的占 1 格、有的 2 格、有的 4 格」。
- App 编辑、下发、设备持久化；断连/重启后保持；非法布局整份拒绝并保留旧布局。
- 布局坐标用**格**而不是像素：App 不需要知道屏幕分辨率，设备负责算像素。

**不做（v1）**

- 地图不作为可放置控件（地图仍是常驻底层，见 §9 的取舍）。
- 不做任意拖拽像素级布局、不做主题/字体自定义、不做每趟骑行不同布局。
- 不做走势图/海拔剖面控件（v2 再用现成的 `helm_spark` / 剖面画布接）。

**关键约束**

- 不新增每帧定时器；数值刷新搭现有的 500 ms 主界面刷新。
- 不在 BLE 回调里碰 LVGL；应用布局一律投递到 UI 线程。
- 控件对象数有上限（≤ 8 个），固定数组，不在 tick 里 malloc。
- 复用现有字体、配色与单元格样式（3px 身份色条 + 12px 标签 + num_28/40/56）。

---

## 2. 现状盘点（为什么是 2×4）

主界面信息卡（`helm_build_standby_card`）：

| 项 | 值 | 出处 |
|---|---|---|
| 卡片 | x=6, y=140, 228×150, 圆角 10, `pad 10` | `HELM_SB_CARD_*` |
| 内容区 | **208×130** | 228−20 × 150−20 |
| 现内容 | 上半 79 + 发丝线 1 + 间隙 6×2 + 操作行 34 = 126（余 4） | 同文件注释 |
| 地图可视区 | 0…140；跟车点 y≈132，卡片从 140 起，不挡 | 同文件注释 |
| 状态栏 | 整条剔除（`HELM_SUBBAR_H=0`），页高 296 | `helm_palette.h` |

所以**不改卡片、不改地图**，把内容区 208×130 里那 126px 做成 8 格：

```
2 列 × 4 行，gap 2：
cell_w = (208 - 1×2) / 2 = 103
cell_h = (126 - 3×2) / 4 = 30
总高 4×30 + 3×2 = 126 ≤ 130（余 4px 做上下留白）
```

这块几何是**严丝合缝**的，而且和字体对得上：

| 字体 | line_height | 单字形宽（实测反推） | 用途 |
|---|---|---|---|
| `helm_num_28` | 30 | ≈16.75（4 位数字实测 67px） | 1 格 / 2×1 / 1×2 的值 |
| `helm_num_40` | 57 | ≈22.2（`22:12` 实测 111px） | 2×2 时钟 / 大值 |
| `helm_num_56` | 61 | ≈29（`28.6` 实测 116px） | 2×2 英雄值 |
| `helm_ui_12` | 15 | — | 标签、单位、日期 |
| `helm_ui_15` | 17 | — | 2 格以上的标签 |

**一个 1 格单元格 = 103×30，正好放一行「色条 + 标签 + num_28 数值」**：
3（色条）+ 4（隙）+ 24（两个汉字标签）+ 4（隙）+ 68（4 位数字 67）= 103。
这是 v1 最重要的边界：1 格只能放**短标签 + 最多 4 位数字**；`时间`（`52:18`）、
`里程`（`123.4`）、`最大心率` 这类必须用 2 格。

---

## 3. 布局模型

### 3.1 网格

```
cols = 2, rows = 4
cell(c,r) 原点 = (16 + c*105, 152 + r*32)   // 卡片内容原点 (16,150) + 2px 留白
widget 像素 = (c*105, r*32) 起，宽 = w*105 - 2，高 = h*32 - 2
```

- 坐标原点左上，`c ∈ [0,1]`，`r ∈ [0,3]`。
- 允许矩形：`1×1`、`2×1`、`1×2`、`2×2`（= 1/2/2/4 格）。
- 不允许 L 形、不允许重叠、不允许越界。
- **格子坐标进存储、像素坐标不进存储**：屏幕/卡片尺寸变了只改设备端常量，
  App 不需要跟着发版。

### 3.2 三种控件的跨度语义

| 类型 | 1×1（103×30） | 2×1（208×30） | 1×2（103×62） | 2×2（208×62） |
|---|---|---|---|---|
| `CLOCK` | `22:12` num_28 | 时间 + 日期同一行 | 时间在上、日期在下 | num_40 时间 + 日期在右 |
| `VALUE` | 色条+短标签+num_28（≤4 位） | 色条+标签+num_28+单位（≤8 位） | 标签在上、num_28 值+单位（≤5 位） | 标签在左、num_56 值（≤4 位）英雄格 |
| `STATUS` | 只显示第一行（卫星数） | 一行（卫星 + 定位 + 上次里程横排） | 三行（现状样式） | 三行 + 大字号 |
| `ACTION` | 短文案「开始骑行」 | 全宽胶囊「长按按键 开始骑行」+ 提示环 | —（非法） | 大按钮 + 提示环 |

> v1 主界面只开放这 4 种。完整的 `DUAL` / `MULTI` / `BAR` / `SPARK` /
> `PROFILE` / `ARC` / `HERO` 目录、各跨度的画法，以及 P01…P05 的逐项迁移见
> [`DASHBOARD_WIDGETS_AND_MIGRATION.md`](DASHBOARD_WIDGETS_AND_MIGRATION.md)。

> `STATUS` 是复合控件（卫星数 / 定位状态 / 上次里程），保留三行样式；它不参与
> 指标目录。其余数据都是 `VALUE` + 指标 ID。

### 3.3 默认布局（建议，保留现在能看到的信息）

```
r0  [ CLOCK 2×1                     ]   时间 + 日期一行
r1  [ STATUS 1×2 ][ VALUE 心率 1×1  ]   STATUS 占 r1~r2 左列
r2  [             ][ VALUE 里程 1×1 ]
r3  [ ACTION 2×1                    ]   长按按键 · 开始骑行
```

= 2 + 2 + 1 + 1 + 2 = **8 格**。

两套内置预设（App 里一键切）：

- **大时钟**：CLOCK 2×2 + HR 1×1 + 里程 1×1 + ACTION 2×1 = 8 格；没有 STATUS。
- **数据向**：CLOCK 2×1 + 心率/踏频/里程/爬升各 1×1 + ACTION 2×1 = 8 格；
  没有 STATUS。

> 取舍点：现在 40px 大时钟占 4 格；要留住「大时钟」就得让出 STATUS 或两个指标。
> 这是产品选择，不是架构限制。

---

## 4. 指标目录（设备是唯一真源）

### 4.1 用 ID，不用中文字符串

现有代码用 `helm_lab_eq(lab, "心率")` 这种**字符串判等**来决定颜色/历史序列，
新增/翻译一个标签就可能静默失效。新面板一律用 **uint8 稳定 ID**：

```c
typedef struct {
  uint8_t  id;            /* 稳定，永不复用 */
  const char *label_zh;   /* 完整标签 */
  const char *label_en;
  const char *short_zh;   /* 1 格用短标签，如「最大心率」→「最大」 */
  const char *short_en;
  const char *unit;       /* "" / "km" / "bpm" / "rpm" / "W" / "m" / "%" */
  uint8_t  decimals;      /* 0 / 1 */
  uint8_t  max_chars;     /* 格式化后的最大字符数（含小数点） */
  uint8_t  min_w, min_h;  /* 最小跨度（格） */
  uint8_t  color_idx;     /* 身份色条（HELM_COLOR_*） */
  uint8_t  icon;          /* 可选 16px 图标，0 = 无 */
  uint8_t  flags;         /* bit0 仅有效时显示；bit1 显示单位；bit2 可上走势图 */
  void (*fmt)(char *buf, size_t n, const bicycle_runtime_t *rt, bool *valid);
} helm_metric_t;
```

v1 指标（沿用数据页 10 项 + 现有 runtime 能直接取的）：

`时间 / 里程 / 均速 / 极速 / 心率 / 均心率 / 最大心率 / 踏频 / 功率 / 爬升 /
当前速度 / 航向 / 导航剩余 / 海拔 / 坡度 / 上次里程`

每个指标的 `max_chars` 决定它能进几格（见 §3.1 的宽度账）。典型：

| 指标 | max_chars | 最小跨度 | 说明 |
|---|---|---|---|
| 心率 / 踏频 | 3 | 1×1 | num_28 3 位 = 50px |
| 速度 / 功率 / 爬升 / 里程 / 均速 / 极速 | 4 | 1×1 | `99.9`、`1200` 都是 4 字符 = 67px |
| 时间 | 5 | 2×1 | `52:18` = 84px，1 格 68px 放不下 |
| 均心率 | 3 | 1×1 | 3 位 = 50px |
| 导航剩余 | 7 | 2×1 | `还剩 12.3 km` |

### 4.2 字号由设备算，不由 App 指定

对每个 `VALUE` 控件，设备按「可用值盒宽 × 高」在 `num_56 → num_40 → num_28`
里选**能放下的最大字号**：

```
值盒宽 = cell_w*w - 色条 3 - 隙 - 标签宽 - 单位宽
选最大 font：line_height ≤ 盒高 且 max_chars*字宽 ≤ 盒宽
```

好处：App 不需要知道字体度量；同一个指标放进 1 格自动变小、放进 4 格自动变大；
换字体/换主题也不会溢出。App 侧用同一份目录 JSON 复算，保证所见即所得。

`max_chars` 是硬上限：编辑器里放不下就标红，不允许「先发下去再截断」。设备端
格式化函数只负责 `snprintf` + 保底 `--`，绝不逐帧测文本宽度。

### 4.3 目录版本

目录表在固件里，App 侧镜像一份（由 `tools/gen_dash_catalog.py` 从 C 表导出
JSON，随 App 打包）。布局头带 `catalog_ver`；设备比 App 新时 App 只读+提示升级，
不发送未知 ID。

---

## 5. 线格式与存储

### 5.1 二进制布局文档（≤144 字节，单包可发）

```c
#define UI_DASH_MAGIC    0x4844u   /* 'H','D' */
#define UI_DASH_VER      1u
#define UI_DASH_MAX_W    16u   /* 上限；HOME_CARD 只用 8 */

typedef struct {                   /* 16 B */
  uint16_t magic;      /* UI_DASH_MAGIC */
  uint8_t  ver;        /* 布局格式版本，当前 1 */
  uint8_t  catalog_ver;/* 指标目录版本 */
  uint8_t  surface;    /* 网格型：0=HOME_CARD,1=RIDE_DATA,2=RIDE_DASH,3=CLIMB；
                        * MAP_STRIP/RIDE_DIAL 是固定组合，走 config 不走本 blob */
  uint8_t  cols;       /* surface 的列数（HOME_CARD=2） */
  uint8_t  rows;       /* surface 的行数（HOME_CARD=4） */
  uint8_t  count;      /* 控件数 0..16 */
  uint16_t crc16;      /* 覆盖「crc=0 的头 + 全部控件」 */
  uint8_t  rsv[6];
} ui_dash_hdr_t;

typedef struct {                   /* 8 B */
  uint8_t type;        /* 0x01 CLOCK / 0x02 VALUE / 0x04 STATUS / 0x05 ACTION / … */
  uint8_t metric;      /* VALUE 用；其它为 0 */
  uint8_t col, row;    /* 左上角格坐标 */
  uint8_t w, h;        /* 跨度：1/2 格（1×1、2×1、1×2、2×2） */
  uint8_t flags;       /* bit0 显示单位；bit1 显示标签；bit2 反色 */
  uint8_t rsv;         /* v2: DUAL 的次指标 ID；MULTI 走扩展记录 */
} ui_dash_widget_t;
```

- 8 字节 × 16 + 16 = 144 B（HOME_CARD 实际 8 个 ⇒ 80 B），仍小于 MTU 247，**不需要分片**。
- `crc16` 用 CCITT（0x1021，初值 0xFFFF），C 与 Dart 各一份，测试向量对拍。
- 为什么不用 JSON：固件没有 JSON 解析器，代码体积/堆/错误分支都不划算；定长
  记录还能做整份校验与「非法整份拒绝」。

### 5.2 持久化

- 文件：`/mnt/kv/db/ui.dash`（LittleFS，掉电安全；不要放 SD，没卡时也要能开机）。
- 写：先写 `/mnt/kv/db/ui.dash.tmp` → `fsync` → `rename` 覆盖。半途断连只留下
  临时文件，旧布局不受影响。
- 读：开机时读；`magic/ver/crc` 任一不符 ⇒ 用编译期默认布局（ROM 常量），
  并打一行 `WARN`。
- 只存**当前生效**的一份；不做多套用户布局（v2 再说）。

### 5.3 校验规则（设备端，纯函数，不碰 LVGL）

按顺序，任一失败整份拒绝：

1. `magic/ver` 匹配；`cols/rows` 与该 surface 描述一致（HOME_CARD = 2×4）。
2. `count ≤ 16`，且 ≤ 该 surface 的格子数。
3. 每个 `w,h ∈ {1,2}`，且 `col+w ≤ cols`、`row+h ≤ rows`。
4. 用 16 位位图查重叠（`mask |= 1 << (row*cols+col)`，最多 16 格）。
5. `type` 合法；`VALUE` 的 `metric` 在目录里。
6. 按 `DASHBOARD_WIDGETS_AND_MIGRATION.md` §1.3 的宽度公式能放下
   （指标 `max_chars` × 字宽 ≤ 值盒宽，行高 ≤ 值盒高）；
   `CLOCK`/`ACTION`/`STATUS` 至少满足各自最小跨度。
7. `CLOCK` 至多 1 个、`ACTION` 至多 1 个；`VALUE` 的指标不重复。
8. `crc16` 正确。

错误码（也用于 BLE ACK）：

| code | 含义 |
|---|---|
| 0 | OK |
| 1 | magic/版本/行列数不对 |
| 2 | CRC 错 |
| 3 | 越界 |
| 4 | 重叠 |
| 5 | 未知指标/类型 |
| 6 | 跨度太小放不下 |
| 7 | 重复（时钟/动作/指标） |
| 8 | 忙（正在应用上一份） |

---

## 6. 下发链路（BLE + 线程）

### 6.1 新特征 `0xFF1B`（UI Layout）

现有 16-bit UUID 已用到 `0xFF1A`（Device Info），下一个是 `0xFF1B`。沿用项目
「一功能一特征」的既有决定（`docs/ble/companion_impl_plan.md` 决定 1）。

| 属性 | 方向 | 载荷 |
|---|---|---|
| Read | 设备→App | 当前布局原始 blob（≤144 B；没有则返回默认布局） |
| Write | App→设备 | `[0x01][blob…]` = SET；`[0x02]` = 恢复默认 |
| Notify | 设备→App | `[0x81][status][ver_lo][ver_hi]` 应用结果 |

- **协议版本保持 `0x0008`**：新特征是可选能力，旧 App 发现不到就不显示编辑入口；
  不制造「旧 App 连不上新固件」的硬断点。
- GATT 权限沿用现有的 `ENCRYPT_REQUIRED`（只允许已配对手机改布局）。
- 布局 ≤144 B，单包写完；`WRITE_NR` 不用，要 ACK 语义。

### 6.2 线程与原子性

```
BLE 线程：
  1. 解析头/CRC/几何（纯函数）→ 失败：直接 ACK 错误码，不落盘
  2. 写 tmp → fsync → rename
  3. myvendor_bicycle_ctl_post(OP_UI_DASH_RELOAD, NULL)   // 立即返回
  4. ACK：status=0（已持久化）
UI 线程（bicycle_ui_ctl_poll）：
  5. 读文件 → 再校验一次 → helm_dash_rebuild()（失败保留旧对象树）
  6. 需要时 notify [0x81 0 0 ver]（可选，App 也可只认 ACK）
```

- **绝不在 BLE 回调里调 LVGL**；现有 `myvendor_bicycle_ctl_post()` 的 512 B 参数
  足够放 hex，但这里不需要传内容，传一个「reload」即可。
- 应用布局的一次性成本 ≈ 8 个控件 × 4~6 个对象 ≈ 40 个对象，与现在那张卡同量级；
  只在「下发/开机/换主题」时发生，不在帧里。

### 6.3 NSH 自测命令

先于 App 落地，便于板上验证与现场取证：

```
dash show              # 打印当前布局（类型/指标/坐标/跨度）+ 校验结果
dash set <hex>         # 直接写一份 80B 布局并应用
dash default           # 恢复默认
dash metrics           # 打印指标目录（id/标签/单位/min 跨度）
dash apply-file <path> # 从文件读（调试用）
```

（命名按现有 `bicycle_nsh` 风格，实际命令名可调。）

---

## 7. 渲染实现

### 7.1 新模块

```
src/lvgl_page/helm_dash.h/.c        # 布局模型 + 渲染器 + tick
src/lvgl_page/helm_dash_metrics.c   # 指标目录（唯一真源）
src/lvgl_page/helm_dash_default.c   # 默认布局（ROM 常量）
```

对外接口（示意）：

```c
typedef struct helm_dash helm_dash_t;

helm_dash_t *helm_dash_create(lv_obj_t *parent, int x, int y, int w, int h);
void helm_dash_destroy(helm_dash_t *d);
bool helm_dash_load(helm_dash_t *d);                 /* KV → 校验 → 建；失败落默认 */
bool helm_dash_apply(helm_dash_t *d, const void *blob, size_t n); /* 运行期换布局 */
void helm_dash_tick(helm_dash_t *d, const bicycle_runtime_t *rt);
bool helm_dash_current(ui_dash_hdr_t *hdr, ui_dash_widget_t *out, size_t max);
```

`helm_dash_t` 内部固定数组：

```c
typedef struct {
  uint8_t type, metric, col, row, w, h, flags;
  lv_obj_t *box;      /* 绝对定位的格子容器（非 flex 子元素） */
  lv_obj_t *rail;     /* 3px 身份色条 */
  lv_obj_t *lab;      /* 标签 */
  lv_obj_t *val;      /* 数值 */
  lv_obj_t *aux;      /* 单位/日期/次要行 */
  lv_obj_t *chart;    /* v2 */
  char last_val[16];  /* 只在文本变化时 helm_label_set */
} helm_dash_widget_t;
```

### 7.2 定位方式：这块**必须**用绝对坐标

2026-09-26 有过现场事故：卡片第一版对子元素用 `lv_obj_set_pos()`，但父对象是
flex，元素被按创建顺序竖着堆、布局全错。这次反过来：

- 卡片本身保持普通对象（**不要给 dash 容器设 flex**）；
- 每个格子 `box` 用 `lv_obj_set_pos/set_size` 绝对定位；
- 格子内部才用 flex（ROW/COLUMN）排色条/标签/数值。

即「外层绝对、内层 flex」，两层不要混在同一个父对象上。

### 7.3 刷新

- `helm_dash_tick()` 由现有 `helm_refresh_numbers()` 的 500 ms 节拍调用；
- 每个控件只做「格式化 → 与 `last_val` 比较 → 变了才 `helm_label_set()`」；
- 时钟每分钟一次；STATUS 每秒一次（卫星数/定位变化很慢）；
- 传感器无效时写 `--` 并把值设为 MUTE（沿用 `helm_dash()` 现有做法）；
- 不新增定时器、不在 tick 里算文本宽度、不做布局重排。

### 7.4 与现有壳的接缝

- `helm_build_standby_card()` 改成：建卡片外框（背景/圆角/padding 不变）→
  在 (10,10) 建 `helm_dash_create(..., 208, 126)` → 不再单独建 `sb_clock/
  sb_stat/sb_cta`。
- `helm_apply_view()` / 入场动效：v1 把整块 dash 当一个动画组（整卡淡入 + 上移），
  不再分「时钟 / 状态列 / CTA」三组；v2 可按控件错峰。
- 提示环浮动（`sb_ring`）只在 `ACTION` 控件存在时创建；`ACTION` 被删就没有环，
  空闲检查定时器要能空转（不访问 NULL）。
- 换主题：`helm_shell_rebuild_once()` 会整层重建，dash 跟着重建并重新
  `helm_dash_load()`；布局不变但对象是新的，**必须**把 `last_val` 清空，否则
  首拍不刷新（`last_val` 是结构体成员，重建时自然清零，注意别用 static 缓存）。
- 布局下发：只在 UI 线程 `helm_dash_apply()`，删旧格子对象 → 建新格子；
  失败则什么都不删。

### 7.5 性能预算

| 项 | 量级 | 说明 |
|---|---|---|
| 重建一次 | ~40 个 LVGL 对象 | 与现在卡片相当，只在应用/主题切换 |
| 每 500 ms | 8 次文本比较 + 少数 `lv_label_set` | 现有卡片本来就每拍刷时钟/状态 |
| 每帧 | 0 次布局计算 | 格坐标在 build 时算死 |
| 内存 | ~8×(6 指针+16B) ≈ 700 B + 对象 | 固定，无堆增长 |

---

## 8. App 编辑体验

位置：**设备 Tab → 主界面布局**（设备操作在设备页；App 的「设置」页只放 App 自身
的语言/缓存/日志，见 `lib/ui/settings/settings_page.dart` 的头注释）。

编辑器：

1. 2×4 网格画布，按设备比例（208:126）等比绘制；空格显示虚线格。
2. 从底部「指标面板」拖入；已放置的控件可拖动、可改跨度（1/2/4）。
3. 放不下/越界/重叠/未知 ID 当场标红，不发包（设备仍会二次校验）。
4. 每个控件显示**示例值**（用与设备相同的格式化规则），不是真数据。
5. 「恢复默认」「预设：默认 / 大时钟 / 数据向」。
6. 「下发」→ 写 0xFF1B → 等 `[0x81]` ACK；错误码翻成中文提示（§5.3 表）。
7. 本地保留一份草稿（断连也能编辑）；重连时读设备布局，冲突时以设备为准并提示。
8. 设备目录比 App 新（`catalog_ver`）时进入只读模式，提示升级 App。

App 侧镜像：

- `lib/ble/companion_proto.dart` 加 `uiLayoutChar = Guid('FF1B')`。
- `companion_client.dart` 加 read/write/notify 与 blob 编解码、CRC16。
- `lib/ui/device/home_layout_page.dart` 新页面 + 路由。
- `assets/dash_catalog.json` 由固件工具导出，保证指标标签/最小跨度不漂。

---

## 9. 取舍与备选方案

### 9.1 为什么不做「整页 8 格、地图也是控件」

- 地图是常驻画布 + 跟车点 + 箭头 + 瓦片缓存，改尺寸/位置要重排 `vmap_view`，
  代价与风险远大于收益；它也是主界面的视觉主体（用户口径「动画吸眼」）。
- 如果将来要做：布局文档里已经有 `surface` 字段，把 surface 从
  `HOME_CARD` 扩成 `HOME_FULL`，网格区域改成 240×296（cell ≈114×70），
  渲染器不用改。这是**预留**，不是 v1 承诺。

### 9.2 为什么不把面板扩高、缩小地图

卡片从 140 起是**为了让开 y≈132 的跟车点**；再往上长就会挡箭头/跟车点，
需要同时改 `map_area` 高度与 vmap 中心，属于运行期重排，风险高。v1 先不动。

### 9.3 为什么不用 LVGL 的 grid/flex 做布局

- LVGL grid 的 `fr`/gap 语义要在 Dart 里复刻才能预览，容易两边算不一致；
- 手算整数矩形（§3.1）是 5 行代码，App 也能一模一样地算；
- 校验（重叠/越界）在位图层面更简单。

### 9.4 为什么 CTA 也变成控件

不改卡片高度的话，内容区只有 126px = 4×30+3×2，**没有给固定 CTA footer 留位置**。
把「长按按键 · 开始骑行」做成 `ACTION` 控件（2×1）是唯一不缩地图、不扩卡片的
解。默认布局保留它；用户可以移动/删除（物理键始终可用，删除只是少了提示）。

---

## 10. 兼容、迁移、回滚

- 默认布局 = 编译期常量；文件缺失/损坏/旧版本 ⇒ 直接落默认，用户看到的就是
  现在这张卡的信息集合（时间和日期变成一行、状态列三行、两个指标、CTA）。
- 不动现有 KV 键（`persist.*`）；新文件独立，删掉即恢复默认。
- 协议版本不变（0x0008）；新特征缺失时 App 隐藏入口。
- 回滚：删 `/mnt/kv/db/ui.dash` 或 `dash default`。
- 未来加控件类型/指标：只增不改 ID；`UI_DASH_VER` 不匹配时整份拒绝并落默认，
  App 读到高版本只读提示。

---

## 11. 分阶段落地

| 阶段 | 内容 | 验收 |
|---|---|---|
| P0 | `helm_dash` 模型 + 渲染器 + 默认布局（编译期） | 模拟器截图与现卡片信息集合一致；1/2/4 格各一张 |
| P1 | KV 持久化 + `dash` NSH 命令 + 八套几何自测 | 板上 `dash show/set/default` 可换布局；掉电保持 |
| P2 | BLE 0xFF1B read/write/notify + 校验错误码 | nRF Connect 写 blob 能换布局；非法包被拒且旧布局保留 |
| P3 | App 编辑器 + 预设 + 错误提示 | 手机改布局 → 设备 1 s 内生效；断连可编辑 |
| P4 | v2：走势图/剖面控件；`surface=RIDE_DATA` 复用 | 骑行数据页也由同一套布局描述 |

P0 不需要 App、不需要 BLE，可以先用模拟器把观感定下来（`lvgl_sim --scene ui`），
避免「先上板再返工」。

---

## 12. 需要拍板的四件事

1. **8 格放哪**：就放现在 228×150 卡片（格 103×30，1 格一行）——本文默认；
   还是把下半屏扩高/整页（格更大，但要动地图或换页面）？
2. **CTA**：接受做成可放置的 `ACTION` 控件（推荐），还是坚持固定 footer
   （那就要么缩地图、要么 1 格高度只剩 ~22px）？
3. **默认布局**：用 §3.3 的「默认」（时间一行 + 三行状态 + 心率 + 里程 + CTA），
   还是「大时钟」或「数据向」？
4. **v1 是否要图表**：建议不要（先数字），v2 再接 `helm_spark`/剖面；否则
   P0 周期会明显变长。
