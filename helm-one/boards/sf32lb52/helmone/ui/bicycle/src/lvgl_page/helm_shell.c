/**
 * @file helm_shell.c
 * @brief 主界面：按 MCU 线稿排版（subbar / rotbar / 十字分割）。
 *
 * @note 时速 / 心率数字与走势按 18/32 km/h、120/150 bpm 三档跳色（低绿、中亮黄、高红），
 *       不做渐变。数据页拆成概览 / 心率 / 速度三屏，KEY 上滑切换。
 *       GPX 导航海拔剖面按 |坡度| 3%/8% 跳色，烘焙到 RGB565 画布。
 *       KEY 翻页骑行中先切内容，再让共享内容根做 8 px / 100 ms 轻微入场；
 *       不透明度交叉淡入、不 stagger 子控件，也不改地图 canvas 自身 pan，
 *       数字放到下一拍再刷，避免和切页抢同一帧。
 */

#include "bicycle_gpx_sim.h"
#include "helm_shell.h"
#include "bicycle_page_ids.h"
#include "lvgl_page.h"

#include "bicycle_runtime.h"
#include "bicycle_ride_gpx.h"
#include "companion_bridge.h"
#include "helm_anim.h"
#include "helm_font.h"
#include "helm_icon.h"
#include "helm_icons.h"
#include "helm_idle.h"
#include "helm_menu.h"
#include "helm_pwr.h"
#include "helm_palette.h"
#include "helm_widget.h"
#include "lv_pm_overlay.h"
#include "myvendor_board_sensor.h"
#include "myvendor_devctl.h"
#include "myvendor_sys.h"
#include "myvendor_sound.h"
#include "Vendor/Board/lv_port/lv_port_buttons.h"
#include "vmap/vmap_alloc.h"
#include "vmap/vmap_config.h"
#include "vmap/vmap_format.h"
#include "lvgl/src/draw/lv_draw_buf.h"
#include "lvgl/src/draw/lv_draw_label.h"
#include "lvgl/src/widgets/canvas/lv_canvas.h"
#include "lvgl/src/draw/lv_draw_arc.h"
#include "lvgl/src/draw/lv_draw_line.h"
#include "lvgl/src/draw/lv_draw_rect.h"
#include "lvgl/src/draw/lv_draw_triangle.h"
#include "lvgl/src/misc/lv_math.h"
#include "lvgl/src/misc/lv_text.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <syslog.h>
#include "lv_pm_gpu_anima.h"
#include "board_malloc.h"
#include <time.h>

/* 待机页信息卡刷新（定义在 helm_build_standby_card 之后）。 */
static void helm_standby_card_tick(const bicycle_runtime_t * rt);

#define HELM_ROT_MS  3000u
#define HELM_UI_MS   500u
#define HELM_AUTOPAUSE_MS  4000u
#define HELM_AUTORESUME_MS 1500u
#define HELM_AUTOPAUSE_HOLD_MS 60000u
#define HELM_HIST_N  60
#define HELM_HIST_PERIOD_MS  1000u
#define HELM_AVG_WIN_MS      30000u
#define HELM_AVG_N           64

/* 搜星扫描圈的重绘节流。
 *
 * lv_anim 的执行回调按 LVGL 全局动画周期（LV_DEF_REFR_PERIOD，约 16 ms）调用，
 * 而 hero 的 draw 回调每帧都要重建整圈折线、重画整条 track 弧、再画三个刻度
 * 文字和游标点——这些都是纯软件光栅化。搜星期间 dial_lock==1，扫描动画
 * LV_ANIM_REPEAT_INFINITE，本板又长期收不到定位（hear 只有个位数），于是这条
 * 路径变成常驻满帧负载。
 *
 * 一圈扫描要 1600 ms 单程，本不需要 60 fps。这里只节流"让 LVGL 重绘"这一步：
 * 相位仍在每个动画帧更新（代价近乎为零），重绘降到 HELM_HERO_SPIN_MS 一次。
 * 可视效果是同一个 ease_in_out 慢扫描，只是重绘帧率下降。
 */
/* 主界面切换：**动一帧就要重光栅化一整屏**（实测一次切换 176~413 ms、却只有 2~3 帧，
 * 也就是每帧 ~100~200 ms），所以"动画"在这块屏上买不到流畅 —— 反而把切换拖长了。
 * 置 0 = 直接 snap（复用现成的 helm_pane_snap，无动画）；想要过渡再置 1。 */
#ifndef HELM_VIEW_ANIM
#define HELM_VIEW_ANIM 0
#endif

/* ── 长测开关：P02（骑行表盘页）是否参与页面轮转 ───────────────────────────
 * 用途（2026-11-04）：现场已确证"卡住"是 UI 线程卡在 SD/FAT 读
 * （fat_findsfnentry→…→sf32lb_sd_bread），而 P02 底部那条海拔剖面走
 * helm_climb_profile_reload() → 读 GPX/剖面缓存，是 P02 独有的读盘入口。
 * 置 0 把 P02 从轮转表里去掉（含把演示 `demo run ride` 改派到仪表台），
 * 其余一切照旧，用来做"A/B 对照长测"：
 *   · P02 关掉后不再卡 ⇒ 定位到 P02 的剖面/刷值读盘路径；
 *   · 仍然卡 ⇒ 与 P02 无关，回到地图瓦片/GPX 主路径。
 * 定位完把这里改回 1 即恢复（默认 1）。 */
#ifndef HELM_ENABLE_PAGE_DATA
#define HELM_ENABLE_PAGE_DATA 1
#endif

/* ── 开关：海拔页（HELM_PAGE_CLIMB）是否参与页面轮转 ─────────────────────
 * 用户 2026-11-04 要求"把新的海拔界面打开" ⇒ 置 1（默认）。
 * 它与 P02 底部那条剖面**共用** `helm_climb_profile_reload()`；单独留这个开关，
 * 方便做"A/B：只关海拔页"的对照（2026-11-01 曾把它从轮转里删掉过）。 */
#ifndef HELM_ENABLE_CLIMB_PAGE
#define HELM_ENABLE_CLIMB_PAGE 1
#endif
/* 页面切换动效（实测口径 [vperf] ui page switch，耗时/帧数为板测）：
 *   0 = snap（当前默认，用户 2026-09-26 定）：无动画，只付"新页面画一次"。
 *   1 = 单屏平移：进屏滑 16px / 90 ms，出屏不动。实测 176~413 ms / 2~3 帧 ——
 *       平移让整屏失效 ⇒ 每帧重光栅化一整屏（约 100 ms/帧）。
 *   2 = pm fade（交叉淡入 220 ms）：**实测最差 458~906 ms**，每帧整屏 alpha 混合。
 *       pm 的 fade 是给"页面级"用的，搬到全屏 pane 上不成立。
 *   3 = pm 的 GPU zoom/transform：transform 图层 + EPIC 混合，但每帧仍要把内容渲进图层。
 *   4 = 左右推挤：两屏各拍一张位图后对推，每帧只剩两次贴图。**实测最好
 *       276~398 ms / 6 帧**；固定开销是切换前两次整屏快照（约 150~250 ms）。
 * 结论：这块屏上"整页动"必付整屏重绘的钱。想既好看又不卡，得先让页面有常驻位图 ——
 * vmap 已经这么做了（输出就在我们自己的 draw_buf 里，见 vmap_view.c:1032），
 * HUD 要是也享受同等待遇，切换才可能真正零成本。四个实验模式都保留在下面，改宏可复现。
 */
#ifndef HELM_PM_ZOOM_SCALE
#define HELM_PM_ZOOM_SCALE 225      /* ~88%，与 lv_pm_gpu_depth_anima 一致 */
#endif
#ifndef HELM_PM_ZOOM_MS
#define HELM_PM_ZOOM_MS 200u
#endif

#ifndef HELM_HERO_SPIN_MS
#define HELM_HERO_SPIN_MS 80u
#endif
#define HELM_SPARK_PTS 30
/** @brief 海拔剖面均匀采样点数（段数 = N−1）。 */
#define HELM_CLIMB_PROF_N 60
/** @brief 当前位置指示器（点阵倒三角）的宽与高，单位 px。尖端朝下指着剖面点。
 *         **必须与下面 `s_climb_mark_data` 的实际尺寸一致**（15×10）。 */
#define HELM_CLIMB_MARK_W 15
#define HELM_CLIMB_MARK_H 10
/** @brief 「经过海拔」剖面填充：图表最上沿的 alpha（往下线性淡到 0）。
 *         原实现是"每段三角 50 + 一块 20% 淡洗"，观感偏薄；改成整幅淡出后取 110。 */
#define HELM_CLIMB_FILL_TOP_OPA 110u
/** @brief 海拔剖面「平坦」档：|坡度|低于此百分数为绿。
 *  @note 2026-09-26 由 3 降到 **1**、陡档 8 降到 **4**：用户实测一条 20.3 km /
 *        起伏 11 m 的路线（最大 2.1%）。用他那条文件算过三组：
 *        3/8 → 绿 59、2/5 → 绿 58、**1/4 → 绿 52 / 黄 7**。
 *        1%/4% 也是码表常见的分档（1% 在这颗屏上是"能感觉到的缓坡"），
 *        真爬坡（≥4%）照样红。 */
#define HELM_CLIMB_GRADE_FLAT  1u
/** @brief 海拔剖面「陡」档：|坡度|大于等于此百分数为红；两档之间为黄。 */
#define HELM_CLIMB_GRADE_STEEP 4u
/** @brief 时速低档上限：低于此 km/h 为绿。 */
#define HELM_SPEED_ZONE_MID   18.0f
/** @brief 时速高档下限：大于等于此 km/h 为红；两档之间为亮黄。 */
#define HELM_SPEED_ZONE_HIGH  32.0f
/** @brief 心率低档上限：低于此 bpm 为绿。 */
#define HELM_HR_ZONE_MID      120u
/** @brief 心率高档下限：大于等于此 bpm 为红；两档之间为亮黄。 */
#define HELM_HR_ZONE_HIGH     150u
/**
 * @brief 主页独立心率页。
 * @details 0：翻页跳过该页（概览仍有均心率，底栏仍可轮到心率）。
 *          改回 1 即恢复 KEY 上滑的心率整页。
 */
/* 记录模式的海拔采样窗口：60 点 × 10 s = **10 分钟**。
 * 2026-11-01 由 30 分钟改成 10 分钟：用户"骑行过程感觉也不怎么动" ——
 * 30 s 一个点、半小时才滚满，剖面条看着像静止的。 */
#define HELM_CLIMB_REC_MS  (10u * 60u * 1000u)
#define HELM_CLIMB_REC_PERIOD_MS  (HELM_CLIMB_REC_MS / HELM_CLIMB_PROF_N)
/* 5 行骑行数据 + 底部三格 2 + 爬升页 2 = 9 根，留一格余量（顶满时后挂的会
 * 被 helm_spark_bind 静默丢弃，症状是"某一格永远没曲线"）。 */
#define HELM_SPARK_MAX 10
#define HELM_SPEED_HEAD_H  22
/**
 * @name 骑行页（X1 仪表台）几何
 * @brief 左栏时速锁死（速度 + 速度走势）；右栏固定数据行，**不轮转**。
 * @details 宽度是**从实测字宽反推**的，不是估数：num_56 画 "28.6" 实测 116px
 *          ⇒ 左栏 124（左右各 4px 边距）；剩下 240-6-124-6-6 = 98 给右栏，
 *          内宽 98-6-6 = 86 ≥ 色条 3 + 隙 6 + num_28 "52:18" 74 = 83。
 *          右栏数值因此只能用 num_28：num_32 的 "52:18" 是 86px，放不下。
 *          卡片高 = 页高 296 - pad 6*2 - 页头 22 - pad_row 4 = 258 ⇒ 4 行各 64，
 *          行内"标签 14 + 数值 30 = 44" + 行底 13px 走势条 = 57 ≤ 64。
 * @note 用户 2026-09-26 原话："右侧数据不需要轮转"——第 5 行轮转去掉，右边就是
 *       4 项固定（时间/里程/均速/均心率），与数据页的分工靠页集合区分。
 * @{
 */
#define HELM_RIDE_HERO_W   124
#define HELM_RIDE_ROWS     4
/** @brief 数据页条目数（2 列 x 5 行 = 10 项，用户 2026-09-26 给的清单：
 *  时间 / 里程 / 均速 / 极速 / 心率 / 均心率 / 最大心率 / 踏频 / 功率 / 爬升）。
 *  行高 = 卡内高 / 5，每行"标签 14 + 数值 30 = 44"⇒ 卡高 258 时行高 51 ✓。 */
#define HELM_ALL_N         10
/** @brief 海拔页条目数（2 列 x 3 行，用户 2026-09-26 给的图：
 *  海拔(m) / 坡度(%) / 爬升(m) / 下降(m) / 最高(m) / 最低(m)）。 */
#define HELM_CLIMB_N       6
/** @brief 最后一行 = 功率。**只有功率计绑定了才显示**（用户 2026-09-26：
 *         "只要有绑定，无论是否连上都是 5 栏"/"功率计没绑定就是 4 个数据"）。
 *         行高走 flex_grow 而不是百分比：隐藏这一行后上面几行自己长满卡片。
 * @note 右栏现在是 里程/均速/心率/功率 —— **时间**挪到左卡的小结块里了
 *       （用户 2026-09-26："给骑行时间还有一些综合数据"），同一页不重复出现。 */
#define HELM_RIDE_PWR_ROW  (HELM_RIDE_ROWS - 1)
/** @brief 左卡小结块里的"综合数据"项数（骑行时间之外的那几行）。
 *  现在是 爬升 / 坡度（用户 2026-10-05 把"极速"换成"坡度"）；
 *  宽度账：色条 3 + 隙 5 + 标签 24 + … + 值 58 + 单位 12
 *  ≤ 左卡内宽 104 ⇒ 标签最多 3 个汉字、值的单位最多 3 个 ASCII 字符。 */
#define HELM_RIDE_TRIP_N   2
/** @brief 小结块顶端相对左卡顶的偏移（量条下 12，原走势区的位置）。
 *  块高：头 18 + 时间 35 + 发丝线(57) + 2×30 = 122 ≤ 左卡 258 - 117 - 8。 */
#define HELM_RIDE_TRIP_Y   125
/**
 * @name 小结块"值 / 单位"的水平几何
 * @brief **宽度是从实测字宽反推的**：num_28 的 4 位数（"1234"）实测 67px ——
 *        爬升到 1000 m 以上就会有 4 位，盒子留 62 会截断。72 够 4 位。
 *        右边缘钉在 20+72 = 92，单位从 94 起；左卡内宽 100（12..112）⇒
 *        "m"（12px）收在 106 ✓；标签最多到 20+36 = 56，不与 92 撞。
 * @{
 */
#define HELM_RIDE_TRIP_VAL_X   20
#define HELM_RIDE_TRIP_VAL_W   72
/** @brief 综合项"单位"的左端（值右边缘 + 2），最宽 12px（"m"）。 */
#define HELM_RIDE_TRIP_UNIT_X  94
/** @} */
#define HELM_RIDE_RAIL_H   40
#define HELM_RIDE_SPARK_H  145
/** @name 行底告警阈值（"默认不上色，数据高了才半透红"）
 * @brief 用户 2026-09-26："均速 心率 功率 默认都不颜色（现在功率是有默认的矩形
 *        颜色的），只有各项数据高了才设置为半透的红色"。
 * @note 阈值是**经验值**，调这三个宏即可（km/h、bpm、W）。半透明强度见
 *       `helm_ride_alert()` 里的 LV_OPA_30（与 helm_palette.h 的用色规则一致：
 *       颜色只做形状与状态，整格淡底做告警）。
 * @{
 */
#define HELM_ALERT_SPEED_KPH  30.0f
#define HELM_ALERT_HR_BPM     150u
#define HELM_ALERT_PWR_W      250u
/** @} */

/** @brief 右栏每行走势图的高度：**0 = 占满整行（数字背后的背景层）**。
 * @note 用户 2026-09-26 定稿："均速 心率 功率，背景都是有一个 chart 的"。
 *       这三行正是 helm_hist_id 取得到历史序列的三项（均速→速度历史、心率→
 *       心率历史、功率→功率历史）；时间/里程没有独立历史，自然什么都不画。
 *       设成正数则退化成"行底一条细条"（5 行时行高 51，"标签14+数值30=44"
 *       已占满，细条只能和数字挤在一起）。 */
#define HELM_RIDE_ROW_SPARK_H  0
/** @} */
/* 海拔页剖面卡高度（用户 2026-09-26 的图）：整页 296 = pad 6*2 + 页头 22 +
 * pad_row 4 + 剖面卡 108 + pad_row 4 + 六格卡 146 ⇒ 正好 */
#define HELM_CLIMB_CARD_H  108
#define HELM_VIEW_MS  90
#define HELM_VIEW_PX  16
#define HELM_DATA_SLIDE_PX  24
#define HELM_RIDE_ENTER_MS  100
#define HELM_RIDE_ENTER_PX  8
/** 连按 KEY 翻页：上一刀未落地则丢这次，避免同一 LVGL 拍叠两页。 */
#define HELM_FLIP_GUARD_MS  ((uint32_t)HELM_RIDE_ENTER_MS)
/** 切页后下一拍再刷数字，先把新页画出来。 */
#define HELM_NUMS_DEFER_MS  16u
/** 菜单淡出 ~180ms、主页淡入 220ms：折线放到转场后再算。 */
#define HELM_POLY_DEFER_MS  240u
#define HELM_DIAL_SPAN     270
#define HELM_DIAL_MAX_KPH  60
#define HELM_DIAL_SWEEP    48
#define HELM_POLY_N        8
#define HELM_POLY_NX       120
#define HELM_POLY_NY       24

typedef enum {
    HELM_PAGE_STANDBY = 0, /**< 主页面（地图 + 信息卡）。 */
    HELM_PAGE_DATA,        /**< 骑行页：表盘（时速弧 + 心率/四格 + 底部剖面）。 */
    HELM_PAGE_DATA_ALL,    /**< 数据页：2x5 十项全量（用户 2026-09-26 定）。 */
    HELM_PAGE_MAP,         /**< 地图页。 */
    HELM_PAGE_CLIMB,       /**< 海拔页。 */
    HELM_PAGE_DASH,        /**< 骑行页：老版 X1 仪表台（O02，2026-11-01 加回轮转）。 */
} helm_page_id_t;

typedef struct {
    map_page_t * map;
    lv_obj_t * standby;
    lv_obj_t * data;
    /* 骑行页（HELM_PAGE_DASH）= 老版 X1 仪表台（O02，用户 2026-11-01：
     * "现在我需要新布局有这个"）：左卡大时速 + 时间/爬升/坡度小结，
     * 右栏四行（里程/均速/心率/功率）带走势背景。 */
    lv_obj_t * dash;
    lv_obj_t * dash_title;
    lv_obj_t * dash_speed;
    lv_obj_t * dash_unit;
    lv_obj_t * dash_rail;      /**< 左卡 0~60 km/h 量条填充。 */
    lv_obj_t * dash_trip_ico;  /**< 小结块时钟图标（Material Symbols 16px）。 */
    lv_obj_t * dash_trip_lab;  /**< 小结块头行"时间"。 */
    lv_obj_t * dash_trip_val;  /**< 骑行时间值（num_32）。 */
    lv_obj_t * dash_trip_k[HELM_RIDE_TRIP_N];    /**< 综合项标签。 */
    lv_obj_t * dash_trip_u[HELM_RIDE_TRIP_N];    /**< 综合项单位。 */
    lv_obj_t * dash_trip_rail[HELM_RIDE_TRIP_N]; /**< 综合项左缘 3px 色条。 */
    lv_obj_t * dash_trip_v[HELM_RIDE_TRIP_N];    /**< 综合项数值（右对齐）。 */
    lv_obj_t * dash_row_box[HELM_RIDE_ROWS];     /**< 右栏四行容器。 */
    lv_obj_t * dash_row_rail[HELM_RIDE_ROWS];    /**< 行首 3px 身份色条。 */
    lv_obj_t * dash_row_lab[HELM_RIDE_ROWS];     /**< 行标签。 */
    lv_obj_t * dash_row_val[HELM_RIDE_ROWS];     /**< 行数值。 */
    lv_obj_t * climb;
    lv_obj_t * turn;
    lv_obj_t * subbar;
    lv_obj_t * sub_left;
    lv_obj_t * sub_mid;
    lv_obj_t * sub_right;
    lv_obj_t * rotbar;
    lv_obj_t * rot_cell[3];
    lv_obj_t * rot_lab[3];
    lv_obj_t * rot_val[3];
    lv_obj_t * nav_ban;
    lv_obj_t * nav_ban_badge;   /**< 转向图标徽章（实心圆角，颜色跟状态走）。 */
    lv_obj_t * nav_ban_ico;     /**< 徽章里的图标字（反白）。 */
    lv_obj_t * nav_ban_m;       /**< 主字：距离（墨色大字）。 */
    lv_obj_t * nav_ban_act;     /**< 次字：动作（灰小字，可为空）。 */
    lv_obj_t * nav_ban_rd;      /**< 次字：目的地（路牌放不下，已恒为空）。 */
    lv_obj_t * nav_sum;         /**< 左下角两行：导航剩余 / 全程。 */
    lv_obj_t * nav_sum_l1;
    lv_obj_t * nav_sum_l2;
    lv_obj_t * nav_scale;       /**< 右下角比例尺（文字 + 1px 下边框当标尺）。 */
    lv_obj_t * nav_scale_lab;
    lv_obj_t * speed_big;
    lv_obj_t * speed_unit;
    lv_obj_t * standby_hint;
    /* 待机页新信息卡（2026-09-26）。几何与预览器
     * zcode/analysis/helm_preview/standby.py 的 compose="tight" 一一对应。 */
    lv_obj_t * sb_card;      /**< 信息卡本体：**挂在 LiveMap 的 root 上**（不是待机
                              *   pane 上）——用户 2026-09-26："把这个下半部分的组件
                              *   放到 livemap 界面，然后把 livemap 界面作为主界面"。
                              *   挂 root 还顺带修掉坐标偏差：原来挂在 pane（pad 6）
                              *   下，位置会被那 6px 内边距顶偏。 */
    lv_obj_t * sb_date;
    lv_obj_t * sb_time;
    /** @brief 左列整块（时钟 + 日期）：主界面入场动效第一组。 */
    lv_obj_t * sb_clock;
    /** @brief 右侧状态列整块（卫星/定位/上次距离）：入场动效第二组。 */
    lv_obj_t * sb_stat;
    /** @brief 「长按按键 · 开始骑行」操作条：入场动效第三组。 */
    lv_obj_t * sb_cta;
    lv_obj_t * sb_st[3];     /**< 状态列：卫星 / 定位 / 上次距离 */
    lv_obj_t * sb_ring;      /**< 浮动提示环（按下） */
    uint16_t sb_hm;          /**< 上次刷过的 HHMM，避免每分钟重复设同一文本 */
    lv_obj_t * hero;
    lv_obj_t * hero_mode;
    lv_obj_t * gps_val;
    lv_obj_t * last_dist;
    lv_obj_t * data_all;      /**< 数据页 pane（2x5 十项）。 */
    lv_obj_t * climb_lab[HELM_CLIMB_N];  /**< 海拔页六格（沿用数据页那套格子）。 */
    lv_obj_t * climb_val[HELM_CLIMB_N];
    lv_obj_t * climb_rail[HELM_CLIMB_N];
    lv_obj_t * all_lab[HELM_ALL_N];
    lv_obj_t * all_val[HELM_ALL_N];
    lv_obj_t * all_rail[HELM_ALL_N];
    /* 骑行页（HELM_PAGE_DATA）= 新表盘：与模拟器 scene_helm.c 的
     * ride_build_dial()/ride_refresh_dial()/ride_elev_strip() 一一对应，
     * 见 docs/agent/evidence/WP-HELM10/R1/S10/NOTES.md。 */
    lv_obj_t * dial_arc;             /**< 270° 时速弧（0..60 km/h）。 */
    lv_obj_t * dial_tick[7];         /**< 弧上 7 根刻度（78..82，整点 3px）。 */
    lv_point_precise_t dial_tick_pts[7][2];
    lv_obj_t * dial_hero;            /**< hero 容器 (40,40,160,72)。 */
    lv_obj_t * dial_speed;           /**< 56px 时速。 */
    lv_obj_t * dial_unit;            /**< km/h（12px）。 */
    lv_obj_t * dial_spd_band[3];     /**< 速度带三段 96/48/48 的浅色底。 */
    lv_obj_t * dial_spd_lab[3];      /**< 带上方刻度 0/30/60。 */
    lv_obj_t * dial_spd_fill[3];     /**< 速度带填充（0→当前值，深色分区色，高 14）。 */
    lv_obj_t * dial_spd_edge;        /**< 填充右缘 2px 墨线（"前沿"）。 */
    lv_obj_t * dial_hr_val;          /**< 心率值 22px。 */
    lv_obj_t * dial_hr_unit;         /**< bpm（12px，紧跟数值）。 */
    lv_obj_t * dial_hr_stats;        /**< 右侧「均 X · 峰 Y」。 */
    lv_obj_t * dial_hr_band[5];      /**< 5 分区浅色带。 */
    lv_obj_t * dial_hr_sep[4];       /**< 分区线 1px。 */
    lv_obj_t * dial_hr_fill[5];      /**< 心率带填充（5 分区各自 0→当前值）。 */
    lv_obj_t * dial_hr_edge;         /**< 心率填充右缘 2px 墨线。 */
    lv_obj_t * dial_met_box[4];      /**< 四格容器 60×30。 */
    lv_obj_t * dial_met_lab[4];      /**< 四格标签（12px，含单位）。 */
    lv_obj_t * dial_met_val[4];      /**< 四格数值（15px）。 */
    lv_obj_t * dial_elev_line;       /**< 剖面顶部基线 192×1。 */
    lv_obj_t * dial_elev_lab;        /**< 「还剩 X km」/「近 N 分钟」/空态 `--`。 */
    lv_obj_t * dial_grid_lab;        /**< 左端「1 km/格」：分格间距（无分格时隐藏）。 */
    lv_obj_t * dial_elev_cv;         /**< 剖面画布 190×28 @ (26,264)。 */
    /** @brief 无剖面时的占位卡（2026-11-01 用户："不然太空旷"）：
     *  爬升 / 坡度 / 均速 三格，占据剖面同一块区域。 */
    lv_obj_t * dial_ph_card;
    lv_obj_t * dial_ph_hint;         /**< 空态提示「正在记录海拔… / 无海拔数据」。 */
    lv_obj_t * climb_mark;
    lv_obj_t * climb_chart;  /**< 剖面坐标源；导航烘焙成功后隐藏。 */
    lv_obj_t * climb_canvas; /**< 导航剖面 RGB565 烘焙图；切页只 blit。 */
    lv_obj_t * climb_cursor; /**< 烘焙图上的当前位置竖线（2 px）。 */
    lv_obj_t * climb_title;
    lv_chart_series_t * climb_ser;
    uint8_t climb_n;
    uint8_t climb_now;
    bool climb_nav;
    lv_obj_t * pause_dock;
    lv_obj_t * pause_title;
    lv_obj_t * pause_time;
    lv_obj_t * pause_dist;
    lv_obj_t * pause_avg;
    lv_obj_t * pause_hr;
    lv_obj_t * save_mask;
    lv_obj_t * save_time;
    lv_obj_t * save_dist;
    lv_obj_t * save_avg;
    lv_obj_t * save_gain;
    lv_obj_t * save_max;
    lv_obj_t * save_hr;
    lv_obj_t * arrive_dock;
    const lv_font_t * font_lab;
    const lv_font_t * font_title;
    const lv_font_t * font_val;
    const lv_font_t * font_quad;
    const lv_font_t * font_rot;
    const lv_font_t * font_speed;
    const lv_font_t * font_speed_ride;
    const lv_font_t * font_mark;
    lv_timer_t * rot_timer;
    lv_timer_t * ui_timer;
    lv_timer_t * poly_timer;
    lv_timer_t * nums_timer;
    uint32_t flip_t0;
    uint32_t flip_ms;
    uint8_t home_idx; /* idle：待机 / 概览 /（心率）/ 速度 / 地图 */
    uint8_t ride_idx; /* ride：概览 /（心率）/ 速度 / 地图 / 转向或爬升 */
    uint8_t rot_idx;
    bool paused;
    bool pause_auto;
    bool attached;
    bool ui_covered;
    bool arrive_dismissed;
    bool view_anim;
    int8_t view_dir;
    helm_page_id_t view_id;
    float last_km;
    uint16_t dial_kph10;
    uint16_t dial_phase;
    uint8_t dial_lock; /* 0 无数据 / 1 搜星 / 2 已定位 */
    bool dial_spin;
    bool dial_poly_defer; /* 菜单返回时先画数字，折线延后 */
} helm_shell_t;

/* 横幅四态自检（临时）：1 = 开机后每 3 秒轮播一遍 常态导航 / 转向 / 偏航 / 到达；
 * 0 = 关闭（用户 2026-09-25 已看完四态，恢复常态）。
 * 演示模式另可经 helm_shell_demo_banner() 逐个强制状态（见 s_banner_demo_mode）。 */
#define HELM_BANNER_DEMO_ON_BOOT 0

/** @brief 演示模式页面锁：0xff = 未锁，否则 helm_page_at() 恒返回该页。 */
static helm_page_id_t s_demo_page = (helm_page_id_t)0xff;
/** @brief 横幅演示状态：0 = 关，1..7 见 helm_shell_demo_banner()。 */
static int s_banner_demo_mode = HELM_BANNER_DEMO_ON_BOOT;
/** @brief 开机自检轮播开关；终端显式设置横幅状态后置 false。 */
static bool s_banner_demo_auto = (HELM_BANNER_DEMO_ON_BOOT != 0);
/** @brief 上次自动轮播的时刻。 */
static uint32_t s_banner_demo_ms;

static helm_shell_t s_sh;
static uint32_t s_still_since;
static uint32_t s_move_since;
static uint32_t s_autopause_hold_since;
/** 搜星扫描圈上次真正触发重绘的时刻（见 HELM_HERO_SPIN_MS）。 */
static uint32_t s_hero_spin_redraw_ms;
static int16_t s_rec_ele[HELM_CLIMB_PROF_N];
/** @brief 与 s_rec_ele 平行的"采样瞬间的本程里程"（米）。
 *  @details 窗口只有 10 分钟，拿**全程**里程摊每点距离会让坡度/分格在骑久后
 *           越算越离谱（缓冲满后尤其明显）：窗口内的真实里程 =
 *           dist[n-1] − dist[0]。 */
static uint32_t s_rec_dist[HELM_CLIMB_PROF_N];
static uint8_t s_rec_ele_n;
static uint32_t s_rec_ele_slot = 0xffffffffu;
static int16_t s_climb_eles[HELM_CLIMB_PROF_N];
/**
 * @brief GPX 导航海拔剖面每段坡度档（仅换路线时重算）。
 * @details 下标 i 对应采样点 i→i+1：0 平坦（绿）、1 中等（黄）、2 陡（红）。
 *          已骑过路段在绘制时改灰，不改写本表。
 */
static uint8_t s_climb_zone[HELM_CLIMB_PROF_N];
/**
 * @brief 导航剖面画布像素缓冲（PSRAM，`vmap_malloc`）。
 * @details 宽高随图表变化；分配失败则回退 `helm_climb_draw_task` 现场绘制。
 */
static void * s_climb_cbuf;
/** @brief `s_climb_cbuf` 当前宽（像素）。 */
static int32_t s_climb_cw;
/** @brief `s_climb_cbuf` 当前高（像素）。 */
static int32_t s_climb_ch;
static uint32_t s_climb_fp_pts = 0xffffffffu;
static uint32_t s_climb_fp_total_cm = 0xffffffffu;
static uint8_t s_climb_fp_rec_n = 0xff;
static uint32_t s_climb_fp_rec_slot = 0xffffffffu;
/** @brief 换主题后"只重烘焙既有剖面数据"的待办（见 helm_shell_theme_mark 的说明）。 */
static bool s_climb_rebake;

typedef enum {
    HELM_HIST_NONE = 0,
    HELM_HIST_SPEED,
    HELM_HIST_HR,
    HELM_HIST_CAD,
    HELM_HIST_PWR,
    HELM_HIST_ALT,
    HELM_HIST_GRADE,
    HELM_HIST_GAIN,
    HELM_HIST_LOSS,
    HELM_HIST_TIME,
    HELM_HIST_DIST,
    HELM_HIST_COUNT
} helm_hist_id_t;

typedef struct {
    lv_obj_t * obj;
    lv_obj_t * lab;
    lv_chart_series_t * ser;
    helm_hist_id_t id;
    uint32_t tone; /**< 上次描边色；时速/心率换档时据此标脏，避免整图重采样。 */
} helm_spark_t;

static float (*s_hist)[HELM_HIST_N];
static uint8_t s_hist_n;
static uint32_t s_hist_min = 0xffffffffu;
static double s_avg_dist[HELM_AVG_N];
static uint32_t s_avg_tick[HELM_AVG_N];
static uint8_t s_avg_n;
static uint8_t s_avg_i;
static helm_spark_t s_spark[HELM_SPARK_MAX];
static uint8_t s_spark_n;

static void helm_apply_view(void);
static void helm_refresh_numbers(void);
static void helm_spark_sync(bool push);
static void helm_nums_defer_begin(void);
static void helm_nums_defer_cancel(void);
static void helm_prompt_save(void);
static bool helm_save_open(void);
static void helm_save_hide(void);
/** @brief 路线指纹变化时重载海拔剖面、坡度档，导航成功则烘焙画布。 */
static void helm_climb_profile_reload(void);
static bool helm_nav_on(void);
static bool helm_riding(void);
static void helm_hero_spin_set(bool on);
/* 表盘页刷值：定义在 helm_build_data 那一带，helm_refresh_numbers 先用。 */
static void helm_dial_tick(const bicycle_runtime_t * rt);
/* X1 仪表台刷值：定义在 helm_build_dash 那一带，helm_refresh_numbers 先用。 */
static void helm_dash_tick(const bicycle_runtime_t * rt);
/* 数据页十项取数：定义在 helm_build_data_all 那一带，refresh 也先用。 */
static void helm_fill_all(const bicycle_runtime_t * rt, const char ** lab,
                          char val[][16]);

static void helm_show(lv_obj_t * obj, bool on)
{
    if (obj == NULL) {
        return;
    }

    if (on) {
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(obj);
        }
    } else if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool helm_obj_shown(const lv_obj_t * obj)
{
    return obj != NULL && !lv_obj_has_flag((lv_obj_t *)obj, LV_OBJ_FLAG_HIDDEN);
}

/* 提示环浮动（实现见 helm_build_standby_card 上方）：切页/按键路径提前引用。 */
static void helm_sb_bob_kick(void);
static void helm_sb_bob_stop(void);

static void helm_raise_if_shown(lv_obj_t * obj)
{
    lv_obj_t * parent;

    if (!helm_obj_shown(obj)) {
        return;
    }

    parent = lv_obj_get_parent(obj);
    if (parent == NULL) {
        return;
    }

    if (lv_obj_get_index(obj)
        != (int32_t)lv_obj_get_child_count(parent) - 1) {
        lv_obj_move_foreground(obj);
    }
}

/** @brief 导航横幅退场动画进行中（完成回调据此决定是否落 HIDDEN）。 */
static bool s_nav_ban_hiding;

static void helm_nav_ban_y_exec(void * var, int32_t v)
{
    if (helm_anim_is_suspended()) {
        return;
    }

    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)v, 0);
}

/** @brief 入场/退场收尾：退场完成才真正藏起来，入场完成只把偏移归零。 */
static void helm_nav_ban_y_done(lv_anim_t * a)
{
    lv_obj_t * obj = (a != NULL) ? (lv_obj_t *)a->var : NULL;

    if (obj == NULL) {
        return;
    }

    if (s_nav_ban_hiding) {
        s_nav_ban_hiding = false;
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_translate_y(obj, 0, 0);
}

/**
 * @brief 导航横幅显隐（幂等）。
 * @param animate 只在导航开始/结束时为 true；切页一律 false（snap，不播退场）。
 * @details `helm_apply_view()` 每次切页都会调用：已显示的横幅必须原样返回，
 *          不能每拍重播入场。退场之后仍复用同一棵对象树，只改 HIDDEN。
 */
static void helm_nav_ban_show(bool on, bool animate)
{
    if (s_sh.nav_ban == NULL) {
        return;
    }

    if (on) {
        if (!lv_obj_has_flag(s_sh.nav_ban, LV_OBJ_FLAG_HIDDEN)) {
            /* 已显示：退场中要取消并复位；**入场中不能打断** —— 周期块每
             * 500 ms 会再进来一次，这里若一律归零，160 ms 的入场会被截短。 */
            if (s_nav_ban_hiding || !animate) {
                s_nav_ban_hiding = false;
                helm_anim_stop(s_sh.nav_ban, helm_nav_ban_y_exec);
                lv_obj_set_style_translate_y(s_sh.nav_ban, 0, 0);
            }
            return;
        }

        s_nav_ban_hiding = false;
        helm_anim_stop(s_sh.nav_ban, helm_nav_ban_y_exec);
        lv_obj_clear_flag(s_sh.nav_ban, LV_OBJ_FLAG_HIDDEN);
        /* 与旧 `helm_show()` 同语义：只有"从藏到显"这一下才抬层级。 */
        lv_obj_move_foreground(s_sh.nav_ban);
        if (animate && !helm_anim_is_suspended()) {
            lv_obj_set_style_translate_y(s_sh.nav_ban, -8, 0);
            helm_anim_run(s_sh.nav_ban, helm_nav_ban_y_exec, -8, 0, 160,
                          lv_anim_path_ease_out, helm_nav_ban_y_done);
        } else {
            lv_obj_set_style_translate_y(s_sh.nav_ban, 0, 0);
        }
        return;
    }

    if (lv_obj_has_flag(s_sh.nav_ban, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    if (animate && !helm_anim_is_suspended()) {
        int32_t cur;

        if (s_nav_ban_hiding) {
            return;  /* 已经在下滑：不要从 0 再启动一次，否则屏上会跳一下 */
        }
        s_nav_ban_hiding = true;
        /* 从当前偏移接着滑（入场还没走完就结束导航时不要先跳回 0）。 */
        cur = lv_obj_get_style_translate_y(s_sh.nav_ban, 0);
        helm_anim_run(s_sh.nav_ban, helm_nav_ban_y_exec, cur, -8, 140,
                      lv_anim_path_ease_in, helm_nav_ban_y_done);
    } else {
        /* 切页/挂起：立即收起，不留半截动画。 */
        helm_anim_stop(s_sh.nav_ban, helm_nav_ban_y_exec);
        s_nav_ban_hiding = false;
        lv_obj_add_flag(s_sh.nav_ban, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_translate_y(s_sh.nav_ban, 0, 0);
    }
}

static lv_obj_t * helm_pane_of(helm_page_id_t id)
{
    switch (id) {
    case HELM_PAGE_STANDBY:
        return s_sh.standby;
    case HELM_PAGE_DATA:
        return s_sh.data;
    case HELM_PAGE_DASH:
        return s_sh.dash;
    case HELM_PAGE_DATA_ALL:
        return s_sh.data_all;
    case HELM_PAGE_CLIMB:
        return s_sh.climb;
    default:
        return NULL;
    }
}

static bool helm_page_is_data(helm_page_id_t id)
{
    return id == HELM_PAGE_DATA || id == HELM_PAGE_DASH ||
           id == HELM_PAGE_DATA_ALL;
}

static void helm_pane_hide(lv_obj_t * obj);
static void helm_sw_report(void);
#if HELM_VIEW_ANIM == 4
static bool helm_pane_push(lv_obj_t * out, lv_obj_t * in, int dir);
#endif
#if HELM_VIEW_ANIM == 2
static bool helm_pane_pm_fade(lv_obj_t * out, lv_obj_t * in);
#endif
#if HELM_VIEW_ANIM == 3
static bool helm_pane_pm_zoom(lv_obj_t * out, lv_obj_t * in);
#endif
static void helm_pane_y(void * var, int32_t v);
static void helm_pane_reset(lv_obj_t * obj);
static lv_obj_t * helm_pane_anim_able(lv_obj_t * obj);

/* 打桩：主界面切换的时长与动画帧数（[vperf] ui page switch）。
 * 背景：现在两边一起动 —— 出屏 -16px、进屏 +16px、90 ms；而"位移"会让整屏失效，
 * 于是每一帧要把**两整屏**重新光栅化（各 240x320），90 ms 的动画实际要烧十几帧的工作量。
 * 先量出来再决定：只动进屏、或干脆 snap（helm_pane_snap 已存在）。 */
static uint32_t s_sw_t0;
static uint32_t s_sw_frames;
static lv_obj_t * s_sw_out;
static void helm_pane_y(void * var, int32_t v)
{
    s_sw_frames++;
    lv_obj_set_style_translate_y((lv_obj_t *)var, v, 0);
}

static void helm_flip_arm(uint32_t ms)
{
    s_sh.flip_t0 = lv_tick_get();
    if (s_sh.flip_t0 == 0u) {
        s_sh.flip_t0 = 1u;
    }
    s_sh.flip_ms = ms;
}

static bool helm_flip_busy(void)
{
    return s_sh.flip_ms != 0u && lv_tick_elaps(s_sh.flip_t0) < s_sh.flip_ms;
}

static void helm_pane_hide_ready(lv_anim_t * a)
{
    lv_obj_t * obj = (lv_obj_t *)a->var;

    if (obj == NULL) {
        return;
    }

    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_y(obj, 0, 0);
}

static void helm_pane_in_ready(lv_anim_t * a)
{
    LV_UNUSED(a);
    if (s_sh.view_id != HELM_PAGE_MAP && s_sh.map) {
        map_page_set_map_ui_visible(s_sh.map, false);
    }

    /* 单屏动画：进屏停稳之后再收出屏（它整段没动过 ⇒ 一帧都没重画）。 */
    if (s_sw_out != NULL) {
        helm_pane_hide(s_sw_out);
        s_sw_out = NULL;
    }

    helm_sw_report();
}

/** @brief 切换打桩：只在慢的时候打一行（含动画帧数）。 */
static void helm_sw_report(void)
{
    if (s_sw_t0 != 0u) {
        uint32_t dt = lv_tick_elaps(s_sw_t0);

        if (dt >= 60u) {
            syslog(LOG_WARNING, "[vperf] ui page switch %ums frames=%u mode=%d\n",
                   (unsigned)dt, (unsigned)s_sw_frames, (int)HELM_VIEW_ANIM);
        }
    }
}


static void helm_pane_reset(lv_obj_t * obj)
{
    if (obj == NULL) {
        return;
    }

    lv_anim_delete(obj, NULL);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_y(obj, 0, 0);
    helm_obj_stagger_clear(obj);
}

static void helm_pane_hide(lv_obj_t * obj)
{
    if (obj == NULL) {
        return;
    }

    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_y(obj, 0, 0);
}

/**
 * @brief 主页面（STANDBY）没有"自己的一页"：它的可见内容 = 地图 + root 上的信息卡。
 *
 * @details 那个 pane 是**旧版整页的不透明底**（内容早被 `helm_build_standby_card()`
 *          取代，只剩一块 SCR 色满屏底）。翻页动画会把"进入的那一页" unhide 并提到
 *          最前 ⇒ 从数据页翻回主页面时它就盖在地图上，现象就是"**切出去再回来
 *          livemap 失效**"。所以它**永远不参与进出场**：离开时不用动画一个看不见的
 *          东西，进入时干脆不 unhide —— 主页面靠下面的地图和卡片直接露出来。
 * @return 参与动画的对象（待机 pane 一律换成 NULL）。
 */
static lv_obj_t * helm_pane_anim_able(lv_obj_t * obj)
{
    return (obj == s_sh.standby) ? NULL : obj;
}

static void helm_pane_snap(lv_obj_t * out, lv_obj_t * in)
{
    out = helm_pane_anim_able(out);
    in = helm_pane_anim_able(in);
    helm_pane_reset(out);
    helm_pane_reset(in);
    if (out && out != in) {
        helm_pane_hide(out);
    }
    if (in) {
        lv_obj_clear_flag(in, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(in);
        lv_obj_set_style_opa(in, LV_OPA_COVER, 0);
        lv_obj_set_style_translate_y(in, 0, 0);
    }
}

#if HELM_VIEW_ANIM == 4
#ifndef HELM_PUSH_MS
#define HELM_PUSH_MS 220u
#endif

static lv_draw_buf_t * s_push_buf[2];
static void * s_push_psram[2];
static lv_obj_t * s_push_layer[2];
static lv_obj_t * s_push_out;
static lv_obj_t * s_push_in;

/** @brief 取一块整屏位图（默认分配器优先，失败落到 PSRAM 池）。 */
static lv_draw_buf_t * helm_push_buf(int i)
{
    uint32_t stride;
    void * px;
    lv_draw_buf_t * b;

    if (s_push_buf[i] != NULL) {
        return s_push_buf[i];
    }

    b = lv_draw_buf_create(240, 320, LV_COLOR_FORMAT_RGB565, 0);
    if (b != NULL) {
        s_push_buf[i] = b;
        return b;
    }

    stride = lv_draw_buf_width_to_stride(240, LV_COLOR_FORMAT_RGB565);
    px = board_malloc_psram((size_t)stride * 320u);
    if (px == NULL) {
        return NULL;
    }

    b = lv_malloc(sizeof(lv_draw_buf_t));
    if (b == NULL) {
        board_free_psram(px);
        return NULL;
    }

    lv_draw_buf_init(b, 240, 320, LV_COLOR_FORMAT_RGB565, stride, px,
                     stride * 320u);
    s_push_buf[i] = b;
    s_push_psram[i] = px;
    syslog(LOG_WARNING, "[vperf] ui push buf%d: PSRAM 兜底 %p\n", i, px);
    return b;
}

static lv_obj_t * helm_push_layer(int i)
{
    lv_draw_buf_t * b = helm_push_buf(i);

    if (s_push_layer[i] != NULL || b == NULL) {
        return s_push_layer[i];
    }

    s_push_layer[i] = lv_canvas_create(lv_layer_top());
    if (s_push_layer[i] == NULL) {
        return NULL;
    }

    lv_canvas_set_draw_buf(s_push_layer[i], b);
    lv_obj_set_style_pad_all(s_push_layer[i], 0, 0);
    lv_obj_set_style_border_width(s_push_layer[i], 0, 0);
    lv_obj_set_style_bg_opa(s_push_layer[i], LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_push_layer[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_push_layer[i], LV_OBJ_FLAG_HIDDEN);
    return s_push_layer[i];
}

static void helm_push_ready(lv_anim_t * a)
{
    LV_UNUSED(a);

    for (int i = 0; i < 2; i++) {
        if (s_push_layer[i] != NULL) {
            lv_obj_add_flag(s_push_layer[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_translate_x(s_push_layer[i], 0, 0);
        }
    }

    s_push_out = NULL;
    s_push_in = NULL;

    if (s_sh.view_id != HELM_PAGE_MAP && s_sh.map) {
        map_page_set_map_ui_visible(s_sh.map, false);
    }

    helm_sw_report();
}

static void helm_push_x(void * var, int32_t v)
{
    s_sw_frames++;
    lv_obj_set_style_translate_x((lv_obj_t *)var, v, 0);
}

/** @brief 左右推挤：两屏拍成位图后对推（每帧只有两次贴图）。 */
static bool helm_pane_push(lv_obj_t * out, lv_obj_t * in, int dir)
{
    lv_coord_t w = 240;
    lv_coord_t from = (dir >= 0) ? w : -w;
    lv_anim_t ax;

    if (helm_push_layer(0) == NULL || helm_push_layer(1) == NULL) {
        return false;
    }

    out = helm_pane_anim_able(out);
    in = helm_pane_anim_able(in);
    if (in == NULL || out == NULL || out == in) {
        return false;
    }

    lv_obj_update_layout(out);
    lv_obj_update_layout(in);
    if (lv_snapshot_take_to_draw_buf(out, LV_COLOR_FORMAT_RGB565, s_push_buf[0]) !=
            LV_RESULT_OK ||
        lv_snapshot_take_to_draw_buf(in, LV_COLOR_FORMAT_RGB565, s_push_buf[1]) !=
            LV_RESULT_OK) {
        return false;
    }

    s_push_out = out;
    s_push_in = in;

    /* 真进屏先就位（两位图盖着它），出屏退场。 */
    helm_pane_reset(in);
    lv_obj_clear_flag(in, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(in);
    helm_pane_reset(out);
    helm_pane_hide(out);

    /* 位图 A = 旧页（从 0 推到 -from），位图 B = 新页（从 +from 推到 0）。 */
    lv_obj_set_pos(s_push_layer[0], 0, 0);
    lv_obj_set_style_translate_x(s_push_layer[0], 0, 0);
    lv_obj_set_style_opa(s_push_layer[0], LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_push_layer[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_push_layer[0]);

    lv_obj_set_pos(s_push_layer[1], 0, 0);
    lv_obj_set_style_translate_x(s_push_layer[1], from, 0);
    lv_obj_set_style_opa(s_push_layer[1], LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_push_layer[1], LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_push_layer[1]);

    s_sw_t0 = lv_tick_get();
    if (s_sw_t0 == 0u) {
        s_sw_t0 = 1u;
    }

    s_sw_frames = 0;

    lv_anim_init(&ax);
    lv_anim_set_var(&ax, s_push_layer[0]);
    lv_anim_set_values(&ax, 0, -from);
    lv_anim_set_time(&ax, HELM_PUSH_MS);
    lv_anim_set_path_cb(&ax, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&ax, helm_push_x);
    lv_anim_set_ready_cb(&ax, helm_push_ready);
    lv_anim_start(&ax);

    lv_anim_init(&ax);
    lv_anim_set_var(&ax, s_push_layer[1]);
    lv_anim_set_values(&ax, from, 0);
    lv_anim_set_time(&ax, HELM_PUSH_MS);
    lv_anim_set_path_cb(&ax, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&ax, helm_push_x);
    lv_anim_start(&ax);
    return true;
}
#endif /* HELM_VIEW_ANIM == 4 */

/** 待机翻页：只位移，不改 opa、不 stagger。 */
static void helm_pane_slide(lv_obj_t * out, lv_obj_t * in, int dir)
{
    lv_anim_t ay;
    lv_coord_t dy = (dir >= 0) ? HELM_VIEW_PX : -HELM_VIEW_PX;

#if HELM_VIEW_ANIM == 4
    if (helm_pane_push(out, in, dir)) {
        return;
    }

    helm_pane_snap(out, in);      /* 缓冲/快照失败就退回 snap */
    return;
#elif HELM_VIEW_ANIM == 2
    if (helm_pane_pm_fade(out, in)) {
        return;
    }

    helm_pane_snap(out, in);      /* 待机 pane 之类不参与动画时退回 snap */
    return;
#elif HELM_VIEW_ANIM == 3
    if (helm_pane_pm_zoom(out, in)) {
        return;
    }

    helm_pane_snap(out, in);
    return;
#elif HELM_VIEW_ANIM == 0
    /* 实测：一次切换 176~413 ms，而动画只跑了 2~3 帧 ⇒ 每帧 100~200 ms（整屏重绘）。
     * 帧数这么少是因为渲染跟不上 16 ms 的动画节拍，动画被"拖"成 200~400 ms。
     * snap 之后切换只剩一次新页面的绘制，观感是"立刻到位"，比慢慢挪 16px 更快。 */
    helm_pane_snap(out, in);
    return;
#else
    LV_UNUSED(dy);
#endif

    /* 待机 pane 不参与动画（它一显示就盖住地图，见 helm_pane_anim_able）。 */
    out = helm_pane_anim_able(out);
    in = helm_pane_anim_able(in);
    helm_pane_reset(out);
    helm_pane_reset(in);

    /* ⚠ 出屏**不参与动画**：它一帧都不重画（原来两边一起动 = 每帧整屏重画两次），
     * 由进屏从 +16px 处滑过来盖住它，等进屏停稳后再把它隐藏（见 helm_pane_in_ready）。 */
    s_sw_t0 = lv_tick_get();
    if (s_sw_t0 == 0u) {
        s_sw_t0 = 1u;
    }

    s_sw_frames = 0;
    s_sw_out = (out != NULL && out != in) ? out : NULL;
    if (s_sw_out == NULL && out != NULL && out == in) {
        helm_pane_reset(out);
    }

    if (in) {
        lv_obj_clear_flag(in, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(in);
        lv_obj_set_style_translate_y(in, dy, 0);
        lv_obj_set_style_opa(in, LV_OPA_COVER, 0);

        lv_anim_init(&ay);
        lv_anim_set_var(&ay, in);
        lv_anim_set_values(&ay, dy, 0);
        lv_anim_set_time(&ay, HELM_VIEW_MS);
        lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&ay, helm_pane_y);
        lv_anim_set_ready_cb(&ay, helm_pane_in_ready);
        lv_anim_start(&ay);
    }
}

#if HELM_VIEW_ANIM == 3
static lv_obj_t * s_sw_pm_in;

static void helm_pane_pm_exec(void * var, int32_t p)
{
    lv_obj_t * o = (lv_obj_t *)var;
    int32_t scale = HELM_PM_ZOOM_SCALE +
                    (LV_SCALE_NONE - HELM_PM_ZOOM_SCALE) * p / 255;

    lv_obj_set_style_transform_scale(o, scale, 0);
    lv_obj_set_style_opa(o, (lv_opa_t)p, 0);
}

static void helm_pane_pm_ready(lv_anim_t * a)
{
    LV_UNUSED(a);

    /* 收尾：清掉 transform（pm 自带这个复位），再把出屏收掉。 */
    lv_pm_gpu_page_reset_transform(s_sw_pm_in);
    s_sw_pm_in = NULL;
    if (s_sw_out != NULL) {
        helm_pane_hide(s_sw_out);
        s_sw_out = NULL;
    }

    helm_sw_report();
}

/** @brief 按 lv_pm 的 GPU anima 做法切换：进屏 transform 缩放 + 淡入。 */
static bool helm_pane_pm_zoom(lv_obj_t * out, lv_obj_t * in)
{
    lv_anim_t ay;

    out = helm_pane_anim_able(out);
    in = helm_pane_anim_able(in);
    if (in == NULL) {
        return false;
    }

    helm_pane_reset(in);
    lv_obj_clear_flag(in, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(in);
    lv_obj_set_style_transform_pivot_x(in, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(in, lv_pct(50), 0);
    lv_obj_set_style_transform_scale(in, HELM_PM_ZOOM_SCALE, 0);
    lv_obj_set_style_opa(in, LV_OPA_TRANSP, 0);

    /* 出屏不参与动画（pm 那边是直接换页）：等进屏停稳再收掉。 */
    s_sw_out = (out != NULL && out != in) ? out : NULL;
    s_sw_pm_in = in;

    s_sw_t0 = lv_tick_get();
    if (s_sw_t0 == 0u) {
        s_sw_t0 = 1u;
    }

    s_sw_frames = 0;

    lv_anim_init(&ay);
    lv_anim_set_var(&ay, in);
    lv_anim_set_values(&ay, 0, 255);
    lv_anim_set_time(&ay, HELM_PM_ZOOM_MS);
    lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&ay, helm_pane_pm_exec);
    lv_anim_set_ready_cb(&ay, helm_pane_pm_ready);
    lv_anim_start(&ay);
    return true;
}
#endif

#if HELM_VIEW_ANIM == 2
#ifndef HELM_PM_FADE_MS
#define HELM_PM_FADE_MS 220u          /* 与 bicycle_page_anima.c 的 MAP 行一致 */
#endif

static void helm_pane_fade_exec(void * var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void helm_pane_pm_fade_ready(lv_anim_t * a)
{
    LV_UNUSED(a);

    /* 出屏一直垫在下面参与交叉淡入，等进屏满不透明了再收掉。 */
    if (s_sw_out != NULL) {
        helm_pane_hide(s_sw_out);
        s_sw_out = NULL;
    }

    helm_sw_report();
}

/** @brief pm 风格 fade：进屏 0→255 交叉淡入，出屏不参与动画、收尾时收起。 */
static bool helm_pane_pm_fade(lv_obj_t * out, lv_obj_t * in)
{
    lv_anim_t ay;

    out = helm_pane_anim_able(out);
    in = helm_pane_anim_able(in);
    if (in == NULL) {
        return false;
    }

    helm_pane_reset(in);
    lv_obj_clear_flag(in, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(in);
    lv_obj_set_style_opa(in, LV_OPA_TRANSP, 0);

    s_sw_out = (out != NULL && out != in) ? out : NULL;

    s_sw_t0 = lv_tick_get();
    if (s_sw_t0 == 0u) {
        s_sw_t0 = 1u;
    }

    s_sw_frames = 0;

    lv_anim_init(&ay);
    lv_anim_set_var(&ay, in);
    lv_anim_set_values(&ay, 0, 255);
    lv_anim_set_time(&ay, HELM_PM_FADE_MS);
    lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&ay, helm_pane_fade_exec);
    lv_anim_set_ready_cb(&ay, helm_pane_pm_fade_ready);
    lv_anim_start(&ay);
    return true;
}
#endif

static void helm_pane_nudge(lv_obj_t * obj, int dir)
{
    lv_anim_t ay;
    lv_coord_t dy = (dir >= 0) ? HELM_DATA_SLIDE_PX : -HELM_DATA_SLIDE_PX;

    if (obj == NULL) {
        return;
    }

    helm_pane_reset(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(obj);
    lv_obj_set_style_translate_y(obj, dy, 0);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);

    lv_anim_init(&ay);
    lv_anim_set_var(&ay, obj);
    lv_anim_set_values(&ay, dy, 0);
    lv_anim_set_time(&ay, HELM_VIEW_MS);
    lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&ay, helm_pane_y);
    lv_anim_start(&ay);
}

/**
 * @brief 骑行页轻量入场：只动共享内容根 8 px，不碰地图 canvas 自身 pan。
 */
static void helm_riding_view_enter(int dir)
{
    lv_obj_t * root = map_page_root(s_sh.map);
    lv_anim_t ay;
    lv_coord_t dy = (dir >= 0) ? HELM_RIDE_ENTER_PX : -HELM_RIDE_ENTER_PX;

    if (root == NULL) {
        return;
    }

    lv_anim_delete(root, helm_pane_y);
    lv_obj_set_style_translate_y(root, dy, 0);
    lv_anim_init(&ay);
    lv_anim_set_var(&ay, root);
    lv_anim_set_values(&ay, dy, 0);
    lv_anim_set_time(&ay, HELM_RIDE_ENTER_MS);
    lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&ay, helm_pane_y);
    lv_anim_start(&ay);
}

static void helm_raise_chrome(void)
{
    if (!s_sh.attached || s_sh.ui_covered) {
        return;
    }

    helm_raise_if_shown(s_sh.subbar);
    helm_raise_if_shown(s_sh.nav_ban);
    helm_raise_if_shown(s_sh.rotbar);
    helm_raise_if_shown(s_sh.pause_dock);
    helm_raise_if_shown(s_sh.arrive_dock);
    if (helm_save_open()) {
        lv_obj_move_foreground(s_sh.save_mask);
    }
}

static lv_obj_t * helm_overlay(lv_obj_t * parent, lv_coord_t y, lv_coord_t h)
{
    lv_obj_t * o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    helm_style_scr(o);
    lv_obj_set_style_pad_all(o, HELM_INSET, 0);
    lv_obj_set_style_pad_row(o, HELM_GAP, 0);
    lv_obj_set_size(o, PAGE_HOR_RES, h);
    lv_obj_add_flag(o, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(o, 0, y);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t * helm_lab(lv_obj_t * parent, const lv_font_t * font, uint32_t color,
                           const char * txt)
{
    lv_obj_t * l = lv_label_create(parent);

    if (font) {
        lv_obj_set_style_text_font(l, font, 0);
    }

    lv_obj_set_style_text_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(l, helm_color(color), 0);
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t * helm_col(lv_obj_t * parent, lv_obj_t ** lab,
                            lv_obj_t ** val, const lv_font_t * vfont)
{
    lv_obj_t * col = lv_obj_create(parent);

    lv_obj_remove_style_all(col);
    helm_grow_x(col);
    lv_obj_set_height(col, lv_pct(100));
    helm_style_card(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    *lab = helm_lab(col, s_sh.font_lab, HELM_COLOR_INK, "--");
    *val = helm_lab(col, vfont ? vfont : s_sh.font_val, HELM_COLOR_INK, "--");
    return col;
}

/** @brief 地图底栏三格：父宽已固定，flex 均分。 */
static lv_obj_t * helm_rot_col(lv_obj_t * parent, lv_obj_t ** lab,
                              lv_obj_t ** val, const lv_font_t * vfont)
{
    lv_obj_t * col = lv_obj_create(parent);

    lv_obj_remove_style_all(col);
    helm_grow_x(col);
    lv_obj_set_height(col, lv_pct(100));
    helm_style_card(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_HIDDEN);

    *lab = helm_lab(col, s_sh.font_lab, HELM_COLOR_INK, "--");
    *val = helm_lab(col, vfont ? vfont : s_sh.font_val, HELM_COLOR_INK, "--");
    lv_obj_remove_flag(*lab, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(*val, LV_OBJ_FLAG_HIDDEN);
    return col;
}

/**
 * @brief 速度文本（1 位小数；**≥100 km/h 只留整数**）。
 *
 * @note 用户 2026-09-27 定：超过 100 不显示小数。两条理由都成立：
 *       ① 定宽值盒（`HELM_RIDE_TRIP_VAL_W=72`）是按 num_28 的**4 字形**反推的（实测 67px），
 *          "100.5" 这种 5 字形 ≈ 82px 放不下 ⇒ 默认换行后第二行被行高切掉，
 *          现场看上去就是"超过 100 小数消失"；
 *       ② 100 km/h 以上的 0.1 km/h 本来也没有意义。
 *       门槛用**四舍五入后的值**判（`v >= 1000` ⇒ 显示 "100" 而不是 "100.0"）。
 */
static void helm_fmt_speed(char * buf, size_t n, float kph)
{
    int v = (int)(kph * 10.0f + 0.5f);

    if (v < 0) {
        v = 0;
    }

    if (v >= 1000) {
        lv_snprintf(buf, n, "%d", v / 10);
    } else {
        lv_snprintf(buf, n, "%d.%d", v / 10, v % 10);
    }
}

static void helm_fmt_time(char * buf, size_t n, uint64_t ms)
{
    unsigned s = (unsigned)(ms / 1000ull);
    unsigned min = s / 60u;
    unsigned sec = s % 60u;

    /* 未满 100 分钟：MM:SS；到顶后低位改分钟，HH:MM（100 min → 01:40）。 */
    if (min < 100u) {
        lv_snprintf(buf, n, "%02u:%02u", min, sec);
    } else {
        lv_snprintf(buf, n, "%02u:%02u", min / 60u, min % 60u);
    }
}

static void helm_fmt_hms(char * buf, size_t n, uint64_t ms)
{
    helm_fmt_time(buf, n, ms);
}

/**
 * @brief 里程文本（1 位小数；**≥100 km 只留整数**）。
 * @note 用户 2026-09-27 定，与 `helm_fmt_speed()` 同规则、同一个定宽盒约束
 *       （见那边的注释）。
 */
static void helm_fmt_km(char * buf, size_t n, float km)
{
    int d10 = (int)(km * 10.0f + 0.5f);

    if (d10 < 0) {
        d10 = 0;
    }

    if (d10 >= 1000) {
        lv_snprintf(buf, n, "%d", d10 / 10);
    } else {
        lv_snprintf(buf, n, "%d.%d", d10 / 10, d10 % 10);
    }
}

static float helm_session_avg_kph(const bicycle_runtime_t * rt)
{
    double hours;

    if (rt == NULL || rt->session_elapsed_ms < 1000) {
        return 0.0f;
    }

    hours = (double)rt->session_elapsed_ms / 3600000.0;
    return (float)((rt->session_distance_m / 1000.0) / hours);
}

static void helm_avg_sample(const bicycle_runtime_t * rt)
{
    s_avg_dist[s_avg_i] = rt ? rt->session_distance_m : 0.0;
    s_avg_tick[s_avg_i] = lv_tick_get();
    s_avg_i = (uint8_t)((s_avg_i + 1u) % HELM_AVG_N);
    if (s_avg_n < HELM_AVG_N) {
        s_avg_n++;
    }
}

/** @brief 最近 30 秒均速（这段位移 / 时间）。 */
static float helm_avg_kph(const bicycle_runtime_t * rt)
{
    uint32_t now;
    uint32_t dt;
    uint8_t i;
    uint8_t idx;
    uint8_t oldest;
    double dd;
    double hours;

    if (rt == NULL || s_avg_n < 2) {
        return 0.0f;
    }

    now = lv_tick_get();
    oldest = 0xff;
    for (i = 0; i < s_avg_n; i++) {
        idx = (uint8_t)((s_avg_i + HELM_AVG_N - s_avg_n + i) % HELM_AVG_N);
        dt = now - s_avg_tick[idx];
        if (dt > HELM_AVG_WIN_MS) {
            continue;
        }

        oldest = idx;
        break;
    }

    if (oldest == 0xff) {
        return 0.0f;
    }

    dt = now - s_avg_tick[oldest];
    if (dt < 1000u) {
        return 0.0f;
    }

    dd = rt->session_distance_m - s_avg_dist[oldest];
    if (dd < 0.0) {
        dd = 0.0;
    }

    hours = (double)dt / 3600000.0;
    return (float)((dd / 1000.0) / hours);
}

static const char * helm_dash(bool ok, char * buf, size_t n, const char * fmt,
                              int v)
{
    if (!ok) {
        return "--";
    }

    lv_snprintf(buf, n, fmt, v);
    return buf;
}

/**
 * @brief 按数据格标签给出默认数字色（静态主题色，不做实时跳色）。
 * @param lab 标签 UTF-8 文本。
 * @return RGB888 主题色。
 * @note 「当前时速 / 心率」的实时三档色走 `helm_live_speed_ink()` /
 *       `helm_live_hr_ink()`，不经过本函数。均速 / 极速仍用导航橙。
 */
static uint32_t helm_tone(const char * lab)
{
    if (lab == NULL) {
        return HELM_COLOR_INK;
    }

    if (strncmp(lab, "心率", 6) == 0 || strncmp(lab, "均心率", 9) == 0 ||
        strncmp(lab, "最大心率", 12) == 0) {
        return HELM_COLOR_HR;
    }

    if (strncmp(lab, "踏频", 6) == 0) {
        return HELM_COLOR_CAD;
    }

    if (strncmp(lab, "功率", 6) == 0) {
        return HELM_COLOR_PWR;
    }

    if (strncmp(lab, "海拔", 6) == 0 || strncmp(lab, "累计爬升", 12) == 0 ||
        strncmp(lab, "爬升", 6) == 0 || strncmp(lab, "坡度", 6) == 0 ||
        strncmp(lab, "下降", 6) == 0) {
        return HELM_COLOR_CLIMB;
    }

    if (strncmp(lab, "速度", 6) == 0 || strncmp(lab, "均速", 6) == 0 ||
        strncmp(lab, "极速", 6) == 0) {
        return HELM_COLOR_NAV;
    }

    return HELM_COLOR_INK;
}

/**
 * @brief 按数据格标签给出卡片底色 / 走势是否启用。
 * @param lab 标签 UTF-8 文本。
 * @return 填充色；`HELM_COLOR_PAPER` 表示无走势。
 */
static uint32_t helm_tone_fill(const char * lab)
{
    if (lab == NULL) {
        return HELM_COLOR_PAPER;
    }

    if (strncmp(lab, "心率", 6) == 0) {
        return HELM_COLOR_HR_FILL;
    }

    if (strncmp(lab, "均心率", 9) == 0 || strncmp(lab, "最大心率", 12) == 0) {
        return HELM_COLOR_PAPER;
    }

    if (strncmp(lab, "踏频", 6) == 0) {
        return HELM_COLOR_CAD_FILL;
    }

    if (strncmp(lab, "功率", 6) == 0) {
        return HELM_COLOR_PWR_FILL;
    }

    if (strncmp(lab, "海拔", 6) == 0 || strncmp(lab, "累计爬升", 12) == 0 ||
        strncmp(lab, "爬升", 6) == 0 || strncmp(lab, "坡度", 6) == 0 ||
        strncmp(lab, "下降", 6) == 0) {
        return HELM_COLOR_CLIMB_FILL;
    }

    if (strncmp(lab, "均速", 6) == 0 || strncmp(lab, "速度", 6) == 0) {
        return HELM_COLOR_NAV_FILL;
    }

    return HELM_COLOR_PAPER;
}

/**
 * @brief 时速 / 心率三档对应的数字描边色。
 * @param z 0 低（绿）、1 中（亮黄）、2 高（红）。
 * @return RGB888；绘制前再 `helm_color()`。
 */
static uint32_t helm_zone_ink(uint8_t z)
{
    if (z == 0u) {
        return HELM_COLOR_CAD;
    }

    if (z == 1u) {
        return HELM_COLOR_YEL;
    }

    return HELM_COLOR_HR;
}

/**
 * @brief 时速 / 心率三档对应的走势填充色。
 * @param z 0 低（绿）、1 中（亮黄）、2 高（红）。
 * @return RGB888 走势面积色（跳过 `helm_color()`）。
 */
static uint32_t helm_zone_spark(uint8_t z)
{
    if (z == 0u) {
        return HELM_COLOR_CAD_SPARK;
    }

    if (z == 1u) {
        return HELM_COLOR_YEL_SPARK;
    }

    return HELM_COLOR_HR_SPARK;
}

/**
 * @brief 把时速归入低 / 中 / 高三档（档内数字跳色）。
 * @param kph 当前时速（km/h）。
 * @return 0 低于 18；1 为 18–32；2 大于等于 32。
 */
static uint8_t helm_speed_zone(float kph)
{
    if (kph < HELM_SPEED_ZONE_MID) {
        return 0u;
    }

    if (kph < HELM_SPEED_ZONE_HIGH) {
        return 1u;
    }

    return 2u;
}

/**
 * @brief 把心率归入低 / 中 / 高三档（不做渐变）。
 * @param bpm 当前心率。
 * @return 0 低于 120；1 为 120–149；2 大于等于 150。
 */
static uint8_t helm_hr_zone(uint16_t bpm)
{
    if (bpm < HELM_HR_ZONE_MID) {
        return 0u;
    }

    if (bpm < HELM_HR_ZONE_HIGH) {
        return 1u;
    }

    return 2u;
}

/**
 * @brief 当前时速的三档数字色。
 * @return `helm_zone_ink()` 结果；无 runtime 时按 0 km/h（绿）。
 */
static uint32_t helm_live_speed_ink(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();

    return helm_zone_ink(helm_speed_zone(rt ? rt->speed_kph : 0.0f));
}

/**
 * @brief 当前心率的三档数字色。
 * @return 无效心率保持 `HELM_COLOR_HR`，不跳档。
 */
static uint32_t helm_live_hr_ink(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();

    if (rt == NULL || !rt->hr_valid) {
        return HELM_COLOR_HR;
    }

    return helm_zone_ink(helm_hr_zone(rt->hr_bpm));
}

static void helm_set_text_hex(lv_obj_t * obj, uint32_t hex)
{
    lv_color_t col;

    if (obj == NULL) {
        return;
    }

    col = helm_color(hex);
    /* 同色不重设（WP-UIFIX/R1）：text_color 是普通 style，set 一次必然
     * refresh_style（头尾各 invalidate 一次）。500 ms 刷新里颜色静止时
     * 完全不变，重设会把 124×84 的时速 hero / 各行数值白刷一遍。
     * 实测骑行静止 flush 105.5→33.9 KB/s（−68%），hero 不再被白刷。 */
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), col)) {
        lv_obj_set_style_text_color(obj, col, 0);
    }
}

/** @brief 同色不重设版 `lv_obj_set_style_bg_color()`（色条每 500 ms 重设）。 */
static void helm_set_bg_hex(lv_obj_t * obj, uint32_t hex)
{
    lv_color_t col;

    if (obj == NULL) {
        return;
    }

    col = helm_color(hex);
    if (!lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), col)) {
        lv_obj_set_style_bg_color(obj, col, 0);
    }
}

/**
 * @brief 把时速数字和单位刷成同一档实时色。
 * @param num 时速数字标签。
 * @param unit 单位标签（可为 NULL）。
 */
static void helm_paint_speed_pair(lv_obj_t * num, lv_obj_t * unit)
{
    uint32_t ink = helm_live_speed_ink();

    helm_set_text_hex(num, ink);
    helm_set_text_hex(unit, ink);
}

static bool helm_lab_eq(const char * lab, const char * key)
{
    return lab != NULL && key != NULL && strcmp(lab, key) == 0;
}

/** @brief 当前心率用实时跳色；均心按会话平均跳档；其余走主题色。 */
static uint32_t helm_metric_ink(const char * lab)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();

    if (helm_lab_eq(lab, "心率")) {
        return helm_live_hr_ink();
    }

    if (helm_lab_eq(lab, "均心率") && rt != NULL && rt->hr_avg_bpm > 0u) {
        return helm_zone_ink(helm_hr_zone(rt->hr_avg_bpm));
    }

    return helm_tone(lab);
}

/**
 * @brief 走势折线描边色。
 * @param id 历史序列种类。
 * @return RGB888。时速 / 心率跟当前档位走，其余用主题色。
 */
static uint32_t helm_spark_stroke_of(helm_hist_id_t id)
{
    switch (id) {
    case HELM_HIST_HR:
        return helm_live_hr_ink();
    case HELM_HIST_CAD:
        return HELM_COLOR_CAD;
    case HELM_HIST_PWR:
        return HELM_COLOR_PWR;
    case HELM_HIST_ALT:
    case HELM_HIST_GRADE:
    case HELM_HIST_GAIN:
    case HELM_HIST_LOSS:
        return HELM_COLOR_CLIMB;
    case HELM_HIST_SPEED:
        return helm_live_speed_ink();
    case HELM_HIST_TIME:
    case HELM_HIST_DIST:
        return HELM_COLOR_NAV;
    default:
        return HELM_COLOR_INK;
    }
}

/**
 * @brief 走势面积填充色。
 * @param id 历史序列种类。
 * @return RGB888。时速 / 心率跟当前档位走；无效心率用 `HELM_COLOR_HR_SPARK`。
 */
static uint32_t helm_spark_fill_of(helm_hist_id_t id)
{
    uint8_t z;

    switch (id) {
    case HELM_HIST_HR: {
        const bicycle_runtime_t * rt = bicycle_runtime_get();

        if (rt == NULL || !rt->hr_valid) {
            return HELM_COLOR_HR_SPARK;
        }

        return helm_zone_spark(helm_hr_zone(rt->hr_bpm));
    }
    case HELM_HIST_CAD:
        return HELM_COLOR_CAD_SPARK;
    case HELM_HIST_PWR:
        return HELM_COLOR_PWR_SPARK;
    case HELM_HIST_ALT:
    case HELM_HIST_GRADE:
    case HELM_HIST_GAIN:
    case HELM_HIST_LOSS:
        return HELM_COLOR_CLIMB_SPARK;
    case HELM_HIST_SPEED: {
        const bicycle_runtime_t * rt = bicycle_runtime_get();

        z = helm_speed_zone(rt ? rt->speed_kph : 0.0f);
        return helm_zone_spark(z);
    }
    case HELM_HIST_TIME:
    case HELM_HIST_DIST:
        return HELM_COLOR_NAV_SPARK;
    default:
        return HELM_COLOR_PAPER;
    }
}

static void helm_draw_seg(lv_layer_t * layer, int32_t x0, int32_t y0,
                          int32_t x1, int32_t y1, uint16_t w, lv_color_t color)
{
    lv_draw_line_dsc_t ld;

    lv_draw_line_dsc_init(&ld);
    ld.color = color;
    ld.width = w;
    ld.opa = LV_OPA_COVER;
    ld.round_start = 1;
    ld.round_end = 1;
    ld.p1.x = x0;
    ld.p1.y = y0;
    ld.p2.x = x1;
    ld.p2.y = y1;
    lv_draw_line(layer, &ld);
}

static void helm_draw_pip(lv_layer_t * layer, int32_t x, int32_t y,
                          lv_coord_t pr, lv_color_t color)
{
    lv_draw_rect_dsc_t rd;
    lv_area_t a;

    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = color;
    rd.bg_opa = LV_OPA_COVER;
    rd.border_width = 1;
    rd.border_color = helm_color(HELM_COLOR_INK);
    rd.border_opa = LV_OPA_COVER;
    rd.radius = LV_RADIUS_CIRCLE;
    a.x1 = (lv_coord_t)(x - pr);
    a.y1 = (lv_coord_t)(y - pr);
    a.x2 = (lv_coord_t)(x + pr);
    a.y2 = (lv_coord_t)(y + pr);
    lv_draw_rect(layer, &rd, &a);
}

static helm_hist_id_t helm_hist_id(const char * lab)
{
    if (lab == NULL) {
        return HELM_HIST_NONE;
    }

    if (strncmp(lab, "累计爬升", 12) == 0 || strncmp(lab, "爬升", 6) == 0) {
        return HELM_HIST_GAIN;
    }

    if (strncmp(lab, "极速", 6) == 0) {
        return HELM_HIST_NONE;
    }

    if (strncmp(lab, "均速", 6) == 0 || strncmp(lab, "速度", 6) == 0) {
        return HELM_HIST_SPEED;
    }

    if (strncmp(lab, "均心率", 9) == 0 || strncmp(lab, "最大心率", 12) == 0) {
        return HELM_HIST_NONE;
    }

    if (strncmp(lab, "心率", 6) == 0) {
        return HELM_HIST_HR;
    }

    if (strncmp(lab, "踏频", 6) == 0) {
        return HELM_HIST_CAD;
    }

    if (strncmp(lab, "功率", 6) == 0) {
        return HELM_HIST_PWR;
    }

    if (strncmp(lab, "海拔", 6) == 0) {
        return HELM_HIST_ALT;
    }

    if (strncmp(lab, "坡度", 6) == 0) {
        return HELM_HIST_GRADE;
    }

    /* 时间 / 里程只显示数字，不铺底走势。 */
    return HELM_HIST_NONE;
}

static void helm_hist_reset(void)
{
    s_hist_n = 0;
    s_hist_min = 0xffffffffu;
    s_avg_n = 0;
    s_avg_i = 0;
    s_rec_ele_n = 0;
    s_rec_ele_slot = 0xffffffffu;
    if (s_hist != NULL) {
        memset(s_hist, 0, sizeof(float) * (size_t)HELM_HIST_COUNT * HELM_HIST_N);
    }
    memset(s_rec_ele, 0, sizeof(s_rec_ele));
    memset(s_rec_dist, 0, sizeof(s_rec_dist));
    s_climb_fp_pts = 0xffffffffu;
    s_climb_fp_total_cm = 0xffffffffu;
    s_climb_fp_rec_n = 0xff;
    s_climb_fp_rec_slot = 0xffffffffu;
}

static bool helm_hist_push(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();
    uint32_t minute;
    int i;

    if (s_hist == NULL || rt == NULL) {
        return false;
    }

    /* 采样节拍：**记录中**按会话时间（暂停/继续与里程计时一致），**没开录**按
     * 系统 tick。旧写法是"没 recording 就整支不采样"，结果骑行页在台子上
     * （没开录）翻到时，速度/心率/功率三根走势全都没有线——用户 2026-09-26
     * 实测："还是没有任何线条绘制出来"。走势是"最近一分钟的实时值"，和会话记不
     * 记录无关：待机翻页就能看到这一页，也该有线。
     * 开录/续录时会 helm_hist_reset()（会话起点从空图开始），所以底座切换不会串。 */
    if (rt->recording || s_sh.paused) {
        minute = (uint32_t)(rt->session_elapsed_ms / HELM_HIST_PERIOD_MS);
    } else {
        minute = lv_tick_get() / HELM_HIST_PERIOD_MS;
    }

    if (s_hist_n > 0 && minute == s_hist_min) {
        return false;
    }

    s_hist_min = minute;
    if (s_hist_n < HELM_HIST_N) {
        i = s_hist_n;
        s_hist_n++;
    } else {
        for (i = 0; i < HELM_HIST_COUNT; i++) {
            memmove(&s_hist[i][0], &s_hist[i][1],
                    (HELM_HIST_N - 1u) * sizeof(float));
        }

        i = HELM_HIST_N - 1;
    }

    s_hist[HELM_HIST_SPEED][i] = helm_avg_kph(rt);
    s_hist[HELM_HIST_HR][i] = (rt && rt->hr_valid) ? (float)rt->hr_bpm : 0.0f;
    s_hist[HELM_HIST_CAD][i] = (rt && rt->cadence_valid) ? (float)rt->cadence_rpm : 0.0f;
    s_hist[HELM_HIST_PWR][i] = (rt && rt->power_valid) ? (float)rt->power_w : 0.0f;
    s_hist[HELM_HIST_ALT][i] = rt ? rt->altitude_m : 0.0f;
    s_hist[HELM_HIST_GRADE][i] = rt ? rt->grade_pct : 0.0f;
    s_hist[HELM_HIST_GAIN][i] = rt ? rt->gain_m : 0.0f;
    s_hist[HELM_HIST_LOSS][i] = rt ? rt->loss_m : 0.0f;
    s_hist[HELM_HIST_TIME][i] = rt ? (float)(rt->session_elapsed_ms / 1000.0) : 0.0f;
    s_hist[HELM_HIST_DIST][i] = rt ? (float)rt->session_distance_m : 0.0f;
    return true;
}

static int32_t helm_spark_sample(helm_hist_id_t id, int i)
{
    float v;

    if (id <= HELM_HIST_NONE || id >= HELM_HIST_COUNT || s_hist == NULL) {
        return 0;
    }

    if (s_hist_n <= 0) {
        return 0;
    }

    if (i < 0) {
        i = 0;
    } else if (i >= s_hist_n) {
        i = s_hist_n - 1;
    }

    v = s_hist[id][i];
    switch (id) {
    case HELM_HIST_SPEED:
    case HELM_HIST_ALT:
    case HELM_HIST_GRADE:
    case HELM_HIST_GAIN:
    case HELM_HIST_LOSS:
        return (int32_t)(v * 10.0f);
    default:
        return (int32_t)(v + (v >= 0.0f ? 0.5f : -0.5f));
    }
}

static void helm_spark_axis(helm_hist_id_t id, int32_t * mn, int32_t * mx)
{
    switch (id) {
    case HELM_HIST_HR:
        *mn = 40;
        *mx = 220;
        break;
    case HELM_HIST_CAD:
        *mn = 0;
        *mx = 180;
        break;
    case HELM_HIST_PWR:
        *mn = 0;
        *mx = 500;
        break;
    case HELM_HIST_ALT:
        *mn = -50;
        *mx = 4000;
        break;
    case HELM_HIST_GRADE:
        *mn = -150;
        *mx = 200;
        break;
    case HELM_HIST_GAIN:
    case HELM_HIST_LOSS:
        *mn = 0;
        *mx = 5000;
        break;
    case HELM_HIST_SPEED:
        *mn = 0;
        *mx = 600;
        break;
    case HELM_HIST_TIME:
        *mn = 0;
        *mx = 3600;
        break;
    case HELM_HIST_DIST:
        *mn = 0;
        *mx = 20000;
        break;
    default:
        *mn = 0;
        *mx = 100;
        break;
    }
}

/**
 * @brief 走势面积：按当前档位填色（时速 / 心率会跳档）。
 * @param e LV_EVENT_DRAW_TASK_ADDED。
 */
static void helm_spark_add_faded_area(lv_event_t * e)
{
    helm_spark_t * sp = lv_event_get_user_data(e);
    lv_obj_t * obj = lv_event_get_target_obj(e);
    lv_draw_task_t * draw_task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t * base_dsc;
    lv_draw_line_dsc_t * draw_line_dsc;
    lv_color_t fill;
    lv_area_t coords;
    int32_t full_h;

    if (sp == NULL || obj == NULL || draw_task == NULL || sp->id == HELM_HIST_NONE) {
        return;
    }

    base_dsc = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(draw_task);
    draw_line_dsc = lv_draw_task_get_line_dsc(draw_task);
    if (base_dsc == NULL || base_dsc->layer == NULL ||
        draw_line_dsc == NULL || draw_line_dsc->points == NULL ||
        draw_line_dsc->point_cnt < 2) {
        return;
    }

    lv_obj_get_coords(obj, &coords);
    full_h = lv_obj_get_height(obj);
    if (full_h < 1) {
        return;
    }

    fill = helm_color_snap(helm_spark_fill_of(sp->id));

    /* 逐行填（板级第二版，2026-09-25）：每行画**纯色**矩形，alpha 取**同一条全局
     * 斜坡**（行内为常数），矩形的 x 区间 = 折线在该行"位于曲线上方"的连续段。
     *
     * 为什么换掉"逐段三角形 + 矩形"（上游示例 lv_example_chart_area_gradient 的写法）：
     * 那个写法有两类 1 px 竖线，且都跟曲线形状有关（所以"有时候"才出现）——
     *   · 峰顶：相邻两段的三角形竖边**落在同一列**、各投满一次色带 ⇒ 混合两次 ⇒
     *     偏深竖线（现场："有时候又竖向的一个小黑线"）；
     *   · 段边界：那一列两边的形状都不覆盖 ⇒ 缺口 ⇒ 偏浅竖线。
     * 逐行填里每行只按 x 区间划分，相邻行、相邻区间互不重叠 ⇒ 两类竖线都不存在。
     *
     * 顺带更省：本板 EPIC 加速单元**没有 TRIANGLE**（只有 FILL/IMAGE/BORDER/LABEL/
     * LAYER），三角形会回退到 SW 并逐行生成遮罩；改成纯色矩形后整片面积全部走
     * EPIC 的 FILL。代价是 H 行 × W 列一次整型扫描（约 55×200），亚毫秒级。 */
    {
        int32_t x_first = (int32_t)draw_line_dsc->points[0].x;
        int32_t x_last = (int32_t)draw_line_dsc->points[draw_line_dsc->point_cnt - 1].x;
        int32_t y;

        if (x_last <= x_first) {
            return;
        }

        /* 现场故障对点：P02（骑行表盘）在**心率线这一处**定格，随后 UI 心跳停
         * ≥10 s、看门狗停喂复位。这段逐行填充是唯一一处"按像素行 × 列"双重循环
         * 的绘制代码（H 行 × W 列），所以给它加两道保险：
         *   ① 硬上限：矩形数超过 4096 直接放弃剩余行（正常约 H/2 × 段数，几十个）。
         *   ② 计时：单次填充 ≥50 ms 打一行 WARN —— 正常亚毫秒。
         * 一旦真凶在这里，日志会直接给出 rows/rects/pts 三个量。 */
        {
            const uint32_t t_fill = lv_tick_get();
            int rects = 0;

            for (y = coords.y1; y <= coords.y2; y++) {
                lv_draw_rect_dsc_t rect_dsc;
                lv_area_t rect_area;
                lv_opa_t opa = (lv_opa_t)(255 - (y - coords.y1) * 255 / full_h);
                uint32_t seg = 0;
                int32_t run = 0;
                bool in = false;
                int32_t x;

                if (opa < 8) {
                    break;              /* 再淡肉眼已不可见：省掉剩下的绘制 */
                }

                if (rects > 4096) {
                    syslog(LOG_WARNING,
                           "[spark] fill abort rects=%d y=%ld rows=%ld..%ld\n",
                           rects, (long)y, (long)coords.y1, (long)coords.y2);
                    break;
                }

                lv_draw_rect_dsc_init(&rect_dsc);
                rect_dsc.bg_color = fill;
                rect_dsc.bg_opa = opa;
                rect_area.y1 = y;
                rect_area.y2 = y;

                for (x = x_first; x <= x_last; x++) {
                    bool under = false;

                    while (seg < draw_line_dsc->point_cnt - 2 &&
                           (int32_t)draw_line_dsc->points[seg + 1].x < x) {
                        seg++;
                    }

                    {
                        lv_point_precise_t a = draw_line_dsc->points[seg];
                        lv_point_precise_t b = draw_line_dsc->points[seg + 1];

                        /* 端点无效（NONE）或该段不向右延伸：这一列当作"曲线不在上方"、
                         * 不填 —— 与老写法"跳过无效段"的语义一致。 */
                        if (a.x != LV_DRAW_LINE_POINT_NONE &&
                            a.y != LV_DRAW_LINE_POINT_NONE &&
                            b.x != LV_DRAW_LINE_POINT_NONE &&
                            b.y != LV_DRAW_LINE_POINT_NONE &&
                            b.x > a.x) {
                            int32_t yc = (int32_t)a.y +
                                         (int32_t)((b.y - a.y) * (x - a.x) / (b.x - a.x));

                            under = (yc < y);
                        }
                    }

                    if (under && !in) {
                        run = x;
                        in = true;
                    } else if (!under && in) {
                        rect_area.x1 = run;
                        rect_area.x2 = x - 1;
                        if (rect_area.x2 >= rect_area.x1) {
                            lv_draw_rect(base_dsc->layer, &rect_dsc, &rect_area);
                            rects++;
                        }
                        in = false;
                    }
                }

                if (in) {
                    rect_area.x1 = run;
                    rect_area.x2 = x_last;
                    if (rect_area.x2 >= rect_area.x1) {
                        lv_draw_rect(base_dsc->layer, &rect_dsc, &rect_area);
                        rects++;
                    }
                }
            }

            {
                uint32_t dt = lv_tick_elaps(t_fill);

                if (dt >= 50u) {
                    syslog(LOG_WARNING,
                           "[spark] fill slow %ums rects=%d rows=%ld pts=%u\n",
                           (unsigned)dt, rects, (long)(coords.y2 - coords.y1 + 1),
                           (unsigned)draw_line_dsc->point_cnt);
                }
            }
        }
    }
}

/**
 * @brief 走势折线绘制前回调：把面积刷成当前档位色。
 * @param e LV_EVENT_DRAW_TASK_ADDED。
 */
static void helm_spark_draw_task(lv_event_t * e)
{
    lv_draw_task_t * draw_task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t * base_dsc;

    if (draw_task == NULL) {
        return;
    }

    base_dsc = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(draw_task);
    if (base_dsc == NULL) {
        return;
    }

    if (base_dsc->part == LV_PART_ITEMS &&
        lv_draw_task_get_type(draw_task) == LV_DRAW_TASK_TYPE_LINE) {
        helm_spark_add_faded_area(e);
    }
}

/**
 * @brief 走势绘制后补描边：时速 / 心率用当前档位色。
 * @param e LV_EVENT_DRAW_POST。
 */
static void helm_spark_draw_post(lv_event_t * e)
{
    helm_spark_t * sp = lv_event_get_user_data(e);
    lv_layer_t * layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_point_t p0;
    lv_point_t p1;
    lv_color_t stroke;
    uint32_t n;

    if (sp == NULL || layer == NULL || sp->obj == NULL || sp->ser == NULL ||
        sp->id == HELM_HIST_NONE) {
        return;
    }

    if (lv_obj_has_flag(sp->obj, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    n = lv_chart_get_point_count(sp->obj);
    if (n < 2) {
        return;
    }

    lv_chart_get_point_pos_by_id(sp->obj, sp->ser, n - 2, &p0);
    lv_chart_get_point_pos_by_id(sp->obj, sp->ser, n - 1, &p1);
    lv_obj_get_coords(sp->obj, &coords);
    stroke = helm_color_snap(helm_spark_stroke_of(sp->id));
    helm_draw_seg(layer, coords.x1 + p0.x, coords.y1 + p0.y,
                  coords.x1 + p1.x, coords.y1 + p1.y, 4, stroke);
    helm_draw_pip(layer, coords.x1 + p1.x, coords.y1 + p1.y, 3, stroke);
}

/**
 * @brief 祖先链上没有 HIDDEN 时走势才参与刷新 / 绘制。
 */
static bool helm_spark_tree_shown(const lv_obj_t * obj)
{
    while (obj != NULL) {
        if (lv_obj_has_flag((lv_obj_t *)obj, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }
        obj = lv_obj_get_parent(obj);
    }
    return true;
}

/**
 * @brief 按标签绑定走势图并写入当前历史点。
 * @param sp 走势槽。
 * @param id 历史序列；`HELM_HIST_NONE` 则隐藏。
 * @details 记下 `tone`，供 `helm_spark_sync()` 检测时速 / 心率换档。
 */
static void helm_spark_reload(helm_spark_t * sp, helm_hist_id_t id)
{
    int32_t mn;
    int32_t mx;
    int i;
    int n;

    sp->id = id;
    if (sp->obj == NULL || sp->ser == NULL) {
        return;
    }

    if (id == HELM_HIST_NONE || s_hist_n < 2
        || !helm_spark_tree_shown(lv_obj_get_parent(sp->obj))) {
        lv_obj_add_flag(sp->obj, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_remove_flag(sp->obj, LV_OBJ_FLAG_HIDDEN);
    sp->tone = helm_spark_stroke_of(id);
    lv_chart_set_series_color(sp->obj, sp->ser, helm_color_snap(sp->tone));
    helm_spark_axis(id, &mn, &mx);
    lv_chart_set_axis_range(sp->obj, LV_CHART_AXIS_PRIMARY_Y, mn, mx);
    n = s_hist_n;
    for (i = 0; i < HELM_SPARK_PTS; i++) {
        int si = (n > 1) ? (i * (n - 1)) / (HELM_SPARK_PTS - 1) : 0;

        lv_chart_set_series_value_by_id(sp->obj, sp->ser, (uint32_t)i,
                                        helm_spark_sample(id, si));
    }

    lv_chart_set_x_start_point(sp->obj, sp->ser, 0);
}

/**
 * @brief 给数据格挂上走势图。
 * @param cell 格子容器。
 * @param lab 指标标签（据此选历史序列与配色）。
 */
/** @param strip_h 非 0 时把走势图压成**底部条**（高度固定、贴底），避免折线
 *                 画到数字上；0 = 原来的整格背景层。 */
static void helm_spark_bind(lv_obj_t * cell, lv_obj_t * lab, lv_coord_t strip_h)
{
    helm_spark_t * sp;
    lv_obj_t * chart;
    lv_obj_t * last;

    if (cell == NULL || lab == NULL || s_spark_n >= HELM_SPARK_MAX) {
        return;
    }

    sp = &s_spark[s_spark_n];
    s_spark_n++;
    chart = lv_chart_create(cell);
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    if (strip_h > 0) {
        lv_obj_set_size(chart, lv_pct(100), strip_h);
        lv_obj_align(chart, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    } else {
        lv_obj_set_size(chart, lv_pct(100), lv_pct(100));
        lv_obj_align(chart, LV_ALIGN_TOP_LEFT, 0, 0);
    }
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, 0, 0);
    lv_obj_set_style_radius(chart, 0, 0);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_CIRCULAR);
    lv_chart_set_point_count(chart, HELM_SPARK_PTS);
    lv_chart_set_div_line_count(chart, 0, 0);
    sp->obj = chart;
    sp->lab = lab;
    /* 对象被别处删掉时把登记项置空（LVGL 原生做法）⇒ `helm_spark_sync()` 按 NULL
     * 跳过，不会去摸野指针。地址必须稳定 ⇒ 登记项是静态数组元素，正好满足。 */
    lv_obj_null_on_delete(&sp->obj);
    lv_obj_null_on_delete(&sp->lab);
    sp->ser = lv_chart_add_series(chart, helm_color_snap(HELM_COLOR_NAV),
                                  LV_CHART_AXIS_PRIMARY_Y);
    sp->id = HELM_HIST_NONE;
    sp->tone = 0;
    lv_obj_add_flag(chart, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(chart, helm_spark_draw_task, LV_EVENT_DRAW_TASK_ADDED, sp);
    lv_obj_add_event_cb(chart, helm_spark_draw_post, LV_EVENT_DRAW_POST, sp);
    lv_obj_move_background(chart);
    lv_obj_move_foreground(lab);
    last = lv_obj_get_child(cell, -1);
    if (last != NULL && last != chart) {
        lv_obj_move_foreground(last);
    }

    helm_spark_reload(sp, helm_hist_id(lv_label_get_text(lab)));
}

/**
 * @brief 同步走势图数据与配色。
 * @param push true 则重采样历史点；false 只刷新当前可见走势。
 * @details 时速 / 心率换档时 `tone` 变化：只改系列色并 invalidate，不重算点。
 */
static void helm_spark_sync(bool push)
{
    uint8_t i;

    for (i = 0; i < s_spark_n; i++) {
        helm_spark_t * sp = &s_spark[i];
        helm_hist_id_t id;
        const char * txt;

        if (sp->obj == NULL || sp->lab == NULL || sp->ser == NULL) {
            continue;
        }

        /* MAP 只刷 rotbar 两根；隐藏数据页走势不要 unhide / 写 30 点。 */
        if (!helm_spark_tree_shown(lv_obj_get_parent(sp->obj))) {
            lv_obj_add_flag(sp->obj, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        txt = lv_label_get_text(sp->lab);
        id = helm_hist_id(txt);
        if (id != sp->id || s_hist_n < 2
            || lv_obj_has_flag(sp->obj, LV_OBJ_FLAG_HIDDEN)) {
            helm_spark_reload(sp, id);
            continue;
        }

        if (id == HELM_HIST_SPEED || id == HELM_HIST_HR) {
            uint32_t stroke = helm_spark_stroke_of(id);

            if (sp->tone != stroke) {
                sp->tone = stroke;
                lv_chart_set_series_color(sp->obj, sp->ser,
                                          helm_color_snap(stroke));
                lv_obj_invalidate(sp->obj);
            }
        }

        if (!push || id == HELM_HIST_NONE) {
            continue;
        }

        helm_spark_reload(sp, id);
    }
}

static void helm_rec_ele_push(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();
    uint32_t slot;
    uint32_t dist;
    int16_t alt;

    if (!helm_riding() || rt == NULL) {
        return;
    }

    if (rt->has_altitude) {
        alt = (int16_t)(rt->altitude_m + (rt->altitude_m >= 0.0f ? 0.5f : -0.5f));
    } else {
        /* DEM 已在 gpx_timer 写入 runtime；这里再扫路网会把 MAP 的 500ms 拍打满。 */
        return;
    }

    dist = (rt->session_distance_m > 0.0)
        ? (uint32_t)(rt->session_distance_m + 0.5) : 0u;
    slot = (uint32_t)(rt->session_elapsed_ms / HELM_CLIMB_REC_PERIOD_MS);
    if (s_rec_ele_n > 0 && slot == s_rec_ele_slot) {
        s_rec_ele[s_rec_ele_n - 1u] = alt;
        s_rec_dist[s_rec_ele_n - 1u] = dist;
        return;
    }

    s_rec_ele_slot = slot;
    if (s_rec_ele_n >= HELM_CLIMB_PROF_N) {
        memmove(&s_rec_ele[0], &s_rec_ele[1],
                (size_t)(HELM_CLIMB_PROF_N - 1u) * sizeof(s_rec_ele[0]));
        memmove(&s_rec_dist[0], &s_rec_dist[1],
                (size_t)(HELM_CLIMB_PROF_N - 1u) * sizeof(s_rec_dist[0]));
        s_rec_ele_n = (uint8_t)(HELM_CLIMB_PROF_N - 1u);
    }

    s_rec_ele[s_rec_ele_n] = alt;
    s_rec_dist[s_rec_ele_n] = dist;
    s_rec_ele_n++;
}

static void helm_label_set(lv_obj_t * lab, const char * s)
{
    const char * cur;

    if (lab == NULL || s == NULL) {
        return;
    }
    cur = lv_label_get_text(lab);
    if (cur != NULL && strcmp(cur, s) == 0) {
        return;
    }
    lv_label_set_text(lab, s);
}

/**
 * @brief 从当前位置起，沿剖面累计剩余爬升（米）。
 * @param now_idx 当前采样点下标。
 * @return 剩余上升米数；剖面不足两点则 -1。
 */
static int32_t helm_climb_remain_from_eles(uint32_t now_idx)
{
    uint32_t i;
    int32_t gain = 0;

    if (s_sh.climb_n < 2u) {
        return -1;
    }
    for (i = now_idx; i + 1u < s_sh.climb_n; i++) {
        const int16_t a = s_climb_eles[i];
        const int16_t b = s_climb_eles[i + 1u];

        if (a == (int16_t)VGRF_ELE_UNKNOWN || b == (int16_t)VGRF_ELE_UNKNOWN) {
            continue;
        }
        if (b > a) {
            gain += (int32_t)b - (int32_t)a;
        }
    }
    return gain;
}

/**
 * @brief 按相邻采样点高差与水平步长，把该段归入平坦 / 中等 / 陡。
 * @param e0 段起点海拔（米）。
 * @param e1 段终点海拔（米）。
 * @param step_cm 该段水平距离（厘米）；剖面按总里程均分。
 * @return 0 平坦，1 中等，2 陡。上坡下坡都用 |高差|，档位直接跳、不插值。
 *
 * @note 只在 `helm_climb_profile_reload()` 换路线时调用，骑行中不重算。
 */
static uint8_t helm_climb_grade_zone(int16_t e0, int16_t e1, uint32_t step_cm)
{
    uint32_t de;
    uint32_t g;

    if (step_cm < 1u) {
        return 0;
    }
    de = (e1 >= e0) ? (uint32_t)((int32_t)e1 - (int32_t)e0)
                    : (uint32_t)((int32_t)e0 - (int32_t)e1);
    /* 坡度% = |Δh|m / 步长m × 100 = de × 10000 / step_cm */
    g = (de * 10000u) / step_cm;
    if (g < HELM_CLIMB_GRADE_FLAT) {
        return 0;
    }
    if (g < HELM_CLIMB_GRADE_STEEP) {
        return 1;
    }
    return 2;
}

/**
 * @brief 坡度档对应的面积填充色（RGB888，绘制时再 `helm_color_snap`）。
 * @param z `helm_climb_grade_zone()` 的返回值。
 * @return `HELM_COLOR_ELEV_*` 填充色。
 */
static uint32_t helm_climb_zone_fill(uint8_t z)
{
    if (z == 1u) {
        return HELM_COLOR_ELEV_MID;
    }
    if (z == 2u) {
        return HELM_COLOR_ELEV_STEEP;
    }
    return HELM_COLOR_ELEV_FLAT;
}

/**
 * @brief 坡度档对应的脊线色（RGB888，绘制时再 `helm_color`）。
 * @param z `helm_climb_grade_zone()` 的返回值。
 * @return `HELM_COLOR_ELEV_*_INK`。
 */
static uint32_t helm_climb_zone_ink(uint8_t z)
{
    if (z == 1u) {
        return HELM_COLOR_ELEV_MID_INK;
    }
    if (z == 2u) {
        return HELM_COLOR_ELEV_STEEP_INK;
    }
    return HELM_COLOR_ELEV_FLAT_INK;
}

/**
 * @brief 判断剖面一段是否与当前脏区相交，避免竖线移动时提交整图三角形。
 * @param clip LVGL 层裁剪区（绝对坐标）；NULL 视为全部可见。
 * @param p1 段起点（屏坐标）。
 * @param p2 段终点（屏坐标）。
 * @param y_base 填充底边的 y。
 * @param pad 线宽余量（像素），至少 2。
 * @return true 需要提交绘制。
 * @note 导航烘焙成功后 DRAW_TASK 已跳过；本函数只服务回退路径和「经过海拔」。
 */
static bool helm_climb_seg_in_clip(const lv_area_t * clip,
    lv_point_precise_t p1, lv_point_precise_t p2, int32_t y_base, int32_t pad)
{
    int32_t x1;
    int32_t x2;
    int32_t y1;

    if (clip == NULL) {
        return true;
    }
    if (pad < 2) {
        pad = 2;
    }
    x1 = (int32_t)(p1.x < p2.x ? p1.x : p2.x) - pad;
    x2 = (int32_t)(p1.x > p2.x ? p1.x : p2.x) + pad;
    y1 = (int32_t)(p1.y < p2.y ? p1.y : p2.y) - pad;
    if (x2 < clip->x1 || x1 > clip->x2) {
        return false;
    }
    if (y_base < clip->y1 || y1 > clip->y2) {
        return false;
    }
    return true;
}

/**
 * @brief 画剖面一段：实心色块（两三角形到基线）加脊线，无纵向渐变。
 * @param layer 目标图层。
 * @param p1 段起点。
 * @param p2 段终点。
 * @param y_base 填充落到图表底边的 y。
 * @param fill 面积色。
 * @param stroke 脊线色。
 * @param lw 脊线宽度；≤0 时按 2 像素。
 */
static void helm_climb_paint_seg(lv_layer_t * layer, lv_point_precise_t p1,
    lv_point_precise_t p2, int32_t y_base, lv_color_t fill, lv_color_t stroke,
    int32_t lw)
{
    lv_draw_triangle_dsc_t tri;
    lv_draw_line_dsc_t ld;

    lv_draw_triangle_dsc_init(&tri);
    tri.color = fill;
    tri.opa = LV_OPA_COVER;
    tri.grad.dir = LV_GRAD_DIR_NONE;
    tri.p[0] = p1;
    tri.p[1] = p2;
    tri.p[2].x = p2.x;
    tri.p[2].y = y_base;
    lv_draw_triangle(layer, &tri);
    tri.p[1].x = p2.x;
    tri.p[1].y = y_base;
    tri.p[2].x = p1.x;
    tri.p[2].y = y_base;
    lv_draw_triangle(layer, &tri);

    lv_draw_line_dsc_init(&ld);
    ld.color = stroke;
    ld.width = lw > 0 ? lw : 2;
    ld.opa = LV_OPA_COVER;
    ld.round_start = 1;
    ld.round_end = 1;
    ld.p1 = p1;
    ld.p2 = p2;
    lv_draw_line(layer, &ld);
}

/** @brief 游标是否已经落过位：只用来判断"上一跳在屏上，可以从当前位置接着滑"。 */
static bool s_cur_valid;

static void helm_climb_cur_x_exec(void * var, int32_t v)
{
    if (helm_anim_is_suspended()) {
        return;
    }

    lv_obj_set_style_translate_x((lv_obj_t *)var, (lv_coord_t)v, 0);
}

static void helm_climb_cur_y_exec(void * var, int32_t v)
{
    if (helm_anim_is_suspended()) {
        return;
    }

    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)v, 0);
}

/** @brief 滑到位后两个方向都归零；suspend 时 exec 停写，这里兜底。 */
static void helm_climb_cur_done(lv_anim_t * a)
{
    lv_obj_t * obj = (a != NULL) ? (lv_obj_t *)a->var : NULL;

    if (obj != NULL) {
        lv_obj_set_style_translate_x(obj, 0, 0);
        lv_obj_set_style_translate_y(obj, 0, 0);
    }
}

/**
 * @brief 隐藏导航剖面画布和竖线（非导航、无数据或回退现场绘制时）。
 */
static void helm_climb_canvas_hide(void)
{
    if (s_sh.climb_canvas) {
        lv_obj_add_flag(s_sh.climb_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_sh.climb_cursor) {
        /* 先停滑动再藏：否则动画会继续在隐藏对象上写 translate。 */
        helm_anim_stop(s_sh.climb_cursor, helm_climb_cur_x_exec);
        helm_anim_stop(s_sh.climb_cursor, helm_climb_cur_y_exec);
        lv_obj_set_style_translate_x(s_sh.climb_cursor, 0, 0);
        lv_obj_set_style_translate_y(s_sh.climb_cursor, 0, 0);
        lv_obj_add_flag(s_sh.climb_cursor, LV_OBJ_FLAG_HIDDEN);
    }
    s_cur_valid = false;
}

/**
 * @brief 把当前位置竖线放到画布对应 x。
 * @details 坐标仍来自隐藏的 `lv_chart`（`get_point_pos_by_id`），不重绘剖面。
 */
static void helm_climb_cursor_place(void)
{
    lv_point_t p;
    int32_t cw;
    int32_t chh;
    int32_t x;
    int32_t y;
    int32_t dx;
    int32_t dy;
    bool was_shown;

    if (s_sh.climb_cursor == NULL || s_sh.climb_chart == NULL ||
        s_sh.climb_ser == NULL || s_sh.climb_n < 2u) {
        return;
    }
    lv_obj_update_layout(s_sh.climb_chart);
    lv_chart_get_point_pos_by_id(s_sh.climb_chart, s_sh.climb_ser,
        (uint32_t)s_sh.climb_now, &p);

    /* 指示器跟着**剖面点**：尖端贴在点**上方 2 px**（整个三角在剖面之上），
     * 水平居中于该点 —— 也就是"下角指向灰绿交接处、高度在折线图上方"。
     *
     * ⚠⚠ 坐标约定（2026-09-25 踩过，别再"修"）：本页剖面有两条绘制路径 ——
     * 现场 `helm_climb_draw_task()` 的填充、导航 `helm_climb_canvas_seg()` 的
     * 烘焙 —— 都把 `lv_chart_get_point_pos_by_id()` 返回的**内容区坐标直接当
     * 画布/盒坐标用**（不补图表自己的 pad）。指示器**必须沿用同一套**，
     * 否则整条差一个 pad（pad_top = 22 px ⇒ 实测三角掉到折线下方、看着"位置全错"）。
     * 真要统一，得两条绘制路径 + 指示器一起改，不能只挪指示器。 */
    cw = lv_obj_get_width(s_sh.climb_chart);
    chh = lv_obj_get_height(s_sh.climb_chart);
    x = p.x - HELM_CLIMB_MARK_W / 2;
    y = p.y - 2 - (HELM_CLIMB_MARK_H - 1);
    if (x > cw - HELM_CLIMB_MARK_W && cw > HELM_CLIMB_MARK_W) {
        x = cw - HELM_CLIMB_MARK_W;
    }
    if (x < 0) {
        x = 0;
    }
    if (y > chh - HELM_CLIMB_MARK_H && chh > HELM_CLIMB_MARK_H) {
        y = chh - HELM_CLIMB_MARK_H;
    }
    if (y < 0) {
        y = 0;
    }

    /* 判断依据是"上次真的在屏上、且这一跳没超出相邻采样点该有的跨度"：
     * 首帧、切页回来、suspend、换路线导致的大跳都直接 snap，别让游标
     * 横穿整张剖面。translate 只是视觉偏移，不参与布局，也不碰 canvas。
     *
     * ⚠ 起点必须取**当前视觉位置**（set_pos + 正在跑的 translate），不能取上一次
     * 目标位置：GNSS 采样可能比 220 ms 补间更快，取目标会在中途重启时把游标
     * 甩到 "2×目标 − 新目标" 的位置上。 */
    was_shown = !lv_obj_has_flag(s_sh.climb_cursor, LV_OBJ_FLAG_HIDDEN);
    if (was_shown && s_cur_valid && !helm_anim_is_suspended()) {
        int32_t cur_x = lv_obj_get_x(s_sh.climb_cursor) +
                        lv_obj_get_style_translate_x(s_sh.climb_cursor, 0);
        int32_t cur_y = lv_obj_get_y(s_sh.climb_cursor) +
                        lv_obj_get_style_translate_y(s_sh.climb_cursor, 0);

        if (LV_ABS(x - cur_x) <= 40 && LV_ABS(y - cur_y) <= 30) {
            /* 对象先落新位，再用 translate 补回旧位置差 ⇒ 视觉上从当前位置滑过去。 */
            dx = cur_x - x;
            dy = cur_y - y;
            lv_obj_set_pos(s_sh.climb_cursor, x, y);
            lv_obj_set_style_translate_x(s_sh.climb_cursor, dx, 0);
            lv_obj_set_style_translate_y(s_sh.climb_cursor, dy, 0);
            helm_anim_run(s_sh.climb_cursor, helm_climb_cur_x_exec, dx, 0, 220,
                          lv_anim_path_ease_out, helm_climb_cur_done);
            helm_anim_run(s_sh.climb_cursor, helm_climb_cur_y_exec, dy, 0, 220,
                          lv_anim_path_ease_out, helm_climb_cur_done);
            goto placed;
        }
    }

    helm_anim_stop(s_sh.climb_cursor, helm_climb_cur_x_exec);
    helm_anim_stop(s_sh.climb_cursor, helm_climb_cur_y_exec);
    lv_obj_set_pos(s_sh.climb_cursor, x, y);
    lv_obj_set_style_translate_x(s_sh.climb_cursor, 0, 0);
    lv_obj_set_style_translate_y(s_sh.climb_cursor, 0, 0);

placed:
    s_cur_valid = true;
    lv_obj_clear_flag(s_sh.climb_cursor, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 按图表实际宽高分配/复用 RGB565 画布缓冲。
 * @return 画布可用则为 true。
 * @details 缓冲在 PSRAM。宽高未变则复用。分配失败返回 false，
 *          调用方回退 `helm_climb_draw_task` 现场绘制。
 */
static bool helm_climb_canvas_ensure(void)
{
    int32_t w;
    int32_t h;
    size_t bytes;

    if (s_sh.climb_canvas == NULL || s_sh.climb_chart == NULL) {
        return false;
    }
    if (s_sh.climb) {
        lv_obj_update_layout(s_sh.climb);
    }
    lv_obj_update_layout(s_sh.climb_chart);
    w = lv_obj_get_width(s_sh.climb_chart);
    h = lv_obj_get_height(s_sh.climb_chart);
    if (w < 8 || h < 8) {
        return false;
    }
    if (s_climb_cbuf != NULL && w == s_climb_cw && h == s_climb_ch) {
        /*
         * ⚠ 尺寸没变也**必须重新挂一次缓冲区**：画布对象可能刚被重建过（换主题 =
         * `helm_shell_rebuild()` 删掉整棵 shell 再建），而 `s_climb_cbuf` 是我们自己的
         * 内存、不属于 LVGL、重建不会动它 ⇒ 尺寸也恰好没变（同一个布局）⇒ 原来这里直接
         * return，**新画布对象上从来没 set 过 buffer** ⇒ `lv_canvas_fill_bg()`/图层绘制
         * 全落在空处，屏上只剩纸色底（用户 2026-09-27 深挖到的真因：数据/几何/可见性/烘焙
         * 日志全都"正常"，就是没挂 buffer）。set_buffer 是幂等的，重复调用无副作用。
         */
        lv_canvas_set_buffer(s_sh.climb_canvas, s_climb_cbuf, w, h,
            LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(s_sh.climb_canvas, w, h);
        syslog(LOG_NOTICE, "[climb] canvas buffer re-attached (reuse %dx%d)\n", (int)w, (int)h);
        return true;
    }
    bytes = (size_t)lv_draw_buf_width_to_stride((uint32_t)w,
        LV_COLOR_FORMAT_RGB565) * (size_t)h;
    vmap_free(s_climb_cbuf);
    s_climb_cbuf = vmap_malloc(bytes);
    if (s_climb_cbuf == NULL) {
        s_climb_cw = 0;
        s_climb_ch = 0;
        return false;
    }
    s_climb_cw = w;
    s_climb_ch = h;
    lv_canvas_set_buffer(s_sh.climb_canvas, s_climb_cbuf, w, h,
        LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(s_sh.climb_canvas, w, h);
    return true;
}

/**
 * @brief 把剖面一段画进画布图层。
 * @param layer 画布图层。
 * @param i 段下标（点 i→i+1）。
 * @param as_done true 则强制灰色（刚骑过的那截）。
 */
static void helm_climb_canvas_seg(lv_layer_t * layer, uint32_t i, bool as_done)
{
    lv_point_t a;
    lv_point_t b;
    lv_point_precise_t p1;
    lv_point_precise_t p2;
    lv_color_t fill;
    lv_color_t ink;
    uint8_t z;

    if (layer == NULL || s_sh.climb_chart == NULL || s_sh.climb_ser == NULL ||
        i + 1u >= s_sh.climb_n) {
        return;
    }
    lv_chart_get_point_pos_by_id(s_sh.climb_chart, s_sh.climb_ser, i, &a);
    lv_chart_get_point_pos_by_id(s_sh.climb_chart, s_sh.climb_ser, i + 1u, &b);
    p1.x = a.x;
    p1.y = a.y;
    p2.x = b.x;
    p2.y = b.y;
    if (as_done || i < (uint32_t)s_sh.climb_now) {
        fill = helm_color_snap(HELM_COLOR_ELEV_DONE);
        ink = helm_color(HELM_COLOR_ELEV_DONE_INK);
    } else {
        z = s_climb_zone[i];
        fill = helm_color_snap(helm_climb_zone_fill(z));
        ink = helm_color(helm_climb_zone_ink(z));
    }
    helm_climb_paint_seg(layer, p1, p2, s_climb_ch - 1, fill, ink, 2);
}

/**
 * @brief 标脏画布上 [i0, i1] 对应的水平窄条。
 * @param i0 起始采样点下标。
 * @param i1 结束采样点下标。
 * @details 关闭 `lv_canvas_finish_layer` 的整对象 invalidate 后，用本函数只刷刚改的那一截。
 */
static void helm_climb_canvas_inval_span(uint32_t i0, uint32_t i1)
{
    lv_point_t a;
    lv_point_t b;
    lv_area_t area;
    int32_t orig;
    int32_t lo;
    int32_t hi;
    uint32_t last;

    if (s_sh.climb_canvas == NULL || s_sh.climb_n < 2u) {
        return;
    }
    last = (uint32_t)s_sh.climb_n - 1u;
    if (i0 > last) {
        i0 = last;
    }
    if (i1 > last) {
        i1 = last;
    }
    lv_obj_get_coords(s_sh.climb_canvas, &area);
    orig = area.x1;
    lv_chart_get_point_pos_by_id(s_sh.climb_chart, s_sh.climb_ser, i0, &a);
    lv_chart_get_point_pos_by_id(s_sh.climb_chart, s_sh.climb_ser, i1, &b);
    lo = a.x < b.x ? a.x : b.x;
    hi = a.x < b.x ? b.x : a.x;
    area.x1 = orig + lo - 2;
    area.x2 = orig + hi + 3;
    lv_obj_invalidate_area(s_sh.climb_canvas, &area);
}

/**
 * @brief 把 [i0, i1) 段画进已有画布（竖线前进时只涂灰，不整图重绘）。
 * @param i0 起始段。
 * @param i1 结束段（不含）。
 * @param as_done 强制灰色。
 * @details `lv_canvas_finish_layer` 前后关掉 display invalidation，再按窄条标脏，
 *          避免整张画布进入切页脏区。
 */
static void helm_climb_canvas_paint_range(uint32_t i0, uint32_t i1, bool as_done)
{
    lv_layer_t layer;
    lv_display_t * disp;
    uint32_t i;

    if (s_sh.climb_canvas == NULL || s_sh.climb_n < 2u || i0 >= i1) {
        return;
    }
    if (i1 > (uint32_t)s_sh.climb_n - 1u) {
        i1 = (uint32_t)s_sh.climb_n - 1u;
    }
    lv_canvas_init_layer(s_sh.climb_canvas, &layer);
    for (i = i0; i < i1; i++) {
        helm_climb_canvas_seg(&layer, i, as_done);
    }
    disp = lv_obj_get_display(s_sh.climb_canvas);
    if (disp) {
        lv_display_enable_invalidation(disp, false);
    }
    lv_canvas_finish_layer(s_sh.climb_canvas, &layer);
    if (disp) {
        lv_display_enable_invalidation(disp, true);
    }
    helm_climb_canvas_inval_span(i0, i1);
}

/**
 * @brief 把当前路线海拔剖面烘焙到画布（只在换路线时调用）。
 * @return 成功则图表隐藏，切页只贴这一张图。
 * @details 切页 220 ms crossfade 若走 DRAW_TASK，每帧会提交约 59 段三角形，
 *          `lv_timer_handler` 可达 150–270 ms。烘焙后只 blit。失败返回 false。
 */
static bool helm_climb_canvas_bake(void)
{
    lv_layer_t layer;
    uint32_t i;

    if (!s_sh.climb_nav || s_sh.climb_n < 2u || !helm_climb_canvas_ensure()) {
        return false;
    }
    lv_canvas_fill_bg(s_sh.climb_canvas, helm_color(HELM_COLOR_PAPER),
        LV_OPA_COVER);
    lv_canvas_init_layer(s_sh.climb_canvas, &layer);
    for (i = 0; i + 1u < s_sh.climb_n; i++) {
        helm_climb_canvas_seg(&layer, i, false);
    }
    lv_canvas_finish_layer(s_sh.climb_canvas, &layer);
    lv_obj_add_flag(s_sh.climb_chart, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_sh.climb_canvas, LV_OBJ_FLAG_HIDDEN);
    helm_climb_cursor_place();

    /*
     * 烘焙收尾（2026-09-27）：
     *   ① `lv_obj_update_layout()`：重建后布局可能还没跑过（几何为 0 ⇒ invalidate 的是空区域）；
     *   ② 显式 `lv_obj_invalidate()`：`lv_canvas_finish_layer()` 只标画布自身的脏区，而换主题
     *      这条路上画布是刚重建的新对象 ⇒ 补一次，保证新配色真的上屏。
     * 保留一行低频日志（一次烘焙一行，路线变化才触发）：换主题后再遇到"图是空的"，
     * 这一行就能分清"没数据（n/ele 为 0）"还是"有数据但没上屏"。
     */
    lv_obj_update_layout(s_sh.climb_canvas);
    lv_obj_invalidate(s_sh.climb_canvas);
    lv_obj_invalidate(s_sh.climb_chart);
    syslog(LOG_NOTICE, "[climb] bake nav=%d n=%d ele=%d..%d\n",
           (int)s_sh.climb_nav, (int)s_sh.climb_n,
           (int)(s_sh.climb_n >= 2u ? (s_climb_eles[0] < s_climb_eles[s_sh.climb_n - 1u]
                                      ? s_climb_eles[0] : s_climb_eles[s_sh.climb_n - 1u]) : 0),
           (int)(s_sh.climb_n >= 2u ? (s_climb_eles[0] > s_climb_eles[s_sh.climb_n - 1u]
                                      ? s_climb_eles[0] : s_climb_eles[s_sh.climb_n - 1u]) : 0));

    return true;
}

/**
 * @brief 只标脏当前位置竖线附近一条窄带（非导航「经过海拔」用）。
 * @param x 点在图表内的相对 x。
 */
/**
 * @brief 标脏当前位置指示器（小倒三角）所在的小方块。
 * @param x 图表内相对 x（三角中心）。
 * @param y 图表内相对 y（尖端）。
 * @details 2026-09-25：指示器由"2 px 通高竖线"改成小倒三角后，标脏也从
 *          "整列高度 × 7 px 宽"缩成三角的包围盒 + 1 px 余量 —— 导航烘焙模式下
 *          这一条要重绘画布，小方块明显更省。
 */
static void helm_climb_inval_cursor_at(int32_t x, int32_t y)
{
    lv_area_t a;
    const int32_t half = HELM_CLIMB_MARK_W / 2 + 1;

    if (s_sh.climb_chart == NULL) {
        return;
    }
    lv_obj_get_coords(s_sh.climb_chart, &a);
    a.x1 += x - half;
    a.x2 = a.x1 + 2 * half;
    a.y1 += y - (HELM_CLIMB_MARK_H + 1);
    a.y2 = a.y1 + (HELM_CLIMB_MARK_H + 2);
    lv_obj_invalidate_area(s_sh.climb_chart, &a);
}

/**
 * @brief 标脏 [xa, xb] 整列高度，让刚骑过的那截从彩块改成灰。
 * @param xa 图表内相对 x。
 * @param xb 图表内相对 x。
 *
 * @note 导航剖面专用。竖线未动时不要调用。
 */
static void helm_climb_inval_span_x(int32_t xa, int32_t xb)
{
    lv_area_t a;
    int32_t orig;
    int32_t lo;
    int32_t hi;

    if (s_sh.climb_chart == NULL) {
        return;
    }
    lv_obj_get_coords(s_sh.climb_chart, &a);
    orig = a.x1;
    lo = xa < xb ? xa : xb;
    hi = xa < xb ? xb : xa;
    a.x1 = orig + lo - 2;
    a.x2 = orig + hi + 3;
    lv_obj_invalidate_area(s_sh.climb_chart, &a);
}

/**
 * @brief 当前位置下标变化时只移动竖线（及导航时那一窄条灰色），不 refresh 整张图。
 * @param id 新的采样点下标。
 * @details 画布已显示：前进则把 [prev, id) 涂灰并挪竖线；后退则整图重烘焙。
 *          未烘焙：只标脏窄条。画布可见时即使图表已隐藏也要处理。
 */
static void helm_climb_cursor_move(uint32_t id)
{
    lv_point_t p;
    lv_point_t prev_p;
    uint32_t n;
    uint8_t prev;
    bool baked;

    if (s_sh.climb_ser == NULL || s_sh.climb_n < 2u) {
        return;
    }
    baked = (s_sh.climb_canvas != NULL) &&
            !lv_obj_has_flag(s_sh.climb_canvas, LV_OBJ_FLAG_HIDDEN);
    if (!baked && (s_sh.climb_chart == NULL ||
            lv_obj_has_flag(s_sh.climb_chart, LV_OBJ_FLAG_HIDDEN))) {
        return;
    }

    n = baked ? (uint32_t)s_sh.climb_n : lv_chart_get_point_count(s_sh.climb_chart);
    if (n < 2u) {
        return;
    }
    if (id >= n) {
        id = n - 1u;
    }
    if (s_sh.climb_now == (uint8_t)id) {
        return;
    }

    prev = s_sh.climb_now;
    s_sh.climb_now = (uint8_t)id;
    if (baked) {
        if ((uint32_t)id > (uint32_t)prev) {
            helm_climb_canvas_paint_range((uint32_t)prev, (uint32_t)id, true);
            helm_climb_cursor_place();
        } else {
            (void)helm_climb_canvas_bake();
        }
        return;
    }

    lv_obj_update_layout(s_sh.climb_chart);
    lv_chart_get_point_pos_by_id(s_sh.climb_chart, s_sh.climb_ser, prev, &prev_p);
    lv_chart_get_point_pos_by_id(s_sh.climb_chart, s_sh.climb_ser, id, &p);
    if (s_sh.climb_nav) {
        helm_climb_inval_span_x(prev_p.x, p.x);
    } else {
        /* 指示器跟着剖面点 ⇒ 新旧位置各标脏一次；坐标与绘制同约定（不补 pad）。 */
        helm_climb_inval_cursor_at(prev_p.x, prev_p.y - 2);
        helm_climb_inval_cursor_at(p.x, p.y - 2);
    }
}

/**
 * @brief 按剖面海拔范围加边距，得到图表 Y 轴。
 * @param eles 海拔采样（米）。
 * @param n 采样点数。
 * @param[out] mn 轴下限。
 * @param[out] mx 轴上限。
 */
static void helm_climb_axis(const int16_t * eles, uint32_t n,
    int32_t * mn, int32_t * mx)
{
    uint32_t i;
    int32_t lo = 32767;
    int32_t hi = -32767;
    int32_t pad;

    for (i = 0; i < n; i++) {
        if (eles[i] == (int16_t)VGRF_ELE_UNKNOWN) {
            continue;
        }
        if ((int32_t)eles[i] < lo) {
            lo = eles[i];
        }
        if ((int32_t)eles[i] > hi) {
            hi = eles[i];
        }
    }
    if (lo > hi) {
        lo = 0;
        hi = 100;
    }
    pad = (hi - lo) / 10;
    if (pad < 8) {
        pad = 8;
    }
    *mn = lo - pad;
    *mx = hi + pad;
}

/**
 * @brief 路线或近 30 分钟记录变化时重载海拔剖面。
 * @details 导航：按总里程均匀取样，并一次性写入 `s_climb_zone`（骑行中不重算坡度）。
 *          烘焙成功则隐藏图表、不再 `lv_chart_refresh`。
 *          未导航或烘焙失败：显示图表并 refresh 一次（「经过海拔」仍用青绿渐变）。
 */
static void helm_climb_profile_reload(void)
{
    int16_t eles[HELM_CLIMB_PROF_N];
    uint32_t n = 0;
    uint32_t now_idx = LV_CHART_POINT_NONE;
    uint32_t i;
    int32_t mn;
    int32_t mx;
    bool nav;
    int16_t last = 0;
    bool have = false;
    uint32_t fp_pts = 0;
    uint32_t fp_total_cm = 0;

    if (s_sh.climb_chart == NULL || s_sh.climb_ser == NULL) {
        return;
    }

    nav = helm_nav_on();
#if VMAP_ROUTE_ENABLE
    if (nav) {
        uint32_t cur = 0;

        n = map_page_nav_profile_ele(s_sh.map, eles, HELM_CLIMB_PROF_N, &cur);
        (void)map_page_nav_progress(s_sh.map, &fp_pts, &fp_total_cm, NULL, n);
        if (n >= 2u) {
            now_idx = cur;
        }
    } else
#endif
    if (helm_riding() && s_rec_ele_n >= 2u) {
        n = s_rec_ele_n;
        memcpy(eles, s_rec_ele, (size_t)n * sizeof(eles[0]));
        fp_pts = n;
        fp_total_cm = n;
    }

    if (s_sh.climb_title) {
        /* 非导航时这条剖面来自本机记录采样；还没采到点就明说「记录中」，
         * 而不是让人对着一张空图猜（用户 2026-09-26）。 */
        if (!nav && s_rec_ele_n < 2u) {
            helm_label_set(s_sh.climb_title, "记录中");
        } else {
            helm_label_set(s_sh.climb_title, nav ? "路线海拔" : "经过海拔");
        }
    }

    s_climb_fp_pts = fp_pts;
    s_climb_fp_total_cm = fp_total_cm;
    s_climb_fp_rec_n = nav ? 0 : (uint8_t)s_rec_ele_n;
    s_climb_fp_rec_slot = nav ? 0xffffffffu : s_rec_ele_slot;
    s_sh.climb_nav = nav;

    if (n < 2u) {
        s_sh.climb_n = 0;
        s_sh.climb_now = 0;
        helm_climb_canvas_hide();
        lv_obj_add_flag(s_sh.climb_chart, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    for (i = 0; i < n; i++) {
        if (eles[i] != (int16_t)VGRF_ELE_UNKNOWN) {
            last = eles[i];
            have = true;
            break;
        }
    }
    if (!have) {
        s_sh.climb_n = 0;
        helm_climb_canvas_hide();
        lv_obj_add_flag(s_sh.climb_chart, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    for (i = 0; i < n; i++) {
        int16_t e = eles[i];

        if (e == (int16_t)VGRF_ELE_UNKNOWN) {
            e = last;
        } else {
            last = e;
        }
        s_climb_eles[i] = e;
    }
    /* 分区（绿/黄/红）**两种模式都要算**：原来只在导航分支里算，记录模式下
     * `s_climb_zone` 一直是 0 ⇒ 表盘页底部那条剖面永远是灰的（用户 2026-11-01：
     * "有海拔数据，显示为什么是灰色"）。记录模式的水平步长用会话里程均分；
     * 里程还太小（<1 m/点）时坡度不可信，保持灰，不伪造。 */
    {
        uint32_t step_cm = 0u;

        if (nav) {
            if (fp_total_cm > 0u && n >= 2u) {
                step_cm = fp_total_cm / (uint32_t)(n - 1u);
            }
            if (step_cm < 1u) {
                step_cm = 1u;
            }
        } else {
            const bicycle_runtime_t * rt = bicycle_runtime_get();

            if (rt != NULL && rt->session_distance_m > 1.0 && n >= 2u) {
                step_cm = (uint32_t)(rt->session_distance_m * 100.0 /
                                     (double)(n - 1u));
            }
            if (step_cm < 100u) {
                step_cm = 0u;
            }
        }

        for (i = 0; i + 1u < n; i++) {
            s_climb_zone[i] = (step_cm != 0u)
                ? helm_climb_grade_zone(s_climb_eles[i],
                                        s_climb_eles[i + 1u], step_cm)
                : 0u;
        }
        for (; i < HELM_CLIMB_PROF_N; i++) {
            s_climb_zone[i] = 0u;
        }
    }

    helm_climb_axis(s_climb_eles, n, &mn, &mx);
    if (lv_chart_get_point_count(s_sh.climb_chart) != n) {
        lv_chart_set_point_count(s_sh.climb_chart, n);
    }
    lv_chart_set_axis_range(s_sh.climb_chart, LV_CHART_AXIS_PRIMARY_Y, mn, mx);
    {
        lv_color_t col = helm_color_snap(HELM_COLOR_CLIMB);

        if (!lv_color_eq(lv_chart_get_series_color(s_sh.climb_chart,
                s_sh.climb_ser), col)) {
            lv_chart_set_series_color(s_sh.climb_chart, s_sh.climb_ser, col);
        }
    }
    {
        int32_t * y = lv_chart_get_series_y_array(s_sh.climb_chart,
            s_sh.climb_ser);

        if (y != NULL) {
            for (i = 0; i < n; i++) {
                y[i] = (int32_t)s_climb_eles[i];
            }
        }
    }
    lv_chart_set_x_start_point(s_sh.climb_chart, s_sh.climb_ser, 0);
    s_sh.climb_n = (uint8_t)n;
    s_sh.climb_now = (uint8_t)((nav && now_idx < n) ? now_idx : (n - 1u));
    if (nav && helm_climb_canvas_bake()) {
        return;
    }
    helm_climb_canvas_hide();
    lv_obj_remove_flag(s_sh.climb_chart, LV_OBJ_FLAG_HIDDEN);
    lv_chart_refresh(s_sh.climb_chart);
}

/**
 * @brief 爬升页周期刷新：路线指纹未变则只挪竖线，否则重载剖面。
 * @param rt 当前骑行快照，可 NULL。
 * @param page_i 当前页序号（无剩余爬升时显示「页码」）。
 * @param page_n 总页数。
 * @details 500 ms UI 节拍。指纹（点数 / 总里程）不变时不重算 `s_climb_zone`、不烘焙。
 */
static void helm_climb_tick(const bicycle_runtime_t * rt, uint8_t page_i,
    uint8_t page_n)
{
    char buf[32];
    uint32_t now_idx = 0;
    int32_t remain_gain = -1;
    bool reload = true;
    int i;

    if (s_sh.climb_chart == NULL) {
        return;
    }

#if VMAP_ROUTE_ENABLE
    if (helm_nav_on()) {
        uint32_t pts = 0;
        uint32_t total_cm = 0;
        uint32_t prof_n = s_sh.climb_n >= 2u ? s_sh.climb_n : HELM_CLIMB_PROF_N;

        if (map_page_nav_progress(s_sh.map, &pts, &total_cm, &now_idx, prof_n)
            && pts == s_climb_fp_pts && total_cm == s_climb_fp_total_cm
            && s_sh.climb_nav) {
            reload = false;
        }
    } else
#endif
    if (!s_sh.climb_nav && s_rec_ele_n == s_climb_fp_rec_n
        && s_rec_ele_slot == s_climb_fp_rec_slot) {
        reload = false;
        now_idx = s_sh.climb_n >= 2u ? (uint32_t)s_sh.climb_n - 1u : 0u;
    }

    if (s_climb_rebake) {
        s_climb_rebake = false;
        /* 用既有数据重烘（没导航剖面时 bake 自己返回假）；真烘到了才不用再去取一遍数。 */
        if (helm_climb_canvas_bake()) {
            reload = false;
        }
    }

    if (reload) {
        helm_climb_profile_reload();
        now_idx = s_sh.climb_now;
    } else {
        helm_climb_cursor_move(now_idx);
    }

#if VMAP_ROUTE_ENABLE
    if (s_sh.climb_nav && s_sh.climb_n >= 2u) {
        remain_gain = helm_climb_remain_from_eles(s_sh.climb_now);
    }
#endif
    /* 海拔页六格：海拔 / 坡度 / 爬升 / 下降 / 最高 / 最低。
     * 刻度与文案都照用户 2026-09-26 给的图；单位放进标签里
     * （"1284 m" 在 num_28 下约 101px，格内宽只有 ~91px，放不下）。 */
    {
        const char * klab[HELM_CLIMB_N] = {
            "海拔 (m)", "坡度 (%)", "爬升 (m)", "下降 (m)", "最高 (m)",
            "最低 (m)"
        };
        char kval[HELM_CLIMB_N][16];
        int alt = 0;
        bool have = false;

        if (rt && rt->has_altitude) {
            alt = (int)(rt->altitude_m + 0.5f);
            have = true;
        }
        if (!have && s_sh.climb_n >= 2u) {
            const int16_t e = s_climb_eles[s_sh.climb_now];

            if (e != (int16_t)VGRF_ELE_UNKNOWN) {
                alt = (int)e;
                have = true;
            }
        }

        if (have) {
            lv_snprintf(kval[0], 16, "%d", alt);
        } else {
            lv_snprintf(kval[0], 16, "--");
        }

        /* 坡度：num_* 字库里没有 '-'，所以这里给**绝对值**（方向由爬升/下降
         * 两格和剖面本身表达）。 */
        {
            int g10 = rt ? (int)(rt->grade_pct * 10.0f) : 0;

            lv_snprintf(kval[1], 16, "%d.%d", g10 / 10,
                        (g10 < 0 ? -g10 : g10) % 10);
        }

        lv_snprintf(kval[2], 16, "%d", rt ? (int)(rt->gain_m + 0.5f) : 0);
        lv_snprintf(kval[3], 16, "%d", rt ? (int)(rt->loss_m + 0.5f) : 0);
        /* 最高/最低：**导航时给这条路线剖面的极值**（用户 2026-09-26：一条
         * 起伏 11 m 的路线整条全绿、看不出"这线到底有多平"）。剖面数据就是
         * s_climb_eles（已把 UNKNOWN 用前值填过），扫一遍即可。
         * 非导航时仍是会话极值；没记录过给 "--"（0 m 是合法海拔，不能当没数据）。 */
        {
            bool have_mm = false;
            int16_t emin = 0;
            int16_t emax = 0;

            if (s_sh.climb_nav && s_sh.climb_n >= 2u) {
                for (i = 0; i < (int)s_sh.climb_n; i++) {
                    int16_t e = s_climb_eles[i];

                    if (e == (int16_t)VGRF_ELE_UNKNOWN) {
                        continue;
                    }
                    if (!have_mm) {
                        have_mm = true;
                        emin = e;
                        emax = e;
                    } else if (e < emin) {
                        emin = e;
                    } else if (e > emax) {
                        emax = e;
                    }
                }
            } else if (rt && rt->alt_range_valid) {
                have_mm = true;
                emin = (int16_t)(rt->altitude_min_m + 0.5f);
                emax = (int16_t)(rt->altitude_max_m + 0.5f);
            }

            if (have_mm) {
                lv_snprintf(kval[4], 16, "%d", (int)emax);
                lv_snprintf(kval[5], 16, "%d", (int)emin);
            } else {
                lv_snprintf(kval[4], 16, "--");
                lv_snprintf(kval[5], 16, "--");
            }
        }

        for (i = 0; i < HELM_CLIMB_N; i++) {
            if (s_sh.climb_val[i] == NULL) {
                continue;
            }
            helm_label_set(s_sh.climb_val[i], kval[i]);
            helm_set_text_hex(s_sh.climb_val[i], HELM_COLOR_INK);
            if (s_sh.climb_rail[i] != NULL) {
                lv_obj_set_style_bg_color(s_sh.climb_rail[i],
                    helm_color(helm_tone(klab[i])), 0);
            }
        }
        LV_UNUSED(klab);
    }

    if (s_sh.climb_mark) {
        /* 剖面卡右上角："剩余爬升 280 m"（没有航线海拔时整条不显示，
         * 不退回页码 —— 用户 2026-09-26）。 */
        if (remain_gain >= 0) {
            lv_snprintf(buf, sizeof(buf), "剩余爬升 %ldm", (long)remain_gain);
            helm_label_set(s_sh.climb_mark, buf);
        } else {
            helm_label_set(s_sh.climb_mark, "");
        }
    }
}

static bool helm_nav_on(void)
{
    return map_page_nav_active(s_sh.map);
}

static bool helm_riding(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();

    return (rt && rt->recording) || s_sh.paused;
}

static void helm_ride_dwell_reset(void)
{
    s_still_since = 0;
    s_move_since = 0;
}

static void helm_autopause_hold_clear(void)
{
    s_autopause_hold_since = 0;
}

static bool helm_autopause_held(void)
{
    if (s_autopause_hold_since == 0u) {
        return false;
    }

    if (lv_tick_elaps(s_autopause_hold_since) >= HELM_AUTOPAUSE_HOLD_MS) {
        s_autopause_hold_since = 0;
        return false;
    }

    return true;
}

static void helm_pause_title_sync(void)
{
    if (s_sh.pause_title == NULL) {
        return;
    }

    lv_label_set_text(s_sh.pause_title,
        s_sh.pause_auto ? "自动暂停" : "手动暂停");
}

static void helm_pause_dock_apply(void)
{
    bool pause_was = helm_obj_shown(s_sh.pause_dock);

    if (s_sh.ui_covered) {
        return;
    }

    if (s_sh.paused) {
        helm_pause_title_sync();
        helm_show(s_sh.pause_dock, true);
        helm_raise_if_shown(s_sh.pause_dock);
        if (!pause_was) {
            helm_obj_stagger_in(s_sh.pause_dock);
        }
    } else {
        helm_obj_stagger_clear(s_sh.pause_dock);
        helm_show(s_sh.pause_dock, false);
    }
    helm_raise_chrome();
}

static void helm_pause_resume_by_key(void)
{
    bool was_auto = s_sh.pause_auto;

    map_page_resume_ride(s_sh.map);
    if (was_auto) {
        s_autopause_hold_since = lv_tick_get();
        if (s_autopause_hold_since == 0u) {
            s_autopause_hold_since = 1u;
        }
    }
    s_sh.pause_auto = false;
    helm_shell_set_paused(false);
    helm_ride_dwell_reset();
    myvendor_sound_ride();
}

static void helm_ride_status_publish(void)
{
    bool session = helm_riding();
    bool moving = session && !s_sh.paused && bicycle_runtime_is_moving(false);

    companion_bridge_ride_set(session, moving);
}

static void helm_autopause_tick(void)
{
    uint32_t now;
    const bicycle_runtime_t * rt;
    bool recording;
    bool moving;

    if (!helm_riding()) {
        helm_ride_dwell_reset();
        return;
    }

    /* 保存框仍在主界面上，不在背后偷偷恢复。菜单盖住时继续判运动。 */
    if (helm_save_open()) {
        return;
    }

    if (s_sh.paused && !s_sh.pause_auto) {
        helm_ride_dwell_reset();
        return;
    }

    if (!myvendor_devctl_autopause_get()) {
        helm_ride_dwell_reset();
        return;
    }

    now = lv_tick_get();
    rt = bicycle_runtime_get();
    recording = rt && rt->recording;
    moving = s_sh.paused ? bicycle_runtime_is_moving(true)
                         : bicycle_runtime_is_moving(false);

    if (moving) {
        s_still_since = 0;
        if (s_move_since == 0) {
            s_move_since = now;
        }

        if (s_sh.paused && s_sh.pause_auto &&
            lv_tick_elaps(s_move_since) >= HELM_AUTORESUME_MS) {
            map_page_resume_ride(s_sh.map);
            s_sh.pause_auto = false;
            helm_shell_set_paused(false);
            s_autopause_hold_since = now;
            if (s_autopause_hold_since == 0u) {
                s_autopause_hold_since = 1u;
            }
            helm_ride_dwell_reset();
        }
    } else {
        s_move_since = 0;
        if (helm_autopause_held()) {
            s_still_since = 0;
        } else if (s_still_since == 0) {
            s_still_since = now;
        }

        /* 模拟轨迹回放期间**不要自动暂停**：回放的 GPS 数据不是真实运动
         * （速度/是否移动由 runtime 的判定决定），一暂停就把"导航测试"打断，
         * 屏幕上弹"自动暂停"。测试链路要的是纯导航，所以这里直接屏蔽。 */
        if (recording && !bicycle_gpx_sim_active() && !s_sh.paused &&
            s_still_since != 0 &&
            lv_tick_elaps(s_still_since) >= HELM_AUTOPAUSE_MS) {
            map_page_pause_ride(s_sh.map);
            s_sh.pause_auto = true;
            helm_shell_set_paused(true);
            helm_ride_dwell_reset();
        }
    }
}

/** @brief 录制、暂停或导航中：用骑行翻页（表盘/仪表台/全数字/地图）。 */
static bool helm_in_session(void)
{
    return helm_riding() || helm_nav_on();
}

static uint8_t * helm_active_idx(void)
{
    return helm_in_session() ? &s_sh.ride_idx : &s_sh.home_idx;
}

/**
 * @brief 页面总数（2026-11-01：骑行 4 页 = 表盘 / 老版仪表台 / 全数字 / 地图）。
 * @details 待机 3 页：主页面 / 表盘页 / 数据页。
 *          骑行 4 页：表盘页 / 仪表台页 / 数据页 / 地图页。
 * @note **爬坡页（HELM_PAGE_CLIMB）已从轮转里删除**：用户 2026-11-01 定
 *       "现在的 P02（骑行表盘页）就是新的爬坡页" —— 爬坡/剖面信息已经在
 *       表盘页底部的海拔剖面里，不再单独占一页。页面对象仍保留（demo/调试
 *       页名 `climb` 可用），只是不进轮转。
 * @note **老版 X1 仪表台（HELM_PAGE_DASH）回到轮转**：用户 2026-11-01
 *       "现在我需要新布局有这个" —— 老 DATA 页（左大时速 + 右四行走势）
 *       作为独立页保留，排在表盘页之后。
 * @note **待机没有单独的"地图页"**：主页面（STANDBY）本来就是地图 + 信息卡，
 *       `map_on` 里 STANDBY 与 MAP 都让地图显示。
 * @note **导航也不再插转向页**：转向信息走地图上的通栏横幅/路牌。
 */
static uint8_t helm_page_count(void)
{
    /* 必须与 helm_page_at() 的数组长度严格一致（idx 取模用）。 */
    if (!helm_in_session()) {
#if HELM_ENABLE_PAGE_DATA && HELM_ENABLE_CLIMB_PAGE
        return 4u;   /* standby / data / data_all / climb */
#elif HELM_ENABLE_PAGE_DATA
        return 3u;   /* standby / data / data_all */
#elif HELM_ENABLE_CLIMB_PAGE
        return 3u;   /* standby / data_all / climb */
#else
        return 2u;   /* standby / data_all */
#endif
    }

#if HELM_ENABLE_PAGE_DATA && HELM_ENABLE_CLIMB_PAGE
    return 5u;       /* data / dash / data_all / climb / map */
#elif HELM_ENABLE_PAGE_DATA
    return 4u;       /* data / dash / data_all / map */
#elif HELM_ENABLE_CLIMB_PAGE
    return 4u;       /* dash / data_all / climb / map */
#else
    return 3u;       /* dash / data_all / map */
#endif
}

static helm_page_id_t helm_page_at(uint8_t idx)
{
    /* 演示页面锁：忽略正常轮转表，恒返回强制页。 */
    if (s_demo_page != (helm_page_id_t)0xff) {
#if !HELM_ENABLE_PAGE_DATA
        /* P02 关闭期间，把"强制 P02"改派到仪表台 —— 否则演示的 `demo run ride`
         * 会绕过轮转表把 P02 又拉回来，A/B 就不成立。 */
        if (s_demo_page == HELM_PAGE_DATA) {
            return HELM_PAGE_DASH;
        }
#endif
        return s_demo_page;
    }

    if (!helm_in_session()) {
        static const helm_page_id_t idle[] = {
            HELM_PAGE_STANDBY,
#if HELM_ENABLE_PAGE_DATA
            HELM_PAGE_DATA,
#endif
            HELM_PAGE_DATA_ALL,
#if HELM_ENABLE_CLIMB_PAGE
            HELM_PAGE_CLIMB,
#endif
        };
        return idle[idx % (uint8_t)(sizeof(idle) / sizeof(idle[0]))];
    }

    {
        /* 骑行页：表盘（新 P02，含底部剖面）/ 老版 X1 仪表台（O02）/ 全数字 /
         * 海拔剖面 / 地图。爬坡页 2026-11-01 曾从轮转里删除，2026-11-04 用户
         * 要求"把新的海拔界面打开"，重新排进轮转（紧跟全数字页，地图仍在最后）。 */
        static const helm_page_id_t r[] = {
#if HELM_ENABLE_PAGE_DATA
            HELM_PAGE_DATA,
#endif
            HELM_PAGE_DASH, HELM_PAGE_DATA_ALL,
#if HELM_ENABLE_CLIMB_PAGE
            HELM_PAGE_CLIMB,
#endif
            HELM_PAGE_MAP
        };
        return r[idx % (uint8_t)(sizeof(r) / sizeof(r[0]))];
    }
}

/* index.html startRide: idle map stays map; otherwise open 数据. */
static void helm_enter_ride(void)
{
    helm_hist_reset();
    bicycle_runtime_reset_session();
    s_sh.paused = false;
    s_sh.pause_auto = false;
    helm_ride_dwell_reset();
    helm_autopause_hold_clear();
    map_page_set_recording(s_sh.map, true);
    /* 已在导航中则留在当前骑行页，勿被待机页索引带跑。 */
    if (!helm_nav_on()) {
        /* 待机 → 骑行：待机 3 页 [主页面, 表盘页, 数据页]、
         * 骑行 4 页 [表盘, 仪表台, 全数字, 地图] ⇒
         * 主页面/表盘页都落到表盘[0]，数据页落到全数字[2]。 */
        s_sh.ride_idx = (s_sh.home_idx >= 2u) ? 2u : 0u;
    }
    helm_apply_view();
    helm_refresh_numbers();
    myvendor_sound_ride();
}

void helm_shell_ensure_ride(void)
{
    if (!s_sh.attached) {
        return;
    }

    if (s_sh.paused) {
        helm_shell_set_paused(false);
        map_page_resume_ride(s_sh.map);
        return;
    }

    if (!helm_riding()) {
        helm_enter_ride();
    }
}

void helm_shell_continue_ride(float km, uint32_t sec, const float * lon,
                              const float * lat, uint16_t n)
{
    if (!s_sh.attached || helm_riding()) {
        bicycle_ride_gpx_cancel_continue();
        return;
    }

    helm_hist_reset();
    s_sh.paused = false;
    s_sh.pause_auto = false;
    helm_ride_dwell_reset();
    helm_autopause_hold_clear();
    map_page_set_recording(s_sh.map, true);
    if (km < 0.0f) {
        km = 0.0f;
    }

    bicycle_runtime_seed_session((double)km * 1000.0, (uint64_t)sec * 1000ull);
    map_page_seed_track(s_sh.map, lon, lat, n);
    bicycle_runtime_seed_session((double)km * 1000.0, (uint64_t)sec * 1000ull);
    if (!helm_nav_on()) {
        s_sh.ride_idx = (s_sh.home_idx >= 2u) ? 1u : 0u;
    }

    helm_apply_view();
    helm_refresh_numbers();
    myvendor_sound_ride();
}

static void helm_leave_ride(void)
{
    helm_hist_reset();
    map_page_end_ride(s_sh.map);
    s_sh.pause_auto = false;
    helm_ride_dwell_reset();
    helm_autopause_hold_clear();
    helm_shell_set_paused(false);
    helm_raise_chrome();
    map_page_continue_render(s_sh.map);
}

static void helm_finish_ride(bool keep)
{
    bicycle_runtime_save_last_pos();
    if (keep) {
        const bicycle_runtime_t * rt = bicycle_runtime_get();
        char path[128];
        const char * base;

        path[0] = '\0';

        {
            int gpx_ret = bicycle_ride_gpx_commit(path, sizeof(path));

            if (gpx_ret == 0 && path[0] != '\0') {
                base = strrchr(path, '/');
                base = (base != NULL && base[1] != '\0') ? base + 1 : path;
                if (rt != NULL) {
                    s_sh.last_km = (float)(rt->session_distance_m / 1000.0);
                    helm_ride_stat_remember(path,
                        rt->session_distance_m / 1000.0,
                        (uint32_t)(rt->session_elapsed_ms / 1000ull));
                    myvendor_devctl_last_ride_m_set(
                        (int32_t)(rt->session_distance_m + 0.5));
                }

                lv_pm_notify_show("已保存", base, 2000);
            } else if (gpx_ret == -ENODATA) {
                lv_pm_notify_show("骑行", "无轨迹，未保存", 2000);
            } else {
                lv_pm_notify_show("骑行", "GPX 保存失败", 2000);
            }
        }

        myvendor_sound_save();
    } else {
        (void)bicycle_ride_gpx_discard();
    }

    helm_save_hide();
    helm_leave_ride();
}

static void helm_discard_ride(void)
{
    helm_finish_ride(false);
    myvendor_sound_discard();
    lv_pm_notify_show("骑行", "本次骑行未保存", 1500);
}

/** @brief 底部三格的第 2/3 格文案（ri 轮转 5 组：时间/里程、均速/极速、
 *  心率/均心率、海拔/坡度、爬升/功率）。 */
static void helm_fill_rot(uint8_t ri, const char ** la, char va[16],
                          const char ** lb, char vb[16])
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();
    char tmp[16];
    float avg = helm_session_avg_kph(rt);
    float dist_km = rt ? (float)(rt->session_distance_m / 1000.0) : 0.0f;
    int alt = rt ? (int)(rt->altitude_m + 0.5f) : 0;

    switch (ri % 5u) {
    case 0:
        *la = "时间";
        helm_fmt_time(va, 16, rt ? rt->session_elapsed_ms : 0);
        *lb = "里程";
        helm_fmt_km(vb, 16, dist_km);
        break;
    case 1:
        *la = "均速";
        helm_fmt_speed(va, 16, avg);
        *lb = "极速";
        helm_fmt_speed(vb, 16, rt ? rt->speed_max_kph : 0);
        break;
    case 2:
        *la = "心率";
        lv_snprintf(va, 16, "%s",
                    helm_dash(rt && rt->hr_valid, tmp, sizeof(tmp), "%u",
                              rt ? (int)rt->hr_bpm : 0));
        *lb = "均心率";
        lv_snprintf(vb, 16, "%s",
                    helm_dash(rt && rt->hr_avg_bpm > 0u, tmp, sizeof(tmp), "%u",
                              rt ? (int)rt->hr_avg_bpm : 0));
        break;
    case 3:
        *la = "海拔";
        lv_snprintf(va, 16, "%d", alt);
        *lb = "坡度";
        if (rt) {
            int g10 = (int)(rt->grade_pct * 10.0f);

            lv_snprintf(vb, 16, "%d.%d", g10 / 10,
                        (g10 < 0 ? -g10 : g10) % 10);
        } else {
            lv_snprintf(vb, 16, "--");
        }
        break;
    default:
        *la = "爬升";
        lv_snprintf(va, 16, "%d", rt ? (int)(rt->gain_m + 0.5f) : 0);
        *lb = "功率";
        lv_snprintf(vb, 16, "%s",
                    helm_dash(rt && rt->power_valid, tmp, sizeof(tmp), "%u",
                              rt ? (int)rt->power_w : 0));
        break;
    }
}

static void helm_apply_view(void)
{
    helm_page_id_t id;
    helm_page_id_t prev;
    bool map_on;
    bool rot_on;
    bool nav_ban;
    bool anim;
    bool rot_was;
    bool pause_was;
#if MYVENDOR_LVGL_STALL_LOG
    uint32_t t0 = lv_tick_get();
#endif

    if (!s_sh.attached || s_sh.map == NULL) {
        return;
    }

    if (s_sh.ui_covered) {
        return;
    }

    id = helm_page_at(*helm_active_idx());
    prev = s_sh.view_id;
    anim = s_sh.view_anim && (prev != id);
    s_sh.view_anim = false;
    s_sh.view_id = id;
    /* 主页态 = LiveMap 页 + 底部信息卡（用户 2026-09-26："把这个下半部分的组件
     * 放到 livemap 界面，然后把 livemap 界面作为主界面"）⇒ 待机态也要显示地图。
     * 注：上一版曾把同样的改动误判成"上板花屏"的原因并回退——花屏其实是**面板
     * 硬件横线**，与这里无关。 */
    map_on = (id == HELM_PAGE_MAP || id == HELM_PAGE_STANDBY);
    /* 底栏三格：只在**地图页**出现。海拔页整页 296 都给了剖面卡 + 六格
     * （用户 2026-09-26 的图），80px 数据条会把剖面卡从 108 压到 56 ⇒ 去掉。 */
    rot_on = (id == HELM_PAGE_MAP);
    /* 路牌：导航中一直挂在左上角（小牌子不吃屏幕），切页时跟着显隐。 */
    nav_ban = map_on && map_page_nav_active(s_sh.map);
    rot_was = (s_sh.rotbar != NULL) &&
              !lv_obj_has_flag(s_sh.rotbar, LV_OBJ_FLAG_HIDDEN);
    pause_was = (s_sh.pause_dock != NULL) &&
                !lv_obj_has_flag(s_sh.pause_dock, LV_OBJ_FLAG_HIDDEN);

    /* 只改可见标志，不在 KEY 这一拍里 catchup/重绘地图。 */
    map_page_set_map_ui_visible(s_sh.map, map_on);

    if (anim) {
        lv_obj_t * panes[5];
        lv_obj_t * po = helm_pane_of(prev);
        lv_obj_t * pi = helm_pane_of(id);
        unsigned i;

        panes[0] = s_sh.standby;
        panes[1] = s_sh.data;
        panes[2] = s_sh.dash;
        panes[3] = s_sh.data_all;
        panes[4] = s_sh.climb;
        if (po == pi && po != NULL) {
            helm_pane_nudge(po, s_sh.view_dir);
        } else if (helm_riding()) {
            helm_pane_snap(po, pi);
            helm_riding_view_enter(s_sh.view_dir);
        } else {
            helm_pane_slide(po, pi, s_sh.view_dir);
        }

        for (i = 0; i < 5; i++) {
            if (panes[i] && panes[i] != po && panes[i] != pi) {
                helm_pane_reset(panes[i]);
                lv_obj_add_flag(panes[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        helm_pane_reset(s_sh.standby);
        helm_pane_reset(s_sh.data);
        helm_pane_reset(s_sh.dash);
        helm_pane_reset(s_sh.data_all);
        helm_pane_reset(s_sh.climb);
        /* 待机 pane 是**不透明满页底**，一旦显示就把地图盖住 ⇒ 永不再显示它。
         * 主页态改由底部信息卡承担（卡片挂在 root 上，见 helm_build_standby_card）。 */
        helm_show(s_sh.standby, false);
        helm_show(s_sh.data, id == HELM_PAGE_DATA);
        helm_show(s_sh.dash, id == HELM_PAGE_DASH);
        helm_show(s_sh.data_all, id == HELM_PAGE_DATA_ALL);
        helm_show(s_sh.climb, id == HELM_PAGE_CLIMB);
    }
    /* 主页态（待机）显示信息卡；骑行态下面是三格数据条（rotbar） */
    helm_show(s_sh.sb_card, id == HELM_PAGE_STANDBY);
    /* 提示环浮动跟着页面显隐：进待机页叫醒、离开立刻停（见 helm_sb_bob_*）。 */
    if (id == HELM_PAGE_STANDBY) {
        helm_sb_bob_kick();
    } else {
        helm_sb_bob_stop();
    }
    helm_show(s_sh.rotbar, rot_on);
    /* 切页是 snap：只在**页面真的换了**时同步横幅显隐。页面没变时不能在这里
     * 把它推到终态 —— 导航开始/结束都会先走 apply_view，紧接着的周期块才是
     * 播放入场/退场的地方；这里一推，周期块看到的永远是"已显示/已隐藏"。 */
    if (prev != id) {
        helm_nav_ban_show(nav_ban, false);
    }
    /* 左下两行 / 右下比例尺：切页时先收起，回到地图页由周期块按导航状态重新决定
     * （它们挂在 root 上，不收起来会飘到别的页面上）。 */
    helm_show(s_sh.nav_sum, false);
    helm_show(s_sh.nav_scale, false);
    helm_show(s_sh.pause_dock, s_sh.paused);
    {
        bool arrive = !s_sh.paused && !helm_save_open() &&
                      !s_sh.arrive_dismissed && map_page_nav_arrived(s_sh.map);
        bool arrive_was = (s_sh.arrive_dock != NULL) &&
                          !lv_obj_has_flag(s_sh.arrive_dock, LV_OBJ_FLAG_HIDDEN);

        helm_show(s_sh.arrive_dock, arrive);
        if (arrive && !arrive_was) {
            myvendor_sound_arrive();
            helm_obj_stagger_in(s_sh.arrive_dock);
        }
    }
    /* 状态栏整条剔除（用户 2026-09-25：LAP 那一条没用还占 22 px）。对象仍建着、代码路径
     * 不变，只是永不显示；它承载的行程标签 / 计时读数如需保留，另行安排位置。 */
    helm_show(s_sh.subbar, false);
    if (id != HELM_PAGE_STANDBY) {
        helm_raise_if_shown(s_sh.subbar);
    }

    if (nav_ban) {
        helm_raise_if_shown(s_sh.nav_ban);
    }

    if (rot_on) {
        helm_raise_if_shown(s_sh.rotbar);
        if (!rot_was && !anim) {
            helm_obj_stagger_in(s_sh.rotbar);
        }
    }

    if (s_sh.paused) {
        helm_pause_title_sync();
        helm_raise_if_shown(s_sh.pause_dock);
        if (!pause_was) {
            helm_obj_stagger_in(s_sh.pause_dock);
        }
    }

    helm_raise_if_shown(s_sh.arrive_dock);

    if (helm_save_open()) {
        lv_obj_move_foreground(s_sh.save_mask);
    }

#if MYVENDOR_LVGL_STALL_LOG
    {
        uint32_t dt = lv_tick_elaps(t0);

        if (dt >= 20u) {
            LVGL_STALL("helm_apply_view %ums id=%u anim=%d",
                (unsigned)dt, (unsigned)id, (int)anim);
        }
    }
#endif
}

static uint32_t helm_gps_label(const bicycle_runtime_t * rt, char * buf, size_t n)
{
    uint8_t sats = rt ? rt->gnss_satellites : 0;
    uint8_t q = rt ? rt->gnss_fix_quality : 0;
    uint8_t h = rt ? rt->gnss_hdop_x10 : 0;
    bool valid = rt && rt->gnss_valid;
    bool alive = rt && rt->gnss_alive;
    bool eph = rt && rt->gnss_eph_busy;
    const char * conf = NULL;

    if (eph) {
        lv_snprintf(buf, n, "星历同步");
        return HELM_COLOR_WARN;
    }

    if (!valid) {
        if (!alive) {
            if (helm_idle_is_sleeping()) {
                lv_snprintf(buf, n, "无定位");
                return HELM_COLOR_INK;
            }

            lv_snprintf(buf, n, "无数据");
            return HELM_COLOR_GPS_DEAD;
        }

        if (sats == 0) {
            lv_snprintf(buf, n, "无定位");
            return HELM_COLOR_INK;
        }

        lv_snprintf(buf, n, "搜索 %u", (unsigned)sats);
        return HELM_COLOR_WARN;
    }

    if (h > 0) {
        if (h <= 10) {
            conf = "高";
        } else if (h <= 20) {
            conf = "中";
        } else {
            conf = "低";
        }
    }

    if (q <= 1) {
        if (conf != NULL) {
            lv_snprintf(buf, n, "2D %s", conf);
        } else {
            lv_snprintf(buf, n, "2D定位");
        }

        return (h > 0 && h <= 20) ? HELM_COLOR_INK : HELM_COLOR_WARN;
    }

    if (conf != NULL) {
        lv_snprintf(buf, n, "3D %s", conf);
    } else {
        lv_snprintf(buf, n, "3D定位");
    }

    return (h > 20) ? HELM_COLOR_INK : HELM_COLOR_CAD;
}

/** @brief 底部三格是否要在这一拍播"落位"（由 `helm_rot_cb()` 换格时置位）。 */
static bool s_rot_anim_pending;

static void helm_lab_y_exec(void * var, int32_t v)
{
    if (helm_anim_is_suspended()) {
        return;
    }

    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)v, 0);
}

static void helm_lab_opa_exec(void * var, int32_t v)
{
    if (helm_anim_is_suspended()) {
        return;
    }

    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

/** @brief 落位收尾：translate 与 opa 一并复位。
 *  @note exec 在 suspend 时早退、但动画仍会走到终点，收尾必须兜底，
 *        否则恢复后屏上留着半透明或 6px 偏移。 */
static void helm_lab_anim_done(lv_anim_t * a)
{
    lv_obj_t * obj = (a != NULL) ? (lv_obj_t *)a->var : NULL;

    if (obj != NULL) {
        lv_obj_set_style_translate_y(obj, 0, 0);
        lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    }
}

#if VMAP_ROUTE_ENABLE
static void helm_nav_badge_y_exec(void * var, int32_t v)
{
    if (helm_anim_is_suspended()) {
        return;
    }

    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)v, 0);
}

static void helm_nav_badge_opa_exec(void * var, int32_t v)
{
    if (helm_anim_is_suspended()) {
        return;
    }

    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void helm_nav_badge_done(lv_anim_t * a)
{
    lv_obj_t * obj = (a != NULL) ? (lv_obj_t *)a->var : NULL;

    if (obj != NULL) {
        lv_obj_set_style_translate_y(obj, 0, 0);
        lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    }
}

/**
 * @brief 转向徽章换向时轻落一下（-6px / 淡入）。
 * @details 只在**图标文本真的变了**时播：`helm_refresh_numbers()` 每 500 ms 都跑，
 *          不能每拍重播。重规划时图标按 `|/-\` 每 500 ms 转一格，属于"进度指示"，
 *          绝不能跟着动画（否则整块徽章一直在抖），直接停动画、复位、只记 last。
 *          横幅隐藏期间只同步 last：等它重新显示时由入场动画负责观感，
 *          不补播一次"换向"。
 */
static void helm_nav_badge_anim(bool replanning)
{
    static char s_nav_ico_last[8];
    const char * ico;

    if (s_sh.nav_ban_badge == NULL || s_sh.nav_ban_ico == NULL) {
        return;
    }

    ico = lv_label_get_text(s_sh.nav_ban_ico);
    if (ico == NULL) {
        return;
    }

    if (replanning) {
        helm_anim_stop(s_sh.nav_ban_badge, helm_nav_badge_y_exec);
        helm_anim_stop(s_sh.nav_ban_badge, helm_nav_badge_opa_exec);
        lv_obj_set_style_translate_y(s_sh.nav_ban_badge, 0, 0);
        lv_obj_set_style_opa(s_sh.nav_ban_badge, LV_OPA_COVER, 0);
        lv_snprintf(s_nav_ico_last, sizeof(s_nav_ico_last), "%s", ico);
        return;
    }

    if (!helm_obj_shown(s_sh.nav_ban)) {
        lv_snprintf(s_nav_ico_last, sizeof(s_nav_ico_last), "%s", ico);
        return;
    }

    if (strcmp(ico, s_nav_ico_last) == 0) {
        return;
    }

    lv_snprintf(s_nav_ico_last, sizeof(s_nav_ico_last), "%s", ico);

    if (helm_anim_is_suspended()) {
        return;
    }

    lv_obj_set_style_translate_y(s_sh.nav_ban_badge, -6, 0);
    lv_obj_set_style_opa(s_sh.nav_ban_badge, LV_OPA_TRANSP, 0);
    helm_anim_run(s_sh.nav_ban_badge, helm_nav_badge_y_exec, -6, 0, 140,
                  lv_anim_path_ease_out, helm_nav_badge_done);
    helm_anim_run(s_sh.nav_ban_badge, helm_nav_badge_opa_exec, LV_OPA_TRANSP, 255,
                  140, lv_anim_path_ease_out, NULL);
}
#endif /* VMAP_ROUTE_ENABLE */

/**
 * @brief 刷新当前页数字、时速/心率三档色与走势。
 * @details 待机 / 数据页 / rotbar 的当前时速走 `helm_paint_speed_pair()` /
 *          `helm_live_speed_ink()`；心率走 `helm_live_hr_ink()`。
 *          均速 / 极速仍用导航橙。
 */
static void helm_refresh_numbers(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();

    if (s_sh.ui_covered) {
        return;
    }
    char buf[32];
    char line[40];
    const char * la;
    const char * lb;
    char va[16];
    char vb[16];
    int i;
    uint8_t n;
    helm_page_id_t id;
    float dist_km;
    float avg;

    if (!s_sh.attached) {
        return;
    }

    n = helm_page_count();
    {
        uint8_t * idx = helm_active_idx();

        if (*idx >= n) {
            *idx = (uint8_t)(n - 1u);
            helm_apply_view();
        }
    }

    id = helm_page_at(*helm_active_idx());
    dist_km = rt ? (float)(rt->session_distance_m / 1000.0) : 0.0f;
    avg = helm_session_avg_kph(rt);

    /* Hidden helm pages only keep runtime/hist data. Draw the active page. */
    if (id == HELM_PAGE_STANDBY) {
        uint16_t kph10 = 0;
        uint8_t lock = 0;
        uint32_t mode_c = HELM_COLOR_INK;
        const char * mode = "0Hz";

        helm_fmt_speed(buf, sizeof(buf), rt ? rt->speed_kph : 0);
        helm_label_set(s_sh.speed_big, buf);

        helm_paint_speed_pair(s_sh.speed_big, s_sh.speed_unit);

        if (s_sh.gps_val) {
            uint32_t col = helm_gps_label(rt, buf, sizeof(buf));

            helm_label_set(s_sh.gps_val, buf);
            lv_obj_set_style_text_color(s_sh.gps_val, helm_color(col), 0);
        }

        if (rt) {
            int v = (int)(rt->speed_kph * 10.0f + 0.5f);

            kph10 = (v < 0) ? 0 : (uint16_t)v;
            if (rt->gnss_eph_busy) {
                lock = 1;
                mode = "星历同步";
                mode_c = HELM_COLOR_WARN;
            } else if (rt->gnss_valid) {
                lock = 2;
                lv_snprintf(buf, sizeof(buf), "%uHz", (unsigned)rt->gnss_rx_hz);
                mode = buf;
                mode_c = HELM_COLOR_GPS;
            } else if (rt->gnss_alive || rt->gnss_rx_hz > 0) {
                lock = 1;
                lv_snprintf(buf, sizeof(buf), "%uHz", (unsigned)rt->gnss_rx_hz);
                mode = buf;
                mode_c = HELM_COLOR_NAV;
            } else {
                lv_snprintf(buf, sizeof(buf), "%uHz", (unsigned)rt->gnss_rx_hz);
                mode = buf;
                mode_c = HELM_COLOR_INK;
            }
        }

        bool hero_dirty = (kph10 != s_sh.dial_kph10 || lock != s_sh.dial_lock);

        s_sh.dial_kph10 = kph10;
        s_sh.dial_lock = lock;
        helm_label_set(s_sh.hero_mode, mode);
        if (s_sh.hero_mode) {
            lv_obj_set_style_text_color(s_sh.hero_mode, helm_color(mode_c), 0);
        }

        if (s_sh.dial_poly_defer) {
            helm_hero_spin_set(false);
        } else {
            helm_hero_spin_set(lock == 1);
            if (s_sh.hero && hero_dirty) {
                lv_obj_invalidate(s_sh.hero);
            }
        }

        helm_label_set(s_sh.standby_hint,
                       map_page_nav_active(s_sh.map) ? "导航中" : "待开始");

        /* 新信息卡：40px 时钟 + 状态列三行（"待开始"标签已隐藏，用户 09-26
         * 说不需要显示；下面这行保留只为让旧对象保持有值，不显示） */
        helm_standby_card_tick(rt);

        if (s_sh.last_dist) {
            if (s_sh.last_km > 0.05f) {
                helm_fmt_km(buf, sizeof(buf), s_sh.last_km);
                lv_snprintf(line, sizeof(line), "%s km", buf);
                helm_label_set(s_sh.last_dist, line);
            } else {
                helm_label_set(s_sh.last_dist, "-- km");
            }
        }
    } else {
        helm_hero_spin_set(false);
    }

    if (id == HELM_PAGE_DATA) {
        /* 表盘页：数据全部来自自行车 runtime（见 helm_dial_tick）。 */
        helm_dial_tick(rt);
    }

    if (id == HELM_PAGE_DASH) {
        /* 老版 X1 仪表台（O02）：大时速 + 右四行 + 时间小结（helm_dash_tick）。 */
        helm_dash_tick(rt);
    }

    if (id == HELM_PAGE_DATA_ALL) {
        const char * alabs[HELM_ALL_N];
        char avals[HELM_ALL_N][16];

        helm_fill_all(rt, alabs, avals);
        for (i = 0; i < HELM_ALL_N; i++) {
            helm_label_set(s_sh.all_lab[i], alabs[i]);
            helm_set_text_hex(s_sh.all_lab[i], HELM_COLOR_MUTE);
            helm_set_bg_hex(s_sh.all_rail[i], helm_tone(alabs[i]));
            helm_label_set(s_sh.all_val[i], avals[i]);
            /* 数值一律 INK；只有"心率"跟着实时区间变色（骑行页同一条规则）。 */
            helm_set_text_hex(s_sh.all_val[i],
                helm_lab_eq(alabs[i], "心率") ? helm_metric_ink(alabs[i])
                                             : HELM_COLOR_INK);
        }
    }

    if (id == HELM_PAGE_CLIMB) {
        helm_climb_tick(rt, *helm_active_idx(), n);
    }

    if (helm_obj_shown(s_sh.rotbar)) {
        helm_fmt_speed(buf, sizeof(buf), rt ? rt->speed_kph : 0);
        helm_label_set(s_sh.rot_val[0], buf);
        helm_set_text_hex(s_sh.rot_val[0], helm_live_speed_ink());

        helm_fill_rot(s_sh.rot_idx, &la, va, &lb, vb);
        lv_snprintf(buf, sizeof(buf), "%s  %u/5", la,
                    (unsigned)(s_sh.rot_idx % 5u) + 1u);
        helm_label_set(s_sh.rot_lab[1], buf);
        helm_set_text_hex(s_sh.rot_lab[1], helm_metric_ink(la));

        {
            uint32_t fill = helm_tone_fill(la);
            bool spark = (fill != HELM_COLOR_PAPER);
            uint32_t ink = helm_metric_ink(la);

            if (spark && !helm_lab_eq(la, "心率")) {
                ink = HELM_COLOR_INK;
            }
            helm_label_set(s_sh.rot_val[1], va);
            helm_set_text_hex(s_sh.rot_val[1], ink);
        }

        helm_label_set(s_sh.rot_lab[2], lb);
        helm_set_text_hex(s_sh.rot_lab[2], helm_metric_ink(lb));

        {
            uint32_t fill = helm_tone_fill(lb);
            bool spark = (fill != HELM_COLOR_PAPER);
            uint32_t ink = helm_metric_ink(lb);

            if (spark && !helm_lab_eq(lb, "心率")) {
                ink = HELM_COLOR_INK;
            }
            helm_label_set(s_sh.rot_val[2], vb);
            helm_set_text_hex(s_sh.rot_val[2], ink);
        }

        /* 3 秒轮转的落位：只动第 2/3 格（速度格 rot_*[0] 每 500 ms 都在变，
         * 跟着播会一直抖）。新文本已写好，从下方 6 px、半透滑到位；
         * pending 只由 `helm_rot_cb()` 换格那一拍置位 ⇒ 普通刷新不会重播。 */
        if (s_rot_anim_pending) {
            s_rot_anim_pending = false;
            if (!helm_anim_is_suspended()) {
                lv_obj_t * anim_labs[4];
                int k;

                anim_labs[0] = s_sh.rot_lab[1];
                anim_labs[1] = s_sh.rot_val[1];
                anim_labs[2] = s_sh.rot_lab[2];
                anim_labs[3] = s_sh.rot_val[2];
                for (k = 0; k < 4; k++) {
                    if (anim_labs[k] == NULL) {
                        continue;
                    }
                    lv_obj_set_style_translate_y(anim_labs[k], 6, 0);
                    lv_obj_set_style_opa(anim_labs[k], 120, 0);
                    helm_anim_run(anim_labs[k], helm_lab_y_exec, 6, 0, 180,
                                  lv_anim_path_ease_out, helm_lab_anim_done);
                    helm_anim_run(anim_labs[k], helm_lab_opa_exec, 120, 255, 180,
                                  lv_anim_path_ease_out, NULL);
                }
            }
        }
    }

    if (helm_obj_shown(s_sh.subbar)) {
        const char * tag = "DATA";
        uint32_t col = HELM_COLOR_INK;

        if (rt && rt->recording) {
            tag = "REC";
            col = HELM_COLOR_HR;
        } else if (s_sh.paused) {
            tag = "PAUSE";
        } else if (helm_nav_on()) {
            tag = "NAV";
            col = HELM_COLOR_NAV;
        } else if (id == HELM_PAGE_MAP) {
            tag = "MAP";
        }

        helm_label_set(s_sh.sub_left, tag);
        if (s_sh.sub_left) {
            lv_obj_set_style_text_color(s_sh.sub_left, helm_color(col), 0);
        }

        helm_fmt_hms(buf, sizeof(buf), rt ? rt->session_elapsed_ms : 0);
        helm_label_set(s_sh.sub_mid, buf);

        /* 顶栏右槽**不再**显示"当前导航信息"（站名 / 行程进度）：
         * 那串东西同时出现在 status 栏右槽、状态栏下的导航横幅（`nav_ban` 的
         * `dest`）、以及导航页正文的"大箭头 + 距离 + 方向"里 —— 用户 2026-09-25
         * 判定重复太多，这里只留 LAP 计数（与无导航时一致）。
         * `map_page_nav_status_text()` 仍被下面的横幅使用，别删那个函数。 */
        /* 顶栏右槽：导航中给"距下一路口 / 剩余里程"（用户 2026-09-25 要）—— 横幅按显示策略
         * 大部分时间不挂，这里补上那个空档的读数；无导航时仍是 LAP。
         * 这一槽走点阵字库，缺字会整块不显示，所以只用 ASCII（"→" 尽力而为；万一没有，
         * 还剩数字 + 橙色可区分）。 */
        {
            const double td = map_page_nav_turn_dist_m(s_sh.map);
            const double rm = map_page_nav_remain_m(s_sh.map);
            uint32_t col = HELM_COLOR_INK;

            if (td >= 0.0 && td <= 1000.0) {
                lv_snprintf(buf, sizeof(buf), "→ %d m", (int)(td + 0.5));
                col = HELM_COLOR_NAV;
            } else if (rm >= 0.0) {
                if (rm >= 1000.0) {
                    lv_snprintf(buf, sizeof(buf), "%.1f km", rm / 1000.0);
                } else {
                    lv_snprintf(buf, sizeof(buf), "%d m", (int)(rm + 0.5));
                }
            } else {
                /* LAP 栏剔除（用户 2026-09-25：判定无用，几个页面一起去掉）：
                 * 没有导航读数时这一槽留空，不再显示圈数。lap_count 的**记录**仍在，
                 * 骑行记录里还用它。 */
                buf[0] = '\0';
            }
            helm_label_set(s_sh.sub_right, buf);
            lv_obj_set_style_text_color(s_sh.sub_right, helm_color(col), 0);
        }
    }

#if VMAP_ROUTE_ENABLE
    /* 路牌可见性 + 左下两行/右下比例尺。`helm_apply_view()` 只在切页/事件时跑，导航刚起步
     * 那一下不会触发，所以这里每拍补一次（幂等：只在状态变化时改标志）。
     * 左下两行只在导航中才有意义；比例尺只看是不是地图页。 */
    if (s_sh.view_id == HELM_PAGE_MAP) {
        const bool nav_live = map_page_nav_active(s_sh.map) ||
                              (HELM_BANNER_DEMO_ON_BOOT != 0) ||
                              (s_banner_demo_mode != 0);

        /* 地图几何**不再**随导航状态变动（用户 2026-09-25：动态改几何导致指针与实际地图
         * 位置出现偏差，而且反复局部重绘会让已画好的导航线丢段）。恢复创建时的静态几何，
         * 横幅就覆盖在地图顶部。map_page_set_banner_inset() 保留但不再调用。 */
        const double mpp = map_page_map_mpp(s_sh.map);

        helm_nav_ban_show(nav_live, true);
        helm_show(s_sh.nav_sum, nav_live);
        helm_show(s_sh.nav_scale, mpp > 0.0);

        if (nav_live && s_sh.nav_sum_l1) {
            const double rm = map_page_nav_remain_m(s_sh.map);
            const double tm = map_page_nav_total_m(s_sh.map);

            if (rm >= 1000.0) {
                /* ≥100km 不要小数（用户 2026-09-27，与 helm_fmt_km 同规则）。 */
                lv_snprintf(buf, sizeof(buf),
                    (rm >= 99950.0) ? "剩余里程 %.0fkm" : "剩余里程 %.1fkm",
                    rm / 1000.0);
            } else if (rm >= 0.0) {
                lv_snprintf(buf, sizeof(buf), "剩余里程 %.0fm", rm);
            } else {
                lv_snprintf(buf, sizeof(buf), "剩余里程 --");
            }
            helm_label_set(s_sh.nav_sum_l1, buf);
            if (tm >= 1000.0) {
                /* ≥100km 不要小数（同上）。 */
                lv_snprintf(buf, sizeof(buf),
                    (tm >= 99950.0) ? "总里程 %.0fkm" : "总里程 %.1fkm",
                    tm / 1000.0);
            } else if (tm >= 0.0) {
                lv_snprintf(buf, sizeof(buf), "总里程 %.0fm", tm);
            } else {
                lv_snprintf(buf, sizeof(buf), "总里程 --");
            }
            helm_label_set(s_sh.nav_sum_l2, buf);
        }
        if (mpp > 0.0 && s_sh.nav_scale_lab) {
            /* 标尺按 40 px 取"好看"的整值：1/2/5 × 10^k。 */
            const double raw = mpp * 40.0;
            double e = 1.0;
            double pick;

            while (e * 10.0 <= raw) {
                e *= 10.0;
            }
            pick = (raw < e * 1.5) ? e : ((raw < e * 3.5) ? e * 2.0 : e * 5.0);
            if (pick >= 1000.0) {
                lv_snprintf(buf, sizeof(buf), "%.0fkm", pick / 1000.0);
            } else {
                lv_snprintf(buf, sizeof(buf), "%.0fm", pick);
            }
            helm_label_set(s_sh.nav_scale_lab, buf);
        }
    }

    bool ban_replan = false;

    if (helm_obj_shown(s_sh.nav_ban)) {
        static const char spin[] = "|/-\\";
        bool arrived = map_page_nav_arrived(s_sh.map);
        bool replanning = map_page_nav_replanning(s_sh.map) && !arrived;
        bool warn = rt && rt->nav_off_route_m > map_page_nav_off_warn_m(s_sh.map)
                    && !arrived && !replanning;
        /* —— 横幅状态演示：终端经 helm_shell_demo_banner() 写 s_banner_demo_mode；
         *    开机自检（HELM_BANNER_DEMO_ON_BOOT）仍每 3 秒轮播一格。 */
        int demo_mode = s_banner_demo_mode;

        if (s_banner_demo_auto &&
            (uint32_t)(lv_tick_get() - s_banner_demo_ms) > 3000u) {
            s_banner_demo_ms = (uint32_t)lv_tick_get();
            s_banner_demo_mode = (s_banner_demo_mode >= 7) ? 1 : s_banner_demo_mode + 1;
            demo_mode = s_banner_demo_mode;
        }
        if (demo_mode != 0) {
            arrived = (demo_mode == 4);
            warn = (demo_mode == 3);
            replanning = (demo_mode == 7);
        }
        ban_replan = replanning;
        /* 回退到第二张参照图那版（用户 2026-09-25）：淡彩底 + 黑/灰字 + 淡橙徽章反白箭头。 */
        /* 底色：中性深灰半透 + 反白字（用户 2026-09-25："横幅底色白色比较丑"）。
         * 淡彩在这块屏上会洗成白、整卡高饱和又太跳；状态色只留在徽章上，
         * 偏航/到达另靠文案（"偏航 120 m" / "已到达"）区分。与通知弹窗同一套观感。 */
        /* 随状态变的**只有横幅背景**（中浅蓝 / 中浅红 / 中浅绿）；徽章固定淡橙 + 白箭头，
         * 字体固定黑/灰 —— 用户 2026-09-25 对着照片定：徽章与字体颜色始终不变。 */
        uint32_t card = arrived ? HELM_COLOR_BAN_ARR_FILL :
                        ((warn || replanning) ? HELM_COLOR_BAN_OFF_FILL
                                              : HELM_COLOR_BAN_NAV_FILL);
        /* 徽章底色加深一档（用户 2026-09-25："徽章背景颜色太浅"）—— 用饱和橙 + 白箭头。 */
        uint32_t badge = HELM_COLOR_NAV;
        uint32_t badge_fg = HELM_COLOR_PAPER;
        uint32_t pri = HELM_COLOR_INK;
        uint32_t sec = (warn || replanning) ? HELM_COLOR_INK : HELM_COLOR_MUTE;
        char ico[8];
        char dist[40];
        char sub[32];
        char dest[32];

        if (!arrived) {
            s_sh.arrive_dismissed = false;
        }

        if (map_page_nav_turn_info(s_sh.map, ico, sizeof(ico), dist, sizeof(dist),
                                   sub, sizeof(sub))) {
            helm_label_set(s_sh.nav_ban_ico, ico);
            /* 距离与动作**分两个标签**：主字（墨色 22px）+ 次字（灰 15px），
             * 不再把"距离 · 动作"塞成一句（sub 为空时次字自动不占宽度）。 */
            /* 数字必须带语义（用户 2026-09-25："显示 3.2km 不知道是做什么的"）：
             *  ≥1km 且下一步是直行 ⇒ "保持直行 3.2km"（这时骑手要知道"一直骑"）；
             *  其余 ≥1km          ⇒ "3.2km 后左转"；
             *  <1km               ⇒ "300m 后左转"（数字在前，便于临近时扫）；
             *  没有转向点          ⇒ "剩余 12.3km"（明确是剩余，不是距路口）。
             *  动作词由转向箭头映射（← → U ↑），不额外引入数据。 */
            {
                const double td = map_page_nav_turn_dist_m(s_sh.map);
                const double rm = map_page_nav_remain_m(s_sh.map);
                const char *act = "直行";

                if (strcmp(ico, "←") == 0) {
                    act = "左转";
                } else if (strcmp(ico, "→") == 0) {
                    act = "右转";
                } else if (strcmp(ico, "U") == 0) {
                    act = "掉头";
                }

                if (td >= 1000.0) {
                    if (strcmp(act, "直行") == 0) {
                        lv_snprintf(dist, sizeof(dist),
                            (td >= 99950.0) ? "保持直行 %.0fkm" : "保持直行 %.1fkm",
                            td / 1000.0);
                    } else {
                        lv_snprintf(dist, sizeof(dist), "%.1fkm 后%s", td / 1000.0, act);
                    }
                } else if (td >= 0.0) {
                    lv_snprintf(dist, sizeof(dist), "%.0fm 后%s", td, act);
                } else if (rm >= 1000.0) {
                    lv_snprintf(dist, sizeof(dist),
                        (rm >= 99950.0) ? "剩余 %.0fkm" : "剩余 %.1fkm",
                        rm / 1000.0);
                } else if (rm >= 0.0) {
                    lv_snprintf(dist, sizeof(dist), "剩余 %.0fm", rm);
                } else {
                    lv_snprintf(dist, sizeof(dist), "导航中");
                }
            }
            helm_label_set(s_sh.nav_ban_m, dist);
            /* 参照图只有"箭头 + 距离"两样，动作次字不再显示（路牌也窄）。 */
            helm_label_set(s_sh.nav_ban_act, "");
        } else {
            helm_label_set(s_sh.nav_ban_ico, "↑");
            helm_label_set(s_sh.nav_ban_m, "--");
            helm_label_set(s_sh.nav_ban_act, "");
            sub[0] = '\0';
        }

        /* 目的地行回来了（通栏放得下；参照图第二张就是这个样式）。 */
        if (map_page_nav_status_text(s_sh.map, dest, sizeof(dest))
            && dest[0] != '\0') {
            lv_snprintf(line, sizeof(line), "去 %s", dest);
            helm_label_set(s_sh.nav_ban_rd, line);
        } else {
            helm_label_set(s_sh.nav_ban_rd, "");
        }
        /* 演示态的文案（覆盖真实取数，保证每一态都能看清样子）。 */
        if (demo_mode != 0) {
            static const char *const d_ico[8] = { "↑", "↑", "←", "!", "●",
                                                  "→", "U", "↑" };
            static const char *const d_txt[8] = {
                "", "保持直行 12.3km", "228m 后左转", "偏航 120m", "已到达",
                "300m 后右转", "300m 后掉头", "正在重新规划路线"
            };

            helm_label_set(s_sh.nav_ban_ico, d_ico[demo_mode]);
            helm_label_set(s_sh.nav_ban_m, d_txt[demo_mode]);
            helm_label_set(s_sh.nav_ban_act, "");
            helm_label_set(s_sh.nav_ban_rd,
                           demo_mode == 4 ? "终点：滁州大道"
                                          : "去 20260529户外骑行");
        }

        lv_obj_set_style_bg_color(s_sh.nav_ban, helm_color(card), 0);
        lv_obj_set_style_bg_color(s_sh.nav_ban_badge, helm_color(badge), 0);
        if (s_sh.nav_ban_ico) {
            /* 徽章：状态彩底 + 反白图标（回退版配色）。 */
            lv_obj_set_style_text_color(s_sh.nav_ban_ico, helm_color(badge_fg), 0);
            if (arrived) {
                helm_label_set(s_sh.nav_ban_ico, "●");
            } else if (replanning) {
                const uint32_t phase = (lv_tick_get() / HELM_UI_MS) & 3u;

                line[0] = spin[phase];
                line[1] = '\0';
                helm_label_set(s_sh.nav_ban_ico, line);
            } else if (warn) {
                helm_label_set(s_sh.nav_ban_ico, "!");
            }
        }

        if (s_sh.nav_ban_m) {
            lv_obj_set_style_text_color(s_sh.nav_ban_m, helm_color(pri), 0);
            lv_obj_set_style_text_color(s_sh.nav_ban_act, helm_color(sec), 0);
            lv_obj_set_style_text_color(s_sh.nav_ban_rd, helm_color(sec), 0);
            if (arrived) {
                helm_label_set(s_sh.nav_ban_m, "已到达");
                helm_label_set(s_sh.nav_ban_act, "");
            } else if (replanning) {
                helm_label_set(s_sh.nav_ban_m, "正在重新规划路线");
                helm_label_set(s_sh.nav_ban_act, "");
            } else if (warn) {
                /* 演示态没有真实偏航距离（rt->nav_off_route_m=0），别显示"偏航 0 m"；
                 * 固定给一个示意值，保持与自检文案一致。 */
                if (demo_mode == 3) {
                    helm_label_set(s_sh.nav_ban_m, "偏航 120m");
                } else {
                    lv_snprintf(line, sizeof(line), "偏航 %d m",
                                (int)(rt->nav_off_route_m + 0.5));
                    helm_label_set(s_sh.nav_ban_m, line);
                }
                helm_label_set(s_sh.nav_ban_act, "");
            }
        }

        if (s_sh.nav_ban_rd) {
            lv_obj_set_style_text_color(s_sh.nav_ban_rd,
                                        helm_color(HELM_COLOR_INK), 0);
            if (arrived) {
                helm_label_set(s_sh.nav_ban_rd, sub[0] != '\0' ? sub : "终点");
            } else if (warn) {
                helm_label_set(s_sh.nav_ban_rd, "其后途经点保留");
            }
        }
    }

    /* ico 文本到这里才最终确定（含 demo 覆盖）⇒ 换向动效放最后。
     * 横幅隐藏时 helper 只同步"上次图标"，不会起动画。 */
    helm_nav_badge_anim(ban_replan);
#endif

    if (helm_obj_shown(s_sh.pause_dock)) {
        helm_fmt_hms(buf, sizeof(buf), rt ? rt->session_elapsed_ms : 0);
        helm_label_set(s_sh.pause_time, buf);

        helm_fmt_km(buf, sizeof(buf), dist_km);
        lv_snprintf(line, sizeof(line), "%s km", buf);
        helm_label_set(s_sh.pause_dist, line);

        helm_fmt_speed(buf, sizeof(buf), avg);
        helm_label_set(s_sh.pause_avg, buf);

        if (rt && rt->hr_avg_bpm > 0u) {
            lv_snprintf(buf, sizeof(buf), "%u", (unsigned)rt->hr_avg_bpm);
            helm_label_set(s_sh.pause_hr, buf);
        } else {
            helm_label_set(s_sh.pause_hr, "--");
        }
    }

    if (helm_save_open()) {
        helm_fmt_hms(buf, sizeof(buf), rt ? rt->session_elapsed_ms : 0);
        helm_label_set(s_sh.save_time, buf);

        helm_fmt_km(buf, sizeof(buf), dist_km);
        lv_snprintf(line, sizeof(line), "%s km", buf);
        helm_label_set(s_sh.save_dist, line);

        helm_fmt_speed(buf, sizeof(buf), avg);
        helm_label_set(s_sh.save_avg, buf);

        lv_snprintf(buf, sizeof(buf), "%d m",
                    rt ? (int)(rt->gain_m + 0.5f) : 0);
        helm_label_set(s_sh.save_gain, buf);

        helm_fmt_speed(buf, sizeof(buf), rt ? rt->speed_max_kph : 0);
        helm_label_set(s_sh.save_max, buf);

        if (rt && rt->hr_avg_bpm > 0u) {
            lv_snprintf(buf, sizeof(buf), "%u", (unsigned)rt->hr_avg_bpm);
            helm_label_set(s_sh.save_hr, buf);
        } else {
            helm_label_set(s_sh.save_hr, "--");
        }
    }
}

static bool helm_save_open(void)
{
    return s_sh.save_mask && !lv_obj_has_flag(s_sh.save_mask, LV_OBJ_FLAG_HIDDEN);
}

static void helm_save_hide(void)
{
    if (s_sh.save_mask) {
        lv_obj_add_flag(s_sh.save_mask, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool helm_arrive_open(void)
{
    return s_sh.arrive_dock && !lv_obj_has_flag(s_sh.arrive_dock, LV_OBJ_FLAG_HIDDEN);
}

static void helm_arrive_hide(void)
{
    s_sh.arrive_dismissed = true;
    if (s_sh.arrive_dock) {
        lv_obj_add_flag(s_sh.arrive_dock, LV_OBJ_FLAG_HIDDEN);
    }
}

static void helm_arrive_confirm(void)
{
    helm_arrive_hide();
#if VMAP_ROUTE_ENABLE
    map_page_nav_stop(s_sh.map);
#endif
}

static void helm_on_prev(void * ud)
{
    uint8_t n = helm_page_count();
    uint8_t * idx;
#if MYVENDOR_LVGL_STALL_LOG
    uint32_t t0;
#endif

    LV_UNUSED(ud);
    if (!s_sh.attached) {
        LVGL_STALL("helm_on_prev drop !attached covered=%d",
            (int)s_sh.ui_covered);
        return;
    }

    /* 任意按键都算"有交互"：把待机页提示环的浮动重新叫醒（3 s 后再自停）。 */
    helm_sb_bob_kick();

    /* 演示页面锁：任意翻页键先解锁，本次按键继续走正常翻页（下面会 apply）。 */
    if (s_demo_page != (helm_page_id_t)0xff) {
        s_demo_page = (helm_page_id_t)0xff;
    }

#if MYVENDOR_LVGL_STALL_LOG
    t0 = lv_tick_get();
#endif
    if (helm_save_open()) {
        helm_obj_press(s_sh.save_mask);
        helm_discard_ride();
        return;
    }

    if (helm_arrive_open()) {
        helm_arrive_confirm();
        myvendor_sound_back();
        return;
    }

    /* pause dock: KEY1 = 继续骑 (short or long) */
    if (s_sh.paused) {
        helm_obj_press(s_sh.pause_dock);
        helm_pause_resume_by_key();
        return;
    }

    if (helm_flip_busy()) {
        LVGL_STALL("helm_on_prev drop busy");
        return;
    }

    idx = helm_active_idx();
    *idx = (uint8_t)((*idx + n - 1u) % n);
    s_sh.view_dir = -1;
    s_sh.view_anim = true;
    LVGL_STALL("helm_on_prev view=%u/%u riding=%d",
           (unsigned)*idx + 1u, (unsigned)n, helm_riding() ? 1 : 0);
    helm_apply_view();
    helm_flip_arm(helm_riding() ? HELM_FLIP_GUARD_MS : HELM_VIEW_MS);
    helm_nums_defer_begin();
    LVGL_STALL("helm_on_prev done %ums",
        (unsigned)lv_tick_elaps(t0));
}

static void helm_save_clicked(lv_event_t * e)
{
    LV_UNUSED(e);
    helm_finish_ride(true);
    /* 骑行结束 = 记录数变了。菜单那边平时只读缓存（用户 2026-09-26：
     * "开机、骑行结束、进入骑行记录"三处才更新），所以这里显式失效一次。 */
    helm_gpx_count_invalidate();
}

static void helm_prompt_save(void)
{
    if (s_sh.save_mask) {
        helm_refresh_numbers();
        lv_obj_clear_flag(s_sh.save_mask, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_sh.save_mask);
        helm_obj_stagger_in(s_sh.save_mask);
        myvendor_sound_prompt();
    }
}

static void helm_on_next(void * ud)
{
    uint8_t n = helm_page_count();
    uint8_t * idx;
#if MYVENDOR_LVGL_STALL_LOG
    uint32_t t0;
#endif

    LV_UNUSED(ud);
    if (!s_sh.attached) {
        LVGL_STALL("helm_on_next drop !attached covered=%d",
            (int)s_sh.ui_covered);
        return;
    }

    /* 任意按键都算"有交互"：把待机页提示环的浮动重新叫醒（3 s 后再自停）。 */
    helm_sb_bob_kick();

    /* 演示页面锁：任意翻页键先解锁，本次按键继续走正常翻页（下面会 apply）。 */
    if (s_demo_page != (helm_page_id_t)0xff) {
        s_demo_page = (helm_page_id_t)0xff;
    }

#if MYVENDOR_LVGL_STALL_LOG
    t0 = lv_tick_get();
#endif
    if (helm_save_open()) {
        helm_obj_press(s_sh.save_mask);
        helm_save_clicked(NULL);
        return;
    }

    if (helm_arrive_open()) {
        helm_arrive_confirm();
        return;
    }

    /* pause dock: KEY2 = 结束 → 保存/不保存 */
    if (s_sh.paused) {
        helm_obj_press(s_sh.pause_dock);
        helm_prompt_save();
        return;
    }

    if (helm_flip_busy()) {
        LVGL_STALL("helm_on_next drop busy");
        return;
    }

    idx = helm_active_idx();
    *idx = (uint8_t)((*idx + 1u) % n);
    s_sh.view_dir = 1;
    s_sh.view_anim = true;
    LVGL_STALL("helm_on_next view=%u/%u riding=%d",
           (unsigned)*idx + 1u, (unsigned)n, helm_riding() ? 1 : 0);
    helm_apply_view();
    helm_flip_arm(helm_riding() ? HELM_FLIP_GUARD_MS : HELM_VIEW_MS);
    helm_nums_defer_begin();
    LVGL_STALL("helm_on_next done %ums",
        (unsigned)lv_tick_elaps(t0));
}

static void helm_on_menu(void * ud)
{
    LV_UNUSED(ud);
    if (!s_sh.attached) {
        return;
    }

    /* 任意按键都算"有交互"：把待机页提示环的浮动重新叫醒（3 s 后再自停）。 */
    helm_sb_bob_kick();

    if (helm_save_open()) {
        helm_save_hide();
        myvendor_sound_back();
        return;
    }

    if (helm_arrive_open()) {
        helm_arrive_confirm();
        myvendor_sound_back();
        return;
    }

    if (s_sh.paused) {
        printf("helm: KEY1 long menu (paused)\n");
        myvendor_sound_long();
        if (helm_menu_open() != 0) {
            printf("helm: menu open failed\n");
        }
        return;
    }

    printf("helm: KEY1 long menu\n");
    myvendor_sound_long();
    if (helm_menu_open() != 0) {
        printf("helm: menu open failed\n");
    }
}

void helm_shell_toggle_ride(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();

    if (!s_sh.attached) {
        printf("helm: KEY2 long ignored (not attached)\n");
        return;
    }

    if (helm_save_open()) {
        helm_save_clicked(NULL);
        return;
    }

    if (helm_arrive_open()) {
        helm_arrive_confirm();
        return;
    }

    printf("helm: KEY2 long ride toggle rec=%d paused=%d home=%u ride=%u\n",
           (rt && rt->recording) ? 1 : 0, s_sh.paused ? 1 : 0,
           (unsigned)s_sh.home_idx, (unsigned)s_sh.ride_idx);

    if (s_sh.paused) {
        helm_prompt_save();
        return;
    }

    if (rt && rt->recording) {
        map_page_pause_ride(s_sh.map);
        s_sh.pause_auto = false;
        helm_shell_set_paused(true);
        return;
    }

    helm_enter_ride();
}

static void helm_on_rec(void * ud)
{
    LV_UNUSED(ud);

    /* 任意按键都算"有交互"：把待机页提示环的浮动重新叫醒（3 s 后再自停）。 */
    helm_sb_bob_kick();
    helm_shell_toggle_ride();
}

bool helm_shell_nav_ban_shown(void)
{
    return s_sh.nav_ban != NULL && helm_obj_shown(s_sh.nav_ban);
}

void helm_shell_on_nav_started(void)
{
    if (!s_sh.attached) {
        return;
    }

    /* 导航不自动开 REC。切到骑行地图页：骑行态轮转是
     * [表盘, 仪表台, 全数字, 地图] ⇒ 地图是 [3]。
     * ⚠ 2026-11-01 加回仪表台页后地图从 [2] 挪到了 [3]；这里原来写死 2，
     *   导航起步会落到"全数字"页（演示的偏航/重规划因此看不到路线）。 */
    s_sh.ride_idx = 3;
    s_sh.arrive_dismissed = false;
    if (s_sh.ui_covered) {
        return;
    }

    helm_apply_view();
    helm_refresh_numbers();
}

static void helm_rot_cb(lv_timer_t * t)
{
    helm_page_id_t id;

    LV_UNUSED(t);
    if (s_sh.ui_covered || !s_sh.attached) {
        return;
    }

    id = helm_page_at(*helm_active_idx());
    if (id == HELM_PAGE_MAP || id == HELM_PAGE_CLIMB) {
        s_sh.rot_idx = (uint8_t)((s_sh.rot_idx + 1u) % 5u);
        /* 只有换格且三格条可见时才播落位（海拔页 rotbar 是藏着的）。 */
        if (helm_obj_shown(s_sh.rotbar)) {
            s_rot_anim_pending = true;
        }
    } else {
        return;
    }

    helm_refresh_numbers();
}

static void helm_ui_cb(lv_timer_t * t)
{
    bool added;
    helm_page_id_t id;
    bicycle_gnss_fix_t fix;
#if MYVENDOR_LVGL_STALL_LOG
    uint32_t t0 = lv_tick_get();
#endif

    LV_UNUSED(t);
    helm_idle_tick();
    helm_pwr_bat_tick();
    /* 主界面自己取 GNSS：不能只靠地图 gpx_timer。启动延迟创建会把
     * 那个定时器 pause，关 splash 若没再 will_appear，就会一直「无数据」。 */
    (void)bicycle_runtime_poll_fix(&fix);
    bicycle_runtime_tick(lv_tick_get());
    bicycle_runtime_update_gnss(&fix);
    helm_shell_autopause_tick();
    if (s_sh.ui_covered) {
        return;
    }

    /* Always ingest ride data; only the visible page is drawn. */
    helm_avg_sample(bicycle_runtime_get());
    added = helm_hist_push();
    helm_rec_ele_push();

    id = s_sh.view_id;
    if (!helm_flip_busy()) {
        helm_refresh_numbers();
        if (helm_page_is_data(id) || helm_obj_shown(s_sh.rotbar)) {
            helm_spark_sync(added);
        }
    }

#if MYVENDOR_LVGL_STALL_LOG
    {
        uint32_t dt = lv_tick_elaps(t0);

        if (dt >= 20u) {
            LVGL_STALL("helm_ui_cb %ums id=%u",
                (unsigned)dt, (unsigned)id);
        }
    }
#endif
}

static lv_obj_t * helm_speed_head(lv_obj_t * parent, const char * title,
                                  lv_obj_t ** title_obj, lv_obj_t ** mark)
{
    lv_obj_t * head = lv_obj_create(parent);
    lv_obj_t * t;

    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), HELM_SPEED_HEAD_H);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(head, 10, 0);
    t = helm_lab(head, s_sh.font_lab, HELM_COLOR_INK, title);
    if (title_obj) {
        *title_obj = t;
    }

    if (mark) {
        *mark = helm_lab(head, s_sh.font_lab, HELM_COLOR_INK, "1/5");
    }
    return head;
}

static const uint8_t helm_poly_nx[HELM_POLY_N] = {
    0, 18, 36, 60, 78, 96, 110, 120
};
static const uint8_t helm_poly_ny[HELM_POLY_N] = {
    18, 8, 14, 0, 11, 6, 16, 12
};

static int16_t helm_dial_span_now(void)
{
    int16_t span;

    if (s_sh.dial_lock == 1) {
        span = (int16_t)s_sh.dial_phase;
    } else {
        span = (int16_t)((HELM_DIAL_SPAN * (int)s_sh.dial_kph10)
                         / (HELM_DIAL_MAX_KPH * 10));
    }

    if (span < 0) {
        return 0;
    }

    if (span > HELM_DIAL_SPAN) {
        return HELM_DIAL_SPAN;
    }

    return span;
}

static int32_t helm_poly_hypot(int32_t dx, int32_t dy)
{
    return (int32_t)lv_sqrt32((uint32_t)(dx * dx + dy * dy));
}

static void helm_poly_build(const lv_area_t * coords, lv_point_t * pts,
                            int32_t * slen, int32_t * total)
{
    lv_coord_t x0 = (lv_coord_t)(coords->x1 + 16);
    lv_coord_t y0 = (lv_coord_t)(coords->y1 + 16);
    lv_coord_t bw = (lv_coord_t)(lv_area_get_width(coords) - 32);
    lv_coord_t bh = 22;
    int i;

    if (bw < 40) {
        bw = 40;
    }

    *total = 0;
    for (i = 0; i < HELM_POLY_N; i++) {
        pts[i].x = (lv_coord_t)(x0 + (bw * helm_poly_nx[i]) / HELM_POLY_NX);
        pts[i].y = (lv_coord_t)(y0 + (bh * helm_poly_ny[i]) / HELM_POLY_NY);
        if (i > 0) {
            slen[i - 1] = helm_poly_hypot(pts[i].x - pts[i - 1].x,
                                          pts[i].y - pts[i - 1].y);
            if (slen[i - 1] < 1) {
                slen[i - 1] = 1;
            }

            *total += slen[i - 1];
        }
    }
}

static void helm_poly_at(const lv_point_t * pts, const int32_t * slen,
                         int32_t total, int32_t dist, int32_t * x, int32_t * y)
{
    int i;
    int32_t acc = 0;

    if (dist <= 0) {
        *x = pts[0].x;
        *y = pts[0].y;
        return;
    }

    if (dist >= total) {
        *x = pts[HELM_POLY_N - 1].x;
        *y = pts[HELM_POLY_N - 1].y;
        return;
    }

    for (i = 0; i < HELM_POLY_N - 1; i++) {
        int32_t L = slen[i];

        if (dist <= acc + L) {
            int32_t t = dist - acc;

            *x = pts[i].x + (pts[i + 1].x - pts[i].x) * t / L;
            *y = pts[i].y + (pts[i + 1].y - pts[i].y) * t / L;
            return;
        }

        acc += L;
    }

    *x = pts[HELM_POLY_N - 1].x;
    *y = pts[HELM_POLY_N - 1].y;
}

static void helm_poly_line(lv_layer_t * layer, int32_t x0, int32_t y0,
                           int32_t x1, int32_t y1, uint16_t w, lv_color_t color)
{
    helm_draw_seg(layer, x0, y0, x1, y1, w, color);
}

static void helm_poly_draw_range(lv_layer_t * layer, const lv_point_t * pts,
                                 const int32_t * slen, int32_t total,
                                 int32_t d0, int32_t d1, uint16_t w,
                                 lv_color_t color)
{
    int i;
    int32_t acc = 0;

    if (d1 <= d0) {
        return;
    }

    if (d0 < 0) {
        d0 = 0;
    }

    if (d1 > total) {
        d1 = total;
    }

    for (i = 0; i < HELM_POLY_N - 1; i++) {
        int32_t L = slen[i];
        int32_t s0 = acc;
        int32_t s1 = acc + L;
        int32_t u0;
        int32_t u1;
        int32_t ax;
        int32_t ay;
        int32_t bx;
        int32_t by;

        acc = s1;
        if (s1 <= d0 || s0 >= d1) {
            continue;
        }

        u0 = (d0 > s0) ? d0 : s0;
        u1 = (d1 < s1) ? d1 : s1;
        helm_poly_at(pts, slen, total, u0, &ax, &ay);
        helm_poly_at(pts, slen, total, u1, &bx, &by);
        helm_poly_line(layer, ax, ay, bx, by, w, color);
    }
}

static void helm_dial_label(lv_layer_t * layer, lv_coord_t x, lv_coord_t y,
                            const char * txt, lv_color_t color)
{
    lv_draw_label_dsc_t d;
    lv_point_t sz;
    lv_area_t a;

    lv_draw_label_dsc_init(&d);
    d.text = txt;
    d.text_local = 1;
    d.font = helm_font_lab();
    d.color = color;
    d.opa = LV_OPA_COVER;
    d.align = LV_TEXT_ALIGN_CENTER;
    lv_text_get_size(&sz, txt, d.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    d.text_size = sz;
    a.x1 = (lv_coord_t)(x - sz.x / 2);
    a.y1 = (lv_coord_t)(y - sz.y / 2);
    a.x2 = (lv_coord_t)(a.x1 + sz.x);
    a.y2 = (lv_coord_t)(a.y1 + sz.y);
    lv_draw_label(layer, &d, &a);
}

static void helm_poly_pip(lv_layer_t * layer, int32_t x, int32_t y, lv_color_t color)
{
    helm_draw_pip(layer, x, y, 5, color);
}

static void helm_hero_spin_exec(void * var, int32_t v)
{
    if (helm_anim_is_suspended()) {
        return;
    }

    s_sh.dial_phase = (uint16_t)v;

    if (s_sh.ui_covered) {
        return;
    }

    /* 见 HELM_HERO_SPIN_MS：相位每帧都更新，但只在节流窗口到点时才让
     * LVGL 重绘 hero（整圈折线 + track 弧 + 刻度文字 + 游标点）。 */
    {
        uint32_t now = lv_tick_get();

        if ((uint32_t)(now - s_hero_spin_redraw_ms) < HELM_HERO_SPIN_MS) {
            return;
        }

        s_hero_spin_redraw_ms = now;
    }

    lv_obj_invalidate((lv_obj_t *)var);
}

static void helm_hero_spin_set(bool on)
{
    lv_anim_t a;

    if (s_sh.hero == NULL) {
        s_sh.dial_spin = false;
        return;
    }

    if (on) {
        if (s_sh.ui_covered || s_sh.dial_spin) {
            return;
        }

        s_hero_spin_redraw_ms = 0; /* 首帧立刻重绘，不受节流窗口影响 */
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_sh.hero);
        lv_anim_set_values(&a, 0, HELM_DIAL_SPAN);
        lv_anim_set_time(&a, 1600);
        lv_anim_set_reverse_duration(&a, 1600);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a, helm_hero_spin_exec);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
        s_sh.dial_spin = true;
        return;
    }

    if (s_sh.dial_spin) {
        lv_anim_delete(s_sh.hero, helm_hero_spin_exec);
        s_sh.dial_spin = false;
        s_sh.dial_phase = 0;
        if (!s_sh.ui_covered) {
            lv_obj_invalidate(s_sh.hero);
        }
    }
}

static void helm_hero_draw(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target_obj(e);
    lv_layer_t * layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_point_t pts[HELM_POLY_N];
    int32_t slen[HELM_POLY_N - 1];
    int32_t total;
    int32_t dist;
    int32_t win;
    int32_t px;
    int32_t py;
    lv_color_t ink;
    lv_color_t accent;
    lv_color_t track;
    int16_t span;

    if (obj == NULL || layer == NULL) {
        return;
    }

    if (s_sh.dial_poly_defer) {
        return;
    }

    lv_obj_get_coords(obj, &coords);
    if (lv_area_get_width(&coords) < 40 || lv_area_get_height(&coords) < 40) {
        return;
    }

    helm_poly_build(&coords, pts, slen, &total);
    if (total < 8) {
        return;
    }

    ink = helm_color(HELM_COLOR_INK);
    accent = helm_color(HELM_COLOR_NAV);
    track = helm_color(HELM_COLOR_NAV_FILL);
    span = helm_dial_span_now();
    dist = (total * (int32_t)span) / HELM_DIAL_SPAN;
    win = (total * HELM_DIAL_SWEEP) / HELM_DIAL_SPAN;
    if (win < 16) {
        win = 16;
    }

    helm_poly_draw_range(layer, pts, slen, total, 0, total, 2, track);

    if (s_sh.dial_lock == 1) {
        int32_t d0 = dist - win / 2;
        int32_t d1 = dist + win / 2;

        if (d0 < 0) {
            d0 = 0;
            d1 = win;
        }

        if (d1 > total) {
            d1 = total;
            d0 = total - win;
        }

        helm_poly_draw_range(layer, pts, slen, total, d0, d1, 5, accent);
    } else if (dist > 8) {
        helm_poly_draw_range(layer, pts, slen, total, 0, dist, 5, accent);
    }

    helm_dial_label(layer, pts[0].x, (lv_coord_t)(pts[0].y + 12), "0", ink);
    helm_dial_label(layer, pts[3].x, (lv_coord_t)(pts[3].y - 9), "30", ink);
    helm_dial_label(layer, pts[HELM_POLY_N - 1].x,
                    (lv_coord_t)(pts[HELM_POLY_N - 1].y + 12), "60", ink);

    helm_poly_at(pts, slen, total, dist, &px, &py);
    helm_poly_pip(layer, px, py, accent);
}

/* ==================== 待机页（主界面）信息卡 ====================
 * 几何与 zcode/analysis/helm_preview/standby.py 的 compose="tight" 一一对应
 * （该脚本是像素级预览器，常量全部由实测字宽反推）。以下坐标都是**卡片内**坐标：
 * 卡片在 pane 内 (6, 162)，尺寸 228x128 ⇒ 屏幕上 186..314。
 * 地图不透明底已由 helm_build_standby 清掉，地图满幅铺到 y=240，跟车点在 y≈132，
 * 所以卡片 (186 起) 盖不到跟车点。
 */
#define HELM_SB_CARD_X    6
/* 卡片纵向：内容 150 − pad 20 = 130
 *   上半 79 + 间隙 6 + 发丝线 1 + 间隙 6 + 动作行 34 = 126 ≤ 130 ✓（余 4）
 * 上半**必须** ≥ 日期 14 + 间隙 6 + 时钟行高 57 = 77，否则日期被挤出卡片
 * （2026-09-26 实测：动作行 24→34 吃掉 10px 后上半只剩 61，日期当场被挤掉）。
 * 卡顶 140（屏 164）仍远低于跟车点 ~132 ⇒ 不挡跟车点。 */
#define HELM_SB_CARD_Y    140
#define HELM_SB_CARD_W    (PAGE_HOR_RES - 12)
#define HELM_SB_CARD_H    150
#define HELM_SB_DIV_X     121    /* 竖直分隔线（离状态列左端 4px） */
#define HELM_SB_ST_X      125    /* 状态列左端 */
#define HELM_SB_ST_Y      8      /* 状态列首行顶端 */
#define HELM_SB_ST_STEP   22     /* 状态列行距 */
#define HELM_SB_DATE_Y    17     /* 日期行顶端 */
#define HELM_SB_TIME_Y    23     /* 40px 时钟**标签顶端**（基线 = +43） */
#define HELM_SB_HAIR_Y    82     /* 信息区/操作区分界 */
#define HELM_SB_BTN_X     12
#define HELM_SB_BTN_Y     94
#define HELM_SB_BTN_W     118
#define HELM_SB_BTN_H     24
#define HELM_SB_RING_CX   200
#define HELM_SB_RING_CY   106
#define HELM_SB_RING_D    22
#define HELM_SB_RING_BOB  4      /* 浮动振幅 px */
/* 环右缘距动作行右缘的距离。目标：环心落在**实体右键正上方**（屏 x≈198，
 * 按用户标注的照片量）。动作行右缘 = 6 + 228 − 10 = 224 ⇒ 224 − 15 − 22/2 = 198。 */
#define HELM_SB_RING_INSET 15
/* 动作行高。不能取 24（= 胶囊高）：环高 22 + 浮动 ±4 ⇒ 需要 30 才装得下，
 * 否则动画那几帧环顶会被行裁掉（用户 2026-09-26："最上方在动画过程中还是又遮挡"）。
 * 取 34 给上下各留 6px。 */
#define HELM_SB_ROW_H     34
/* 状态列**固定宽度**。原来是 LV_SIZE_CONTENT ⇒ "2 颗卫星" 变 "10 颗卫星"、
 * "上次 0.3" 变 "上次 24.7" 都会改变列宽，而分隔线与左列是按剩余空间算的，
 * 于是整条竖线跟着左右晃（用户 2026-09-26："骨架竖线不要动"）。
 * 84 = 图标槽 16 + 间隙 6 + 最宽行 62
 *      （62 要按**三位数里程**算："上次 123.4"；按两位数算 78 会让它被裁，
 *        2026-09-26 实测"上次骑行距离显示不全"就是这个）。
 * 横向总账：时钟 111 + 间隙 4×2 + 竖线 1 + 状态列 84 = 204 ≤ 卡片内容 208 ✓ */
#define HELM_SB_ST_W      84
/* helm_num_40 的 line_height - base_line = 57 - 14 = 43 */
#define HELM_SB_CLOCK_ASC 43

/** 提示环：圆环 + "按下去"（下箭头 + 底座）。
 *  chevron 的顶点必须在两端**下方**才真是向下箭头——预览器第一版把顶点写在
 *  上方，两个箭头都朝上、读作"上滑"，而本机是实体键、没有滑动手势。 */
static void helm_sb_ring_draw(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target_obj(e);
    lv_layer_t * layer = lv_event_get_layer(e);
    lv_area_t co;
    lv_draw_arc_dsc_t arc;
    lv_draw_line_dsc_t ln;
    lv_color_t col = lv_obj_get_style_text_color(obj, 0);
    int32_t cx;
    int32_t cy;

    lv_obj_get_coords(obj, &co);
    cx = (co.x1 + co.x2) / 2;
    cy = (co.y1 + co.y2) / 2;

    lv_draw_arc_dsc_init(&arc);
    arc.color = col;
    arc.width = 2;
    arc.opa = LV_OPA_COVER;
    arc.center.x = cx;
    arc.center.y = cy;
    arc.radius = (uint16_t)(lv_area_get_width(&co) / 2 - 1);
    arc.start_angle = 0;
    arc.end_angle = 360;
    lv_draw_arc(layer, &arc);

    lv_draw_line_dsc_init(&ln);
    ln.color = col;
    ln.width = 2;
    ln.opa = LV_OPA_COVER;

    ln.p1.x = cx - 4; ln.p1.y = cy - 3;      /* 下箭头左半 */
    ln.p2.x = cx;     ln.p2.y = cy + 2;
    lv_draw_line(layer, &ln);
    ln.p1.x = cx;     ln.p1.y = cy + 2;      /* 下箭头右半 */
    ln.p2.x = cx + 4; ln.p2.y = cy - 3;
    lv_draw_line(layer, &ln);
    ln.p1.x = cx - 5; ln.p1.y = cy + 6;      /* 底座 */
    ln.p2.x = cx + 5; ln.p2.y = cy + 6;
    lv_draw_line(layer, &ln);
}

static void helm_sb_bob_exec(void * var, int32_t v)
{
    if (helm_anim_is_suspended()) {
        return;
    }

    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)v, 0);
}

/* ==================== 提示环浮动：只在有交互时跑 ====================
 * 环的"活着"代价实测（WP-VERIFY2/R1）：待机静止 22×22 环 = 45.7 次失效/s
 * （占待机页 91%）、11 次/s flush、≈80 ms/s 渲染，而其中 >80% 的动画帧并不
 * 改变像素。根因是 translate 属 transform：`lv_obj_set_local_style_prop()`
 * 每次都 `lv_obj_invalidate()`（lv_obj_style.c:362-364），动画每帧都 set ⇒
 * **放慢动画/改曲线都不会减少失效**，只能少 set 或彻底不 set。
 *
 * 处置（WP-UIFIX/R1）：环仍然会动，但**只在有人看的时候动** ——
 *   · 进待机页 / 任意按键（KEY1/KEY2 短按、长按、录制）→ kick() 重新起动画；
 *   · 连续 30 s 没有任何交互且**没有运动** → stop()：删动画并把 translate_y
 *     归 0（静息位）；
 *   · 离开待机页 / 菜单等覆盖层挡住 / 熄屏挂起 → 立即 stop()。
 * 静止期失效 45.7/s → 0；交互时观感与原动画完全一致（同一条 ±4 px/1400 ms
 * ease_in_out 无限往返）。
 *
 * 恢复（WP-UIBOB/R1，用户 2026-10-30）：环冻结后**晃一下设备就重新起飘**，
 * 并把 30 s 计时重置。判据就是静止休眠现成的那条"有运动"信号
 * （helm_idle_moving()，helm_idle.c）：BMI270 raw accel 与低通滤波值的矢量
 * 残差 > 0.45 m/s² 或陀螺模 > 0.20 rad/s。挂在已有的 500 ms 空闲检查定时器上
 * ⇒ 不新开线程/定时器；只有"已过 30 s 且环已冻结"才读 IMU，静止时不多产生
 * 一次 invalidate（仍然 0/s），晃动后 ≤1 s 恢复（500 ms 轮询 + 一帧刷新；
 * 2026-10-30 上板：晃动时刻 212.2 s，212.8 s 的 1 s 窗口即见环活动）。
 */
#define HELM_SB_BOB_IDLE_MS 30000u  /**< 无交互多久后停（ms）。用户口径：环要飘，静止 30 s 才停。 */
#define HELM_SB_BOB_POLL_MS  500u   /**< 空闲检查周期（ms，本身不产生失效）。 */
/* "有运动"门限，与 helm_idle.c 的 HELM_IDLE_ACCEL_TH / HELM_IDLE_GYRO_TH 相同：
 * 桌面微振不过门，拿起/晃动会过门（该口径在静止休眠上已上板验证：桌上放 10 min
 * 不会误判为"在动"）。 */
#define HELM_SB_BOB_ACC_TH   0.45f  /**< raw−filt 加速度残差（m/s²）。 */
#define HELM_SB_BOB_GYRO_TH  0.20f  /**< 陀螺模（rad/s）。 */

static lv_timer_t * s_sb_bob_poll;      /**< 空闲检查定时器（只建一次）。 */
static uint32_t s_sb_bob_last_kick;     /**< 最近一次"有交互/刚进页"的时刻。 */

static bool helm_sb_bob_can_run(void)
{
    return s_sh.sb_ring != NULL && s_sh.sb_card != NULL &&
           !s_sh.ui_covered && !helm_anim_is_suspended() &&
           helm_obj_shown(s_sh.sb_card);
}

static void helm_sb_bob_stop(void)
{
    lv_obj_t * ring = s_sh.sb_ring;

    if (ring == NULL) {
        return;
    }

    helm_anim_stop(ring, helm_sb_bob_exec);

    /* 停在任意 offset 上会让环看起来"卡歪了" ⇒ 归静息位。
     * 同值不重设：translate 是 transform，set 一次 = 3 次 invalidate。 */
    if (lv_obj_get_style_translate_y(ring, LV_PART_MAIN) != 0) {
        lv_obj_set_style_translate_y(ring, 0, 0);
    }
}

static void helm_sb_bob_start(void)
{
    lv_anim_t a;
    lv_obj_t * ring = s_sh.sb_ring;

    if (!helm_sb_bob_can_run()) {
        return;
    }
    if (lv_anim_get(ring, helm_sb_bob_exec) != NULL) {
        return;                                  /* 已经在动 */
    }

    lv_anim_init(&a);
    lv_anim_set_var(&a, ring);
    lv_anim_set_exec_cb(&a, helm_sb_bob_exec);
    lv_anim_set_values(&a, -HELM_SB_RING_BOB, HELM_SB_RING_BOB);
    lv_anim_set_duration(&a, 700);
    lv_anim_set_playback_duration(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

/** @brief 记一次"有交互/刚进页"，并把环重新叫醒（隐藏/挂起时只记时间）。 */
static void helm_sb_bob_kick(void)
{
    s_sb_bob_last_kick = lv_tick_get();
    helm_sb_bob_start();
}

/**
 * @brief "设备在动"（晃动 / 拿起 / 骑行）—— 复制 helm_idle_moving() 的判据。
 *
 * 为什么不用速度：台上晃设备 GNSS 速度不动，只有 IMU 能反映"人在晃它"。
 * IMU 无效（无 BMI270 / 刚上电还没采到）一律按**静止**：读不到时若当成
 * "在动"，冻结后会每 500 ms 重新起飘，把 80 ms/s 的失效还回去。
 *
 * @note 只在 UI/LVGL 线程、且 30 s 空闲窗已过时调用（见 poll_cb）；板卡传感器
 *       线程每 100 ms 采一次（I2C_SENSOR_IMU_MS），低通 τ≈0.6 s（LP_A=0.16）
 *       ⇒ 一次几百毫秒的晃动在随后 1~2 s 的采样里仍留有残差，500 ms 轮询不会漏。
 */
static bool helm_sb_bob_motion(void)
{
    myvendor_sys_vec3_t raw;
    myvendor_sys_vec3_t filt;
    myvendor_sys_vec3_t gyro;
    float dx;
    float dy;
    float dz;
    float mag2;
    const float ath2 = HELM_SB_BOB_ACC_TH * HELM_SB_BOB_ACC_TH;
    const float gth2 = HELM_SB_BOB_GYRO_TH * HELM_SB_BOB_GYRO_TH;

    myvendor_board_sensor_accel_get(&raw);
    if (!raw.valid) {
        return false;
    }

    myvendor_board_sensor_accel_filt_get(&filt);
    dx = raw.x - filt.x;
    dy = raw.y - filt.y;
    dz = raw.z - filt.z;
    mag2 = dx * dx + dy * dy + dz * dz;
    if (mag2 > ath2) {
        return true;
    }

    myvendor_board_sensor_gyro_get(&gyro);
    if (gyro.valid) {
        mag2 = gyro.x * gyro.x + gyro.y * gyro.y + gyro.z * gyro.z;
        if (mag2 > gth2) {
            return true;
        }
    }

    return false;
}

static void helm_sb_bob_poll_cb(lv_timer_t * tmr)
{
    LV_UNUSED(tmr);

    if (s_sh.sb_ring == NULL) {
        return;
    }
    if (!helm_sb_bob_can_run()) {
        helm_sb_bob_stop();
        return;
    }
    if (lv_tick_elaps(s_sb_bob_last_kick) < HELM_SB_BOB_IDLE_MS) {
        return;              /* 还在 30 s 窗口里：不读 IMU、不动画、不产生失效 */
    }

    /* 窗已过、环已冻结：只有真的动了才重新起飘并重置计时（kick 内部做）。
     * 静止 ⇒ 仍走 stop()，与只有"超时即停"的旧逻辑等价（无任何失效）。
     * 每次"冻结后晃动"只打一行（量级：用户晃一次一行），便于现场核对。 */
    if (helm_sb_bob_motion()) {
        syslog(LOG_INFO, "helm: standby ring resume (motion), frozen %u ms",
               (unsigned)lv_tick_elaps(s_sb_bob_last_kick));
        helm_sb_bob_kick();
        return;
    }
    helm_sb_bob_stop();
}

/** @param parent LiveMap 页的 root（不是待机 pane）。父对象是 root ⇒ 卡片与
 *                 待机 pane 的显隐解耦，可以独立于翻页状态出现。 */
static void helm_build_standby_card(lv_obj_t * parent)
{
    lv_obj_t * card;
    lv_obj_t * top;
    lv_obj_t * lef;
    lv_obj_t * col;
    lv_obj_t * row;
    lv_obj_t * o;
    uint8_t i;

    /* ⚠ 2026-09-26 重写：第一版对子元素用 lv_obj_set_pos() **绝对定位**，上板布局
     * 全错（元素被按创建顺序竖着堆）。本文件既有写法一律用 **flex**（helm_overlay
     * 是 COLUMN、helm_col/helm_ride_row 是 flex 对齐、helm_speed_head 是 ROW
     * SPACE_BETWEEN），混用两种定位方式会被布局重排。
     * 下面全部改成嵌套 flex：外层 COLUMN → 上半 ROW（左列 | 竖线 | 状态列）→
     * 发丝线 → 操作行（CTA 左 / 提示环右，SPACE_BETWEEN 让环贴右）。
     * 唯一保留的绝对定位是卡片自己相对 pane 的位置（pane 是 flex，卡片带
     * FLOATING 被排除在布局外，这是本文件既有做法）。 */
    card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    helm_style_card(card);
    lv_obj_set_size(card, HELM_SB_CARD_W, HELM_SB_CARD_H);
    lv_obj_add_flag(card, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(card, HELM_SB_CARD_X, HELM_SB_CARD_Y);
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);   /* 由 helm_apply_view 按状态显隐 */
    s_sh.sb_card = card;
    /* 初值必须不是 0：0 的语义正好是 "00:00"，会让"时分变了才刷新"的判据
     * 在时间无效时永远为假、时钟一直空白。 */
    s_sh.sb_hm = 0xFFFFu;
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_style_pad_column(card, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

    /* 上半：左列（日期 / 40px 时钟） | 竖直分隔线 | 右列（状态列三行） */
    top = lv_obj_create(card);
    lv_obj_remove_style_all(top);
    lv_obj_set_width(top, lv_pct(100));
    lv_obj_set_flex_grow(top, 1);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    /* 间隙 8 -> 6 -> 4：时钟补冒号边距后 111、状态列固定 84，横向只剩 208，
     * 间隙必须收到 4 才放得下（总账见 HELM_SB_ST_W 的注释）。 */
    lv_obj_set_style_pad_column(top, 4, 0);

    lef = lv_obj_create(top);
    lv_obj_remove_style_all(lef);
    /* 左列**内容宽**（= 时钟宽 111，恒宽：时钟永远是 HH:MM 5 个等宽字符），
     * 不再 flex_grow。用户 2026-09-26："时间向左移动，这样左侧空间更大，右侧对应的
     * 空间也能增加"：原先左列吃剩余空间 + 时钟在其中居中 ⇒ 两侧各留 2px 白；
     * 改成内容宽后时钟贴左（cross=START），腾出的余量全给状态列。
     * 副作用是好的：分隔线位置恒等于 111+间隙，**不再随状态列数据变宽而移动**。 */
    lv_obj_set_size(lef, LV_SIZE_CONTENT, lv_pct(100));
    lv_obj_set_flex_flow(lef, LV_FLEX_FLOW_COLUMN);
    /* 横向贴左（cross=START）、整块纵向居中 */
    lv_obj_set_flex_align(lef, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(lef, 6, 0);
    /* 入场动效第一组：时钟 + 日期整块（见 helm_shell_enter_anim）。 */
    s_sh.sb_clock = lef;
    /* 日期+时钟整块**上移**（用户 2026-09-26："时间可以上移一些"）：主轴上原本
     * 只是 CENTER，改成在底部留 12px ⇒ 内容整体上移 6px。 */
    lv_obj_set_style_pad_bottom(lef, 12, 0);
    /* ⚠ 这里**不能**加 pad_right 来"左移几像素"：左列可用宽 107 = 时钟标签宽
     * 107，加 8px 内边距后内容区只剩 99 ⇒ 居中的标签左右各溢出 4px 被裁掉
     * （用户 2026-09-26："现在左侧又显示不全了"）。
     * 用户要的"以中间的 `:` 为锚点、左右对称"本来就成立：时钟是「2 位:2 位」
     * 等宽，冒号恒在字符串正中 ⇒ 只要标签不被裁、居中放置，冒号就是枢轴。 */
    /* 顺序：**时钟在上、日期在下**（用户 2026-09-26："时间在上，日期在下"）。
     * flex COLUMN 里创建顺序 = 视觉顺序，所以这两行的**先后不能随手调换**——
     * 中间那句注释里说的"时钟是枢轴"只跟水平方向有关，与先后无关。 */
    s_sh.sb_time = helm_lab(lef, &helm_num_40, HELM_COLOR_INK, "");
    s_sh.sb_date = helm_lab(lef, s_sh.font_lab, HELM_COLOR_MUTE, "");
    /* 日期**相对时钟水平居中**：左列是 cross=START（时钟贴左，这样左列恒宽 111、
     * 分隔线不随数据动），但日期只有 ~77px，跟着贴左就显得"歪"
     * （用户 2026-09-26："日期歪了"）。把它拉到列宽 100% 再让文字居中，
     * 于是它以时钟的 111px 为基准居中，而列宽不变 ⇒ 不影响分隔线。 */
    lv_obj_set_width(s_sh.sb_date, lv_pct(100));
    lv_obj_set_style_text_align(s_sh.sb_date, LV_TEXT_ALIGN_CENTER, 0);

    o = lv_obj_create(top);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 1, lv_pct(100));
    lv_obj_set_style_bg_color(o, helm_color(HELM_COLOR_HAIR), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);

    col = lv_obj_create(top);
    lv_obj_remove_style_all(col);
    /* 状态列改为吃剩余空间（原来固定 HELM_SB_ST_W=84）。左列收到内容宽后这里从
     * 84 涨到 88；固定宽度那条要求的本意——"竖线不随数据晃动"——由**左列恒宽**
     * 来保证（时钟恒 111），比钉死右列更稳。 */
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, lv_pct(100));
    lv_obj_set_style_min_width(col, HELM_SB_ST_W, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 8, 0);
    /* 入场动效第二组：状态列三行整块（见 helm_shell_enter_anim）。 */
    s_sh.sb_stat = col;

    /* 状态列三行：卫星 / 定位 / 上次距离。每行 = ROW(图标或圆点 + 文字)，
     * 图标与圆点因此自动对齐同一条竖线（第一版用绝对坐标把它甩到了左边）。 */
    for (i = 0; i < 3u; i++) {
        row = lv_obj_create(col);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_SIZE_CONTENT, 16);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);

        /* ⚠ 图标槽必须是**固定 16x16**、圆点在其中居中：原来图标 16px、圆点 7px
         * 各自按内容宽排，于是每行的文字起点都差 9px、左边缘参齐（上板照片可见，
         * 用户 2026-09-26："卫星图标下方图标竖向中心对齐，文字也要对齐"）。 */
        o = lv_obj_create(row);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, 16, 16);
        lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        if (i == 0u) {
            o = helm_icon_create(o, HELM_ICO_GPS, 16);
            helm_icon_set_color(o, helm_color(HELM_COLOR_GPS));
        } else {
            lv_obj_t * dot = lv_obj_create(o);

            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 7, 7);
            lv_obj_set_style_radius(dot, 3, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(dot, helm_color(i == 1u ? HELM_COLOR_GPS
                                                               : HELM_COLOR_MUTE),
                                      0);
        }
        s_sh.sb_st[i] = helm_lab(row, s_sh.font_lab, HELM_COLOR_MUTE, "");
    }

    /* 信息区 / 操作区 分界 */
    o = lv_obj_create(card);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, lv_pct(100), 1);
    lv_obj_set_style_bg_color(o, helm_color(HELM_COLOR_HAIR), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);

    /* 操作行：CTA（左） + 提示环（右） */
    row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), HELM_SB_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 右侧不再留内边距：环已改成 FLOATING + 显式 align（见下），位置由
     * HELM_SB_RING_INSET 决定；留 pad_right 会把 align 的基准一起推左。 */

    /* 操作条（主）：橙底 + 「长按右键 开始骑行」。
     * 文案里写"右键"（不写 KEY2——用户不知道键位名，但"右键"是位置，能懂），
     * 且不再画键帽：环的位置已经把"哪颗键"指出来了，键帽反而像第二个按钮。 */
    o = lv_obj_create(row);
    lv_obj_remove_style_all(o);
    lv_obj_set_flex_grow(o, 0);
    lv_obj_set_style_pad_hor(o, 14, 0);
    lv_obj_set_style_pad_ver(o, 0, 0);
    /* ⚠ 宽度**必须**显式 LV_SIZE_CONTENT：`remove_style_all()` 之后 lv_obj 的宽度
     * 退回类默认 LV_DPI_DEF = **130px**（helm_widget.h 里记过这个坑）。只设 height
     * 不设 width ⇒ 胶囊恒 130 宽、装不下内容 ⇒ 动作字被截成"开始"。
     * 换句话说上一版"文案太长"是误判：真因是宽度没按内容算。 */
    lv_obj_set_size(o, LV_SIZE_CONTENT, HELM_SB_BTN_H);
    lv_obj_set_style_radius(o, HELM_SB_BTN_H / 2, 0);
    lv_obj_set_style_bg_color(o, helm_color(HELM_COLOR_NAV), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, 6, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    /* 入场动效第三组：操作条胶囊本体（见 helm_shell_enter_anim）。
     * 只动胶囊、不动外层 row：row 高 34 还托管着右侧提示环（FLOATING 浮动），
     * 整行位移会连环一起推，且会把上方的发丝线让出一条缝。 */
    s_sh.sb_cta = o;
    /* 两级文字（用户 2026-09-26："「长按右键」这几字放在胶囊里，文字和之前设计
     * 一样小一号，胶囊放不下就加宽"）：前置条件 12px + 动作 15px，沿用之前
     * "键帽小、动作字大"的层级。胶囊宽度由内容决定（pad_hor 14），放不下会自己
     * 撑开——卡片内容宽 208、右侧给环留 37，所以胶囊最大可用约 171。 */
    {
        /* 前置条件放**深红键帽里**（用户照片里圈的就是这片红色区域）：键帽作
         * "这是个按键动作"的形状，文字 12px；动作字 15px 跟在后面。 */
        lv_obj_t * cap = lv_obj_create(o);

        lv_obj_remove_style_all(cap);
        /* 同上：键帽宽度也要显式 LV_SIZE_CONTENT，否则被撑成 130 宽的横条 */
        lv_obj_set_size(cap, LV_SIZE_CONTENT, 16);
        lv_obj_set_style_pad_hor(cap, 6, 0);
        lv_obj_set_style_radius(cap, 4, 0);
        lv_obj_set_style_bg_color(cap, helm_color(HELM_COLOR_INK), 0);
        lv_obj_set_style_bg_opa(cap, LV_OPA_60, 0);
        lv_obj_set_flex_flow(cap, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        /* 键帽里放**四个字**的前置条件（用户 2026-09-26："只有「长按按键」这四个字
         * 在键帽里，「开始骑行」在外面。总长度需要计算的"）。
         * 总长 = 键帽(4×12 + pad 12 = 60) + 间隙 6 + 动作字(4×15 = 60) + pad 28
         *       = 154 ≤ 可用 171（卡片内容 208 − 环 22 − 环右内缩 15）⇒ 放得下。
         * 上一版把它截断与文案长短无关，是胶囊宽度没设 LV_SIZE_CONTENT（见上）。 */
        helm_lab(cap, s_sh.font_lab, HELM_COLOR_PAPER, "长按按键");

        helm_lab(o, s_sh.font_title, HELM_COLOR_PAPER, "开始骑行");
    }

    /* 浮动指示（辅）：22px 环，面积只有操作条的 1/6 ⇒ 主次分明。
     * ⚠ 环必须 **FLOATING + 显式对齐**，不能当 flex 子元素：它的位置既要落在
     *   实体右键正上方（屏 x≈198），又要做 ±4px 的 translate_y 动画。作为 flex
     *   子元素时"布局算位置 + 动画加位移"两者叠加，环会漂到上方可见区之外
     *   （用户 2026-09-26 报："会移动到上方不可显示区域"）。
     *   FLOATING 把它排除在布局外 ⇒ 位置只由 align 决定，浮动只在这个位置上
     *   叠加 ±4px，完全确定。 */
    s_sh.sb_ring = lv_obj_create(row);
    lv_obj_remove_style_all(s_sh.sb_ring);
    lv_obj_set_size(s_sh.sb_ring, HELM_SB_RING_D, HELM_SB_RING_D);
    lv_obj_add_flag(s_sh.sb_ring, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(s_sh.sb_ring, LV_ALIGN_RIGHT_MID, -(HELM_SB_RING_INSET), 0);
    lv_obj_set_style_text_color(s_sh.sb_ring, helm_color(HELM_COLOR_NAV), 0);
    lv_obj_set_style_bg_opa(s_sh.sb_ring, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_sh.sb_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_sh.sb_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_sh.sb_ring, helm_sb_ring_draw, LV_EVENT_DRAW_MAIN, NULL);

    /* 环的浮动动画不再常驻：kick() 起、3 s 无交互由 poll 停（见上方长注释）。
     * 这里只建一次空闲检查定时器；首次 kick 由 apply_view 显卡时触发。 */
    if (s_sb_bob_poll == NULL) {
        s_sb_bob_poll = lv_timer_create(helm_sb_bob_poll_cb,
                                        HELM_SB_BOB_POLL_MS, NULL);
    }
    helm_sb_bob_kick();
}

/** @brief 有主题重建待办（重建会整批换掉三个控件，入场动画要让位，见下方）。 */
static bool s_theme_rebuild_pending;
/** @brief 等重建期间"万一重建没来"的兜底定时器（一次性，见 helm_shell_enter_anim）。 */
static lv_timer_t * s_sb_in_wait;
/** @brief 本次页面出现期间三组是否**已经亮过**（正常播完 / 兜底落末态）。
 *  重建会换掉控件，只有"还没亮过"的那一次才需要补播入场；已经亮过就只让新控件
 *  直接可见 —— 否则重建会把已经显示的内容再淡入一遍（"出现→消失→再出现"）。 */
static bool s_sb_in_visible;

/* ==================== 主界面入场动效 ====================
 * 用户 2026-10-28："进入主界面，主界面的各个控件无动画进入退出，所以生硬"。
 * 每组 opa 0→255 与 translate_y +12→0 由同一条动画同步走 200 ms（ease_out），
 * 首组再等 40 ms ⇒ 首波 40~240 ms；状态列再错 80 ms ⇒ 120~320 ms。
 *
 * **分组（2026-10-30 改）**：时钟块 + 操作条同一波（40~240 ms），状态列第二波
 * （120~320 ms）。原先顺序是"时钟块 → 状态列 → 操作条"，用户 2026-10-30
 * 报"从菜单返回主界面时『长按按键 / 开始骑行』最后才出现，中间卡一下"：
 * 实测 CTA 首次可见 208 ms、走完 400 ms，而**页面级 ENTER fade 在 240 ms 结束**
 * （lv_pm 关闭菜单时给主界面页播 open_options 里的 220 ms fade，整页每帧重绘），
 * 于是 CTA 恰好落在整流 fade 之后单独冒出，观感就是"别人都到了它还卡一下"。
 * CTA 是主动作，必须和卡片/时钟同波（同一波结束 = 240 ms）；状态列是小字信息，
 * 留在第二波不影响主次，也保留错峰。
 *
 * **视觉上只播一轮**：触发点两处 —— 一是页面出现（`map_will_appear`），二是
 * 主题重建末尾（`helm_shell_rebuild_once`，旧控件连同动画被删、新控件要重新入场）。
 * 两者靠 `s_theme_rebuild_pending` 互斥：有待重建时页面出现这一轮**保持起始透明态**
 * （不亮出来，避免"先出现再消失"），由重建那一轮播；没有待重建时正常播。
 * 若重建始终没来，500 ms 兜底定时器把三组落末态。周期刷新不会调用本函数。
 *
 * @note 错峰用 lv_anim 自带的 delay（helm_anim_run 没有 delay 参数，故此处直接
 *       起 lv_anim）；一条动画跑完即自然释放，不留定时器、不做逐帧判断。
 * @note 挂起（熄屏/待机）时不启动，且显式落到末态：绝不停在 opa=0。
 * @note 回位靠 translate_y 归 0：这三个对象的 x/y 由 flex 布局拥有，translate
 *       只被本动效使用 ⇒ 归 0 就是"回到动画前的位置"。
 */
#define HELM_SB_IN_MS   200u   /**< 每组时长 ms（opa 与位移共用一条动画）。 */
#define HELM_SB_IN_GAP  80u    /**< 组间错开 ms。 */
#define HELM_SB_IN_DLY  40u    /**< 第一组延迟 ms：让页面底色/页面级 fade 先落定。 */
#define HELM_SB_IN_DY   12     /**< 入场起始下移量 px。 */

/** @brief 入场 exec：一个 0→255 的进度同时驱动不透明度与下移量。 */
static void helm_sb_in_exec(void * var, int32_t v)
{
    lv_obj_t * obj = (lv_obj_t *)var;
    int32_t dy;

    if (helm_anim_is_suspended()) {
        return;
    }

    if (v < 0) {
        v = 0;
    } else if (v > 255) {
        v = 255;
    }

    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
    /* v=0 ⇒ 全透明且下移 HELM_SB_IN_DY(12)px；v=255 ⇒ 归位。四舍五入到整像素。 */
    dy = ((255 - v) * HELM_SB_IN_DY + 127) / 255;
    lv_obj_set_style_translate_y(obj, (lv_coord_t)dy, 0);
}

/** @brief 入场末态：显式复位（全不透明 + 原位），不依赖最后一帧是否被挂起。 */
static void helm_sb_in_settle(lv_obj_t * obj)
{
    if (obj == NULL) {
        return;
    }

    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_y(obj, 0, 0);
}

static void helm_sb_in_done(lv_anim_t * a)
{
    if (a != NULL) {
        helm_sb_in_settle((lv_obj_t *)a->var);
    }
}

/** @brief 兜底：等重建超时仍没来 ⇒ 把三组落末态，别让它们一直是透明的。 */
static void helm_sb_in_wait_cb(lv_timer_t * t)
{
    lv_obj_t * objs[3];

    if (t != NULL && t == s_sb_in_wait) {
        s_sb_in_wait = NULL;
    }

    objs[0] = s_sh.sb_clock;
    objs[1] = s_sh.sb_stat;
    objs[2] = s_sh.sb_cta;

    /*
     * 只有"重建始终没来"（pending 仍为真）才需要兜底；pending 已被
     * `helm_shell_theme_apply()` 清掉，说明重建接手了、它的入场那一轮正在跑，
     * 这里落末态会把动画打断。
     */
    if (!s_theme_rebuild_pending) {
        return;
    }

    for (uint32_t i = 0; i < 3u; i++) {
        helm_anim_stop(objs[i], helm_sb_in_exec);
        helm_sb_in_settle(objs[i]);
    }
    s_sb_in_visible = true;
}

/** @brief 起一组：先清同对象旧动画并同步落起始态，再走一条带 delay 的 lv_anim。 */
static void helm_sb_in_run(lv_obj_t * obj, uint32_t delay)
{
    lv_anim_t a;

    if (obj == NULL) {
        return;
    }

    /* 等同 helm_anim_run 的约定：同 var + exec 的旧动画先删。 */
    helm_anim_stop(obj, helm_sb_in_exec);
    /* 起始态同步落位：上一轮若被打断（快速退出/换主题重建），
     * 避免先闪一帧旧位置/旧透明度再从头走。 */
    helm_sb_in_exec(obj, 0);

    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, HELM_SB_IN_MS);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, helm_sb_in_exec);
    lv_anim_set_completed_cb(&a, helm_sb_in_done);
    if (lv_anim_start(&a) == NULL) {
        /* 动画对象分配失败（内存紧张）⇒ 没有 completed 回调，必须立刻落末态，
         * 否则这一组会永久停在 opa=0（周期刷新不会重跑入场）。 */
        helm_sb_in_settle(obj);
    }
}

/**
 * @brief 主界面入场动效（幂等，可重复调用）。
 *
 * @details 触发点两处：① `map_will_appear()`（开机 splash 关闭后进主界面、
 *          从菜单/USB/工厂页回到主界面）；② `helm_shell_rebuild_once()` 末尾
 *          （换主题把控件换新后重播）。两处互斥，见文件里的 `s_theme_rebuild_pending` 说明。
 * @note 卡片此刻不可见（骑行页）或屏幕挂起：不播，但必须落到末态 ——
 *       下次显示时不能是一块透明的卡片。
 */
void helm_shell_enter_anim(void)
{
    static const uint32_t delays[3] = {
        /* 顺序与 objs[] 对齐：0=时钟块、1=状态列、2=操作条。 */
        HELM_SB_IN_DLY,                              /* clock 40~240 */
        HELM_SB_IN_DLY + HELM_SB_IN_GAP,             /* stat 120~320 */
        HELM_SB_IN_DLY                               /* cta   40~240（与时钟同波） */
    };
    lv_obj_t * objs[3];
    uint32_t i;

    if (!s_sh.attached) {
        return;
    }

    objs[0] = s_sh.sb_clock;
    objs[1] = s_sh.sb_stat;
    objs[2] = s_sh.sb_cta;

    /*
     * 有待办主题重建：重建会把这三个对象整批换新（`helm_shell_rebuild_once` 末尾
     * 还会再调一次本函数，那一轮才真正播）。这一轮**不能**落末态：页面 220 ms
     * 淡入会把旧控件先亮出来，随后重建换新、新控件再从 0 淡入 ⇒ 内容"出现→消失→
     * 再出现"。所以这里保持它们停在**起始透明态**（opa=0、下移），让页面淡入时
     * 这块本来就没内容。
     *
     * 兜底：万一重建没来（不在主界面/转场一直没结束），一次性定时器在 500 ms 后
     * 落末态，绝不把时钟/状态列/操作条永久留在透明。
     */
    if (s_theme_rebuild_pending) {
        s_sb_in_visible = false;
        for (i = 0; i < 3u; i++) {
            helm_anim_stop(objs[i], helm_sb_in_exec);
            helm_sb_in_exec(objs[i], 0);
        }

        if (s_sb_in_wait == NULL) {
            s_sb_in_wait = lv_timer_create(helm_sb_in_wait_cb, 500, NULL);
            if (s_sb_in_wait != NULL) {
                lv_timer_set_repeat_count(s_sb_in_wait, 1);
            } else {
                /* 定时器都建不出来（内存紧张）：没有兜底就必须当场落末态，
                 * 否则重建万一不来，这三块会永久透明。 */
                for (i = 0; i < 3u; i++) {
                    helm_sb_in_settle(objs[i]);
                }
                s_sb_in_visible = true;
            }
        }
        return;
    }

    /* 有兜底在等 ⇒ 这一轮是重建后的真正入场，撤掉它（否则它会在 500 ms 处
     * 把动画中途落末态）。 */
    if (s_sb_in_wait != NULL) {
        lv_timer_delete(s_sb_in_wait);
        s_sb_in_wait = NULL;
    }

    if (!helm_obj_shown(s_sh.sb_card) || helm_anim_is_suspended()) {
        for (i = 0; i < 3u; i++) {
            helm_anim_stop(objs[i], helm_sb_in_exec);
            helm_sb_in_settle(objs[i]);
        }
        return;
    }

    s_sb_in_visible = true;
    for (i = 0; i < 3u; i++) {
        helm_sb_in_run(objs[i], delays[i]);
    }
}

/** 待机页信息卡每拍刷新：40px 时钟 + 状态列三行。 */
static void helm_standby_card_tick(const bicycle_runtime_t * rt)
{
    char buf[32];
    time_t now = time(NULL);
    struct tm tmv;
    bool have = false;
    int hour = 0;
    int min = 0;
    int mon = 0;
    int day = 0;
    int wday = 0;

    /* 本地时间：系统时间是 UTC，时区偏移在 devctl 里（tz_hms 同源）。
     * 这里要的不只是时分还要年月日/星期，所以自己做一次偏移 + gmtime_r。 */
    if (now >= 1704067200 && gmtime_r(&(time_t){(time_t)((long)now
            + (long)myvendor_devctl_tz_min_get() * 60)}, &tmv) != NULL) {
        have = true;
        hour = tmv.tm_hour;
        min = tmv.tm_min;
        mon = tmv.tm_mon + 1;
        day = tmv.tm_mday;
        wday = (tmv.tm_wday >= 0 && tmv.tm_wday < 7) ? tmv.tm_wday : 0;
    }

    /* **时间无效也必须显示**（用户 2026-09-26："时间即使不正常也得显示，和静止界面
     * 一样"）：没对上星/没设置时给 00:00，和状态栏同样的兜底。
     * ⚠ 配套：sb_hm 初值必须是 0xFFFF，不能是 0——0 的语义恰好就是 "00:00"，
     * 于是"hm != sb_hm"永远为假、标签一次都不会被设、一直空白（上板实测）。 */
    {
        const uint16_t hm = (uint16_t)(hour * 60 + min);

        if (hm != s_sh.sb_hm) {
            static const char * wd[7] = { "周日", "周一", "周二", "周三",
                                          "周四", "周五", "周六" };

            s_sh.sb_hm = hm;
            if (have) {
                lv_snprintf(buf, sizeof(buf), "%d月%d日 %s", mon, day, wd[wday]);
            } else {
                lv_snprintf(buf, sizeof(buf), "--月--日");
            }
            helm_label_set(s_sh.sb_date, buf);
            lv_snprintf(buf, sizeof(buf), "%02d:%02d", hour, min);
            helm_label_set(s_sh.sb_time, buf);
            /* 半反屏局部刷新会留灰影 / 闪一下（用户 2026-09-26："和静止界面一样，
             * 闪烁，消失时能看到灰色"；旧静止页同样如此 ⇒ 属既有的面板特性，
             * 不是这轮引入的）。缓解：时间一变就把**整个左列**（日期 + 时钟）一起
             * 失效重画，让新数字落在一块干净重绘的区域上，而不是只补字形那一小块。 */
            if (s_sh.sb_date != NULL) {
                lv_obj_invalidate(lv_obj_get_parent(s_sh.sb_date));
            }
        }
    }

    if (rt != NULL) {
        lv_snprintf(buf, sizeof(buf), "%u 颗卫星",
                    (unsigned)rt->gnss_satellites);
        helm_label_set(s_sh.sb_st[0], buf);
        helm_label_set(s_sh.sb_st[1], rt->gnss_valid ? "已定位"
                                                     : "未定位");
        if (s_sh.last_km > 0.05f) {
            /* **取整、不留小数**（用户 2026-09-26："去掉小数点吧"）。
             * 宽度实测（mism4_12）：
             *   "上次 25 km"  ≈ 61px ⇒ 状态列需 83 ≤ 固定宽 84 ✅
             *   "上次 128 km" ≈ 71px ⇒ 需 93 ❌ 三位数带单位放不下
             *   "上次 128"    ≈ 52px ⇒ 需 74 ✅
             * ⇒ 里程 ≥100 km 时**省掉单位**：宁可少个单位，也不要把字裁掉。
             *   （单位不能无条件省：「公里/英里可切」所以数字离开单位就读不出来。） */
            const int kmi = (int)(s_sh.last_km + 0.5f);

            if (kmi < 100) {
                lv_snprintf(buf, sizeof(buf), "上次 %d km", kmi);
            } else {
                lv_snprintf(buf, sizeof(buf), "上次 %d", kmi);
            }
        } else {
            lv_snprintf(buf, sizeof(buf), "上次 -- km");
        }
        helm_label_set(s_sh.sb_st[2], buf);
    }
}

static void helm_build_standby(lv_obj_t * root)
{
    lv_obj_t * hero;
    lv_obj_t * row;
    lv_obj_t * hint;

    s_sh.standby = helm_overlay(root, 0, HELM_PAGE_H);

    /* ⚠ 2026-09-26 回退：这里曾把底色清成透明、想让下面的地图透出来 ⇒ 上板花屏
     * （待机这条路的地图画布从没被绘制过）。地图区共用那件事要靠"待机是地图页的
     * 一个状态"来实现，不是靠透明 pane 硬叠。 */

    /* 待机页仍然是**不透明满页底**，信息卡叠在它上面。 */

    /* Speed sits under a polyline ridge; orange chunk + pip travel the path. */
    hero = lv_obj_create(s_sh.standby);
    lv_obj_remove_style_all(hero);
    helm_grow_y(hero);
    lv_obj_set_style_bg_opa(hero, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(hero, 40, 0);
    lv_obj_set_style_pad_bottom(hero, 8, 0);
    lv_obj_set_style_pad_row(hero, 0, 0);
    lv_obj_add_flag(hero, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hero, helm_hero_draw, LV_EVENT_DRAW_MAIN, NULL);
    s_sh.hero = hero;

    s_sh.speed_big = helm_lab(hero, s_sh.font_speed, HELM_COLOR_NAV, "0.0");
    lv_obj_set_width(s_sh.speed_big, lv_pct(100));
    lv_obj_set_style_text_align(s_sh.speed_big, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(s_sh.speed_big, -1, 0);
    lv_obj_add_flag(s_sh.speed_big, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    s_sh.speed_unit = helm_lab(hero, s_sh.font_lab, HELM_COLOR_NAV, "km/h");
    lv_obj_set_width(s_sh.speed_unit, lv_pct(100));
    lv_obj_set_style_text_align(s_sh.speed_unit, LV_TEXT_ALIGN_CENTER, 0);

    s_sh.hero_mode = helm_lab(hero, s_sh.font_lab, HELM_COLOR_INK, "0Hz");
    lv_obj_set_width(s_sh.hero_mode, lv_pct(100));
    lv_obj_set_style_text_align(s_sh.hero_mode, LV_TEXT_ALIGN_CENTER, 0);

    row = lv_obj_create(s_sh.standby);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), HELM_ROW2_H);
    lv_obj_set_flex_grow(row, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, HELM_GAP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    {
        lv_obj_t * lab;

        helm_col(row, &lab, &s_sh.last_dist, s_sh.font_val);
        lv_label_set_text_static(lab, "上次里程");
        lv_label_set_text_static(s_sh.last_dist, "-- km");

        helm_col(row, &lab, &s_sh.gps_val, s_sh.font_val);
        lv_label_set_text_static(lab, "GPS");
        lv_label_set_text_static(s_sh.gps_val, "无定位");
    }

    hint = lv_obj_create(s_sh.standby);
    lv_obj_remove_style_all(hint);
    helm_style_card(hint);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_height(hint, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(hint, 0);
    lv_obj_set_flex_flow(hint, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_ver(hint, 8, 0);
    lv_obj_set_style_pad_hor(hint, 10, 0);
    lv_obj_set_flex_align(hint, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    s_sh.standby_hint = helm_lab(hint, s_sh.font_title, HELM_COLOR_NAV, "待开始");
    lv_obj_set_width(s_sh.standby_hint, lv_pct(100));
    lv_obj_set_style_text_align(s_sh.standby_hint, LV_TEXT_ALIGN_CENTER, 0);

    /* 旧的英雄区（折线脊 + 大速度 + 模式）/两格（上次里程、GPS）/提示条整组隐藏：
     * 刷新逻辑对这些指针仍有非空的判断，隐藏后一行都不用改、也不会 NULL 崩溃
     * （helm_paint_speed_pair 等函数直接调 lv_obj_set_style_* 不做 NULL 判断）。
     * 它们承载的信息已由下面的状态列与操作条接管；物理删除留作后续清理。 */
    lv_obj_add_flag(hero, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);

    /* 信息卡挂在 LiveMap 的 root 上，与待机 pane 解耦 */
    helm_build_standby_card(root);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 骑行页：新表盘（HELM_PAGE_DATA，WP-HELM10/R1-S10）
 *
 * 几何 1:1 搬模拟器 `lvgl_sim/src/scene_helm.c` 的 ride_build_dial()/
 * ride_refresh_dial()/ride_elev_strip()，只减状态栏 24px（页面根从屏幕
 * y=24 开始）：
 *   弧心 (120,82) d=152 粗 7；hero (40,40,160,72)；速度带 y=160、心率带
 *   y=199、四格 y=218、剖面基线 y=250、画布 (26,264,190,28)。
 * 数据只读 `bicycle_runtime`；剖面走**老 CLIMB 数据链**（s_climb_eles /
 * s_climb_zone / helm_climb_profile_reload），拿不到的心率/踏频/海拔显示
 * "--"，剖面不足 2 点只留基线 + 当前位置竖线 + "--"。
 * ═══════════════════════════════════════════════════════════════════════════ */

#define HELM_DIAL_CX        120
#define HELM_DIAL_CY        82          /* 屏幕 106 − 24 */
#define HELM_DIAL_D         152
#define HELM_SPD_BAR_X      24
#define HELM_SPD_BAR_Y      160         /* 屏幕 184 − 24 */
#define HELM_SPD_BAR_W      192
#define HELM_SPD_BAR_H      18
#define HELM_SPD_LAB_Y      144         /* 0/30/60 挪到带**上方**；再上移 2px 给
                                         * 探出带外的工字滑杆（±2px）让位 */
#define HELM_HR_BAR_X       24
#define HELM_HR_BAR_Y       199         /* 屏幕 223 − 24 */
#define HELM_HR_BAR_W       192
#define HELM_HR_BAR_H       18
/* 指针改圆形滑块（用户 2026-11-01："表示心率和速度的竖线不明显"）：
 * 2px 竖线在半反屏上被彩色带吞掉，10px 圆 + 1px PAPER 描边在任何底色上都读得出。 */
#define HELM_BAR_PTR_W      10
#define HELM_BAR_PTR_H      10
/** @brief 剖面"当前位置"竖条：加宽到 4px（同样因为 2px 太细）。 */
#define HELM_ELEV_CUR_W     4
#define HELM_DIAL_ELEV_W    190
#define HELM_DIAL_ELEV_H    28
#define HELM_DIAL_ELEV_X    26
#define HELM_DIAL_ELEV_Y    264         /* 屏幕 288 − 24 */
/** @brief 导航态剖面窗口总长（米）：游标从左端走到 1/3 时刷新一次。
 *  @details 用户 2026-11-01："预瞄 4 km、2 km 更新一次" ⇒ 窗口 6 km、刷新间隔 2 km
 *           ⇒ 任何时刻前方至少可见 **4 km**。 */
#define HELM_DIAL_NAV_TOTAL_M    6000.0
/** @brief 导航态剖面重采样间隔（米）。两次刷新之间只移动游标 + 灰化已骑段。 */
#define HELM_DIAL_NAV_REFRESH_M  2000.0
/** @brief 剖面距离分格：竖线把剖面切成若干**等里程**格（用户 2026-11-04：
 *         "最好做成一个一个竖向的线隔开，每段代表一个距离"）。
 *  @details 间距从阶梯（100 m / 200 m / 250 m / 500 m / 1 km / 2 km / 5 km）里挑
 *           第一个满足"格数 ≤ 6"的 ⇒ 导航窗口恒 6 km = **1 km/格**；记录窗口随
 *           速度变（10 分钟能骑多远），跨过阈值才换档。窗口 <200 m 不分格。 */
#define HELM_DIAL_GRID_MAX_SEG   6
/** @brief 剖面「当前位置」倒三角的宽/高（px）：尖端朝下贴折线，整个三角在
 *         折线之上（老剖面页 15×10 的缩小版 —— 这条剖面只有 28 px 高）。 */
#define HELM_DIAL_MARK_W         9
#define HELM_DIAL_MARK_H         6
#define HELM_DIAL_PH_X      24
#define HELM_DIAL_PH_Y      252
#define HELM_DIAL_PH_W      192
#define HELM_DIAL_PH_H      40

/** @brief 剖面画布（RGB565）：自己管内存，换主题重建时重新挂到新画布上。 */
static uint8_t * s_dial_cbuf;
/** @brief 上次画进画布的剖面窗口（用来避免每拍重画）。 */
/** @brief 表盘剖面条的窗口数据（导航 = 前方 6 km；记录 = 最近 10 分钟）。 */
static int16_t s_dial_win[HELM_CLIMB_PROF_N];
/** @brief 上次烘到画布上的窗口指纹（导航 = 版本号 + 游标像素；记录 = 采样槽）。 */
static uint32_t s_dial_prof_sig;
static bool s_dial_prof_valid;
/** @brief 上次烘进画布的每格里程（米）：0 = 不分格。分格换档不属于 sig
 *        的口径（sig 的位被游标/版本号占满），单独比一次。 */
static uint32_t s_dial_prof_grid;
/** @brief 演示用的"当前位置"覆盖（像素，-1 = 用真实沿路线位置）。 */
static int s_dial_cursor_ovr = -1;
/** @brief 导航剖面窗口状态：起点（米）/ 总长 / 跨距 / 点数 / 步长 / 版本号。
 *  窗口 = [base, base+6 km]，**每前进 2 km 重采样一次**；两次刷新之间只移动
 *  游标、把游标左侧灰化，不重采路线。 */
static double s_dial_nav_base = -1.0;
static double s_dial_nav_total = -1.0;
static double s_dial_nav_span;
static uint32_t s_dial_nav_step_cm;
static uint32_t s_dial_nav_ver;
static uint8_t s_dial_nav_n;

/** @brief 页面根坐标版的 trig（与待机页 hero 同一算法，不引 libm）。 */
static int helm_dial_trig(int32_t v, int r)
{
    int64_t p = (int64_t)v * r;

    if (p >= 0) {
        return (int)((p + 16384) / 32767);
    }
    return (int)((p - 16384) / 32767);
}

/** @brief 角度（0 = 12 点，顺时针）→ 页面根坐标。 */
static void helm_dial_pt(int deg, int r, int * x, int * y)
{
    *x = HELM_DIAL_CX + helm_dial_trig(lv_trigo_sin((int16_t)deg), r);
    *y = HELM_DIAL_CY - helm_dial_trig(lv_trigo_cos((int16_t)deg), r);
}

/** @brief 整页纸底 pane（绝对定位；不套 flex 容器，坐标即页面坐标）。 */
static lv_obj_t * helm_page_pane(lv_obj_t * root)
{
    lv_obj_t * p = lv_obj_create(root);

    lv_obj_remove_style_all(p);
    helm_style_paper(p);
    lv_obj_set_size(p, PAGE_HOR_RES, HELM_PAGE_H);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

/** @brief 纯色小方块（色带/刻度条/指针都走它）。 */
static lv_obj_t * helm_dial_box(lv_obj_t * parent, int x, int y, int w, int h,
                                uint32_t col, lv_opa_t opa, int radius)
{
    lv_obj_t * b = lv_obj_create(parent);

    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, helm_color(col), 0);
    lv_obj_set_style_bg_opa(b, opa, 0);
    lv_obj_set_style_radius(b, radius, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

/* =====================================================================
 * 指示器：**填充前沿**（2026-11-01 第七版，Codex 复核后的方案一）
 *
 * 用户连续否掉 圆球 / 尖角 / 工字 / 胶囊取景框："不要简单的字符或者箭头糊弄"。
 * 这次不再画"一个标记"，而是把 18px 的带子拆成两层：
 *   · 上层 14px = 状态区：从带左端**填充到当前值**，颜色沿经过的分区深色；
 *     填充的右缘是一条 2px 墨线（"前沿"）——位置由填充区的边缘定义；
 *   · 下层 4px = 分区底尺：始终完整显示三段/五段的深色条（已有）。
 * 心率带撤掉"当前分区 1px 框"（避免两套定位语言）；剖面同理：已骑段是整块
 * 灰域，交界是一条 2px 墨线，不再放任何独立标记。
 * ===================================================================== */

/** @brief 心率分区（WP 口径 Z1..Z5：<100 / 100-119 / 120-139 / 140-159 / ≥160）。 */
static int helm_dial_hr_zone(int bpm)
{
    if (bpm < 100) {
        return 1;
    }
    if (bpm < 120) {
        return 2;
    }
    if (bpm < 140) {
        return 3;
    }
    if (bpm < 160) {
        return 4;
    }
    return 5;
}

/** @brief 心率指针落点（2px 宽，落在 x..x+1）：五格等宽、格内按区间线性细分。 */
static int helm_dial_hr_x(int bpm)
{
    int32_t u;

    if (bpm < 60) {
        bpm = 60;
    }
    if (bpm > 200) {
        bpm = 200;
    }
    if (bpm < 100) {
        u = (int32_t)(bpm - 60) * 1000 / 40;
    } else if (bpm < 120) {
        u = 1000 + (int32_t)(bpm - 100) * 1000 / 20;
    } else if (bpm < 140) {
        u = 2000 + (int32_t)(bpm - 120) * 1000 / 20;
    } else if (bpm < 160) {
        u = 3000 + (int32_t)(bpm - 140) * 1000 / 20;
    } else {
        u = 4000 + (int32_t)(bpm - 160) * 1000 / 40;
    }
    {
        /* 返回**尖端位置**（带内 0..W）；对象 x = 返回值 − 5（游标宽 10）。 */
        int x = HELM_HR_BAR_X +
                (int)(((int32_t)HELM_HR_BAR_W * u + 2500) / 5000);

        if (x < HELM_HR_BAR_X) {
            x = HELM_HR_BAR_X;
        }
        if (x > HELM_HR_BAR_X + HELM_HR_BAR_W) {
            x = HELM_HR_BAR_X + HELM_HR_BAR_W;
        }
        return x;
    }
}

/** @brief 把剖面画布挂到（可能刚重建的）画布对象上；失败返回 false。 */
static bool helm_dial_canvas_attach(lv_obj_t * cv)
{
    if (cv == NULL) {
        return false;
    }
    if (s_dial_cbuf == NULL) {
        s_dial_cbuf = vmap_malloc((size_t)HELM_DIAL_ELEV_W *
                                  (size_t)HELM_DIAL_ELEV_H * 2u);
    }
    if (s_dial_cbuf == NULL) {
        lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
        return false;
    }
    lv_canvas_set_buffer(cv, s_dial_cbuf, HELM_DIAL_ELEV_W,
                         HELM_DIAL_ELEV_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(cv, HELM_DIAL_ELEV_W, HELM_DIAL_ELEV_H);
    return true;
}

/** @brief 剖面显示层：平滑曲线 / 逐段 zone / 连续带（只服务这条 strip）。 */
static int16_t s_dial_smooth[HELM_CLIMB_PROF_N];
static uint8_t s_dial_seg_zone[HELM_CLIMB_PROF_N];
static uint8_t s_dial_run_start[HELM_CLIMB_PROF_N];
static uint8_t s_dial_run_len[HELM_CLIMB_PROF_N];
static uint8_t s_dial_run_zone[HELM_CLIMB_PROF_N];
static uint8_t s_dial_run_n;
static uint8_t s_dial_run_min;

/** @brief 剖面档 → 浅填充色：平路灰 / 中坡绿 / 陡坡红（S9 口径，现有 elev_*）。 */
/** @brief 剖面档 → 填充色（绿=平路 / 黄=缓坡 / 红=陡坡）。
 *  @note 2026-11-01 修正：原来 z==0 走的是 `ELEV_DONE`（灰，"已骑过"色），
 *        而 `helm_climb_grade_zone()` 的 0 档是**平路** ⇒ 平路多一点的行程
 *        整条剖面都是灰的（用户："有海拔数据，显示为什么是灰色"）。
 *        这里改成与海拔页同一套（0 绿 / 1 黄 / 2 红）。
 */
static uint32_t helm_dial_zone_fill(uint8_t z)
{
    if (z == 1u) {
        return HELM_COLOR_ELEV_MID;
    }
    if (z == 2u) {
        return HELM_COLOR_ELEV_STEEP;
    }
    return HELM_COLOR_ELEV_FLAT;
}

/** @brief 剖面档 → 2px 脊线色（与填充同族的深色）。 */
static uint32_t helm_dial_zone_ink(uint8_t z)
{
    if (z == 1u) {
        return HELM_COLOR_ELEV_MID_INK;
    }
    if (z == 2u) {
        return HELM_COLOR_ELEV_STEEP_INK;
    }
    return HELM_COLOR_ELEV_FLAT_INK;
}

/** @brief 剖面显示层 5 点滑动平均（两端窗口收窄；不改数据链）。 */
static void helm_dial_smooth5(const int16_t *src, uint8_t n)
{
    uint8_t i;

    for (i = 0; i < n; i++) {
        int lo = (int)i - 2;
        int hi = (int)i + 2;
        int sum = 0;
        int cnt = 0;
        int k;

        if (lo < 0) {
            lo = 0;
        }
        if (hi > (int)n - 1) {
            hi = (int)n - 1;
        }

        /* **跳过未知点**：`VGRF_ELE_UNKNOWN` 是 -32768，直接参与平均会把
         * 5 个点一起拉成 -16000 量级 —— 轴是按已知点算的，这些点随后会被
         * 映射到画布外，画出一根通高色柱（路线剖面开头没海拔时必然出现）。 */
        for (k = lo; k <= hi; k++) {
            if (src[k] == (int16_t)VGRF_ELE_UNKNOWN) {
                continue;
            }
            sum += src[k];
            cnt++;
        }

        s_dial_smooth[i] = (cnt > 0)
            ? (int16_t)((sum + cnt / 2) / cnt)
            : (int16_t)VGRF_ELE_UNKNOWN;
    }
}

/**
 * @brief 把平滑后仍未知的点用最近一个已知值填平（前导未知 → 后一个已知值）。
 * @note 整窗都未知时填 0，画成一条平线（轴线本身会给出 0..100 的兜底）。
 */
static void helm_dial_fill_unknown(uint8_t n)
{
    int i;
    int16_t last = (int16_t)VGRF_ELE_UNKNOWN;

    for (i = 0; i < (int)n; i++) {
        if (s_dial_smooth[i] == (int16_t)VGRF_ELE_UNKNOWN) {
            if (last != (int16_t)VGRF_ELE_UNKNOWN) {
                s_dial_smooth[i] = last;
            }
        } else {
            last = s_dial_smooth[i];
        }
    }

    for (i = (int)n - 1; i >= 0; i--) {
        if (s_dial_smooth[i] == (int16_t)VGRF_ELE_UNKNOWN) {
            if (last != (int16_t)VGRF_ELE_UNKNOWN) {
                s_dial_smooth[i] = last;
            }
        } else {
            last = s_dial_smooth[i];
        }
    }

    for (i = 0; i < (int)n; i++) {
        if (s_dial_smooth[i] == (int16_t)VGRF_ELE_UNKNOWN) {
            s_dial_smooth[i] = 0;
        }
    }
}

/**
 * @brief 显示层分区：逐段坡度 → 滞回（连续 2 段同档才换区）→ 合并 <3 段短带。
 * @param wn 窗口点数（≥2）。
 * @param step_cm 相邻点水平距离（厘米）；0 = 无距离，全按平路（灰）。
 * @details 只改这条 strip 的绘制，不改 `s_climb_zone` / 数据链 / CLIMB 页。
 */
static void helm_dial_runs_build(uint8_t wn, uint32_t step_cm)
{
    uint8_t raw[HELM_CLIMB_PROF_N];
    uint8_t hz[HELM_CLIMB_PROF_N];
    uint8_t i;

    s_dial_run_n = 0;
    s_dial_run_min = 0;
    if (wn < 2u) {
        return;
    }

    for (i = 0; i + 1u < wn; i++) {
        raw[i] = helm_climb_grade_zone(s_dial_smooth[i],
            s_dial_smooth[i + 1u], step_cm);
    }

    /* 滞回：只有 raw 连续两段都是新档才换；孤立 1 段毛刺被吸收。 */
    hz[0] = raw[0];
    for (i = 1; i + 1u < wn; i++) {
        if (raw[i] != hz[i - 1u] && raw[i] == raw[i - 1u]) {
            hz[i] = raw[i];
        } else {
            hz[i] = hz[i - 1u];
        }
    }

    /* 连续带。 */
    i = 0;
    while (i + 1u < wn) {
        uint8_t j = (uint8_t)(i + 1u);

        while (j + 1u < wn && hz[j] == hz[i]) {
            j++;
        }
        if (s_dial_run_n < HELM_CLIMB_PROF_N) {
            s_dial_run_start[s_dial_run_n] = i;
            s_dial_run_len[s_dial_run_n] = (uint8_t)(j - i);
            s_dial_run_zone[s_dial_run_n] = hz[i];
            s_dial_run_n++;
        }
        i = j;
    }

    /* <3 段的短带并入邻带：首带并入后一带，其余并入前一带。 */
    while (s_dial_run_n > 1u) {
        uint8_t idx = 0xffu;
        bool found = false;

        for (i = 0; i < s_dial_run_n; i++) {
            if (s_dial_run_len[i] < 3u) {
                idx = i;
                found = true;
                break;
            }
        }
        if (!found) {
            break;
        }
        if (idx == 0u) {
            s_dial_run_start[1] = s_dial_run_start[0];
            s_dial_run_len[1] = (uint8_t)(s_dial_run_len[0] +
                                          s_dial_run_len[1]);
            for (i = 1; i < s_dial_run_n; i++) {
                s_dial_run_start[i - 1u] = s_dial_run_start[i];
                s_dial_run_len[i - 1u] = s_dial_run_len[i];
                s_dial_run_zone[i - 1u] = s_dial_run_zone[i];
            }
            s_dial_run_n--;
        } else {
            s_dial_run_len[idx - 1u] = (uint8_t)(s_dial_run_len[idx - 1u] +
                                                 s_dial_run_len[idx]);
            for (i = idx + 1u; i < s_dial_run_n; i++) {
                s_dial_run_start[i - 1u] = s_dial_run_start[i];
                s_dial_run_len[i - 1u] = s_dial_run_len[i];
                s_dial_run_zone[i - 1u] = s_dial_run_zone[i];
            }
            s_dial_run_n--;
        }
    }

    /* 摊回逐段 zone（列扫描取色用）+ 最短段长度（dump 断言用）。 */
    for (i = 0; i < s_dial_run_n; i++) {
        uint8_t k;

        for (k = 0; k < s_dial_run_len[i]; k++) {
            s_dial_seg_zone[s_dial_run_start[i] + k] =
                s_dial_run_zone[i];
        }
        if (s_dial_run_min == 0u || s_dial_run_len[i] < s_dial_run_min) {
            s_dial_run_min = s_dial_run_len[i];
        }
    }
}

/**
 * @brief 剖面第 x 列的折线 y（与填充、分格、三角共用同一套插值）。
 * @param ys 各采样点的 y（已含上下留白）。
 * @param wn 点数（≥2）。
 * @param x  画布列（0..W-1）。
 */
static int helm_dial_y_at(const int * ys, uint8_t wn, int x)
{
    const int wlast = HELM_DIAL_ELEV_W - 1;
    const int num = x * (int)(wn - 1u);
    int si = num / wlast;
    const int rem = num % wlast;

    if (si >= (int)wn - 1) {
        return ys[wn - 1u];
    }
    return ys[si] + (ys[si + 1u] - ys[si]) * rem / wlast;
}

/**
 * @brief 每格代表多少米：从阶梯里挑"格数 ≤ HELM_DIAL_GRID_MAX_SEG"的最小间隔。
 * @param win_m 窗口总里程（米）。
 * @return 0 = 窗口太短，切不出 ≥2 格 ⇒ 不分格。
 * @note 用户 2026-11-04："最好做成一个一个竖向的线隔开，每段代表一个距离"。
 */
static uint32_t helm_dial_grid_m(uint32_t win_m)
{
    static const uint16_t ladder[] = { 100, 200, 250, 500, 1000, 2000, 5000 };
    size_t i;

    if (win_m == 0u) {
        return 0u;
    }
    for (i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
        const uint32_t seg = win_m / (uint32_t)ladder[i];

        if (seg >= 2u && seg <= HELM_DIAL_GRID_MAX_SEG) {
            return (uint32_t)ladder[i];
        }
    }
    return 0u;
}

/**
 * @brief 把当前窗口烘到画布（5 点平滑 + 滞回分区 + 按列填充 + 距离分格 + 2px 脊线）。
 * @param eles 窗口海拔。
 * @param wn   点数（≥2）。
 * @param step_cm 相邻点水平距离（厘米）；0 = 平路（灰）。
 * @param win_m 窗口总里程（米）。
 * @param grid_m 每格里程（米）；0 = 不分格。
 * @param cursor_px 当前位置 x（0..W-1）。
 * @param has_cursor true = 导航态：游标左侧（已骑过）整段灰化，并画 2px 竖线。
 * @note 2026-11-01 用户："骑行过的区域变灰色，有明显竖线表示骑行的位置"。
 */
static void helm_dial_elev_paint(const int16_t * eles, uint8_t wn,
                                 uint32_t step_cm, uint32_t win_m,
                                 uint32_t grid_m, int cursor_px,
                                 bool has_cursor)
{
    lv_obj_t * cv = s_sh.dial_elev_cv;
    lv_layer_t layer;
    const int base = HELM_DIAL_ELEV_H - 1;
    const int top = 3;
    const int bot = base - top;
    const int wlast = HELM_DIAL_ELEV_W - 1;
    int32_t mn;
    int32_t mx;
    int span;
    uint32_t i;

    if (cv == NULL || s_dial_cbuf == NULL || wn < 2u) {
        return;
    }

    if (!has_cursor) {
        cursor_px = -1;
    } else if (cursor_px < 0) {
        cursor_px = 0;
    } else if (cursor_px > wlast) {
        cursor_px = wlast;
    }

    helm_dial_smooth5(eles, wn);
    helm_dial_fill_unknown(wn);
    helm_dial_runs_build(wn, step_cm);
    helm_climb_axis(s_dial_smooth, wn, &mn, &mx);
    span = mx - mn;
    if (span < 1) {
        span = 1;
    }

    lv_canvas_fill_bg(cv, helm_color(HELM_COLOR_PAPER), LV_OPA_COVER);
    lv_canvas_init_layer(cv, &layer);
    {
        int ys[HELM_CLIMB_PROF_N];
        int x;

        for (i = 0; i < wn; i++) {
            ys[i] = bot -
                (int)(((int32_t)s_dial_smooth[i] - mn) * (bot - top)) / span;
        }

        /* 填充：每列一个 1px 矩形（轴对齐）。不用逐段三角形：相邻段共享的
         * 竖边会被抗锯齿按 50% 覆盖，画布上会出现一列列浅色竖缝。
         * 全段按坡度上色 —— 已骑过的那段不在这里分色，交给最后那张**遮盖**。 */
        for (x = 0; x < HELM_DIAL_ELEV_W; x++) {
            const int num = x * (int)(wn - 1u);
            int si = num / wlast;
            int y = helm_dial_y_at(ys, wn, x);
            lv_draw_rect_dsc_t rd;
            lv_area_t a;

            if (si >= (int)wn - 1) {
                si = (int)wn - 2;
            }
            lv_draw_rect_dsc_init(&rd);
            /* 骑过的（游标左侧）填充一律退成灰，右侧才按坡度上色。 */
            rd.bg_color = helm_color_snap(
                (has_cursor && x < cursor_px)
                    ? HELM_COLOR_ELEV_DONE
                    : helm_dial_zone_fill(s_dial_seg_zone[si]));
            rd.bg_opa = LV_OPA_40;
            a.x1 = x;
            a.y1 = y;
            a.x2 = x;
            a.y2 = base;
            lv_draw_rect(&layer, &rd, &a);
        }

        /* 距离分格：2px 淡墨竖线，**只从基线升到折线**（不伸出折线之上），
         * 所以每根线的高度随该列海拔变化，看着像把填充切成一片片。
         * 用整数比例算 x，避免逐格累加漂移；两端不画（0 与 W 就是图框）。
         * 用户 2026-11-04："只到海拔折线同高，不要伸出来"。 */
        if (grid_m != 0u && win_m != 0u) {
            uint32_t k;

            for (k = 1u; k * grid_m < win_m; k++) {
                const int gx = (int)((k * (uint32_t)HELM_DIAL_ELEV_W * grid_m) /
                                     win_m);
                lv_draw_rect_dsc_t rd;
                lv_area_t a;

                if (gx <= 0 || gx + 1 >= HELM_DIAL_ELEV_W) {
                    continue;
                }
                lv_draw_rect_dsc_init(&rd);
                rd.bg_color = helm_color(HELM_COLOR_MUTE);
                rd.bg_opa = LV_OPA_50;
                a.x1 = gx;
                a.x2 = gx + 1;
                a.y1 = helm_dial_y_at(ys, wn, gx);
                a.y2 = base;
                lv_draw_rect(&layer, &rd, &a);
            }
        }

        /* 脊线：逐段画 2px 圆头线（段数 ≤59，代价可忽略）。 */
        for (i = 0; i + 1u < wn; i++) {
            const int x0 = (int)((i * (uint32_t)wlast) / (uint32_t)(wn - 1u));
            const int x1 = (int)(((i + 1u) * (uint32_t)wlast) /
                                 (uint32_t)(wn - 1u));
            lv_point_precise_t pts[2];
            lv_draw_line_dsc_t ld;

            pts[0].x = x0;
            pts[0].y = ys[i];
            pts[1].x = x1;
            pts[1].y = ys[i + 1u];
            lv_draw_line_dsc_init(&ld);
            /* 脊线同样按游标分色：骑过的段是灰脊，前方才按坡度上色。 */
            ld.color = helm_color((has_cursor && x0 < cursor_px)
                ? HELM_COLOR_ELEV_DONE_INK
                : helm_dial_zone_ink(s_dial_seg_zone[i]));
            ld.width = 2;
            ld.opa = LV_OPA_COVER;
            ld.round_start = 1;
            ld.round_end = 1;
            ld.points = pts;
            ld.point_cnt = 2;
            lv_draw_line(&layer, &ld);
        }

        /* 当前位置 = **跟着折线走的倒三角**（用户 2026-11-04："换成和之前一样
         * 的高低可变的三角形"）：与老剖面页同一个标记 —— 尖端朝下指着剖面点、
         * 整个三角在折线之上，海拔变它就上下走，所以"高低可变"。
         * 颜色沿用 `HELM_COLOR_CLIMB_MARK`（橙红），在这页的浅色块上最跳。 */
        if (has_cursor) {
            const int apex = helm_dial_y_at(ys, wn, cursor_px) - 1;
            int top = apex - HELM_DIAL_MARK_H;
            lv_draw_triangle_dsc_t td;
            lv_point_precise_t p[3];

            if (top < 0) {
                top = 0;        /* 顶到画布上沿就压扁，别画到框外 */
            }
            p[0].x = cursor_px;
            p[0].y = apex;
            p[1].x = cursor_px - HELM_DIAL_MARK_W / 2;
            p[1].y = top;
            p[2].x = cursor_px + HELM_DIAL_MARK_W / 2;
            p[2].y = top;
            lv_draw_triangle_dsc_init(&td);
            td.color = helm_color(HELM_COLOR_CLIMB_MARK);
            td.opa = LV_OPA_COVER;
            td.p[0] = p[0];
            td.p[1] = p[1];
            td.p[2] = p[2];
            lv_draw_triangle(&layer, &td);
        }
    }
    lv_canvas_finish_layer(cv, &layer);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(cv);
}

/** @brief 底部剖面：数据走老 CLIMB 链，窗口/文案变了才重画。
 * @details 导航 = 从当前位置到路线终点（「前方 X km」）；本机记录 = 最近
 *          10 分钟剖面（「近 N 分钟」）；不足 2 点只留基线 + 提示。
 */
/** @brief 无剖面占位：只给一句状态提示，**不再摆爬升/坡度/均速**。
 *  @note 用户 2026-11-01："我要的填充不是里程什么的，毕竟无海拔数据时，这里
 *        显示的是历史里程海拔数据" —— 这条剖面条的语义就是"本程海拔历史"，
 *        空态时应该说明"正在采集/没有海拔"，而不是拿别的统计数来凑。10 s
 *        一个采样点 ⇒ 空态最多持续十几秒，不会长期空着。
 */
static void helm_dial_ph_update(const bicycle_runtime_t * rt, bool show)
{
    if (s_sh.dial_ph_card == NULL) {
        return;
    }
    if (!show) {
        helm_show(s_sh.dial_ph_card, false);
        return;
    }

    helm_show(s_sh.dial_ph_card, true);
    helm_label_set(s_sh.dial_ph_hint,
                   (rt != NULL && rt->has_altitude) ? "正在记录海拔…"
                                                    : "无海拔数据");
    helm_set_text_hex(s_sh.dial_ph_hint, HELM_COLOR_MUTE);
}

/** @brief 表盘底部剖面：导航 = 6 km 窗口（每 2 km 重采，游标左侧灰化）；
 *         记录 = 最近 10 分钟。
 *
 * 2026-11-01 用户："预瞄 4 km，2 km 更新一次，骑行过的区域变灰色，有明显竖线
 * 表示骑行的位置，以及显示后方还有多长距离"。
 *   · 窗口 = [刷新点, 刷新点+6 km]：游标从 0 走到 1/3 就重采 ⇒ 前方恒有 ≥4 km；
 *   · 两次刷新之间只移动 2px 游标竖线 + 把左侧（已骑过）灰化，不重采路线；
 *   · 标签「后 X.X km」= 窗口里灰化段的长度（0~2 km，刷新后归零）。
 */
static void helm_dial_elev_tick(const bicycle_runtime_t * rt)
{
    bool nav = helm_nav_on();
    uint32_t step_cm = 0u;
    uint32_t win_m = 0u;
    uint32_t grid_m = 0u;
    uint32_t sig = 0u;
    uint8_t wn = 0u;
    int cursor = -1;
    char lab[32];

    if (nav) {
#if VMAP_ROUTE_ENABLE
        double along = map_page_nav_along_m(s_sh.map);
        /* **按当前应用的 route 开窗**：GPX 密集跟线时 route 只是课程的一个
         * 窗口片，用课程全长（20 km）去算 6 km 窗口会让折线挤在左边。
         * 采样函数 `map_page_nav_profile_span()` 取的也是这条 route。 */
        double total = map_page_nav_route_total_m(s_sh.map);

        if (total < 0.0) {
            total = 0.0;
        }
        /* 换线 / 回退 / 骑满 2 km ⇒ 重采窗口（起点 = 当前沿路线位置）。 */
        if (s_dial_nav_base < 0.0 || total != s_dial_nav_total ||
            along < s_dial_nav_base - 1.0 ||
            along - s_dial_nav_base >= HELM_DIAL_NAV_REFRESH_M) {
            s_dial_nav_base = along;
            s_dial_nav_total = total;
            s_dial_nav_span = HELM_DIAL_NAV_TOTAL_M;
            if (total > 0.0 && s_dial_nav_base + s_dial_nav_span > total) {
                s_dial_nav_span = total - s_dial_nav_base;
            }
            s_dial_nav_n = 0u;
            s_dial_nav_step_cm = 0u;
            if (s_dial_nav_span > 1.0) {
                s_dial_nav_n = (uint8_t)map_page_nav_profile_span(s_sh.map,
                    s_dial_nav_base, s_dial_nav_span, s_dial_win,
                    HELM_CLIMB_PROF_N);
            }
            if (s_dial_nav_n >= 2u) {
                s_dial_nav_step_cm = (uint32_t)(s_dial_nav_span * 100.0 /
                                                (double)(s_dial_nav_n - 1u));
                if (s_dial_nav_step_cm < 1u) {
                    s_dial_nav_step_cm = 1u;
                }
            }
            s_dial_nav_ver++;
        }

        wn = s_dial_nav_n;
        step_cm = s_dial_nav_step_cm;
        if (wn >= 2u && s_dial_nav_span > 1.0) {
            double u = (along - s_dial_nav_base) / s_dial_nav_span;

            win_m = (uint32_t)(s_dial_nav_span + 0.5);
            if (u < 0.0) {
                u = 0.0;
            } else if (u > 1.0) {
                u = 1.0;
            }
            cursor = (int)(u * (double)(HELM_DIAL_ELEV_W - 1) + 0.5);
        }
        /* 标签 = **到终点的剩余里程**（含窗口外还没画出来的部分）。
         * 用户 2026-11-01：'"后 xx km" 什么意思，我的意思是还剩多少 km
         * （包含还未显示出来的里程）'。 */
        {
            /* 与地图页「剩余里程」**同一个口径**（`map_page_nav_remain_m()`）。
             * 原来写 `total - along`：GPX 密集窗口少算了 `win_start`，
             * 同一时刻 P02 显示 17 km、地图显示 12 km（2026-11-04 用户实测）。 */
            double remain = map_page_nav_remain_m(s_sh.map);

            if (remain < 0.0) {
                remain = (total > along) ? (total - along) : 0.0;
            }

            lv_snprintf(lab, sizeof(lab), "还剩 %.1f km", remain / 1000.0);
        }
        sig = 0x40000000u | (s_dial_nav_ver << 8) | (uint32_t)(cursor + 1);
#else
        (void)lab;
#endif
    } else if (helm_riding() && s_rec_ele_n >= 2u) {
        s_dial_nav_base = -1.0;      /* 下次进导航重新开窗 */
        wn = s_rec_ele_n;
        memcpy(s_dial_win, s_rec_ele, (size_t)wn * sizeof(s_dial_win[0]));
        /* 窗口里程 = 末点 − 首点。**不能用 `session_distance_m / (wn-1)`**：
         * 窗口只有 10 分钟，骑满后拿全程里程摊会让每点距离越算越大
         * （坡度和分格一起失真）。10 分钟最多 10 km ⇒ ×100 不会溢出。 */
        if (s_rec_dist[wn - 1u] > s_rec_dist[0]) {
            win_m = s_rec_dist[wn - 1u] - s_rec_dist[0];
        }
        if (win_m > 0u) {
            step_cm = (win_m * 100u) / (uint32_t)(wn - 1u);
        }
        if (step_cm < 100u) {
            step_cm = 0u;      /* <1 m/点：坡度不可信，保持灰，不伪造 */
        }
        sig = 0x80000000u | ((uint32_t)s_rec_ele_slot << 1) |
              ((s_rec_ele_n < HELM_CLIMB_PROF_N) ? 1u : 0u);
        {
            unsigned mins = (unsigned)(((uint32_t)(wn - 1u) *
                (uint32_t)(HELM_CLIMB_REC_PERIOD_MS / 1000u) + 59u) / 60u);

            if (mins < 1u) {
                mins = 1u;
            }
            lv_snprintf(lab, sizeof(lab), "近 %u 分钟", mins);
        }
    } else {
        s_dial_nav_base = -1.0;
    }

    /* 取证用：把游标钉在指定像素（-1 = 真实位置）。sig 的低 8 位就是
     * 游标像素，改完必须同步写回去，否则"指纹没变"= 不重烘。 */
    if (cursor >= 0 && s_dial_cursor_ovr >= 0) {
        cursor = (s_dial_cursor_ovr > HELM_DIAL_ELEV_W - 1)
            ? HELM_DIAL_ELEV_W - 1 : s_dial_cursor_ovr;
        sig = (sig & 0xffffff00u) | ((uint32_t)(cursor + 1) & 0xffu);
    }

    if (wn < 2u) {
        if (s_dial_prof_valid) {
            s_dial_prof_valid = false;
            if (s_sh.dial_elev_cv != NULL) {
                lv_obj_add_flag(s_sh.dial_elev_cv, LV_OBJ_FLAG_HIDDEN);
            }
        }
        helm_label_set(s_sh.dial_elev_lab, "--");
        helm_show(s_sh.dial_elev_lab, false);
        helm_show(s_sh.dial_grid_lab, false);
        helm_dial_ph_update(rt, true);
        s_dial_run_n = 0;
        s_dial_run_min = 0;
        s_dial_prof_grid = 0u;
        return;
    }

    /* 距离分格：每格多少米由窗口总里程决定（见 helm_dial_grid_m）。 */
    grid_m = helm_dial_grid_m(win_m);
    if (!s_dial_prof_valid || s_dial_prof_sig != sig ||
        s_dial_prof_grid != grid_m) {
        helm_dial_elev_paint(s_dial_win, wn, step_cm, win_m, grid_m, cursor,
                             nav);
        s_dial_prof_valid = true;
        s_dial_prof_sig = sig;
        s_dial_prof_grid = grid_m;
    }

    if (grid_m != 0u) {
        char glab[16];

        if (grid_m >= 1000u) {
            lv_snprintf(glab, sizeof(glab), "%u km/格",
                        (unsigned)(grid_m / 1000u));
        } else {
            lv_snprintf(glab, sizeof(glab), "%u m/格", (unsigned)grid_m);
        }
        helm_label_set(s_sh.dial_grid_lab, glab);
    }
    helm_show(s_sh.dial_grid_lab, grid_m != 0u);

    helm_label_set(s_sh.dial_elev_lab, lab);
    /* 有剖面 ⇒ 露画布 + "后 X km / 近 N 分钟"，收起占位提示。 */
    helm_show(s_sh.dial_ph_card, false);
    helm_show(s_sh.dial_elev_lab, true);
    if (s_sh.dial_elev_cv != NULL) {
        lv_obj_remove_flag(s_sh.dial_elev_cv, LV_OBJ_FLAG_HIDDEN);
    }
}

/** @brief 速度带/心率带的分区色（全部取自 palette；浅色底 + 深色条/线）。
 *  @note 调色板是运行期查表 ⇒ 这些宏不是常量表达式，统一在
 *        `helm_ride_colors_init()` 里赋值（builder 开头调一次）。 */
static uint32_t s_dial_spd_light[3];
static uint32_t s_dial_spd_deep[3];
static uint32_t s_dial_hr_light[5];
static uint32_t s_dial_hr_deep[5];

static void helm_ride_colors_init(void)
{
    s_dial_spd_light[0] = HELM_COLOR_CLIMB_FILL;
    s_dial_spd_light[1] = HELM_COLOR_CAD_FILL;
    s_dial_spd_light[2] = HELM_COLOR_NAV_FILL;
    s_dial_spd_deep[0] = HELM_COLOR_CLIMB;
    s_dial_spd_deep[1] = HELM_COLOR_CAD;
    s_dial_spd_deep[2] = HELM_COLOR_NAV;
    s_dial_hr_light[0] = HELM_COLOR_TRACK;
    s_dial_hr_light[1] = HELM_COLOR_CLIMB_FILL;
    s_dial_hr_light[2] = HELM_COLOR_CAD_FILL;
    s_dial_hr_light[3] = HELM_COLOR_PWR_FILL;
    s_dial_hr_light[4] = HELM_COLOR_HR_FILL;
    s_dial_hr_deep[0] = HELM_COLOR_MUTE;
    s_dial_hr_deep[1] = HELM_COLOR_CLIMB;
    s_dial_hr_deep[2] = HELM_COLOR_CAD;
    s_dial_hr_deep[3] = HELM_COLOR_PWR;
    s_dial_hr_deep[4] = HELM_COLOR_HR;
}

/** @brief 速度带三档（0 <30 / 1 30–45 / 2 ≥45）：与带的三段 96/48/48 一一对应。
 *  @note 弧的指示色用**同一套**档位色（s_dial_spd_deep），否则会出现"圆点落在
 *        绿段、弧却是红的"这种自相矛盾（18/32 那套是走势图/仪表台量条的切点，
 *        不是这条 0–60 速度带的）。
 */
static uint8_t helm_dial_spd_zone(float kph)
{
    if (kph < 30.0f) {
        return 0u;
    }
    if (kph < 45.0f) {
        return 1u;
    }
    return 2u;
}

/** @brief 表盘页整页刷值（500 ms UI 节拍；只 set_text / set_x，不重建）。
 * @details 时速弧/指针、心率值/单位/均峰/分区、四格、底部剖面。数字颜色
 *          只有暂停时退 TRACK（与老页同一口径），单位保持 MUTE。
 */
static void helm_dial_tick(const bicycle_runtime_t * rt)
{
    const bool paused = s_sh.paused;
    const uint32_t ink = paused ? HELM_COLOR_TRACK : HELM_COLOR_INK;
    float speed = rt ? rt->speed_kph : 0.0f;
    float arc_kph;
    char buf[32];
    int v10;
    int i;

    if (speed < 0.0f) {
        speed = 0.0f;
    }
    /* 弧/指针量程 0..60；**数字仍显示真实时速**（不把 62 显示成 60）。 */
    arc_kph = (speed > 60.0f) ? 60.0f : speed;
    v10 = (int)(arc_kph * 10.0f + 0.5f);

    if (s_sh.dial_arc != NULL) {
        const int v = (v10 + 5) / 10;

        if (lv_arc_get_value(s_sh.dial_arc) != v) {
            lv_arc_set_value(s_sh.dial_arc, v);
        }
        /* 弧的指示色跟速度档走（低绿 / 中黄 / 高红）：与下面速度带同一套身份色，
         * 让"码表"不用读数字也能看出当前在哪一档（2026-11-01 重设计）。 */
        {
            uint32_t zhex = paused
                ? HELM_COLOR_TRACK
                : s_dial_spd_deep[helm_dial_spd_zone(arc_kph)];
            lv_color_t want = helm_color(zhex);

            if (!lv_color_eq(lv_obj_get_style_arc_color(
                    s_sh.dial_arc, LV_PART_INDICATOR), want)) {
                lv_obj_set_style_arc_color(s_sh.dial_arc, want,
                                           LV_PART_INDICATOR);
            }
        }
    }
    helm_fmt_speed(buf, sizeof(buf), speed);
    helm_label_set(s_sh.dial_speed, buf);
    helm_set_text_hex(s_sh.dial_speed, ink);
    helm_set_text_hex(s_sh.dial_unit, HELM_COLOR_MUTE);
    /* 速度带：填充到当前值（颜色沿经过的分区深色）+ 2px 墨色前沿。 */
    if (s_sh.dial_spd_edge != NULL) {
        static const int seg_x[3] = {
            HELM_SPD_BAR_X, HELM_SPD_BAR_X + HELM_SPD_BAR_W / 2,
            HELM_SPD_BAR_X + HELM_SPD_BAR_W * 3 / 4
        };
        static const int seg_w[3] = {
            HELM_SPD_BAR_W / 2, HELM_SPD_BAR_W / 4, HELM_SPD_BAR_W / 4
        };
        int vx = HELM_SPD_BAR_X +
                 (int)(((int32_t)HELM_SPD_BAR_W * v10 + 300) / 600);
        int i;

        if (vx < HELM_SPD_BAR_X) {
            vx = HELM_SPD_BAR_X;
        } else if (vx > HELM_SPD_BAR_X + HELM_SPD_BAR_W) {
            vx = HELM_SPD_BAR_X + HELM_SPD_BAR_W;
        }
        for (i = 0; i < 3; i++) {
            int w = vx - seg_x[i];

            if (w < 0) {
                w = 0;
            } else if (w > seg_w[i]) {
                w = seg_w[i];
            }
            if (lv_obj_get_width(s_sh.dial_spd_fill[i]) != w) {
                lv_obj_set_width(s_sh.dial_spd_fill[i], w);
            }
        }
        if (vx > HELM_SPD_BAR_X) {
            if (lv_obj_get_x(s_sh.dial_spd_edge) != vx - 2) {
                lv_obj_set_x(s_sh.dial_spd_edge, vx - 2);
            }
            lv_obj_remove_flag(s_sh.dial_spd_edge, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_sh.dial_spd_edge, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 心率：值 + 紧跟的单位 + 右侧「均 / 峰」；无有效心率整行退到 "--"，
     * 指针/描边收起（不自造 0 bpm）。 */
    if (rt != NULL && rt->hr_valid && rt->hr_bpm > 0u) {
        lv_point_t sz;
        int bpm = (int)rt->hr_bpm;
        int z = helm_dial_hr_zone(bpm);
        int bx0 = HELM_HR_BAR_X + (z - 1) * HELM_HR_BAR_W / 5;
        int bx1 = HELM_HR_BAR_X + z * HELM_HR_BAR_W / 5;

        lv_snprintf(buf, sizeof(buf), "%d", bpm);
        helm_label_set(s_sh.dial_hr_val, buf);
        lv_txt_get_size(&sz, buf, s_sh.font_val, 0, 0, LV_COORD_MAX,
                        LV_TEXT_FLAG_NONE);
        if (s_sh.dial_hr_unit != NULL) {
            const int ux = HELM_HR_BAR_X + sz.x + 4;

            if (lv_obj_get_x(s_sh.dial_hr_unit) != ux) {
                lv_obj_set_x(s_sh.dial_hr_unit, ux);
            }
        }
        {
            int vx = helm_dial_hr_x(bpm);
            int i;

            for (i = 0; i < 5; i++) {
                int bx = HELM_HR_BAR_X + i * HELM_HR_BAR_W / 5;
                int bw = HELM_HR_BAR_X + (i + 1) * HELM_HR_BAR_W / 5 - bx;
                int w = vx - bx;

                if (w < 0) {
                    w = 0;
                } else if (w > bw) {
                    w = bw;
                }
                if (lv_obj_get_width(s_sh.dial_hr_fill[i]) != w) {
                    lv_obj_set_width(s_sh.dial_hr_fill[i], w);
                }
            }
            if (vx > HELM_HR_BAR_X) {
                if (lv_obj_get_x(s_sh.dial_hr_edge) != vx - 2) {
                    lv_obj_set_x(s_sh.dial_hr_edge, vx - 2);
                }
                lv_obj_remove_flag(s_sh.dial_hr_edge, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_sh.dial_hr_edge, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (s_sh.dial_hr_stats != NULL) {
            if (rt->hr_avg_bpm > 0u || rt->hr_max_bpm > 0u) {
                lv_snprintf(buf, sizeof(buf), "均 %u · 峰 %u",
                            (unsigned)rt->hr_avg_bpm,
                            (unsigned)rt->hr_max_bpm);
            } else {
                lv_snprintf(buf, sizeof(buf), "均 -- · 峰 --");
            }
            helm_label_set(s_sh.dial_hr_stats, buf);
        }
    } else {
        lv_point_t sz;

        helm_label_set(s_sh.dial_hr_val, "--");
        lv_txt_get_size(&sz, "--", s_sh.font_val, 0, 0, LV_COORD_MAX,
                        LV_TEXT_FLAG_NONE);
        if (s_sh.dial_hr_unit != NULL) {
            const int ux = HELM_HR_BAR_X + sz.x + 4;

            if (lv_obj_get_x(s_sh.dial_hr_unit) != ux) {
                lv_obj_set_x(s_sh.dial_hr_unit, ux);
            }
        }
        {
            int i;

            for (i = 0; i < 5; i++) {
                if (lv_obj_get_width(s_sh.dial_hr_fill[i]) != 0) {
                    lv_obj_set_width(s_sh.dial_hr_fill[i], 0);
                }
            }
            helm_show(s_sh.dial_hr_edge, false);
        }
        helm_label_set(s_sh.dial_hr_stats, "均 -- · 峰 --");
    }
    helm_set_text_hex(s_sh.dial_hr_val, ink);
    helm_set_text_hex(s_sh.dial_hr_stats, HELM_COLOR_MUTE);

    /* 四格：里程 / 时间 / 踏频 / 海拔（标签带单位，值 15px）。 */
    {
        char b[4][16];

        helm_fmt_km(b[0], sizeof(b[0]), rt
            ? (float)(rt->session_distance_m / 1000.0) : 0.0f);
        helm_fmt_time(b[1], sizeof(b[1]), rt ? rt->session_elapsed_ms : 0ull);
        if (rt != NULL && rt->cadence_valid) {
            lv_snprintf(b[2], sizeof(b[2]), "%u", (unsigned)rt->cadence_rpm);
        } else {
            lv_snprintf(b[2], sizeof(b[2]), "--");
        }
        if (rt != NULL && rt->has_altitude) {
            lv_snprintf(b[3], sizeof(b[3]), "%d",
                        (int)(rt->altitude_m + 0.5f));
        } else {
            lv_snprintf(b[3], sizeof(b[3]), "--");
        }
        for (i = 0; i < 4; i++) {
            helm_label_set(s_sh.dial_met_val[i], b[i]);
            helm_set_text_hex(s_sh.dial_met_val[i], ink);
        }
    }

    helm_dial_elev_tick(rt);
}

/* ======================================================================
 * 骑行页（HELM_PAGE_DASH）：老版 X1 仪表台（legacy/O02）
 *
 * 用户 2026-11-01："O02 应该需要提到新布局的说明吧，现在我需要新布局有这个"
 * —— 老 DATA 页原样回到新布局，作为表盘页之后的独立一页（demo 页名 `dash`）。
 * 代码从回退前的 HEAD 版本搬回，只做两处适配：
 *   ① 对象/字段统一加 dash_ 前缀，与表盘页的 dial_* 分开；
 *   ② 时钟图标改用 Material Symbols 16px（原 helm_fa_16 已随图标体系移除）。
 * 几何沿其他页的约定：shell root 本身已经让开 24px 状态栏（root 原点 =
 * 屏幕 y=24），所以 pane 从 root y=0 起算、高 HELM_PAGE_H=296 正好铺满
 * 24..320；不要再额外加 HELM_STATUS_H，否则整页下移 24px 且卡片被压扁。
 * ====================================================================== */

/** @brief 骑行页量条宽度补间：宽度变化只脏量条自己，别走整页重排。 */
static void helm_rail_set_exec(void * var, int32_t v)
{
    if (helm_anim_is_suspended()) {
        return;
    }

    lv_obj_set_width((lv_obj_t *)var, v);
}

/** @brief 走势图 30 个采样点的平均时速（与图上曲线同源，取样逐点相同）。
 * @return km/h；历史点不足 2 个给 0。
 */
static float helm_spark_avg_kph(void)
{
    int32_t sum = 0;
    int i;

    if (s_hist_n < 2) {
        return 0.0f;
    }

    for (i = 0; i < HELM_SPARK_PTS; i++) {
        const int si = (i * (s_hist_n - 1)) / (HELM_SPARK_PTS - 1);

        sum += helm_spark_sample(HELM_HIST_SPEED, si);
    }

    /* helm_spark_sample 对时速给的是 ×10 定点。 */
    return (float)sum / ((float)HELM_SPARK_PTS * 10.0f);
}

/** @brief 仪表台右栏取数：时间 / 里程 / 均速 / 心率（功率由刷值另加）。 */
static void helm_fill_group(uint8_t gi, const char ** lab, char val[][16])
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();
    char tmp[16];
    float avg;
    float dist_km = rt ? (float)(rt->session_distance_m / 1000.0) : 0.0f;

    avg = helm_session_avg_kph(rt);

    if (gi == 0) {
        lab[0] = "时间";
        helm_fmt_time(val[0], 16, rt ? rt->session_elapsed_ms : 0);
        lab[1] = "里程";
        helm_fmt_km(val[1], 16, dist_km);
        lab[2] = "均速";
        helm_fmt_speed(val[2], 16, avg);
        /* 第 4 行是**实时心率**（骑行中抬眼要看当前心率；均值在数据页）。 */
        lab[3] = "心率";
        lv_snprintf(val[3], 16, "%s",
                    helm_dash(rt && rt->hr_valid, tmp, sizeof(tmp), "%u",
                              rt ? (int)rt->hr_bpm : 0));
        return;
    }

    if (gi == 1) {
        lab[0] = "心率";
        lv_snprintf(val[0], 16, "%s",
                    helm_dash(rt && rt->hr_valid, tmp, sizeof(tmp), "%u",
                              rt ? (int)rt->hr_bpm : 0));
        lab[1] = "均心率";
        lv_snprintf(val[1], 16, "%s",
                    helm_dash(rt && rt->hr_avg_bpm > 0u, tmp, sizeof(tmp), "%u",
                              rt ? (int)rt->hr_avg_bpm : 0));
        lab[2] = "最大心率";
        lv_snprintf(val[2], 16, "%s",
                    helm_dash(rt && rt->hr_max_bpm > 0u, tmp, sizeof(tmp), "%u",
                              rt ? (int)rt->hr_max_bpm : 0));
        lab[3] = "踏频";
        lv_snprintf(val[3], 16, "%s",
                    helm_dash(rt && rt->cadence_valid, tmp, sizeof(tmp), "%u",
                              rt ? (int)rt->cadence_rpm : 0));
        return;
    }

    lab[0] = "均速";
    helm_fmt_speed(val[0], 16, helm_avg_kph(rt));
    lab[1] = "极速";
    helm_fmt_speed(val[1], 16, rt ? rt->speed_max_kph : 0);
    lab[2] = "功率";
    lv_snprintf(val[2], 16, "%s",
                helm_dash(rt && rt->power_valid, tmp, sizeof(tmp), "%u",
                          rt ? (int)rt->power_w : 0));
    lab[3] = "爬升";
    lv_snprintf(val[3], 16, "%d", rt ? (int)(rt->gain_m + 0.5f) : 0);
}

/** @brief 仪表台右栏一行：3px 色条 + 标签/数值两行 + 走势背景。
 *
 * 标签上 / 数值下：右栏只有 98px 宽，"时间 52:18"并排要 24 + 74 + 边距 > 98。
 * 行间发丝线做在行自己的**底边**上——flex 列里再塞一个 1px 对象会被分掉高度。
 */
static lv_obj_t * helm_dash_row(lv_obj_t * parent, int idx)
{
    lv_obj_t * r = lv_obj_create(parent);
    lv_obj_t * col;

    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, lv_pct(100));
    /* 高度走 flex_grow（不是 lv_pct）：功率计不在时第 4 行隐藏，
     * 剩下 3 行要自己长满卡片——百分比不会重算。 */
    helm_grow_y(r);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 6, 0);
    lv_obj_set_style_pad_left(r, 6, 0);
    lv_obj_set_style_pad_right(r, 6, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    if (idx < HELM_RIDE_ROWS - 1) {
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, helm_color(HELM_COLOR_HAIR), 0);
    } else {
        /* 末行（功率）默认不铺底色，只有数据高了才半透红（见 helm_dash_alert）。 */
        lv_obj_set_style_radius(r, HELM_RADIUS - 4, 0);
    }

    s_sh.dash_row_box[idx] = r;
    s_sh.dash_row_rail[idx] = lv_obj_create(r);
    lv_obj_remove_style_all(s_sh.dash_row_rail[idx]);
    lv_obj_set_size(s_sh.dash_row_rail[idx], 3, HELM_RIDE_RAIL_H);
    lv_obj_set_style_radius(s_sh.dash_row_rail[idx], 2, 0);
    lv_obj_set_style_bg_opa(s_sh.dash_row_rail[idx], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_sh.dash_row_rail[idx],
                              helm_color(HELM_COLOR_MUTE), 0);
    lv_obj_clear_flag(s_sh.dash_row_rail[idx], LV_OBJ_FLAG_CLICKABLE);

    col = lv_obj_create(r);
    lv_obj_remove_style_all(col);
    helm_grow_x(col);
    lv_obj_set_height(col, lv_pct(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    s_sh.dash_row_lab[idx] = helm_lab(col, s_sh.font_lab, HELM_COLOR_MUTE, "--");
    /* 数值走 num_28 不是 num_32：num_32 的 "52:18" 实测 86px，加左缘色条后
     * 超出 98px 卡片的内宽（83px）。 */
    s_sh.dash_row_val[idx] = helm_lab(col, s_sh.font_rot, HELM_COLOR_INK, "--");
    /* 每行挂一根走势，**图占满整行 = 数字背后的背景层**。序列由**标签文本**
     * 决定（helm_hist_id），取不到历史的行（时间/里程）不出图。 */
    helm_spark_bind(r, s_sh.dash_row_lab[idx], HELM_RIDE_ROW_SPARK_H);
    return r;
}

/** @brief 这一行的"数据高了"判据。只看带语义的三项，时间/里程没有高低之分。 */
static bool helm_dash_alert_on(const char * lab, const bicycle_runtime_t * rt)
{
    if (lab == NULL || rt == NULL) {
        return false;
    }

    if (helm_lab_eq(lab, "心率")) {
        return rt->hr_valid && rt->hr_bpm >= HELM_ALERT_HR_BPM;
    }

    if (helm_lab_eq(lab, "功率")) {
        return rt->power_valid && rt->power_w >= HELM_ALERT_PWR_W;
    }

    if (helm_lab_eq(lab, "均速")) {
        /* 判据跟显示值同源：这一行显示的是走势均值，告警也按同一个数判。 */
        return helm_spark_avg_kph() >= HELM_ALERT_SPEED_KPH;
    }

    return false;
}

/** @brief 行底告警底色：默认透明，越阈值时整格半透红（带比较，不每拍标脏）。 */
static void helm_dash_alert(lv_obj_t * row, bool on)
{
    lv_opa_t want = on ? LV_OPA_30 : LV_OPA_TRANSP;

    if (row == NULL || lv_obj_get_style_bg_opa(row, 0) == want) {
        return;
    }

    lv_obj_set_style_bg_opa(row, want, 0);
    if (on) {
        lv_obj_set_style_bg_color(row, helm_color(HELM_COLOR_HR), 0);
    }
}

/** @brief 右栏卡片：四行数据（里程/均速/心率/功率），每行带该指标走势。 */
static lv_obj_t * helm_dash_rows(lv_obj_t * parent)
{
    lv_obj_t * card = lv_obj_create(parent);
    int i;

    lv_obj_remove_style_all(card);
    helm_style_card(card);
    helm_grow_x(card);
    lv_obj_set_height(card, lv_pct(100));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    for (i = 0; i < HELM_RIDE_ROWS; i++) {
        helm_dash_row(card, i);
    }

    return card;
}

/** @brief 左栏：时速锁死（最大字号、不参与轮转）+ 量条 + 时间/爬升/坡度小结。 */
static lv_obj_t * helm_dash_hero(lv_obj_t * parent)
{
    /* 每次建卡读一次即可（爬升=青 / 坡度=青，与 helm_tone() 的身份色一致）。 */
    const uint32_t trip_tone[HELM_RIDE_TRIP_N] = {
        HELM_COLOR_CLIMB, HELM_COLOR_CLIMB
    };
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_t * rail;
    lv_obj_t * box;
    int i;

    lv_obj_remove_style_all(card);
    helm_style_card(card);
    lv_obj_set_size(card, HELM_RIDE_HERO_W, lv_pct(100));
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    s_sh.dash_speed = helm_lab(card, s_sh.font_speed_ride, HELM_COLOR_NAV, "0.0");
    lv_obj_set_width(s_sh.dash_speed, lv_pct(100));
    lv_obj_set_style_text_align(s_sh.dash_speed, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(s_sh.dash_speed, -1, 0);
    lv_obj_align(s_sh.dash_speed, LV_ALIGN_TOP_MID, 0, 8);
    s_sh.dash_unit = helm_lab(card, s_sh.font_lab, HELM_COLOR_MUTE, "km/h");
    lv_obj_set_width(s_sh.dash_unit, lv_pct(100));
    lv_obj_set_style_text_align(s_sh.dash_unit, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(s_sh.dash_unit, s_sh.dash_speed, LV_ALIGN_OUT_BOTTOM_MID,
                    0, 2);

    /* 量条：0~60 km/h，与时速表盘同一量程。轨道 + 填充两个对象——条高只有
     * 5px，lv_bar 的内边距会把可见高度吃掉。 */
    rail = lv_obj_create(card);
    lv_obj_remove_style_all(rail);
    lv_obj_set_size(rail, HELM_RIDE_HERO_W - 24, 5);
    lv_obj_align_to(rail, s_sh.dash_unit, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);
    lv_obj_set_style_radius(rail, 2, 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rail, helm_color(HELM_COLOR_TRACK), 0);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_CLICKABLE);
    s_sh.dash_rail = lv_obj_create(rail);
    lv_obj_remove_style_all(s_sh.dash_rail);
    lv_obj_set_size(s_sh.dash_rail, 0, lv_pct(100));
    lv_obj_align(s_sh.dash_rail, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(s_sh.dash_rail, 2, 0);
    lv_obj_set_style_bg_opa(s_sh.dash_rail, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_sh.dash_rail, helm_color(HELM_COLOR_CAD), 0);
    lv_obj_clear_flag(s_sh.dash_rail, LV_OBJ_FLAG_CLICKABLE);

    /* 下半段 = "骑行时间 + 综合数据"小结块（用户 2026-09-26 定稿）。
     *
     * 排版（左卡内 x 从 12 起、宽 100）：
     *   时钟图标 16 + "时间"（12px MUTE）
     *   时间值 num_32（"72:40" 实测 86 ≤ 100）
     *   发丝线
     *   2 项综合：3px 色条 + 标签 MUTE + 数值 num_28 右对齐 + 单位 MUTE
     * 每拍只刷数值文本，"标签/单位/色条"建好就不动。
     */
    s_sh.dash_trip_ico = helm_lab(card, &helm_icons_16, HELM_COLOR_MUTE,
                                  helm_icon_text(HELM_ICON_CLOCK));
    lv_obj_align(s_sh.dash_trip_ico, LV_ALIGN_TOP_LEFT, 12, HELM_RIDE_TRIP_Y);
    s_sh.dash_trip_lab = helm_lab(card, s_sh.font_lab, HELM_COLOR_MUTE, "时间");
    lv_obj_align(s_sh.dash_trip_lab, LV_ALIGN_TOP_LEFT, 32,
                 HELM_RIDE_TRIP_Y + 2);
    s_sh.dash_trip_val = helm_lab(card, s_sh.font_quad, HELM_COLOR_INK, "0:00");
    lv_obj_align(s_sh.dash_trip_val, LV_ALIGN_TOP_LEFT, 12,
                 HELM_RIDE_TRIP_Y + 18);
    box = lv_obj_create(card);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, HELM_RIDE_HERO_W - 24, 1);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, 12, HELM_RIDE_TRIP_Y + 57);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, helm_color(HELM_COLOR_HAIR), 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

    for (i = 0; i < HELM_RIDE_TRIP_N; i++) {
        lv_coord_t y = HELM_RIDE_TRIP_Y + 62 + i * 30;

        s_sh.dash_trip_rail[i] = lv_obj_create(card);
        lv_obj_remove_style_all(s_sh.dash_trip_rail[i]);
        lv_obj_set_size(s_sh.dash_trip_rail[i], 3, 18);
        lv_obj_align(s_sh.dash_trip_rail[i], LV_ALIGN_TOP_LEFT, 12, y + 6);
        lv_obj_set_style_radius(s_sh.dash_trip_rail[i], 2, 0);
        lv_obj_set_style_bg_opa(s_sh.dash_trip_rail[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_sh.dash_trip_rail[i],
                                  helm_color(trip_tone[i]), 0);
        lv_obj_clear_flag(s_sh.dash_trip_rail[i], LV_OBJ_FLAG_CLICKABLE);

        /* 第 2 项是"坡度"（不带小数点）——极速在保存小结卡 / 数据页仍有。 */
        s_sh.dash_trip_k[i] = helm_lab(card, s_sh.font_lab, HELM_COLOR_MUTE,
                                       (i == 0) ? "爬升" : "坡度");
        lv_obj_align(s_sh.dash_trip_k[i], LV_ALIGN_TOP_LEFT, 20, y + 9);

        /* 值右对齐、单位贴右端：用定宽盒子的右对齐钉住右边缘（align_to
         * 不会随文本变宽重算）。 */
        s_sh.dash_trip_u[i] = helm_lab(card, s_sh.font_lab, HELM_COLOR_MUTE,
                                       (i == 0) ? "m" : "%");
        lv_obj_align(s_sh.dash_trip_u[i], LV_ALIGN_TOP_LEFT,
                     HELM_RIDE_TRIP_UNIT_X, y + 14);
        s_sh.dash_trip_v[i] = helm_lab(card, s_sh.font_rot, HELM_COLOR_INK, "--");
        lv_obj_set_width(s_sh.dash_trip_v[i], HELM_RIDE_TRIP_VAL_W);
        lv_obj_set_style_text_align(s_sh.dash_trip_v[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_sh.dash_trip_v[i], LV_ALIGN_TOP_LEFT,
                     HELM_RIDE_TRIP_VAL_X, y + 1);
    }

    lv_obj_move_foreground(s_sh.dash_speed);
    lv_obj_move_foreground(s_sh.dash_unit);
    return card;
}

/** @brief 搭仪表台页（HELM_PAGE_DASH，老版 O02 布局）。 */
static void helm_build_dash(lv_obj_t * root)
{
    lv_obj_t * row;

    s_sh.dash = helm_overlay(root, HELM_SUBBAR_H,
                             HELM_PAGE_H - HELM_SUBBAR_H);
    /* 第三参 NULL = 不建页码标签（右上角的页面位置指示不需要显示）。 */
    helm_speed_head(s_sh.dash, "骑行", &s_sh.dash_title, NULL);

    /* X1 仪表台：左栏时速锁死（字号最大、位置固定、不参与轮转），右栏数据行。
     * 竖向扫读比 2x2 网格快——骑行中眼睛只需要看左栏那一个大数字。 */
    row = lv_obj_create(s_sh.dash);
    lv_obj_remove_style_all(row);
    helm_grow_y(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, HELM_GAP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    helm_dash_hero(row);
    helm_dash_rows(row);
}

/** @brief 仪表台刷值：左卡时速/量条/小结，右栏四行。 */
static void helm_dash_tick(const bicycle_runtime_t * rt)
{
    const char * labs[HELM_RIDE_ROWS];
    char vals[HELM_RIDE_ROWS][16];
    char buf[16];
    uint32_t i;

    if (s_sh.dash == NULL) {
        return;
    }

    helm_label_set(s_sh.dash_title, "骑行");
    helm_fmt_speed(buf, sizeof(buf), rt ? rt->speed_kph : 0);
    helm_label_set(s_sh.dash_speed, buf);
    helm_label_set(s_sh.dash_unit, "km/h");
    helm_paint_speed_pair(s_sh.dash_speed, s_sh.dash_unit);

    /* 左栏量条：0~60 km/h，与时速表盘同量程（HELM_DIAL_MAX_KPH）。 */
    if (s_sh.dash_rail != NULL) {
        int32_t rw = HELM_RIDE_HERO_W - 24;
        float kph = rt ? rt->speed_kph : 0.0f;
        int32_t fw;

        if (kph < 0.0f) {
            kph = 0.0f;
        }
        if (kph > (float)HELM_DIAL_MAX_KPH) {
            kph = (float)HELM_DIAL_MAX_KPH;
        }
        fw = (int32_t)((float)rw * kph / (float)HELM_DIAL_MAX_KPH);
        /* 宽度补间 240 ms。切页/suspend/隐藏一律 snap；差 <2 px 不动，
         * 避免每拍因浮点取整抖出一条 1px 的小动画。 */
        if (helm_anim_is_suspended() || s_sh.view_id != HELM_PAGE_DASH ||
            !helm_obj_shown(s_sh.dash_rail)) {
            helm_anim_stop(s_sh.dash_rail, helm_rail_set_exec);
            lv_obj_set_width(s_sh.dash_rail, fw);
        } else {
            int32_t cur = lv_obj_get_width(s_sh.dash_rail);

            if (LV_ABS(cur - fw) >= 2) {
                helm_anim_run(s_sh.dash_rail, helm_rail_set_exec, cur, fw,
                              240, lv_anim_path_ease_out, NULL);
            }
        }
        helm_set_bg_hex(s_sh.dash_rail, helm_zone_ink(helm_speed_zone(kph)));
    }

    /* 右栏四行：里程 / 均速 / 心率 /（功率），不轮转。 */
    helm_fill_group(0u, labs, vals);
    {
        /* 组里第 0 项是"时间"，它已经在左卡小结块里（同一页不出现两次）
         * ⇒ 右栏整体上移一位，最后一行留给功率。 */
        for (i = 0; i + 1u < (uint32_t)HELM_RIDE_ROWS; i++) {
            labs[i] = labs[i + 1u];
            memcpy(vals[i], vals[i + 1u], sizeof(vals[0]));
        }
    }
    {
        /* 均速跟它那一行的背景走势挂钩：显示值 = 图上 30 点的平均。 */
        for (i = 0; i + 1u < (uint32_t)HELM_RIDE_PWR_ROW; i++) {
            if (helm_lab_eq(labs[i], "均速")) {
                helm_fmt_speed(vals[i], sizeof(vals[0]), helm_spark_avg_kph());
                break;
            }
        }
    }
    {
        /* 第 4 行：功率，显隐只看**功率计有没有绑定**（绑定就常驻，
         * 没数据时显示 "--"），不看这一秒有没有数据。 */
        bool pwr_row = myvendor_sys_sensor_bound(MYVENDOR_SYS_SENSOR_KIND_CPS);

        if (s_sh.dash_row_box[HELM_RIDE_PWR_ROW] != NULL &&
            helm_obj_shown(s_sh.dash_row_box[HELM_RIDE_PWR_ROW]) != pwr_row) {
            helm_show(s_sh.dash_row_box[HELM_RIDE_PWR_ROW], pwr_row);
        }
        if (pwr_row) {
            char tmp_pw[16];

            labs[HELM_RIDE_PWR_ROW] = "功率";
            lv_snprintf(vals[HELM_RIDE_PWR_ROW], sizeof(vals[0]), "%s",
                        helm_dash(rt != NULL && rt->power_valid,
                                  tmp_pw, sizeof(tmp_pw), "%u",
                                  rt ? (unsigned)rt->power_w : 0u));
        }
    }
    for (i = 0; i < (uint32_t)HELM_RIDE_ROWS; i++) {
        uint32_t tone;
        uint32_t ink;

        if (s_sh.dash_row_box[i] == NULL ||
            !helm_obj_shown(s_sh.dash_row_box[i])) {
            continue;
        }
        tone = helm_tone(labs[i]);

        /* 标签一律 MUTE（色条负责身份）；数值一律 INK，只有"心率"这类
         * 活体分区色例外。 */
        helm_label_set(s_sh.dash_row_lab[i], labs[i]);
        helm_set_text_hex(s_sh.dash_row_lab[i], HELM_COLOR_MUTE);
        helm_set_bg_hex(s_sh.dash_row_rail[i], tone);

        ink = helm_lab_eq(labs[i], "心率") ? helm_metric_ink(labs[i])
                                           : HELM_COLOR_INK;
        helm_label_set(s_sh.dash_row_val[i], vals[i]);
        helm_set_text_hex(s_sh.dash_row_val[i], ink);

        /* 默认不上底色；只有"这项数据高了"才整格半透红。 */
        helm_dash_alert(s_sh.dash_row_box[i], helm_dash_alert_on(labs[i], rt));
    }

    /* 左卡小结块：骑行时间 + 爬升 / 坡度。每拍只刷文本。 */
    if (s_sh.dash_trip_val != NULL) {
        char tbuf[16];
        float grade = rt ? rt->grade_pct : 0.0f;

        helm_fmt_time(tbuf, sizeof(tbuf), rt ? rt->session_elapsed_ms : 0ull);
        helm_label_set(s_sh.dash_trip_val, tbuf);
        lv_snprintf(tbuf, sizeof(tbuf), "%d",
                    rt ? (int)(rt->gain_m + 0.5f) : 0);
        helm_label_set(s_sh.dash_trip_v[0], tbuf);
        /* 坡度：整数百分比；num_* 字库没有 '-'，取绝对值（方向看爬升/下降）。 */
        if (grade < 0.0f) {
            grade = -grade;
        }
        lv_snprintf(tbuf, sizeof(tbuf), "%d", (int)(grade + 0.5f));
        helm_label_set(s_sh.dash_trip_v[1], tbuf);
    }
}

/**
 * @brief 搭表盘页（HELM_PAGE_DATA）。
 * @details 270° 弧 + 56px 时速 + 速度带/心率带 + 四格 + 底部前方海拔剖面。
 *          上沿约束：12 点刻度外沿按线宽内缩到 r=80（3px 刻度墨迹 ≤81.5），
 *          不越过页面 y=0（屏幕 y=24 的状态栏下沿）。
 */
static void helm_build_data(lv_obj_t * root)
{
    lv_obj_t * p;
    int i;

    helm_ride_colors_init();
    s_sh.data = helm_page_pane(root);
    p = s_sh.data;

    /* 270° 仪表：中心 (120,82)、d=152、粗 7；轨道 track、指示 OK（绿）。 */
    s_sh.dial_arc = lv_arc_create(p);
    lv_obj_set_size(s_sh.dial_arc, HELM_DIAL_D, HELM_DIAL_D);
    lv_obj_set_pos(s_sh.dial_arc, HELM_DIAL_CX - HELM_DIAL_D / 2,
                   HELM_DIAL_CY - HELM_DIAL_D / 2);
    lv_obj_remove_flag(s_sh.dial_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_sh.dial_arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_sh.dial_arc, 0, 0);
    lv_obj_set_style_bg_opa(s_sh.dial_arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_sh.dial_arc, 0, 0);
    lv_obj_set_style_outline_width(s_sh.dial_arc, 0, 0);
    lv_obj_set_style_shadow_width(s_sh.dial_arc, 0, 0);
    /* 弧配色对齐模拟器参照（scene_helm.c ride_build_dial）：
     *   指示 = 深青绿。板端现有色里 HELM_COLOR_CLIMB(0x086163) 最接近参照的
     *          0x0B6E4F；HELM_COLOR_OK 是亮绿，比参照跳太多（用户 R2 反馈）。
     *   轨道 = 暖灰。参照 P_TRACK=0xBDB7AA；现有色里 HELM_COLOR_ELEV_DONE
     *          (0xB0A898) 最接近（HELM_COLOR_TRACK 的 0xE2DACE 偏白）。
     *   旋钮 = 墨色（参照也是 INK）。不新增十六进制。 */
    lv_obj_set_style_arc_width(s_sh.dial_arc, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_sh.dial_arc, helm_color(HELM_COLOR_ELEV_DONE),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_sh.dial_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_sh.dial_arc, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_sh.dial_arc, helm_color(HELM_COLOR_CLIMB),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_sh.dial_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_sh.dial_arc, helm_color(HELM_COLOR_INK),
                              LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_sh.dial_arc, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_sh.dial_arc, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_sh.dial_arc, 3, LV_PART_KNOB);
    lv_arc_set_bg_angles(s_sh.dial_arc, 0, 270);
    lv_arc_set_rotation(s_sh.dial_arc, 135);
    lv_arc_set_range(s_sh.dial_arc, 0, 60);
    lv_arc_set_value(s_sh.dial_arc, 0);

    /* 7 根刻度（225°..135°，每 45°）：整点 270/0/90 用 3px，其余 1px。
     * 外端按线宽内缩（1px→81、3px→80），整数三角取整后墨迹 ≤82，
     * 12 点方向不会越过页面 y=0。 */
    for (i = 0; i < 7; i++) {
        const int deg = 225 + i * 45;
        const bool major = (deg % 90) == 0;
        const int r_out = major ? 80 : 81;
        int x1;
        int y1;
        int x2;
        int y2;

        helm_dial_pt(deg, 78, &x1, &y1);
        helm_dial_pt(deg, r_out, &x2, &y2);
        s_sh.dial_tick_pts[i][0].x = x1;
        s_sh.dial_tick_pts[i][0].y = y1;
        s_sh.dial_tick_pts[i][1].x = x2;
        s_sh.dial_tick_pts[i][1].y = y2;
        s_sh.dial_tick[i] = lv_line_create(p);
        lv_obj_remove_style_all(s_sh.dial_tick[i]);
        lv_line_set_points(s_sh.dial_tick[i], s_sh.dial_tick_pts[i], 2);
        lv_obj_set_style_line_width(s_sh.dial_tick[i], major ? 3 : 1, 0);
        lv_obj_set_style_line_color(s_sh.dial_tick[i],
            helm_color(major ? HELM_COLOR_INK : HELM_COLOR_MUTE), 0);
        lv_obj_set_style_line_opa(s_sh.dial_tick[i], LV_OPA_COVER, 0);
        lv_obj_set_style_line_rounded(s_sh.dial_tick[i], false, 0);
        lv_obj_remove_flag(s_sh.dial_tick[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_sh.dial_tick[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* hero：容器 (40,40,160,72)；速度 56px 居中，单位 12px 居中在下方。 */
    s_sh.dial_hero = lv_obj_create(p);
    lv_obj_remove_style_all(s_sh.dial_hero);
    lv_obj_set_pos(s_sh.dial_hero, 40, 40);
    lv_obj_set_size(s_sh.dial_hero, 160, 72);
    lv_obj_clear_flag(s_sh.dial_hero, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_sh.dial_hero, LV_OBJ_FLAG_CLICKABLE);

    s_sh.dial_speed = helm_lab(s_sh.dial_hero, s_sh.font_speed_ride,
                               HELM_COLOR_INK, "0.0");
    lv_obj_set_pos(s_sh.dial_speed, 0, 0);
    lv_obj_set_width(s_sh.dial_speed, 160);
    lv_obj_set_style_text_align(s_sh.dial_speed, LV_TEXT_ALIGN_CENTER, 0);
    s_sh.dial_unit = helm_lab(s_sh.dial_hero, s_sh.font_lab,
                              HELM_COLOR_MUTE, "km/h");
    lv_obj_set_pos(s_sh.dial_unit, 0, 56);
    lv_obj_set_width(s_sh.dial_unit, 160);
    lv_obj_set_style_text_align(s_sh.dial_unit, LV_TEXT_ALIGN_CENTER, 0);

    /* 速度带：x=24..216、y=160、高 18；三段 96/48/48（0-30/30-45/45-60），
     * 顶部 2px 深色条、30/45 处 1px 深色分隔、带内刻度 0/30/60。 */
    {
        const int w1 = HELM_SPD_BAR_W / 2;
        const int w2 = HELM_SPD_BAR_W / 4;
        const int xs[3] = {
            HELM_SPD_BAR_X, HELM_SPD_BAR_X + w1, HELM_SPD_BAR_X + w1 + w2
        };
        const int ws[3] = { w1, w2, w2 };

        for (i = 0; i < 3; i++) {
            s_sh.dial_spd_band[i] = helm_dial_box(p, xs[i], HELM_SPD_BAR_Y,
                ws[i], HELM_SPD_BAR_H, s_dial_spd_light[i], LV_OPA_COVER, 0);
            /* 分区色条放在**下沿** 4px：滑块在带内正中，不再压住色条；
             * 4px（原 2px）是为半透半反面板留的——2px 洗淡后基本看不见。 */
            (void)helm_dial_box(p, xs[i],
                HELM_SPD_BAR_Y + HELM_SPD_BAR_H - 4, ws[i], 4,
                s_dial_spd_deep[i], LV_OPA_COVER, 0);
        }
        for (i = 1; i < 3; i++) {
            (void)helm_dial_box(p, xs[i], HELM_SPD_BAR_Y, 1, HELM_SPD_BAR_H,
                HELM_COLOR_MUTE, LV_OPA_COVER, 0);
        }
        /* 刻度挪到带**上方**：10px 滑块在带内会压住 0/30/60 的字。 */
        s_sh.dial_spd_lab[0] = helm_lab(p, s_sh.font_lab, HELM_COLOR_MUTE, "0");
        lv_obj_set_pos(s_sh.dial_spd_lab[0], HELM_SPD_BAR_X, HELM_SPD_LAB_Y);
        s_sh.dial_spd_lab[1] = helm_lab(p, s_sh.font_lab, HELM_COLOR_MUTE, "30");
        lv_obj_set_pos(s_sh.dial_spd_lab[1], HELM_SPD_BAR_X + w1 + 2,
                       HELM_SPD_LAB_Y);
        s_sh.dial_spd_lab[2] = helm_lab(p, s_sh.font_lab, HELM_COLOR_MUTE, "60");
        lv_obj_set_pos(s_sh.dial_spd_lab[2],
                       HELM_SPD_BAR_X + HELM_SPD_BAR_W - 13, HELM_SPD_LAB_Y);
        /* 状态区填充：从左端填到当前值（颜色沿经过的分区深色），上层 14px；
         * 下层 4px 分区底尺保持完整。前沿是 2px 墨线。 */
        for (i = 0; i < 3; i++) {
            s_sh.dial_spd_fill[i] = helm_dial_box(p, xs[i], HELM_SPD_BAR_Y,
                ws[i], HELM_SPD_BAR_H - 4, s_dial_spd_deep[i], LV_OPA_COVER, 0);
            lv_obj_set_width(s_sh.dial_spd_fill[i], 0);
        }
        s_sh.dial_spd_edge = helm_dial_box(p, HELM_SPD_BAR_X, HELM_SPD_BAR_Y,
            2, HELM_SPD_BAR_H - 4, HELM_COLOR_INK, LV_OPA_COVER, 0);
        lv_obj_add_flag(s_sh.dial_spd_edge, LV_OBJ_FLAG_HIDDEN);
    }

    /* 心率行：值 22px @ (24,175)，bpm 12px 紧跟其后；右侧「均 · 峰」右对齐
     * 216；5 分区浅色带 y=199 高 18，1px 分区线，当前分区 1px 深色描边 +
     * 2px 指针。 */
    s_sh.dial_hr_val = helm_lab(p, s_sh.font_val, HELM_COLOR_INK, "--");
    lv_obj_set_pos(s_sh.dial_hr_val, HELM_HR_BAR_X, 175);
    s_sh.dial_hr_unit = helm_lab(p, s_sh.font_lab, HELM_COLOR_MUTE, "bpm");
    lv_obj_set_pos(s_sh.dial_hr_unit, 64, 182);
    s_sh.dial_hr_stats = helm_lab(p, s_sh.font_lab, HELM_COLOR_MUTE,
                                  "均 -- · 峰 --");
    lv_obj_set_pos(s_sh.dial_hr_stats, 120, 182);
    lv_obj_set_width(s_sh.dial_hr_stats, 96);
    lv_obj_set_style_text_align(s_sh.dial_hr_stats, LV_TEXT_ALIGN_RIGHT, 0);
    for (i = 0; i < 5; i++) {
        int bx = HELM_HR_BAR_X + i * HELM_HR_BAR_W / 5;
        int bw = HELM_HR_BAR_X + (i + 1) * HELM_HR_BAR_W / 5 - bx;

        s_sh.dial_hr_band[i] = helm_dial_box(p, bx, HELM_HR_BAR_Y, bw,
            HELM_HR_BAR_H, s_dial_hr_light[i], LV_OPA_COVER, 0);
        /* 下沿 4px 着重色，与速度带同一套处理（用户 2026-11-01："心率数据也
         * 增加下方着重色"）：浅底负责分区面积，深色条负责"这五段是什么"。 */
        (void)helm_dial_box(p, bx, HELM_HR_BAR_Y + HELM_HR_BAR_H - 4, bw, 4,
            s_dial_hr_deep[i], LV_OPA_COVER, 0);
    }
    for (i = 1; i < 5; i++) {
        s_sh.dial_hr_sep[i - 1] = helm_dial_box(p,
            HELM_HR_BAR_X + i * HELM_HR_BAR_W / 5, HELM_HR_BAR_Y, 1,
            HELM_HR_BAR_H, HELM_COLOR_MUTE, LV_OPA_COVER, 0);
    }
    /* 心率带同理：5 段填充（各自 0→当前值）+ 2px 墨色前沿；撤掉"当前分区框"，
     * 避免同时出现两种定位语言。 */
    for (i = 0; i < 5; i++) {
        int bx = HELM_HR_BAR_X + i * HELM_HR_BAR_W / 5;
        int bw = HELM_HR_BAR_X + (i + 1) * HELM_HR_BAR_W / 5 - bx;

        s_sh.dial_hr_fill[i] = helm_dial_box(p, bx, HELM_HR_BAR_Y, bw,
            HELM_HR_BAR_H - 4, s_dial_hr_deep[i], LV_OPA_COVER, 0);
        lv_obj_set_width(s_sh.dial_hr_fill[i], 0);
    }
    s_sh.dial_hr_edge = helm_dial_box(p, HELM_HR_BAR_X, HELM_HR_BAR_Y, 2,
        HELM_HR_BAR_H - 4, HELM_COLOR_INK, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_sh.dial_hr_edge, LV_OBJ_FLAG_HIDDEN);

    /* 四格：盒 60×30、中心 x=48/96/144/192、y=218；标签 12px（带单位）+
     * 值 15px。 */
    {
        static const int cx[4] = { 48, 96, 144, 192 };
        static const char * const lab[4] = {
            "里程 km", "时间", "踏频 rpm", "海拔 m"
        };

        for (i = 0; i < 4; i++) {
            s_sh.dial_met_box[i] = lv_obj_create(p);
            lv_obj_remove_style_all(s_sh.dial_met_box[i]);
            lv_obj_set_pos(s_sh.dial_met_box[i], cx[i] - 30, 218);
            lv_obj_set_size(s_sh.dial_met_box[i], 60, 30);
            lv_obj_clear_flag(s_sh.dial_met_box[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(s_sh.dial_met_box[i], LV_OBJ_FLAG_CLICKABLE);

            s_sh.dial_met_lab[i] = helm_lab(s_sh.dial_met_box[i],
                s_sh.font_lab, HELM_COLOR_MUTE, lab[i]);
            lv_obj_set_pos(s_sh.dial_met_lab[i], 0, 0);
            lv_obj_set_width(s_sh.dial_met_lab[i], 60);
            lv_obj_set_style_text_align(s_sh.dial_met_lab[i],
                                        LV_TEXT_ALIGN_CENTER, 0);
            s_sh.dial_met_val[i] = helm_lab(s_sh.dial_met_box[i],
                s_sh.font_title, HELM_COLOR_INK, "--");
            lv_obj_set_pos(s_sh.dial_met_val[i], 0, 12);
            lv_obj_set_width(s_sh.dial_met_val[i], 60);
            lv_obj_set_style_text_align(s_sh.dial_met_val[i],
                                        LV_TEXT_ALIGN_CENTER, 0);
        }
    }

    /* 海拔剖面：基线 y=250（192×1）、「前方 X km」标签、当前位置 2×28 竖线
     * @ (24,264)、画布 190×28 @ (26,264)。无剖面时只留基线/竖线/"--"。 */
    s_sh.dial_elev_line = helm_dial_box(p, HELM_SPD_BAR_X, 250,
        HELM_SPD_BAR_W, 1, HELM_COLOR_HAIR, LV_OPA_COVER, 0);
    s_sh.dial_elev_lab = helm_lab(p, s_sh.font_lab, HELM_COLOR_MUTE, "--");
    lv_obj_set_pos(s_sh.dial_elev_lab, 120, 251);
    lv_obj_set_width(s_sh.dial_elev_lab, 96);
    lv_obj_set_style_text_align(s_sh.dial_elev_lab, LV_TEXT_ALIGN_RIGHT, 0);
    /* 左端分格说明「1 km/格」：与右端「还剩 X km」同一行，左对齐到内容边 24。
     * 没有分格（窗口 <200 m）时整条隐藏，不占视觉。 */
    s_sh.dial_grid_lab = helm_lab(p, s_sh.font_lab, HELM_COLOR_MUTE, "");
    lv_obj_set_pos(s_sh.dial_grid_lab, HELM_DIAL_PH_X, 251);
    lv_obj_set_width(s_sh.dial_grid_lab, 90);
    lv_obj_set_style_text_align(s_sh.dial_grid_lab, LV_TEXT_ALIGN_LEFT, 0);
    helm_show(s_sh.dial_grid_lab, false);

    s_sh.dial_elev_cv = lv_canvas_create(p);
    lv_obj_remove_style_all(s_sh.dial_elev_cv);
    lv_obj_remove_flag(s_sh.dial_elev_cv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_sh.dial_elev_cv, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_sh.dial_elev_cv, HELM_DIAL_ELEV_X, HELM_DIAL_ELEV_Y);
    if (!helm_dial_canvas_attach(s_sh.dial_elev_cv)) {
        lv_obj_add_flag(s_sh.dial_elev_cv, LV_OBJ_FLAG_HIDDEN);
    }
    /* 空态：先不显示画布，等有 ≥2 点剖面再露出来。 */
    lv_obj_add_flag(s_sh.dial_elev_cv, LV_OBJ_FLAG_HIDDEN);

    /* 无剖面占位：只放一条居中状态提示（"正在记录海拔…/无海拔数据"）。
     * 用户 2026-11-01：这条 strip 的语义是"本程海拔历史"，别拿别的统计数来凑。 */
    s_sh.dial_ph_card = lv_obj_create(p);
    lv_obj_remove_style_all(s_sh.dial_ph_card);
    lv_obj_set_pos(s_sh.dial_ph_card, HELM_DIAL_PH_X, HELM_DIAL_PH_Y);
    lv_obj_set_size(s_sh.dial_ph_card, HELM_DIAL_PH_W, HELM_DIAL_PH_H);
    lv_obj_clear_flag(s_sh.dial_ph_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_sh.dial_ph_card, LV_OBJ_FLAG_CLICKABLE);
    s_sh.dial_ph_hint = helm_lab(s_sh.dial_ph_card, s_sh.font_title,
                                 HELM_COLOR_MUTE, "正在记录海拔…");
    lv_obj_set_width(s_sh.dial_ph_hint, HELM_DIAL_PH_W);
    lv_obj_set_style_text_align(s_sh.dial_ph_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_sh.dial_ph_hint, LV_ALIGN_CENTER, 0, 0);

    /* 首帧先按"无剖面"显示（值下一拍刷）。 */
    helm_show(s_sh.dial_elev_lab, false);

    /* 换主题重建后画布对象是新的 ⇒ 下一拍无条件重画一次。 */
    s_dial_prof_valid = false;
    s_dial_prof_sig = 0;
}

/** @brief 数据页十项取数（顺序 = 用户清单，两列按 i*2 / i*2+1 排）。 */
static void helm_fill_all(const bicycle_runtime_t * rt, const char ** lab,
                          char val[][16])
{
    char tmp[16];
    float dist_km = rt ? (float)(rt->session_distance_m / 1000.0) : 0.0f;

    lab[0] = "时间";
    helm_fmt_time(val[0], 16, rt ? rt->session_elapsed_ms : 0ull);
    lab[1] = "里程";
    helm_fmt_km(val[1], 16, dist_km);
    lab[2] = "均速";
    helm_fmt_speed(val[2], 16, helm_session_avg_kph(rt));
    lab[3] = "极速";
    helm_fmt_speed(val[3], 16, rt ? rt->speed_max_kph : 0.0f);
    lab[4] = "心率";
    lv_snprintf(val[4], 16, "%s",
                helm_dash(rt && rt->hr_valid, tmp, sizeof(tmp), "%u",
                          rt ? (int)rt->hr_bpm : 0));
    lab[5] = "均心率";
    lv_snprintf(val[5], 16, "%s",
                helm_dash(rt && rt->hr_avg_bpm > 0u, tmp, sizeof(tmp), "%u",
                          rt ? (int)rt->hr_avg_bpm : 0));
    lab[6] = "最大心率";
    lv_snprintf(val[6], 16, "%s",
                helm_dash(rt && rt->hr_max_bpm > 0u, tmp, sizeof(tmp), "%u",
                          rt ? (int)rt->hr_max_bpm : 0));
    lab[7] = "踏频";
    lv_snprintf(val[7], 16, "%s",
                helm_dash(rt && rt->cadence_valid, tmp, sizeof(tmp), "%u",
                          rt ? (int)rt->cadence_rpm : 0));
    lab[8] = "功率";
    lv_snprintf(val[8], 16, "%s",
                helm_dash(rt && rt->power_valid, tmp, sizeof(tmp), "%u",
                          rt ? (int)rt->power_w : 0));
    lab[9] = "爬升";
    lv_snprintf(val[9], 16, "%d", rt ? (int)(rt->gain_m + 0.5f) : 0);
}

/** @brief 通用数据格：左缘 3px 身份色条 + 标签（MUTE）+ 数值（num_28，INK）。
 *
 * **不带走势背景**（用户 2026-09-26 给的数据页/海拔页图里都没有曲线），也不
 * 参与行底告警（表盘/方阵都不再有行底告警对象）。
 * 数据页（十格）和海拔页（六格）共用它，各自传自己的对象数组。
 */
static lv_obj_t * helm_cell_make(lv_obj_t * parent, lv_obj_t ** rail,
                                 lv_obj_t ** lab, lv_obj_t ** val, int idx)
{
    lv_obj_t * cell = lv_obj_create(parent);
    lv_obj_t * col;

    lv_obj_remove_style_all(cell);
    helm_grow_x(cell);
    lv_obj_set_height(cell, lv_pct(100));
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cell, 6, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    rail[idx] = lv_obj_create(cell);
    lv_obj_remove_style_all(rail[idx]);
    lv_obj_set_size(rail[idx], 3, HELM_RIDE_RAIL_H);
    lv_obj_set_style_radius(rail[idx], 2, 0);
    lv_obj_set_style_bg_opa(rail[idx], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rail[idx], helm_color(HELM_COLOR_MUTE), 0);
    lv_obj_clear_flag(rail[idx], LV_OBJ_FLAG_CLICKABLE);

    col = lv_obj_create(cell);
    lv_obj_remove_style_all(col);
    helm_grow_x(col);
    lv_obj_set_height(col, lv_pct(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lab[idx] = helm_lab(col, s_sh.font_lab, HELM_COLOR_MUTE, "--");
    val[idx] = helm_lab(col, s_sh.font_rot, HELM_COLOR_INK, "--");
    return cell;
}

static lv_obj_t * helm_all_cell(lv_obj_t * parent, int idx)
{
    return helm_cell_make(parent, s_sh.all_rail, s_sh.all_lab, s_sh.all_val,
                          idx);
}

/** @brief 数据页：2 列 x 5 行十项（用户 2026-09-26 的清单）。
 *
 * @details 行高 flex_grow（5 行均分卡内高 258 ⇒ 51）：行内"标签 14 + 数值 30
 *          = 44"✓。两列之间是**整卡通高的 1px 竖线**（行里留了 12px 间隙给它），
 *          行分隔发丝线做在行自己的底边上（flex 列里塞 1px 对象会参与分高）。
 */
static void helm_build_data_all(lv_obj_t * root)
{
    lv_obj_t * card;
    lv_obj_t * div;
    int i;

    s_sh.data_all = helm_overlay(root, HELM_SUBBAR_H,
                                 HELM_PAGE_H - HELM_SUBBAR_H);
    helm_speed_head(s_sh.data_all, "骑行数据", NULL, NULL);

    card = lv_obj_create(s_sh.data_all);
    lv_obj_remove_style_all(card);
    helm_style_card(card);
    helm_grow_y(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_pad_row(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    for (i = 0; i < HELM_ALL_N / 2; i++) {
        lv_obj_t * row = lv_obj_create(card);

        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        helm_grow_y(row);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (i < HELM_ALL_N / 2 - 1) {
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, helm_color(HELM_COLOR_HAIR), 0);
        }

        helm_all_cell(row, i * 2);
        helm_all_cell(row, i * 2 + 1);
    }

    div = lv_obj_create(card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 1, lv_pct(100));
    lv_obj_add_flag(div, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(div, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(div, helm_color(HELM_COLOR_HAIR), 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(div);
}

/**
 * @brief 图表折线绘制前回调：导航烘焙失败时的现场绘制回退路径。
 * @param e LV_EVENT_DRAW_TASK_ADDED。
 *
 * @details 画布可见时把折线透明并立即返回，切页只 blit 烘焙图。
 *          回退路径隐藏 LVGL 单色折线，只提交与脏区相交的段。
 *          非导航「经过海拔」仍用青绿渐变面积。
 */
static void helm_climb_draw_task(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target_obj(e);
    lv_draw_task_t * draw_task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t * base_dsc;
    lv_draw_line_dsc_t * draw_line_dsc;
    lv_color_t fill;
    lv_area_t coords;
    int32_t i;

    if (obj == NULL || draw_task == NULL) {
        return;
    }

    base_dsc = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(draw_task);
    if (base_dsc == NULL || base_dsc->layer == NULL ||
        base_dsc->part != LV_PART_ITEMS ||
        lv_draw_task_get_type(draw_task) != LV_DRAW_TASK_TYPE_LINE) {
        return;
    }

    draw_line_dsc = lv_draw_task_get_line_dsc(draw_task);
    if (draw_line_dsc == NULL || draw_line_dsc->points == NULL ||
        draw_line_dsc->point_cnt < 2) {
        return;
    }

    lv_obj_get_coords(obj, &coords);

    if (s_sh.climb_canvas && !lv_obj_has_flag(s_sh.climb_canvas, LV_OBJ_FLAG_HIDDEN)) {
        draw_line_dsc->opa = LV_OPA_TRANSP;
        return;
    }

    if (s_sh.climb_nav && s_sh.climb_n >= 2u) {
        int32_t lw = draw_line_dsc->width;
        uint32_t now = s_sh.climb_now;
        const lv_area_t * clip = &base_dsc->layer->_clip_area;

        draw_line_dsc->opa = LV_OPA_TRANSP;
        for (i = 0; i < draw_line_dsc->point_cnt - 1; i++) {
            lv_point_precise_t p1 = draw_line_dsc->points[i];
            lv_point_precise_t p2 = draw_line_dsc->points[i + 1];
            uint8_t z;
            uint32_t fh;
            uint32_t sh;

            if (p1.x == LV_DRAW_LINE_POINT_NONE || p1.y == LV_DRAW_LINE_POINT_NONE ||
                p2.x == LV_DRAW_LINE_POINT_NONE || p2.y == LV_DRAW_LINE_POINT_NONE) {
                continue;
            }
            if (!helm_climb_seg_in_clip(clip, p1, p2, coords.y2, lw)) {
                continue;
            }
            z = ((uint32_t)i + 1u < s_sh.climb_n) ? s_climb_zone[i] : 0u;
            if ((uint32_t)i < now) {
                fh = HELM_COLOR_ELEV_DONE;
                sh = HELM_COLOR_ELEV_DONE_INK;
            } else {
                fh = helm_climb_zone_fill(z);
                sh = helm_climb_zone_ink(z);
            }
            helm_climb_paint_seg(base_dsc->layer, p1, p2, coords.y2,
                helm_color_snap(fh), helm_color(sh), lw);
        }
        return;
    }

    /* 逐行填（2026-09-25 第二版，与主页 spark 同法）：每行一个**纯色**矩形，alpha 由
     * 一条**全局**竖直斜坡给出 —— 越靠图表上方越实、到图底淡成 0，于是山峰处厚实、
     * 谷地处轻薄；比原来"每段一个三角形 + 一块 20% 淡洗"干净得多。
     *
     * 换掉老写法的两个理由：
     *   · 老写法的矩形 x 区间是 [min, max] **两端都含** ⇒ 相邻段在数据点那一列重叠
     *     1 px、混合两次 ⇒ 偏深竖线；三角形的 bbox 又各按自己取整 ⇒ 等 alpha 线是
     *     按段错开的锯齿（观感"脏"）。
     *   · 本板 EPIC 加速单元**没有 TRIANGLE**（只有 FILL/IMAGE/BORDER/LABEL/LAYER），
     *     三角形要回退 SW 逐行生成遮罩；纯色矩形全部走 EPIC 的 FILL。
     *
     * 行、列都只扫**裁剪区内** —— 光标移动时只标脏一个小方块，不能整片重画。 */
    fill = helm_color_snap(HELM_COLOR_CLIMB_FILL);
    {
        const lv_area_t * clip = &base_dsc->layer->_clip_area;
        const int32_t n_pts = (int32_t)draw_line_dsc->point_cnt;
        const int32_t fh = coords.y2 - coords.y1 + 1;
        int32_t x_first = (int32_t)draw_line_dsc->points[0].x;
        int32_t x_last = (int32_t)draw_line_dsc->points[n_pts - 1].x;
        int32_t y_from = coords.y1 > clip->y1 ? coords.y1 : clip->y1;
        int32_t y_to = coords.y2 < clip->y2 ? coords.y2 : clip->y2;
        int32_t x_from = x_first > clip->x1 ? x_first : clip->x1;
        int32_t x_to = x_last < clip->x2 ? x_last : clip->x2;
        int32_t y;

        if (fh > 0 && x_to >= x_from) {
            for (y = y_from; y <= y_to; y++) {
                lv_draw_rect_dsc_t rect_dsc;
                lv_area_t rect_area;
                lv_opa_t opa = (lv_opa_t)(HELM_CLIMB_FILL_TOP_OPA -
                    (uint32_t)(y - coords.y1) * HELM_CLIMB_FILL_TOP_OPA / (uint32_t)fh);
                uint32_t seg = 0;
                int32_t run = 0;
                bool in = false;
                int32_t x;

                if ((int32_t)opa < 6) {
                    continue;       /* 淡到看不见了：这一行不画 */
                }

                lv_draw_rect_dsc_init(&rect_dsc);
                rect_dsc.bg_color = fill;
                rect_dsc.bg_opa = opa;
                rect_area.y1 = y;
                rect_area.y2 = y;

                for (x = x_from; x <= x_to; x++) {
                    bool under = false;

                    while (seg < (uint32_t)n_pts - 2u &&
                           (int32_t)draw_line_dsc->points[seg + 1].x < x) {
                        seg++;
                    }

                    {
                        lv_point_precise_t a = draw_line_dsc->points[seg];
                        lv_point_precise_t b = draw_line_dsc->points[seg + 1];

                        /* 端点无效或该段不向右延伸：这一列当作"曲线不在上方"，不填。 */
                        if (a.x != LV_DRAW_LINE_POINT_NONE &&
                            a.y != LV_DRAW_LINE_POINT_NONE &&
                            b.x != LV_DRAW_LINE_POINT_NONE &&
                            b.y != LV_DRAW_LINE_POINT_NONE &&
                            b.x > a.x) {
                            int32_t yc = (int32_t)a.y +
                                         (int32_t)((b.y - a.y) * (x - a.x) / (b.x - a.x));

                            under = (yc < y);
                        }
                    }

                    if (under && !in) {
                        run = x;
                        in = true;
                    } else if (!under && in) {
                        rect_area.x1 = run;
                        rect_area.x2 = x - 1;
                        if (rect_area.x2 >= rect_area.x1) {
                            lv_draw_rect(base_dsc->layer, &rect_dsc, &rect_area);
                        }
                        in = false;
                    }
                }

                if (in) {
                    rect_area.x1 = run;
                    rect_area.x2 = x_to;
                    if (rect_area.x2 >= rect_area.x1) {
                        lv_draw_rect(base_dsc->layer, &rect_dsc, &rect_area);
                    }
                }
            }
        }
    }
}

/* 当前位置指示：橙红倒三角**点阵**（RGB565A8，15×10，边缘抗锯齿）。
 *
 * 为什么是"颜色烧进图里"的点阵，而不是"蒙版 + recolor"：
 *   · recolor 的图会被 EPIC 的 eval 直接拒掉（源码里见 recolor 就 `return 0`）
 *     ⇒ 落到 SW 且颜色不由这里掌控；烧进 RGB565A8 后走的是 EPIC 的 IMAGE 路径。
 *   · 本板 EPIC 加速单元**没有 TRIANGLE**，画三角形会回退 SW 并逐行建 mask
 *     （`lv_draw_sw_triangle.c` 里有 `lv_malloc(area_w)`），而光标每秒要挪好几次。
 *   ⇒ 点阵 + 烘色 = 硬件路径 + 抗锯齿 + 颜色确定。
 *
 * 颜色 = `HELM_COLOR_CLIMB_MARK`（0xFF4500，CSS orangered）⇒ RGB565 = 0xFA20
 * （小端字节 0x20,0xFA）。**要改色就得改这里的数据**，生成方式（复现用）：
 * 15×10 画布，顶点 (0,0)-(15,0)-(7.5,10)，每像素 8×8 超采样取覆盖率 ×255；
 * 布局按 LVGL 的 RGB565A8：色平面 15×10×2 B 在前、alpha 平面 15×10 B 在后。
 * alpha 平面里 60 全透明 / 61 全不透明 / 29 个半透明做抗锯齿。 */
static const uint8_t s_climb_mark_data[HELM_CLIMB_MARK_W * HELM_CLIMB_MARK_H * 3] = {
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x9f,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x9f,0x0c,0xd3,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xd3,0x0c,
    0x00,0x2c,0xf3,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xf3,0x2c,0x00,0x00,0x00,0x60,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x60,0x00,0x00,
    0x00,0x00,0x00,0x9f,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x9f,0x00,0x00,0x00,0x00,0x00,0x00,0x0c,0xd3,0xff,0xff,0xff,0xff,0xff,0xd3,0x0c,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x2c,0xf3,0xff,0xff,0xff,0xf3,0x2c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x60,0xff,0xff,0xff,0x60,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x9f,0xff,0x9f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0c,0xa7,0x0c,0x00,0x00,0x00,0x00,0x00,0x00,
};

static const lv_image_dsc_t s_climb_mark_dsc = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_RGB565A8,
    .header.flags = 0,
    .header.w = HELM_CLIMB_MARK_W,
    .header.h = HELM_CLIMB_MARK_H,
    .header.stride = HELM_CLIMB_MARK_W * 2,
    .data_size = sizeof(s_climb_mark_data),
    .data = s_climb_mark_data,
};

/**
 * @brief 把"当前位置"指示（点阵倒三角）画到图层上。
 * @param layer 目标层。
 * @param cx    三角形水平中心（图层坐标）。
 * @param tip_y 尖端 y（图层坐标）；三角形占据它上方 HELM_CLIMB_MARK_H−1 行。
 *
 * @details 现场模式（图表在画、未烘焙）走这里；烘焙模式（导航）由 `climb_cursor`
 *          这个 `lv_image` 对象自己画 ⇒ 两条路径共用同一份点阵，外观一致。
 */
static void helm_climb_mark_blit(lv_layer_t * layer, int32_t cx, int32_t tip_y)
{
    lv_draw_image_dsc_t d;
    lv_area_t a;

    lv_draw_image_dsc_init(&d);
    d.src = &s_climb_mark_dsc;
    a.x1 = cx - HELM_CLIMB_MARK_W / 2;
    a.x2 = a.x1 + (HELM_CLIMB_MARK_W - 1);
    a.y2 = tip_y;
    a.y1 = a.y2 - (HELM_CLIMB_MARK_H - 1);
    lv_draw_image(layer, &d, &a);
}

/**
 * @brief 绘制当前位置指示（小倒三角）。导航用黑色，经过海拔用青绿。
 * @param e LV_EVENT_DRAW_POST。
 * @details 画布可见时指示由 `climb_cursor` 对象承担，本回调直接返回。
 */
static void helm_climb_draw(lv_event_t * e)
{
    lv_layer_t * layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_point_t p;
    uint32_t id;
    uint32_t n;

    if (layer == NULL || s_sh.climb_chart == NULL || s_sh.climb_ser == NULL) {
        return;
    }
    if (s_sh.climb_canvas && !lv_obj_has_flag(s_sh.climb_canvas, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (lv_obj_has_flag(s_sh.climb_chart, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    n = lv_chart_get_point_count(s_sh.climb_chart);
    if (n < 2u || s_sh.climb_n < 2u) {
        return;
    }

    id = s_sh.climb_nav ? (uint32_t)s_sh.climb_now : (n - 1u);
    if (id >= n) {
        id = n - 1u;
    }
    lv_chart_get_point_pos_by_id(s_sh.climb_chart, s_sh.climb_ser, id, &p);
    lv_obj_get_coords(s_sh.climb_chart, &coords);
    /* 尖端贴在剖面点**上方 2 px**，三角整体在折线之上；坐标约定与剖面绘制一致
     * （**不补图表自己的 pad** —— 见 `helm_climb_cursor_place()` 的长注释）。 */
    {
        const int32_t half = HELM_CLIMB_MARK_W / 2;
        int32_t cx = coords.x1 + p.x;
        int32_t tip = coords.y1 + p.y - 2;

        if (cx < coords.x1 + half) {
            cx = coords.x1 + half;
        } else if (cx > coords.x2 - half) {
            cx = coords.x2 - half;
        }
        if (tip < coords.y1 + (HELM_CLIMB_MARK_H - 1)) {
            tip = coords.y1 + (HELM_CLIMB_MARK_H - 1);
        } else if (tip > coords.y2) {
            tip = coords.y2;
        }
        helm_climb_mark_blit(layer, cx, tip);
    }
}

/**
 * @brief 搭建爬升页：海拔 / 累计爬升数字 + 路线（或经过）海拔剖面图。
 * @param root 主界面根对象。
 * @details 导航剖面烘焙到 `climb_canvas`，图表隐藏仅作坐标源；
 *          竖线是独立 2 px 对象。烘焙失败仍走图表 DRAW_TASK。
 */
static void helm_build_climb(lv_obj_t * root)
{
    lv_obj_t * card;
    lv_obj_t * chart;

    /* 用户 2026-09-26 给的图：**剖面卡在上、六格在下**，整页满高。
     * ⇒ 海拔页不再给底部三格数据条让位（`rot_on` 里去掉了 CLIMB），
     *   否则 80px 会把剖面卡从 108 压到 56。 */
    s_sh.climb = helm_overlay(root, HELM_SUBBAR_H,
                              HELM_PAGE_H - HELM_SUBBAR_H);
    helm_speed_head(s_sh.climb, "海拔", NULL, NULL);

    /* 剖面卡（内层 chart/canvas/cursor 机制完全不动，只把"剩余爬升"从页头挪
     * 到卡内右上角——图里它在那儿）。 */
    card = lv_obj_create(s_sh.climb);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, lv_pct(100), HELM_CLIMB_CARD_H);
    helm_style_card(card);
    lv_obj_add_flag(card, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    chart = lv_chart_create(card);
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(chart, lv_pct(100), lv_pct(100));
    lv_obj_align(chart, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_top(chart, 22, 0);
    lv_obj_set_style_pad_bottom(chart, 8, 0);
    lv_obj_set_style_pad_hor(chart, 8, 0);
    lv_obj_set_style_radius(chart, 0, 0);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    /* CIRCULAR：写点不得整图 invalidate（SHIFT 会）。导航烘焙成功后图表隐藏。 */
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_CIRCULAR);
    lv_chart_set_point_count(chart, HELM_CLIMB_PROF_N);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(chart, helm_climb_draw_task, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_event_cb(chart, helm_climb_draw, LV_EVENT_DRAW_POST, NULL);

    s_sh.climb_chart = chart;
    s_sh.climb_ser = lv_chart_add_series(chart, helm_color_snap(HELM_COLOR_CLIMB),
                                         LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(chart, s_sh.climb_ser, LV_CHART_POINT_NONE);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);

    s_sh.climb_canvas = lv_canvas_create(card);
    lv_obj_remove_style_all(s_sh.climb_canvas);
    lv_obj_remove_flag(s_sh.climb_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_sh.climb_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_sh.climb_canvas, lv_pct(100), lv_pct(100));
    lv_obj_align(s_sh.climb_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(s_sh.climb_canvas, LV_OBJ_FLAG_HIDDEN);

    /* 位置指示就是一张 `lv_image`（点阵倒三角，颜色已烧进 RGB565A8 数据）：
     * 不需要自绘回调，尺寸由图片自己给，走的也是 EPIC 的 IMAGE 路径。 */
    s_sh.climb_cursor = lv_image_create(card);
    lv_obj_remove_flag(s_sh.climb_cursor, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(s_sh.climb_cursor, &s_climb_mark_dsc);
    lv_obj_align(s_sh.climb_cursor, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(s_sh.climb_cursor, LV_OBJ_FLAG_HIDDEN);

    s_sh.climb_title = helm_lab(card, s_sh.font_lab, HELM_COLOR_CLIMB, "海拔剖面");
    lv_obj_add_flag(s_sh.climb_title, LV_OBJ_FLAG_HIDDEN);  /* 图里没有这个标题 */
    /* "剩余爬升 280 m"：标签 MUTE + 数值 INK，整体贴卡内右上角 */
    s_sh.climb_mark = helm_lab(card, s_sh.font_lab, HELM_COLOR_MUTE, "");
    lv_obj_align(s_sh.climb_mark, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_move_foreground(s_sh.climb_mark);
    lv_obj_move_foreground(s_sh.climb_cursor);
    /* 六格卡：2 列 x 3 行 = 海拔(m) | 坡度(%) / 爬升(m) | 下降(m) /
     * 最高(m) | 最低(m)。格子沿用数据页那套（helm_cell_make）。 */
    {
        static const char * kname[HELM_CLIMB_N] = {
            "海拔 (m)", "坡度 (%)", "爬升 (m)", "下降 (m)", "最高 (m)",
            "最低 (m)"
        };
        lv_obj_t * grid;
        int i2;

        grid = lv_obj_create(s_sh.climb);
        lv_obj_remove_style_all(grid);
        helm_style_card(grid);
        helm_grow_y(grid);
        lv_obj_set_width(grid, lv_pct(100));
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(grid, 0, 0);
        lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

        for (i2 = 0; i2 < HELM_CLIMB_N / 2; i2++) {
            lv_obj_t * r2 = lv_obj_create(grid);

            lv_obj_remove_style_all(r2);
            lv_obj_set_width(r2, lv_pct(100));
            helm_grow_y(r2);
            lv_obj_set_flex_flow(r2, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(r2, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_hor(r2, 10, 0);
            lv_obj_set_style_pad_column(r2, 12, 0);
            lv_obj_clear_flag(r2, LV_OBJ_FLAG_SCROLLABLE);
            if (i2 < HELM_CLIMB_N / 2 - 1) {
                lv_obj_set_style_border_side(r2, LV_BORDER_SIDE_BOTTOM, 0);
                lv_obj_set_style_border_width(r2, 1, 0);
                lv_obj_set_style_border_color(r2,
                                              helm_color(HELM_COLOR_HAIR), 0);
            }

            helm_cell_make(r2, s_sh.climb_rail, s_sh.climb_lab,
                           s_sh.climb_val, i2 * 2);
            helm_cell_make(r2, s_sh.climb_rail, s_sh.climb_lab,
                           s_sh.climb_val, i2 * 2 + 1);
            helm_label_set(s_sh.climb_lab[i2 * 2], kname[i2 * 2]);
            helm_label_set(s_sh.climb_lab[i2 * 2 + 1], kname[i2 * 2 + 1]);
        }
    }

    s_sh.climb_n = 0;
    s_sh.climb_now = 0;
    s_sh.climb_nav = false;
}

static void helm_build_chrome(lv_obj_t * root)
{
    lv_obj_t * col;

    s_sh.subbar = lv_obj_create(root);
    lv_obj_remove_style_all(s_sh.subbar);
    helm_style_paper(s_sh.subbar);
    lv_obj_set_size(s_sh.subbar, PAGE_HOR_RES, HELM_SUBBAR_H);
    lv_obj_align(s_sh.subbar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_flex_flow(s_sh.subbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sh.subbar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(s_sh.subbar, 10, 0);
    lv_obj_add_flag(s_sh.subbar, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(s_sh.subbar, 0, 0);
    lv_obj_add_flag(s_sh.subbar, LV_OBJ_FLAG_HIDDEN);
    s_sh.sub_left = helm_lab(s_sh.subbar, s_sh.font_lab, HELM_COLOR_INK, "DATA");
    s_sh.sub_mid = helm_lab(s_sh.subbar, s_sh.font_lab, HELM_COLOR_INK, "00:00");
    /* 右槽：导航中显示"距下一路口 / 剩余里程"，其余时候留空（LAP 已剔除）。 */
    s_sh.sub_right = helm_lab(s_sh.subbar, s_sh.font_lab, HELM_COLOR_INK, "");

    s_sh.rotbar = lv_obj_create(root);
    lv_obj_remove_style_all(s_sh.rotbar);
    helm_style_scr(s_sh.rotbar);
    lv_obj_set_size(s_sh.rotbar, PAGE_HOR_RES, HELM_ROTBAR_H);
    lv_obj_set_style_bg_opa(s_sh.rotbar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_sh.rotbar, HELM_GAP, 0);
    lv_obj_set_style_pad_column(s_sh.rotbar, HELM_GAP, 0);
    lv_obj_set_flex_flow(s_sh.rotbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sh.rotbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_sh.rotbar, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(s_sh.rotbar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_remove_flag(s_sh.rotbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_sh.rotbar, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    s_sh.rot_cell[0] = helm_rot_col(s_sh.rotbar, &s_sh.rot_lab[0],
                                    &s_sh.rot_val[0], s_sh.font_rot);
    lv_label_set_text_static(s_sh.rot_lab[0], "速度");
    lv_obj_set_style_text_color(s_sh.rot_val[0], helm_color(HELM_COLOR_NAV), 0);

    s_sh.rot_cell[1] = helm_rot_col(s_sh.rotbar, &s_sh.rot_lab[1],
                                    &s_sh.rot_val[1], s_sh.font_val);
    s_sh.rot_cell[2] = helm_rot_col(s_sh.rotbar, &s_sh.rot_lab[2],
                                    &s_sh.rot_val[2], s_sh.font_val);
    helm_spark_bind(s_sh.rot_cell[1], s_sh.rot_lab[1], 13);
    helm_spark_bind(s_sh.rot_cell[2], s_sh.rot_lab[2], 13);
    lv_label_set_text_static(s_sh.rot_lab[1], "时间");
    lv_label_set_text_static(s_sh.rot_val[1], "00:00");
    lv_label_set_text_static(s_sh.rot_lab[2], "里程");
    lv_label_set_text_static(s_sh.rot_val[2], "0.0");
    lv_obj_move_foreground(s_sh.rot_lab[0]);
    lv_obj_move_foreground(s_sh.rot_val[0]);
    lv_obj_move_foreground(s_sh.rot_lab[1]);
    lv_obj_move_foreground(s_sh.rot_val[1]);
    lv_obj_move_foreground(s_sh.rot_lab[2]);
    lv_obj_move_foreground(s_sh.rot_val[2]);

    s_sh.nav_ban = lv_obj_create(root);
    lv_obj_remove_style_all(s_sh.nav_ban);
    /* 回到通栏横幅（用户 2026-09-25："不需要路牌了，直接蓝色横幅"）。 */
    lv_obj_set_size(s_sh.nav_ban, PAGE_HOR_RES, HELM_NAV_BAN_H);
    lv_obj_add_flag(s_sh.nav_ban, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(s_sh.nav_ban, 0, HELM_SUBBAR_H);
    /* 卡片底色 = **状态语义**（导航蓝 / 偏航红 / 到达绿，由状态块每拍刷，见 helm_palette.h
     * 的 BAN_*）。参照市面码表的深色实心转向牌：字反白、卡片近乎实心；原来的"橙字压橙底"
     * （NAV on NAV_FILL）主次不分的问题不再有。 */
    lv_obj_set_style_bg_color(s_sh.nav_ban, helm_color(HELM_COLOR_BAN_NAV), 0);
    /* 半透强度：200（与全局通知弹窗同档）。白卡时代试过 150 —— 地图是透出来了，但地图
     * 路名会和横幅文字叠在一起；200 时底图只剩两成，够看出"卡是半透的"又不抢字。
     * 性能：EPIC 混合 <1% 屏带宽（240×58 混合 ~0.2 ms vs 同面积上屏 ~35 ms）。 */
    lv_obj_set_style_bg_opa(s_sh.nav_ban, 200, 0);
    /* 参照图是圆角牌：去掉原来通栏时代的底边发丝线，改成圆角。 */
    lv_obj_set_style_radius(s_sh.nav_ban, 8, 0);
    lv_obj_set_style_border_width(s_sh.nav_ban, 0, 0);
    /* 参照图：透明黑 + **圆角矩形**，不描边（底边发丝线是通栏横幅时代的东西）。 */
    lv_obj_set_style_radius(s_sh.nav_ban, HELM_NAV_SIGN_RADIUS, 0);
    lv_obj_set_style_border_width(s_sh.nav_ban, 0, 0);
    lv_obj_set_style_border_side(s_sh.nav_ban, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_sh.nav_ban, helm_color(HELM_COLOR_HAIR), 0);
    lv_obj_set_flex_flow(s_sh.nav_ban, LV_FLEX_FLOW_ROW);
    /* 主轴 START：徽章**贴左**（用户 2026-09-25："箭头可以继续向左侧移动"）。文字块不跟着
     * 一起居中，而是 flex_grow 吃掉徽章右侧的全部宽度、再在自己这块里居中（"文字在它显示
     * 的那个区域居中，不要靠左"）。 */
    lv_obj_set_flex_align(s_sh.nav_ban, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(s_sh.nav_ban, 8, 0);
    lv_obj_set_style_pad_gap(s_sh.nav_ban, 8, 0);
    lv_obj_add_flag(s_sh.nav_ban, LV_OBJ_FLAG_HIDDEN);

    /* 转向图标 = 实心圆角徽章（34×34），字反白。 */
    /* 参照图：白箭头**直接**画在灰卡上（没有白方块），所以徽章只当容器、背景透明。 */
    s_sh.nav_ban_badge = lv_obj_create(s_sh.nav_ban);
    lv_obj_remove_style_all(s_sh.nav_ban_badge);
    lv_obj_set_size(s_sh.nav_ban_badge, 42, 42);
    lv_obj_set_style_radius(s_sh.nav_ban_badge, 13, 0);
    lv_obj_set_style_bg_color(s_sh.nav_ban_badge, helm_color(HELM_COLOR_PAPER), 0);
    /* 徽章必须**不透明**：路牌那版曾把它设成透明（想让白箭头直接压在卡上），
     * 回退配色时漏了这一处，导致白箭头落在浅色卡上完全看不见（用户 2026-09-25 现场发现）。 */
    lv_obj_set_style_bg_opa(s_sh.nav_ban_badge, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_sh.nav_ban_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_sh.nav_ban_badge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sh.nav_ban_badge, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 箭头走 FreeType 取 30 px（点阵最大只有 22，配 42 px 徽章太瘦；
     * 缺字时回退点阵，两种都能显示 ↑←→U●）。 */
    s_sh.nav_ban_ico = helm_lab(s_sh.nav_ban_badge,
                                s_sh.font_val,
                                HELM_COLOR_PAPER, "↑");
    lv_obj_set_style_text_align(s_sh.nav_ban_ico, LV_TEXT_ALIGN_CENTER, 0);

    col = lv_obj_create(s_sh.nav_ban);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    /* 吃掉徽章右侧的剩余宽度；列内两个子块（r1 距离行 / rd 目的地行）横向居中。 */
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_width(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    /* 深色卡自带对比（地图只透出两成），不再需要白卡时代的"文字底板"。 */
    {
        /* 第一行：距离（主字，墨色 22px）+ 动作（次字，灰 15px）—— 原来把
         * "距离 · 动作"塞进同一个标签，主次不分而且低对比。 */
        lv_obj_t * r1 = lv_obj_create(col);

        lv_obj_remove_style_all(r1);
        lv_obj_set_width(r1, LV_SIZE_CONTENT);
        lv_obj_set_height(r1, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r1, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_column(r1, 6, 0);
        lv_obj_clear_flag(r1, LV_OBJ_FLAG_SCROLLABLE);
        s_sh.nav_ban_m = helm_lab(r1, helm_font_sys(22, s_sh.font_val),
                                  HELM_COLOR_INK, "--");
        s_sh.nav_ban_act = helm_lab(r1, helm_font_sys(15, s_sh.font_title),
                                    HELM_COLOR_MUTE, "");
    }
    /* 路名必须走 FreeType：点阵字库（helm_mism4_*）只收了 UI 文案那 486 个字，
     * 真实路名（「金陵路」的 金/陵 都在缺字之列）会整块显示不出来。
     * 地图上的路名一直用 myvendor_system_font_get() 所以正常，横幅这里漏了。 */
    s_sh.nav_ban_rd = helm_lab(col, helm_font_sys(15, s_sh.font_title),
                               HELM_COLOR_MUTE, "");
    /* DOT 模式需限制为一行高度，否则内容高度会随长地名增长而换行。 */
    lv_label_set_long_mode(s_sh.nav_ban_rd, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_sh.nav_ban_rd, lv_pct(100));
    lv_obj_set_height(s_sh.nav_ban_rd,
                      lv_font_get_line_height(lv_obj_get_style_text_font(s_sh.nav_ban_rd, 0)));
    lv_obj_set_style_text_align(s_sh.nav_ban_rd, LV_TEXT_ALIGN_CENTER, 0);

    /* 左下角两行（导航剩余 / 全程）与右下角比例尺（用户 2026-09-25）。都浮在地图下沿之上
     * 一点（地图体到 HELM_SUBBAR_H + HELM_MAP_BODY_H 为止，下面就是数据条）。 */
    s_sh.nav_sum = lv_obj_create(root);
    lv_obj_remove_style_all(s_sh.nav_sum);
    lv_obj_set_size(s_sh.nav_sum, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_flag(s_sh.nav_sum, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s_sh.nav_sum, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_sh.nav_sum, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_sh.nav_sum, 1, 0);
    s_sh.nav_sum_l1 = helm_lab(s_sh.nav_sum, s_sh.font_title,
                               HELM_COLOR_INK, "--");
    /* 两行都用墨色（用户 2026-09-25："里程不要用灰色字体"）。 */
    s_sh.nav_sum_l2 = helm_lab(s_sh.nav_sum, s_sh.font_title,
                               HELM_COLOR_INK, "--");
    lv_obj_align(s_sh.nav_sum, LV_ALIGN_BOTTOM_LEFT, 6,
                 -(HELM_PAGE_H - HELM_SUBBAR_H - HELM_MAP_BODY_H) - 4);

    s_sh.nav_scale = lv_obj_create(root);
    lv_obj_remove_style_all(s_sh.nav_scale);
    lv_obj_set_size(s_sh.nav_scale, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_flag(s_sh.nav_scale, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s_sh.nav_scale, LV_OBJ_FLAG_SCROLLABLE);
    /* 标尺 = 文字 + 下边框当横线 + 两端小竖线（用户 2026-09-25："比例尺线两端缺少竖线"）。 */
    lv_obj_set_flex_flow(s_sh.nav_scale, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sh.nav_scale, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_border_width(s_sh.nav_scale, 1, 0);
    lv_obj_set_style_border_side(s_sh.nav_scale, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_sh.nav_scale, helm_color(HELM_COLOR_INK), 0);
    lv_obj_set_style_pad_hor(s_sh.nav_scale, 2, 0);
    {
        lv_obj_t * tick_l = lv_obj_create(s_sh.nav_scale);

        lv_obj_remove_style_all(tick_l);
        lv_obj_set_size(tick_l, 1, 6);
        lv_obj_set_style_bg_color(tick_l, helm_color(HELM_COLOR_INK), 0);
        lv_obj_set_style_bg_opa(tick_l, LV_OPA_COVER, 0);
        lv_obj_clear_flag(tick_l, LV_OBJ_FLAG_SCROLLABLE);
        s_sh.nav_scale_lab = helm_lab(s_sh.nav_scale, s_sh.font_title,
                                      HELM_COLOR_INK, "200m");
        {
            lv_obj_t * tick_r = lv_obj_create(s_sh.nav_scale);

            lv_obj_remove_style_all(tick_r);
            lv_obj_set_size(tick_r, 1, 6);
            lv_obj_set_style_bg_color(tick_r, helm_color(HELM_COLOR_INK), 0);
            lv_obj_set_style_bg_opa(tick_r, LV_OPA_COVER, 0);
            lv_obj_clear_flag(tick_r, LV_OBJ_FLAG_SCROLLABLE);
        }
    }
    lv_obj_align(s_sh.nav_scale, LV_ALIGN_BOTTOM_RIGHT, -6,
                 -(HELM_PAGE_H - HELM_SUBBAR_H - HELM_MAP_BODY_H) - 4);

    lv_obj_add_flag(s_sh.nav_sum, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_sh.nav_scale, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t * helm_sum_row(lv_obj_t * parent)
{
    lv_obj_t * row = lv_obj_create(parent);

    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, HELM_DOCK_COL_GAP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static void helm_build_pause(lv_obj_t * root)
{
    lv_obj_t * head;
    lv_obj_t * ico;
    lv_obj_t * grid;
    lv_obj_t * row;

    s_sh.pause_dock = helm_dock_create(root, true);
    lv_obj_add_flag(s_sh.pause_dock, LV_OBJ_FLAG_HIDDEN);

    head = lv_obj_create(s_sh.pause_dock);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), HELM_DOCK_HEAD_H);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, HELM_DOCK_COL_GAP, 0);
    ico = helm_icon_create(head, HELM_ICO_PAUSE, HELM_DOCK_ICO_HEAD);
    helm_icon_set_color(ico, helm_color(HELM_COLOR_INK));
    s_sh.pause_title = helm_lab(head, s_sh.font_title, HELM_COLOR_INK, "手动暂停");

    grid = helm_sum_grid(s_sh.pause_dock);
    row = helm_sum_row(grid);
    helm_sum_cell(row, HELM_ICO_TIME, HELM_COLOR_NAV, "时间", &s_sh.pause_time);
    helm_sum_cell(row, HELM_ICO_DIST, HELM_COLOR_INK, "里程", &s_sh.pause_dist);
    row = helm_sum_row(grid);
    helm_sum_cell(row, HELM_ICO_AVG, HELM_COLOR_NAV, "均速", &s_sh.pause_avg);
    helm_sum_cell(row, HELM_ICO_HR, HELM_COLOR_HR, "均心率", &s_sh.pause_hr);
    helm_softkeys_create(s_sh.pause_dock, HELM_COLOR_INK);
}

static void helm_build_save(lv_obj_t * root)
{
    lv_obj_t * card;
    lv_obj_t * body;
    lv_obj_t * grid;
    lv_obj_t * row;

    s_sh.save_mask = helm_mask_create(root);
    lv_obj_add_flag(s_sh.save_mask, LV_OBJ_FLAG_HIDDEN);
    card = helm_card_create(s_sh.save_mask, HELM_ICO_SAVE, "保存本次骑行？", false);
    body = helm_card_body(card);
    grid = helm_sum_grid(body);
    lv_obj_set_style_margin_top(grid, 8, 0);
    row = helm_sum_row(grid);
    helm_sum_cell(row, HELM_ICO_TIME, HELM_COLOR_NAV, "时间", &s_sh.save_time);
    helm_sum_cell(row, HELM_ICO_DIST, HELM_COLOR_INK, "里程", &s_sh.save_dist);
    row = helm_sum_row(grid);
    helm_sum_cell(row, HELM_ICO_AVG, HELM_COLOR_NAV, "均速", &s_sh.save_avg);
    helm_sum_cell(row, HELM_ICO_CLIMB, HELM_COLOR_CLIMB, "爬升", &s_sh.save_gain);
    row = helm_sum_row(grid);
    helm_sum_cell(row, HELM_ICO_MAX, HELM_COLOR_NAV, "极速", &s_sh.save_max);
    helm_sum_cell(row, HELM_ICO_HR, HELM_COLOR_HR, "均心率", &s_sh.save_hr);
    helm_softkeys_create(body, HELM_COLOR_INK);
}

static void helm_build_arrive(lv_obj_t * root)
{
    lv_obj_t * head;
    lv_obj_t * ico;

    s_sh.arrive_dock = helm_dock_create(root, true);
    lv_obj_add_flag(s_sh.arrive_dock, LV_OBJ_FLAG_HIDDEN);

    head = lv_obj_create(s_sh.arrive_dock);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), HELM_DOCK_HEAD_H);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, HELM_DOCK_COL_GAP, 0);
    ico = helm_icon_create(head, HELM_ICO_FLAG, HELM_DOCK_ICO_HEAD);
    helm_icon_set_color(ico, helm_color(HELM_COLOR_INK));
    helm_lab(head, s_sh.font_title, HELM_COLOR_INK, "导航结束");
}

void helm_shell_bind_keys(void)
{
    lv_port_buttons_set_page_scroll_cb(helm_on_prev, NULL);
    /* 主界面不要 KEY1 双击的"延后单击"：注销掉（菜单页注册过，见 helm_menu.c）。 */
    lv_port_buttons_set_page_scroll_double_cb(NULL, NULL);
    lv_port_buttons_set_page_confirm_cb(helm_on_next, NULL);
    lv_port_buttons_set_page_longpress_cb(helm_on_menu, NULL);
    lv_port_buttons_set_page_longpress2_cb(helm_on_rec, NULL);
    lv_port_buttons_set_page_longpress2_up_cb(NULL, NULL);
    LVGL_STALL("helm_bind_keys attached=%d covered=%d",
        (int)s_sh.attached, (int)s_sh.ui_covered);
}

void helm_shell_autopause_tick(void)
{
    helm_autopause_tick();
    helm_ride_status_publish();
}

void helm_shell_set_paused(bool paused)
{
    if (s_sh.paused == paused) {
        return;
    }

    if (paused && !s_sh.paused) {
        myvendor_sound_pause();
    }

    s_sh.paused = paused;
    if (s_sh.ui_covered) {
        return;
    }

    /* 只翻暂停条，避免从菜单/保存框回来后整页 apply_view 卡住。 */
    helm_pause_dock_apply();
    helm_nums_defer_begin();
}

bool helm_shell_paused(void)
{
    return s_sh.paused;
}

/* ── 演示：表盘页几何/文案 dump（仅模拟器；布局断言脚本读 verify.log）────
 * 坐标口径与 S3a 的 [ride]/[dial] dump 相同：geo 是**屏幕坐标**（页面根从
 * y=24 起，脚本减 24 得页面坐标）；刻度 pts 是页面坐标点集 + 线宽。 */
static void helm_dial_dump_geo(const char * name, lv_obj_t * obj)
{
    lv_area_t c;

    if (obj == NULL) {
        printf("[dial] geo %s=null\n", name);
        return;
    }
    lv_obj_get_coords(obj, &c);
    printf("[dial] geo %s=%d,%d,%d,%d\n", name, (int)c.x1, (int)c.y1,
           (int)c.x2, (int)c.y2);
}

static void helm_dial_dump_ink(const char * name, lv_obj_t * obj)
{
    const lv_font_t * font;
    const char * txt;
    lv_point_t sz;

    if (obj == NULL) {
        printf("[dial] ink %s=null\n", name);
        return;
    }
    font = lv_obj_get_style_text_font(obj, 0);
    txt = lv_label_get_text(obj);
    if (font == NULL || txt == NULL) {
        printf("[dial] ink %s=0x0\n", name);
        return;
    }
    lv_txt_get_size(&sz, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    printf("[dial] ink %s=%dx%d\n", name, (int)sz.x, (int)sz.y);
}

static void helm_dial_dump_text(const char * name, lv_obj_t * obj)
{
    if (obj == NULL) {
        printf("[dial] text %s=null\n", name);
        return;
    }
    printf("[dial] text %s=%s\n", name, lv_label_get_text(obj));
}

static void helm_dial_dump_color(const char * name, lv_obj_t * obj)
{
    lv_color_t c;

    if (obj == NULL) {
        printf("[dial] color %s=null\n", name);
        return;
    }
    c = lv_obj_get_style_text_color(obj, 0);
    printf("[dial] color %s=%02X%02X%02X\n", name,
           (unsigned)c.red, (unsigned)c.green, (unsigned)c.blue);
}

/** @brief 演示：表盘页布局/数据 dump（仅调试；模拟器巡页调用）。 */
void helm_shell_demo_dial_dump(void)
{
    char name[16];
    int i;
    const bicycle_runtime_t * rt = bicycle_runtime_get();
    lv_color_t track = helm_color(HELM_COLOR_TRACK);
    lv_color_t mute = helm_color(HELM_COLOR_MUTE);

    printf("[dial] page=%d view=%d riding=%d paused=%d idx=%u n=%u rotbar=%d\n",
           (int)lvgl_page_current_id(), (int)s_sh.view_id,
           helm_riding() ? 1 : 0, s_sh.paused ? 1 : 0,
           (unsigned)*helm_active_idx(), (unsigned)helm_page_count(),
           helm_obj_shown(s_sh.rotbar) ? 1 : 0);
    if (rt == NULL) {
        printf("[dial] data speed=0.0 dist=0 t=0\n");
    } else {
        printf("[dial] data speed=%.1f dist=%.0f t=%llu rec=%d\n",
               (double)rt->speed_kph, (double)rt->session_distance_m,
               (unsigned long long)rt->session_elapsed_ms,
               (int)rt->recording);
        printf("[dial] hr %u/%d avg=%u max=%u cad=%u/%d\n",
               (unsigned)rt->hr_bpm, (int)rt->hr_valid,
               (unsigned)rt->hr_avg_bpm, (unsigned)rt->hr_max_bpm,
               (unsigned)rt->cadence_rpm, (int)rt->cadence_valid);
        printf("[dial] alt=%.0f/%d grade=%.1f\n",
               (double)rt->altitude_m, (int)rt->has_altitude,
               (double)rt->grade_pct);
    }
    if (!s_sh.attached || s_sh.data == NULL) {
        printf("[dial] dump unavailable\n");
        return;
    }

    lv_obj_update_layout(lv_screen_active());
    printf("[dial] shown pane=%d rotbar=%d\n",
           helm_obj_shown(s_sh.data) ? 1 : 0,
           helm_obj_shown(s_sh.rotbar) ? 1 : 0);
    printf("[dial] ref track=%02X%02X%02X mute=%02X%02X%02X\n",
           (unsigned)track.red, (unsigned)track.green, (unsigned)track.blue,
           (unsigned)mute.red, (unsigned)mute.green, (unsigned)mute.blue);
    {
        lv_color_t climb = helm_color(HELM_COLOR_CLIMB);
        lv_color_t done = helm_color(HELM_COLOR_ELEV_DONE);
        lv_color_t ink = helm_color(HELM_COLOR_INK);

        printf("[dial] ref2 climb=%02X%02X%02X done=%02X%02X%02X ink=%02X%02X%02X\n",
               (unsigned)climb.red, (unsigned)climb.green, (unsigned)climb.blue,
               (unsigned)done.red, (unsigned)done.green, (unsigned)done.blue,
               (unsigned)ink.red, (unsigned)ink.green, (unsigned)ink.blue);
    }
    if (s_sh.dial_arc != NULL) {
        lv_color_t ind = lv_obj_get_style_arc_color(s_sh.dial_arc,
                                                    LV_PART_INDICATOR);
        lv_color_t trk = lv_obj_get_style_arc_color(s_sh.dial_arc,
                                                    LV_PART_MAIN);
        lv_color_t knob = lv_obj_get_style_bg_color(s_sh.dial_arc,
                                                    LV_PART_KNOB);

        printf("[dial] arc ind=%02X%02X%02X trk=%02X%02X%02X knob=%02X%02X%02X\n",
               (unsigned)ind.red, (unsigned)ind.green, (unsigned)ind.blue,
               (unsigned)trk.red, (unsigned)trk.green, (unsigned)trk.blue,
               (unsigned)knob.red, (unsigned)knob.green, (unsigned)knob.blue);
    }

    helm_dial_dump_geo("pane", s_sh.data);
    helm_dial_dump_geo("arc", s_sh.dial_arc);
    helm_dial_dump_geo("hero", s_sh.dial_hero);
    helm_dial_dump_geo("speed", s_sh.dial_speed);
    helm_dial_dump_ink("speed", s_sh.dial_speed);
    helm_dial_dump_text("speed", s_sh.dial_speed);
    helm_dial_dump_color("speed", s_sh.dial_speed);
    helm_dial_dump_geo("unit", s_sh.dial_unit);
    helm_dial_dump_text("unit", s_sh.dial_unit);
    helm_dial_dump_color("unit", s_sh.dial_unit);
    for (i = 0; i < 7; i++) {
        const int deg = 225 + i * 45;
        const int w = ((deg % 90) == 0) ? 3 : 1;

        printf("[dial] pts tick%d=%d,%d,%d,%d w=%d\n", i,
               (int)s_sh.dial_tick_pts[i][0].x,
               (int)s_sh.dial_tick_pts[i][0].y,
               (int)s_sh.dial_tick_pts[i][1].x,
               (int)s_sh.dial_tick_pts[i][1].y, w);
        lv_snprintf(name, sizeof(name), "tick%d", i);
        helm_dial_dump_geo(name, s_sh.dial_tick[i]);
    }
    for (i = 0; i < 3; i++) {
        lv_snprintf(name, sizeof(name), "spdband%d", i);
        helm_dial_dump_geo(name, s_sh.dial_spd_band[i]);
        lv_snprintf(name, sizeof(name), "spdlab%d", i);
        helm_dial_dump_geo(name, s_sh.dial_spd_lab[i]);
        helm_dial_dump_text(name, s_sh.dial_spd_lab[i]);
    }
    for (i = 0; i < 3; i++) {
        lv_snprintf(name, sizeof(name), "spdfill%d", i);
        helm_dial_dump_geo(name, s_sh.dial_spd_fill[i]);
    }
    helm_dial_dump_geo("spdedge", s_sh.dial_spd_edge);
    helm_dial_dump_geo("hr_val", s_sh.dial_hr_val);
    helm_dial_dump_ink("hr_val", s_sh.dial_hr_val);
    helm_dial_dump_text("hr_val", s_sh.dial_hr_val);
    helm_dial_dump_color("hr_val", s_sh.dial_hr_val);
    helm_dial_dump_geo("hr_unit", s_sh.dial_hr_unit);
    helm_dial_dump_text("hr_unit", s_sh.dial_hr_unit);
    helm_dial_dump_geo("hr_stats", s_sh.dial_hr_stats);
    helm_dial_dump_text("hr_stats", s_sh.dial_hr_stats);
    helm_dial_dump_color("hr_stats", s_sh.dial_hr_stats);
    for (i = 0; i < 5; i++) {
        lv_snprintf(name, sizeof(name), "hrband%d", i);
        helm_dial_dump_geo(name, s_sh.dial_hr_band[i]);
    }
    for (i = 0; i < 4; i++) {
        lv_snprintf(name, sizeof(name), "hrsep%d", i);
        helm_dial_dump_geo(name, s_sh.dial_hr_sep[i]);
    }
    for (i = 0; i < 5; i++) {
        lv_snprintf(name, sizeof(name), "hrfill%d", i);
        helm_dial_dump_geo(name, s_sh.dial_hr_fill[i]);
    }
    helm_dial_dump_geo("hredge", s_sh.dial_hr_edge);
    for (i = 0; i < 4; i++) {
        lv_snprintf(name, sizeof(name), "metbox%d", i);
        helm_dial_dump_geo(name, s_sh.dial_met_box[i]);
        lv_snprintf(name, sizeof(name), "metlab%d", i);
        helm_dial_dump_geo(name, s_sh.dial_met_lab[i]);
        helm_dial_dump_ink(name, s_sh.dial_met_lab[i]);
        helm_dial_dump_text(name, s_sh.dial_met_lab[i]);
        lv_snprintf(name, sizeof(name), "metval%d", i);
        helm_dial_dump_geo(name, s_sh.dial_met_val[i]);
        helm_dial_dump_ink(name, s_sh.dial_met_val[i]);
        helm_dial_dump_text(name, s_sh.dial_met_val[i]);
        helm_dial_dump_color(name, s_sh.dial_met_val[i]);
    }
    helm_dial_dump_geo("elev_line", s_sh.dial_elev_line);
    helm_dial_dump_geo("elev_lab", s_sh.dial_elev_lab);
    helm_dial_dump_text("elev_lab", s_sh.dial_elev_lab);
    helm_dial_dump_geo("grid_lab", s_sh.dial_grid_lab);
    helm_dial_dump_text("grid_lab", s_sh.dial_grid_lab);
    helm_dial_dump_geo("elev_cv", s_sh.dial_elev_cv);
    helm_dial_dump_geo("ph_card", s_sh.dial_ph_card);
    helm_dial_dump_geo("ph_hint", s_sh.dial_ph_hint);
    helm_dial_dump_text("ph_hint", s_sh.dial_ph_hint);
    printf("[dial] prof valid=%d sig=%08X grid=%um shown=%d runs=%u minrun=%u\n",
           s_dial_prof_valid ? 1 : 0, (unsigned)s_dial_prof_sig,
           (unsigned)s_dial_prof_grid,
           helm_obj_shown(s_sh.dial_elev_cv) ? 1 : 0,
           (unsigned)s_dial_run_n, (unsigned)s_dial_run_min);
}

/** @brief 演示/取证：强制剖面窗口的"当前位置"像素（-1 = 用真实位置）。
 *  @note 模拟器里沿路线位置推不动（GPX 回放按真实时间步进），拍"已骑遮盖"
 *        要它停在中间，故留这个注入口。产品路径不会调。
 */
void helm_shell_demo_nav_cursor(int px)
{
    s_dial_cursor_ovr = px;
}

/** @brief 演示/取证：把"已记录"的剖面窗口灌满（记录态要真骑 10 分钟才出图）。
 *  @param n 点数（2..HELM_CLIMB_PROF_N）；step_m 相邻点里程（米）。
 *  @note 只填 `s_rec_ele` / `s_rec_dist` 两个环形缓冲，不碰 runtime；海拔走
 *        24 点三角波，只为让分格线与脊线都看得见。同 demo_banner 一类，
 *        属调试注入，不在产品路径上。
 */
void helm_shell_demo_elev_seed(uint8_t n, uint32_t step_m)
{
    /* 24 点起伏，逐点落差只有 0 / ±1 / ±2 m ⇒ 按 50 m 步长正好落在
     * 平路（绿）/ 缓坡（黄）/ 陡坡（红）三档，三种色一起看得到。 */
    static const int8_t bump[24] = {
        0,  1,  3,  3,  5,  5,  6,  6,  4,  4,  3,  3,
        3,  4,  6,  6,  6,  5,  5,  3,  3,  2,  1,  0
    };
    uint32_t i;

    if (n < 2u || n > HELM_CLIMB_PROF_N) {
        return;
    }
    for (i = 0; i < n; i++) {
        s_rec_ele[i] = (int16_t)(30 + bump[i % 24u]);
        s_rec_dist[i] = i * step_m;
    }
    s_rec_ele_n = n;
    s_rec_ele_slot = 0xffffffffu;   /* 别让下一拍的 in-place 更新盖掉末点 */
    s_dial_prof_valid = false;      /* 强制重烘一次 */
}

/** @brief 演示：暂停态（收起暂停条，只验页面数字退灰；仅调试）。 */
void helm_shell_demo_dial_pause(bool on)
{
    if (!s_sh.attached) {
        return;
    }
    s_sh.paused = on;
    /* 本包只验页面里的"数字退灰"；暂停条是覆盖层，取证时先收起。 */
    helm_show(s_sh.pause_dock, false);
    helm_refresh_numbers();
}

/**
 * @brief 演示：直接显示指定页面（UI 线程）。
 * @details 名字映射到固定页并加页面锁，`helm_page_at()` 之后恒返回该页；
 *          同时把活动 index 挪到轮转表里对应的槽位（海拔页的采样槽靠
 *          `helm_active_idx()` 取，必须落在 CLIMB 槽）。任意翻页键会清掉锁，
 *          恢复正常轮转。
 */
bool helm_shell_demo_page(const char * name)
{
    helm_page_id_t id;
    uint8_t * idx;

    if (name == NULL) {
        return false;
    }

    if (strcmp(name, "standby") == 0) {
        id = HELM_PAGE_STANDBY;
    } else if (strcmp(name, "ride") == 0) {
        id = HELM_PAGE_DATA;
    } else if (strcmp(name, "dash") == 0) {
        id = HELM_PAGE_DASH;
    } else if (strcmp(name, "data") == 0) {
        id = HELM_PAGE_DATA_ALL;
    } else if (strcmp(name, "map") == 0) {
        id = HELM_PAGE_MAP;
    } else if (strcmp(name, "climb") == 0) {
        id = HELM_PAGE_CLIMB;
    } else {
        return false;
    }

    s_demo_page = id;
    idx = helm_active_idx();
    if (helm_in_session()) {
        /* 骑行轮转：表盘 / 仪表台 / 数据页 / 地图页。 */
        switch (id) {
        case HELM_PAGE_DASH:
            *idx = 1;
            break;
        case HELM_PAGE_DATA_ALL:
            *idx = 2;
            break;
        case HELM_PAGE_MAP:
            *idx = 3;
            break;
        case HELM_PAGE_CLIMB:
            /* 2026-11-04 起海拔页排在全数字之后、地图之前（P02 关闭时是第 3 槽）。 */
            *idx = HELM_ENABLE_PAGE_DATA ? 3 : 2;
            break;
        default:
            *idx = 0;
            break;
        }
    } else {
        /* 待机轮转：主页面 / 表盘页 / 数据页；其余页没有槽位，退回 0。 */
        switch (id) {
        case HELM_PAGE_DATA:
            *idx = 1;
            break;
        case HELM_PAGE_DATA_ALL:
            *idx = 2;
            break;
        default:
            *idx = 0;
            break;
        }
    }

    s_sh.view_anim = false;
    helm_apply_view();
    helm_refresh_numbers();
    helm_raise_chrome();
    return true;
}

/** @brief 演示：清除页面锁，恢复正常轮转。 */
void helm_shell_demo_page_clear(void)
{
    s_demo_page = (helm_page_id_t)0xff;
    s_sh.view_anim = false;
    helm_apply_view();
    helm_refresh_numbers();
    helm_raise_chrome();
}

/**
 * @brief 演示：强制导航横幅状态。
 * @param mode 0=off；1=直行,2=左转,3=偏航,4=到达,5=右转,6=掉头,7=重规划。
 * @details 置位后关掉开机自检轮播；横幅只在地图页可见，所以这里只在前台且
 *          未被菜单盖住时立即刷一拍，其余情形交给周期刷新。
 */
void helm_shell_demo_banner(int mode)
{
    if (mode < 0) {
        mode = 0;
    } else if (mode > 7) {
        mode = 7;
    }

    s_banner_demo_mode = mode;
    s_banner_demo_auto = false;
    s_banner_demo_ms = (uint32_t)lv_tick_get();

    if (s_sh.attached && !s_sh.ui_covered) {
        helm_refresh_numbers();
    }
}

/** @brief 演示：显示/隐藏"保存"确认层。 */
void helm_shell_demo_save(bool on)
{
    if (on) {
        helm_prompt_save();
    } else {
        helm_save_hide();
    }
}

void helm_shell_raise_chrome(void)
{
    helm_raise_chrome();
}

static void helm_poly_defer_cancel(void)
{
    if (s_sh.poly_timer) {
        lv_timer_delete(s_sh.poly_timer);
        s_sh.poly_timer = NULL;
    }
}

static void helm_nums_defer_cancel(void)
{
    if (s_sh.nums_timer) {
        lv_timer_delete(s_sh.nums_timer);
        s_sh.nums_timer = NULL;
    }
}

static void helm_nums_defer_cb(lv_timer_t * t)
{
    helm_page_id_t id;

    LV_UNUSED(t);
    s_sh.nums_timer = NULL;
    if (s_sh.ui_covered || !s_sh.attached) {
        return;
    }

    helm_refresh_numbers();
    id = s_sh.view_id;
    if (helm_page_is_data(id) || helm_obj_shown(s_sh.rotbar)) {
        helm_spark_sync(false);
    }
}

static void helm_nums_defer_begin(void)
{
    helm_nums_defer_cancel();
    s_sh.nums_timer = lv_timer_create(helm_nums_defer_cb, HELM_NUMS_DEFER_MS,
                                      NULL);
    if (s_sh.nums_timer) {
        lv_timer_set_repeat_count(s_sh.nums_timer, 1);
    } else {
        helm_refresh_numbers();
    }
}

static void helm_poly_defer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    s_sh.poly_timer = NULL;
    s_sh.dial_poly_defer = false;
    if (s_sh.ui_covered || !s_sh.attached) {
        return;
    }

    if (helm_page_at(*helm_active_idx()) != HELM_PAGE_STANDBY) {
        return;
    }

    helm_hero_spin_set(s_sh.dial_lock == 1);
    if (s_sh.hero) {
        lv_obj_invalidate(s_sh.hero);
    }
}

static void helm_poly_defer_begin(void)
{
    helm_poly_defer_cancel();
    s_sh.dial_poly_defer = true;
    s_sh.poly_timer = lv_timer_create(helm_poly_defer_cb, HELM_POLY_DEFER_MS,
                                      NULL);
    if (s_sh.poly_timer) {
        lv_timer_set_repeat_count(s_sh.poly_timer, 1);
    } else {
        s_sh.dial_poly_defer = false;
    }
}

void helm_shell_pause_for_cover(void)
{
    if (!s_sh.attached || s_sh.ui_covered) {
        return;
    }

    LVGL_STALL("helm_pause");
    s_sh.ui_covered = true;
    /* 这里**不能**挂起全局动效：菜单/设置页自己覆盖在 shell 上，开关旋钮等
     * 通用控件动效还要正常播。全局闸门只给"熄屏/待机"用（helm_idle）。 */
    helm_poly_defer_cancel();
    helm_nums_defer_cancel();
    helm_hero_spin_set(false);
    if (s_sh.ui_timer) {
        lv_timer_pause(s_sh.ui_timer);
    }
    if (s_sh.rot_timer) {
        lv_timer_pause(s_sh.rot_timer);
    }
    helm_pane_reset(s_sh.standby);
    helm_pane_reset(s_sh.data);
    helm_pane_reset(s_sh.dash);
    helm_pane_reset(s_sh.data_all);
    helm_pane_reset(s_sh.climb);
}

void helm_shell_resume_after_cover(void)
{
#if MYVENDOR_LVGL_STALL_LOG
    uint32_t t0;
#endif

    if (!s_sh.attached) {
        LVGL_STALL("helm_resume drop !attached");
        return;
    }

#if MYVENDOR_LVGL_STALL_LOG
    t0 = lv_tick_get();
#endif
    s_sh.ui_covered = false;
    if (s_sh.ui_timer) {
        lv_timer_resume(s_sh.ui_timer);
    }
    if (s_sh.rot_timer) {
        lv_timer_resume(s_sh.rot_timer);
    }
    if (helm_page_at(*helm_active_idx()) == HELM_PAGE_STANDBY) {
        helm_poly_defer_begin();
    } else {
        s_sh.dial_poly_defer = false;
        helm_poly_defer_cancel();
    }
    /* 菜单里可能已恢复记录：这里才收起暂停条。 */
    helm_apply_view();
    helm_refresh_numbers();
    helm_raise_chrome();
    LVGL_STALL("helm_resume done %ums id=%u",
        (unsigned)lv_tick_elaps(t0), (unsigned)s_sh.view_id);
}

void helm_shell_refresh(void)
{
    if (s_sh.ui_covered) {
        return;
    }

    /* 停导航后须收起转向页/横幅，不能只刷数字。 */
    helm_apply_view();
    helm_refresh_numbers();
    helm_raise_chrome();
}

/** @brief attach 之前 root 已有的子对象数（地铁画布等外来件个数，重建时保留）。 */
static uint8_t s_build_keep_n;

/** @brief 换主题后用：本层重建一遍（颜色是建页时烘进样式的）。
 *
 *  @details `helm_color()` 的值写在 `lv_obj_set_style_*` 那一刻 ⇒ 换主题只
 *           `lv_obj_invalidate()` 只是"把旧样子重画一遍"，必须**重建对象**才能重新
 *           取值。菜单那边靠 `helm_menu_paint()` 重建行，本层靠这里。
 *
 *  @note 只删**本层自己建**的对象（`helm_shell_attach()` 里打过 LV_OBJ_FLAG_USER_2
 *        标记的）；地图画布等外来件（vmap 建的兄弟节点）必须留着 —— 待机 pane 的
 *        注释里记着"地图画布从没被绘制过"那次花屏的教训。 */
/** @brief 重入保护：本函数正在重建时，内层再进来直接返回。
 *
 *  @details 2026-09-26 上板实测：把 `helm_shell_rebuild()` 接进换主题通知后，
 *           **一次切换就把 UI 卡死**（log 停住、无 panic/assert = 硬挂）。
 *           机制是重入：重建走 `helm_shell_attach()` → 各 builder →
 *           其中有 `map_page_set_map_ui_visible()` 一类会**再触发一次主题通知**，
 *           于是内层又一次进来删对象 —— **删的正是外层正在遍历的子对象**，
 *           链表当场错乱。所以保护的不是"重复做功"，是**外层的遍历**。
 *  @note 内层返回后外层照常跑完，本轮重建仍然生效（不是"跳过"）。 */
static bool s_rebuilding;

static void helm_shell_rebuild_once(void)
{
    map_page_t * map = s_sh.map;
    lv_obj_t * root;
    uint32_t i;

    if (map == NULL || !s_sh.attached) {
        return;
    }

    root = map_page_root(map);
    if (root == NULL) {
        return;
    }

    /* 换主题才走这里 ⇒ 打点不心疼：真卡住时"最后一行"就是现场（这一族踩过两次）。 */
    syslog(LOG_WARNING, "[theme] delete begin n=%u",
           (unsigned)lv_obj_get_child_count(root));

    for (i = lv_obj_get_child_count(root); i > 0u; i--) {
        lv_obj_t * c = lv_obj_get_child(root, i - 1u);

        if (c != NULL && lv_obj_has_flag(c, LV_OBJ_FLAG_USER_2)) {
            lv_obj_delete(c);
        }
    }

    syslog(LOG_WARNING, "[theme] delete done");

    s_sh.attached = false;
    helm_shell_attach(map);        /* 按开机顺序重建 */
    syslog(LOG_WARNING, "[theme] attach done");
    helm_apply_view();             /* 恢复当前页 */
    syslog(LOG_WARNING, "[theme] view done");

    /*
     * 重建把上一轮入场动画的对象一起删了（`lv_obj_delete` 会清掉挂在对象上的
     * 动画），新控件默认全不透明：
     *   · 之前"还没亮过"（pending 期间保持透明，或动画还没走完就被删）⇒ 补播一次；
     *   · 之前"已经亮过"（正常播完，或兜底定时器已落末态）⇒ 不补播，新控件直接
     *     可见。否则会把已经显示的内容再淡入一遍（"出现→消失→再出现"）。
     */
    if (!s_sb_in_visible) {
        helm_shell_enter_anim();
    }
}

void helm_shell_rebuild(void)
{
    if (s_rebuilding) {
        return;                    /* 内层重入：见 s_rebuilding 的注释，这里是防挂的关键 */
    }

    s_rebuilding = true;
    helm_shell_rebuild_once();
    s_rebuilding = false;

    /*
     * ⚠ 换主题后的"剖面重烘焙"必须登记在**重建之后**（对象是新的）：绘制路径拿到这个标志
     * 会用既有数据 `helm_climb_canvas_bake()` 重烘；若重建把剖面数据也清了（日志里的
     * `[theme] hist_reset`），bake() 返回假 ⇒ 走正常 reload 重新取数再烘 —— 两条路都不会
     * 留下空白，也不会烘在已被删除的对象上（2026-09-27 打桩定位到的顺序问题）。
     */
    s_climb_rebake = true;

    /*
     * ⚠ 还要作废**剖面指纹**：`s_climb_eles[]`（每点海拔）与 `s_climb_zone[]`（坡度分段）
     * 是静态缓存，它们的重算被"点数 / 总里程"指纹挡着 —— 而**重建不等于指纹变化**
     * （点数、总里程都没变）⇒ 重建把数组清掉后重算会被跳过 ⇒ 烘焙出来的折线是空的。
     */
    s_climb_fp_pts = 0xffffffffu;
    s_climb_fp_total_cm = 0xffffffffu;
    s_climb_fp_rec_n = 0xff;
    s_climb_fp_rec_slot = 0xffffffffu;
}

/** @brief 待换主题的"主界面组件重建"（① ② 不受影响，见 `helm_shell_theme_mark()`）。 */


/** @brief 换主题后登记"主界面组件要重建"（颜色是建页时烘进样式的）。 */
void helm_shell_theme_mark(void)
{
    s_theme_rebuild_pending = true;

    /*
     * 剖面（海拔折线）是**烘焙**成 RGB565 图的（底色 `HELM_COLOR_PAPER`、线色都烘进去了），
     * 换主题必须重烘焙，否则图上还是旧主题的颜色。
     * ⚠ 但登记点**不能在这里**：此时 shell 还没重建，绘制路径会先跑一拍、把图烘进"马上要
     * 被 `helm_shell_rebuild()` 删掉的那个 canvas 对象"上（2026-09-27 打桩实测：
     * `[climb] theme rebake nav=1 n=60 baked=1` 紧跟着就是 `[theme] delete begin` ⇒
     * 烘了等于没烘、用户看到空白）。真正的登记在 `helm_shell_rebuild()` 末尾（重建之后）。
     */
}

/** @brief 待办的主界面重建：**只在地图页是当前页时**执行，否则继续等。
 *
 *  @details 2026-09-26 上板实测（这是本函数存在的全部理由）：
 *           在**菜单页开着**的时候重建地图页那一层，会把菜单页弄死 —— 表现是
 *           菜单按键回调一律 0ms 返回（= `s_menu == NULL`，菜单页被关掉/状态被释放），
 *           画面停在换色后的那一帧不再更新，而主界面（此时才是真正的当前页）一切正常；
 *           对照实验：只跑 ①②（不动这一层）菜单全程活着，接回 ③ 就复现。
 *           而**地图页可见时**重建是好的（实测：连续切换后菜单仍能正常翻行）。
 *           ⇒ 规则：这一层重建只在该页为当前页时做；不在地图页就挂着，
 *             等主循环下一次问的时候（= 用户回到主界面）再重建 —— 这也正是
 *             "颜色要重新取"的那一刻，用户看到的就是重建后的新配色。 */
void helm_shell_theme_apply(void)
{
    if (!s_theme_rebuild_pending) {
        return;
    }

    if (lvgl_page_current_id() != (int)BICYCLE_PM_ID_MAP ||
        lvgl_page_nav_busy()) {
        return;                    /* 不在主界面/转场还没走完 ⇒ 等（见上） */
    }

    s_theme_rebuild_pending = false;
    helm_shell_rebuild();
}

void helm_shell_attach(map_page_t * map)
{
    lv_obj_t * root;

    if (map == NULL || s_sh.attached) {
        return;
    }

    if (s_hist == NULL) {
        s_hist = vmap_malloc(sizeof(float) * (size_t)HELM_HIST_COUNT * HELM_HIST_N);
        if (s_hist != NULL) {
            memset(s_hist, 0, sizeof(float) * (size_t)HELM_HIST_COUNT * HELM_HIST_N);
        }
    }

    root = map_page_root(map);
    if (root == NULL) {
        return;
    }

    /* ⚠ 临时诊断（换主题卡死定位，定位完删）：每步一条，卡住的**最后一条**就是现场。 */
    syslog(LOG_WARNING, "[theme] attach enter children=%u keep_n=%u",
           (unsigned)lv_obj_get_child_count(root), (unsigned)s_build_keep_n);

    /* attach 之前 root 已有的子对象数 = 外来件（地图画布等）的个数，重建时要留着；
     * 本层建完（见下面 build 之后那段）再把**新增**的子对象打上 LV_OBJ_FLAG_USER_2，
     * helm_shell_rebuild() 只删这些。 */
    s_build_keep_n = (uint8_t)lv_obj_get_child_count(root);

    /* ⚠ 走势图登记表（`s_spark[]`）**必须跟着本层的对象一起重建**：表里存的是本层
     * chart/label/series 的指针，而每次 attach 都会把这些对象删掉重造。
     * 2026-09-26 现场（coredump n623）：表**只增不清** ⇒ 重建后表里留着**已释放**的
     * 指针，主界面左右翻页走 `helm_spark_sync()` 时 `lv_obj_has_flag(野指针)`
     * 直接内存故障重启（判据：pc=`lv_obj_has_flag`、lr=`helm_spark_tree_shown`、
     * 故障地址 0x07e90024 是野值）。放在 builders **之前**：delete 回调会先把旧表项
     * 置 NULL，这里再整表清零，随后 builders 按新对象重新登记。 */
    memset(s_spark, 0, sizeof(s_spark));
    s_spark_n = 0;

    s_sh.map = map;
    s_sh.font_lab = helm_font_lab();
    s_sh.font_title = helm_font_title();
    s_sh.font_val = helm_font_val();
    s_sh.font_quad = helm_font_quad();
    s_sh.font_rot = helm_font_rot_speed();
    s_sh.font_speed = helm_font_speed();
    s_sh.font_speed_ride = helm_font_speed_ride();
    s_sh.font_mark = helm_font_mark();

    syslog(LOG_WARNING, "[theme] chrome");
    helm_build_chrome(root);
    syslog(LOG_WARNING, "[theme] standby");
    helm_build_standby(root);
    syslog(LOG_WARNING, "[theme] data");
    helm_build_data(root);
    syslog(LOG_WARNING, "[theme] dash");
    helm_build_dash(root);
    syslog(LOG_WARNING, "[theme] data_all");
    helm_build_data_all(root);
    syslog(LOG_WARNING, "[theme] climb");
    helm_build_climb(root);
    syslog(LOG_WARNING, "[theme] pause");
    helm_build_pause(root);
    syslog(LOG_WARNING, "[theme] save");
    helm_build_save(root);
    syslog(LOG_WARNING, "[theme] arrive");
    helm_build_arrive(root);
    syslog(LOG_WARNING, "[theme] hist_reset");
    helm_hist_reset();
    syslog(LOG_WARNING, "[theme] flag");

    /* 本层新建的子对象打标记 ⇒ helm_shell_rebuild() 重建时只删它们。 */    {
        uint32_t n = lv_obj_get_child_count(root);
        uint32_t k;

        for (k = s_build_keep_n; k < n; k++) {
            lv_obj_t * c = lv_obj_get_child(root, k);

            if (c != NULL) {
                lv_obj_add_flag(c, LV_OBJ_FLAG_USER_2);
            }
        }
    }

    s_sh.attached = true;
    s_sh.home_idx = 0;
    s_sh.ride_idx = 0;
    {
        int32_t last_m = myvendor_devctl_last_ride_m_get();

        s_sh.last_km = (last_m > 50) ? ((float)last_m / 1000.0f) : 0.0f;
    }
    helm_apply_view();
    helm_refresh_numbers();
    helm_shell_bind_keys();
    /* 换主题重建会再次进本函数：旧定时器必须先删，否则旧回调继续跑在刚被
     * 重建掉的 rotbar/信息卡上。attach 全程同步（重建入口另有 s_rebuilding
     * 防重入），放在创建前不会出现"本函数执行到一半被旧 timer 打断"的窗口。 */
    if (s_sh.rot_timer != NULL) {
        lv_timer_delete(s_sh.rot_timer);
        s_sh.rot_timer = NULL;
    }
    if (s_sh.ui_timer != NULL) {
        lv_timer_delete(s_sh.ui_timer);
        s_sh.ui_timer = NULL;
    }
    s_sh.rot_timer = lv_timer_create(helm_rot_cb, HELM_ROT_MS, NULL);
    s_sh.ui_timer = lv_timer_create(helm_ui_cb, HELM_UI_MS, NULL);
}
