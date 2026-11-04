/**
 * @file bicycle_status_bar.c
 * @brief 24 px 顶栏：纸底、两簇墨迹对齐、16 px Material 图标。
 *
 * 样式（与模拟器 `scene_helm.c` 的状态栏同一套，WP-HELM10/R1-S1 定稿，
 * R1-S11 按用户反馈把图标从 14 px 换成 **16 px 原生字形**）：
 *
 *   - **纸底**：240×24 纸色底 + y=23 一条 1 px 发丝线。整幅画在自建的
 *     240×24 子对象上 —— `lv_pm_status_bar_apply_theme()` 会把容器刷成黑色，
 *     不能依赖容器的 bg；
 *   - **左簇**：时间（12 px）→ GPS → BLE → 心率 → 功率 → 踏频，时间墨迹从
 *     x=2 起；时间→GPS 墨迹间隙 6，图标之间 6；图标统一 **16 px Material
 *     字形**（`helm_icons_16.c`，不缩放）。GPS/BLE 带 3 px 连接点：已连/有效
 *     实心，未连/无效空心；
 *   - **右簇**：REC（8 px 实心圆）→ NAV → 电量图标，电量图标墨迹右沿 x=238；
 *     REC→NAV 6，NAV→电量 6。隐藏项让整簇右移，相邻间隙按规则表保持不变。
 *     电量只画图标（三档 + 低电红色），不显示百分比；
 *   - **两簇之间**弹性留白；左簇末墨迹与右簇首墨迹间距 < 12 px 时按
 *     踏频 → 功率 → NAV → BLE 依次隐藏并重排（与模拟器同一顺序）；
 *   - **状态语义**：已连/有效 = 墨色实心；未连/无效 = 浅灰（TRACK）；
 *     REC 记录中 = 红、暂停 = 灰、未记录 = 不显示；NAV 导航中 = 蓝、
 *     未导航 = 不显示；电量 <20% 图标转红；电量图标按
 *     `<20% / <60% / 其余` 取 `battery_horiz_000 / 050 / 075`。
 *
 * 对象只创建一次；1 s 轮询只改文本/颜色/显隐（布局键不变时连位置都不碰），
 * 不做任何入场/闪烁动画。
 *
 * 旧告警（2026-09-27 加的闪烁）全部取消动画：
 *   - GPS 弱（fix_quality/sats/hdop 阈值）→ 图标**红色常亮**（原黄闪）；
 *   - BLE 诊断异常 → 图标**红色常亮**（原红闪）；
 *   - GNSS 星历下发 → **丢弃**（不再有黄色闪烁）；
 *   - GPS 失联 → 红色常亮（保留原红色语义）；
 *   - 未定位 / 蓝牙未连 / 传感器未就绪 → 浅灰（原来分别是白/白/白）。
 */

#include "bicycle_status_bar.h"

#include "bicycle_runtime.h"
#include "bicycle_gpx_sim.h"
#include "eta9184.h"
#include "helm_font.h"
#include "helm_icons.h"
#include "helm_palette.h"
#include "helm_shell.h"
#include "lv_pm_bar.h"
#include "myvendor_devctl.h"
#include "myvendor_sys.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#ifndef BICYCLE_STATUS_BAR_POLL_MS
#define BICYCLE_STATUS_BAR_POLL_MS 1000u
#endif

/* BLE 诊断异常年龄（旧"红闪"窗口）。myvendor_diag.h 没声明这个函数，旧代码靠
 * 隐式声明调用；这里显式 extern，避免新编译单元再出一条 implicit-declaration。 */
extern uint32_t myvendor_diag_ble_unhealthy_age_ms(void);

/** @brief BLE 异常上报后红灯常亮的窗口（ms）；期间有新异常就自动续期。 */
#define SB_BLE_ALERT_MS 10000u

/* ── 几何（与模拟器 scene_helm.c 的 SB_* 同一套墨迹口径；16 px 数值见
 *    docs/agent/evidence/WP-HELM10/R1/S11/NOTES.md）────────────────────── */
#define SB_W              240
#define SB_H              24
#define SB_HAIR_Y         23
#define SB_ICON_PX        16   /* 用户：图标清晰但 14px 偏小 ⇒ 16px 原生字形 */
#define SB_ICON_CY        12   /* 16 px 盒中心；墨迹实测 y=5..18 ⇒ 关于 11.5 对称 */
#define SB_DOT_Y          16
#define SB_DOT_D          3
/* REC：用户反馈 5 px 红点"太小，不明显"，改为 **8 px 实心圆**并垂直居中
 * （对象 y = 16 px 图标盒中心 − 直径/2；暂停仍变灰、未记录仍隐藏）。 */
#define SB_REC_D          8
#define SB_REC_Y          (SB_ICON_CY - SB_REC_D / 2)
/* 老状态栏的间距/对齐（用户 2026-11-01 指定）：
 *   · 时间贴最左（墨迹 x0=2，老版实测 1..30）；
 *   · 图标之间墨迹间隙 6（老版实测 GPS→BLE 6、时间→GPS 7）；
 *   · 右簇贴最右：REC→NAV 6、NAV→电量 6，电量**墨迹**右沿 238。
 * 电量改为**自绘电池**：Material 的 battery_horiz 字形只有 14×6，用户反馈
 * "上下高度太窄"。不显示百分比，填充宽度按电量比例，<20% 转红。 */
#define SB_TIME_X         2
#define SB_TIME_Y         4
#define SB_TIME_BOX_W     44
#define SB_TIME_BOX_H     16
#define SB_BATT_INK_RIGHT 238
/* 自绘电池几何（R1-S14 三版样张，用户选 **V2 最小** 定稿）：用户反馈
 * S13 的体 20×12 + 极 2×6（墨迹 22×12）太宽、太高。
 * V2 最小：体 15×9 + 右极 2×4，墨迹 17×9（窄 5、矮 3）；边框 1px ink；
 * 填充放在内容区里（子对象坐标 0,0，自动落在边框内），宽 = 13×电量%。
 * 体 y=7..15（垂直中心 11.0）；右极 y 由体/极高度公式居中（见
 * sb_slot_place() 与 bicycle_status_bar_init()，两处同一公式）。低电 <20%
 * 整体转红。另两版样张见 evidence/WP-HELM10/R1/S14。 */
#define SB_BATT_BODY_W    15
#define SB_BATT_BODY_H    9
#define SB_BATT_BODY_Y    7
#define SB_BATT_NUB_W     2
#define SB_BATT_NUB_H     4
#define SB_BATT_INK_W     (SB_BATT_BODY_W + SB_BATT_NUB_W)
#define SB_INK_GAP_TIME   6
#define SB_INK_GAP_ICON   6
#define SB_INK_GAP_REC_NAV 6
#define SB_INK_GAP_NAV_BAT 6
#define SB_MIN_BLANK      12

/** @brief 槽位顺序 = 左簇从左到右，其后是右簇的 NAV / BAT。 */
enum {
    SB_GPS = 0,
    SB_BLE,
    SB_HR,
    SB_PWR,
    SB_CAD,
    SB_NAV,
    SB_BAT,
    SB_SLOT_N
};

typedef enum {
    SB_TONE_NONE = 0,   /**< 未取色（调色板换过，强制重上色）。 */
    SB_TONE_HIDE,       /**< 本拍不显示。 */
    SB_TONE_INK,        /**< 已连/有效：墨色。 */
    SB_TONE_TRACK,      /**< 未连/无效：浅灰。 */
    SB_TONE_RED,        /**< 告警/低电：红。 */
    SB_TONE_BLUE,       /**< 导航中：蓝。 */
} sb_tone_t;

/**
 * @brief 一个 16 px 图标（+ 可选连接点）。
 *
 * `ink_w / cx_off / dot_dx` 是 16 px 字形的**实测墨迹**参数（WP-HELM10/R1-S11，
 * 位图解码 + 模拟器像素复核）：ink_w = 墨迹宽（BLE 含连接点）；cx_off =
 * 对象中心 − 墨迹左沿；dot_dx = 连接点墨迹左沿 − 字形墨迹左沿（-1 = 没有
 * 连接点）。GPS 的点落在字形墨迹框右下角（dx=13，同 14px 的 11/14 比例）；
 * BLE 的点在字形墨迹右沿 +3（9+3+3=15，同老表的 7+3+3）。
 */
typedef struct {
    helm_icon_t id;
    uint8_t ink_w;
    uint8_t cx_off;
    int8_t dot_dx;
    lv_obj_t * ico;
    lv_obj_t * dot;
    uint8_t tone_last;
    uint8_t dot_last;   /**< 0=隐藏 1=实心 2=空心 */
    bool vis_last;
} sb_slot_t;

static sb_slot_t s_slot[SB_SLOT_N] = {
    [SB_GPS] = { HELM_ICON_GPS,     16, 8, 13 },
    [SB_BLE] = { HELM_ICON_BLE,     15, 5, 12 },   /* 9 字形 + 3 + 3 点 */
    [SB_HR]  = { HELM_ICON_HR,      14, 7, -1 },
    [SB_PWR] = { HELM_ICON_POWER,   10, 5, -1 },
    [SB_CAD] = { HELM_ICON_CADENCE, 12, 6, -1 },
    [SB_NAV] = { HELM_ICON_NAV,     12, 6, -1 },
    [SB_BAT] = { HELM_ICON_BATTERY, SB_BATT_INK_W, 0, -1 },  /* 自绘电池，不用字形 */
};

static lv_obj_t * s_bg;
static lv_obj_t * s_hair;
static lv_obj_t * s_time;
static lv_obj_t * s_rec;
static lv_obj_t * s_batt_body;
static lv_obj_t * s_batt_fill;
static lv_obj_t * s_batt_nub;
static lv_timer_t * s_timer;
static const lv_font_t * s_font;

static int s_battery_pct = -1;
/** 模拟器巡页注入的电量覆盖（-1 = 用服务值；板端恒为 -1）。 */
static int s_batt_demo = -1;
static char s_time_buf[8] = "00:00";
static int s_time_ink_w;
static uint16_t s_last_time_hm = 0xFFFFu;
static uint8_t s_batt_tone_last = SB_TONE_NONE;
static uint8_t s_rec_last;
static int s_rec_x_last = -1;

/** @brief 调色板签名（同指针也会被夜间表原地重算，所以看值不看指针）。 */
static uint32_t s_pal_sig;

/* 布局结果：s_ink_x[k] < 0 = 本拍不显示。 */
static int s_ink_x[SB_SLOT_N];
static bool s_shown[SB_SLOT_N];
static int s_rec_x = -1;
static uint32_t s_layout_key;
static bool s_has_layout;

/* ── 小工具 ─────────────────────────────────────────────────────────── */

static lv_color_t sb_tone_color(sb_tone_t tone)
{
    switch (tone) {
    case SB_TONE_INK:
        return helm_color(HELM_COLOR_INK);
    case SB_TONE_RED:
        return helm_color(HELM_COLOR_GPS_DEAD);
    case SB_TONE_BLUE:
        return helm_color(HELM_COLOR_BAN_NAV);
    case SB_TONE_TRACK:
    default:
        return helm_color(HELM_COLOR_TRACK);
    }
}

/** @brief 一段文字的量：advance（排版宽）+ 首字墨迹偏移 + 墨迹宽。 */
typedef struct {
    int adv;
    int ink_x0;
    int ink_w;
} sb_text_metrics_t;

/**
 * @brief 12 px 状态栏字体里实际用到的 13 个字形的**渲染墨迹**列（相对笔位）。
 *
 * @details `lv_font_get_glyph_dsc()` 给的 `box_w` 是字模框，不等于屏上墨迹：
 *          这份 2 bpp 字模把多行像素连续打包（`stride == 0`），且框的左/右
 *          各可能有全空列（如 `%` 框 12 列，只有第 1..10 列有墨）。设计又要求
 *          **按墨迹**对齐（时间 x0=2；电量只画图标、不排文字），所以按实测表算。
 *          数值来自 /tmp 下的字模探针（逐像素解包 `helm_ui_12` 的 2 bpp 流，
 *          与模拟器截图逐列核对）；字符串只可能是 `0-9 : % -`。
 *          ⚠ 换状态栏字体或字号后必须重测本表。
 */
typedef struct {
    char ch;
    int8_t ink0;
    int8_t ink1;
} sb_glyph_ink_t;

static const sb_glyph_ink_t s_glyph_ink[] = {
    { '0', 0, 6 }, { '1', 1, 3 }, { '2', 1, 6 }, { '3', 1, 6 },
    { '4', 0, 7 }, { '5', 1, 6 }, { '6', 1, 6 }, { '7', 1, 6 },
    { '8', 1, 6 }, { '9', 1, 6 }, { ':', 1, 1 }, { '%', 1, 10 },
    { '-', 1, 6 },
};

static bool sb_glyph_ink_of(char ch, int * ink0, int * ink1)
{
    for (size_t i = 0; i < sizeof(s_glyph_ink) / sizeof(s_glyph_ink[0]); i++) {
        if (s_glyph_ink[i].ch == ch) {
            *ink0 = s_glyph_ink[i].ink0;
            *ink1 = s_glyph_ink[i].ink1;
            return true;
        }
    }
    return false;
}

/**
 * @brief 量时间/电量这类纯 ASCII 短串的 advance 与**墨迹**边界。
 *
 * @details 设计按墨迹对齐（时间墨迹 x0=2；电量只画图标），而 12 px 点阵的
 *          side bearing 与空列都不为 0，所以不能拿 advance 当墨迹用。
 *          字符串只可能是 `0-9 : % -`，按字节迭代 + 上面的实测表即可。
 */
static void sb_text_measure(const char * txt, sb_text_metrics_t * m)
{
    lv_point_t sz;
    int pen = 0;
    int minx = 0x7FFF;
    int maxx = -0x8000;

    lv_text_get_size(&sz, txt, s_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    m->adv = (int)sz.x;

    for (const char * p = txt; *p != '\0'; p++) {
        const uint32_t cp = (uint8_t)*p;
        int ink0;
        int ink1;

        if (sb_glyph_ink_of((char)cp, &ink0, &ink1)) {
            ink0 += pen;
            ink1 += pen;
        } else {
            ink0 = pen;
            ink1 = pen + (int)lv_font_get_glyph_width(s_font, cp, 0) - 1;
        }

        if (ink0 < minx) {
            minx = ink0;
        }
        if (ink1 > maxx) {
            maxx = ink1;
        }
        pen += (int)lv_font_get_glyph_width(s_font, cp, 0);
    }

    if (maxx < minx) {
        m->ink_x0 = 0;
        m->ink_w = 0;
    } else {
        m->ink_x0 = minx;
        m->ink_w = maxx - minx + 1;
    }
}

static void sb_hide(lv_obj_t * obj)
{
    if (obj != NULL) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void sb_show(lv_obj_t * obj)
{
    if (obj != NULL) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── 调色板 ─────────────────────────────────────────────────────────── */

static uint32_t sb_pal_signature(const struct helm_pal_t * pal)
{
    if (pal == NULL) {
        return 0;
    }

    /* 只关心顶栏用到的几支：任一支变了就重上色。 */
    return pal->paper ^ (pal->hair << 1) ^ (pal->ink << 2) ^
           (pal->track << 3) ^ (pal->gps_dead << 4) ^ (pal->ban_nav << 5);
}

static void sb_palette_apply(bool force)
{
    const struct helm_pal_t * pal = helm_pal_active();
    uint32_t sig = sb_pal_signature(pal);

    if (!force && sig == s_pal_sig) {
        return;
    }
    s_pal_sig = sig;

    lv_obj_set_style_bg_color(s_bg, helm_color(HELM_COLOR_PAPER), 0);
    lv_obj_set_style_bg_color(s_hair, helm_color(HELM_COLOR_HAIR), 0);
    lv_obj_set_style_text_color(s_time, helm_color(HELM_COLOR_INK), 0);

    for (int k = 0; k < SB_SLOT_N; k++) {
        s_slot[k].tone_last = SB_TONE_NONE;
        s_slot[k].dot_last = 0;
    }
    s_batt_tone_last = SB_TONE_NONE;
    s_rec_last = 0;
}

/* ── 图标/圆点 ───────────────────────────────────────────────────────── */

static void sb_slot_place(int k)
{
    sb_slot_t * s = &s_slot[k];
    int x0 = s_ink_x[k];

    if (x0 < 0) {
        return;
    }

    if (k == SB_BAT) {
        /* 自绘电池：体从 x0 起，右极贴在体右侧；墨迹总宽 17。 */
        if (s_batt_body != NULL) {
            lv_obj_set_pos(s_batt_body, x0, SB_BATT_BODY_Y);
        }
        if (s_batt_nub != NULL) {
            lv_obj_set_pos(s_batt_nub, x0 + SB_BATT_BODY_W,
                           SB_BATT_BODY_Y + (SB_BATT_BODY_H - SB_BATT_NUB_H) / 2);
        }
        return;
    }

    if (s->ico == NULL) {
        return;
    }

    lv_obj_set_pos(s->ico, x0 + s->cx_off - SB_ICON_PX / 2,
                   SB_ICON_CY - SB_ICON_PX / 2);
    if (s->dot != NULL) {
        lv_obj_set_pos(s->dot, x0 + s->dot_dx, SB_DOT_Y);
    }
}

/**
 * @brief 上一次色 + 显隐（幂等）。
 *
 * @param tone    本拍语义色；SB_TONE_HIDE 表示数据层就不显示。
 * @param shown   布局层是否保留（碰撞隐藏时为 false）。
 * @param dot_on  连接点是否实心（仅 GPS/BLE 有连接点）。
 */
static void sb_slot_apply(int k, sb_tone_t tone, bool shown, bool dot_on)
{
    sb_slot_t * s = &s_slot[k];

    if (k == SB_BAT) {
        /* 自绘电池：体/极/填充同色；显隐与 tone 一起处理（ico 为 NULL）。 */
        const bool show = shown && tone != SB_TONE_HIDE;
        lv_color_t c = sb_tone_color(tone);

        if (!show) {
            sb_hide(s_batt_body);
            sb_hide(s_batt_nub);
            s->vis_last = false;
            s->tone_last = SB_TONE_HIDE;
            return;
        }
        if (s_batt_body != NULL) {
            sb_show(s_batt_body);
            lv_obj_set_style_border_color(s_batt_body, c, 0);
        }
        if (s_batt_fill != NULL) {
            lv_obj_set_style_bg_color(s_batt_fill, c, 0);
        }
        if (s_batt_nub != NULL) {
            sb_show(s_batt_nub);
            lv_obj_set_style_bg_color(s_batt_nub, c, 0);
        }
        s->vis_last = true;
        s->tone_last = (uint8_t)tone;
        return;
    }

    if (s->ico == NULL) {
        return;
    }

    if (!shown || tone == SB_TONE_HIDE) {
        if (s->vis_last || !lv_obj_has_flag(s->ico, LV_OBJ_FLAG_HIDDEN)) {
            sb_hide(s->ico);
            s->vis_last = false;
        }
        if (s->dot != NULL &&
            (s->dot_last != 0 ||
             !lv_obj_has_flag(s->dot, LV_OBJ_FLAG_HIDDEN))) {
            sb_hide(s->dot);
            s->dot_last = 0;
        }
        s->tone_last = SB_TONE_HIDE;
        return;
    }

    if (!s->vis_last) {
        sb_show(s->ico);
        s->vis_last = true;
        s->tone_last = SB_TONE_NONE;   /* 重新取色 */
    }

    {
        bool tone_changed = (s->tone_last != (uint8_t)tone);

        if (tone_changed) {
            lv_obj_set_style_text_color(s->ico, sb_tone_color(tone), 0);
            s->tone_last = (uint8_t)tone;
        }

        if (s->dot != NULL) {
            const uint8_t want = dot_on ? 1u : 2u;

            if (want != s->dot_last || tone_changed) {
                lv_color_t c = sb_tone_color(tone);

                if (want == 1u) {
                    lv_obj_set_style_bg_color(s->dot, c, 0);
                    lv_obj_set_style_bg_opa(s->dot, LV_OPA_COVER, 0);
                    lv_obj_set_style_border_width(s->dot, 0, 0);
                } else {
                    lv_obj_set_style_bg_opa(s->dot, LV_OPA_TRANSP, 0);
                    lv_obj_set_style_border_width(s->dot, 1, 0);
                    lv_obj_set_style_border_color(s->dot, c, 0);
                    lv_obj_set_style_border_opa(s->dot, LV_OPA_COVER, 0);
                }
                sb_show(s->dot);
                s->dot_last = want;
            }
        }
    }
}

/** @brief REC 圆点：记录中红、暂停灰、未记录隐藏。 */
static void sb_rec_apply(bool recording, bool paused)
{
    if (s_rec == NULL) {
        return;
    }

    if (!recording) {
        if (s_rec_last != 0 ||
            !lv_obj_has_flag(s_rec, LV_OBJ_FLAG_HIDDEN)) {
            sb_hide(s_rec);
            s_rec_last = 0;
        }
        return;
    }

    {
        const uint8_t want = paused ? 2u : 1u;

        if (s_rec_last != want) {
            lv_obj_set_style_bg_color(s_rec,
                                      paused ? helm_color(HELM_COLOR_TRACK)
                                             : helm_color(HELM_COLOR_GPS_DEAD), 0);
            sb_show(s_rec);
            s_rec_last = want;
        }
    }

    if (s_rec_x != s_rec_x_last) {
        lv_obj_set_pos(s_rec, s_rec_x, SB_REC_Y);
        s_rec_x_last = s_rec_x;
    }
}

/* ── 墨迹布局 ───────────────────────────────────────────────────────── */

/** @brief 左簇：从 x0 往右打包；返回左簇末墨迹 x1。pos[] = 各图标墨迹 x0。 */
static int sb_left_layout(int x0, bool ble, bool hr, bool pwr, bool cad,
                          int pos[5])
{
    const bool on[5] = { true, ble, hr, pwr, cad };
    int x = x0;
    int end = x - 1;

    for (int i = 0; i < 5; i++) {
        if (!on[i]) {
            pos[i] = -1;
            continue;
        }
        pos[i] = x;
        end = x + s_slot[i].ink_w - 1;
        x = end + 1 + SB_INK_GAP_ICON;
    }
    return end;
}

/**
 * @brief 右簇：电量图标**墨迹右沿钉在 238**，从右往左打包（全部按墨迹间隙算）。
 *
 * pos[0]=REC 墨迹 x0、pos[1]=NAV、pos[2]=电量图标；-1 = 隐藏。
 * 电量不显示百分比，图标就是右簇最右一项。
 */
static int sb_right_layout(bool rec, bool nav, int pos[3])
{
    int x;
    int left;

    pos[2] = SB_BATT_INK_RIGHT - s_slot[SB_BAT].ink_w + 1;
    left = pos[2];
    /* x = 紧邻左侧那一项的**右边界（不含）**；NAV 隐藏时 REC 直接用
     * “NAV→电量 8” 的间隙贴到电量左边（模拟器 right_layout 同一规则）。 */
    x = pos[2] - SB_INK_GAP_NAV_BAT;
    pos[1] = nav ? (x - s_slot[SB_NAV].ink_w) : -1;
    if (nav) {
        left = pos[1];
        x = pos[1] - SB_INK_GAP_REC_NAV;
    }
    pos[0] = rec ? (x - SB_REC_D) : -1;
    if (rec) {
        left = pos[0];
    }
    return left;
}

static uint32_t sb_layout_make_key(int time_ink_w, bool ble,
                                   bool hr, bool pwr, bool cad, bool rec,
                                   bool nav)
{
    uint32_t key = (uint32_t)(time_ink_w & 0x3F);

    key = key * 2u + (ble ? 1u : 0u);
    key = key * 2u + (hr ? 1u : 0u);
    key = key * 2u + (pwr ? 1u : 0u);
    key = key * 2u + (cad ? 1u : 0u);
    key = key * 2u + (rec ? 1u : 0u);
    key = key * 2u + (nav ? 1u : 0u);
    return key;
}

/**
 * @brief 按墨迹规则排两簇；碰撞时按 踏频→功率→NAV→BLE 依次隐藏。
 *
 * @details 布局只依赖「时间宽度、六个可隐藏项是否在」——这些都没变的 1 s 拍
 *          直接跳过，不碰对象位置。右簇右端固定 238（电量图标墨迹右沿）。
 */
static void sb_layout(int time_ink_w, bool ble, bool hr,
                      bool pwr, bool cad, bool rec, bool nav)
{
    const uint32_t key = sb_layout_make_key(time_ink_w, ble, hr, pwr, cad,
                                            rec, nav);
    const int x0 = SB_TIME_X + time_ink_w + SB_INK_GAP_TIME;   /* 时间墨迹 x1+1+6 */
    int pos[5];
    int rpos[3];
    int left_end;
    int right_left;

    if (s_has_layout && key == s_layout_key) {
        return;
    }
    s_layout_key = key;
    s_has_layout = true;

    for (int guard = 0; guard < 5; guard++) {
        left_end = sb_left_layout(x0, ble, hr, pwr, cad, pos);
        right_left = sb_right_layout(rec, nav, rpos);
        if (right_left - left_end - 1 >= SB_MIN_BLANK) {
            break;
        }
        if (cad) {
            cad = false;
        } else if (pwr) {
            pwr = false;
        } else if (nav) {
            nav = false;
        } else if (ble) {
            ble = false;
        } else {
            break;
        }
    }

    left_end = sb_left_layout(x0, ble, hr, pwr, cad, pos);
    right_left = sb_right_layout(rec, nav, rpos);
    (void)left_end;
    (void)right_left;

    for (int i = 0; i < 5; i++) {
        s_ink_x[i] = pos[i];
        s_shown[i] = (pos[i] >= 0);
    }
    s_ink_x[SB_NAV] = rpos[1];
    s_shown[SB_NAV] = (rpos[1] >= 0);
    s_ink_x[SB_BAT] = rpos[2];
    s_shown[SB_BAT] = (rpos[2] >= 0);
    s_rec_x = rpos[0];

    for (int k = 0; k < SB_SLOT_N; k++) {
        if (s_shown[k]) {
            sb_slot_place(k);
        }
    }
}

/* ── 数据 → 语义色 ─────────────────────────────────────────────────── */

static sb_tone_t sb_gps_tone(const bicycle_runtime_t * rt)
{
    bool alive = rt && rt->gnss_alive;
    bool valid = rt && rt->gnss_valid;
    uint8_t q = rt ? rt->gnss_fix_quality : 0;
    uint8_t sats = rt ? rt->gnss_satellites : 0;
    uint8_t hdop = rt ? rt->gnss_hdop_x10 : 0;

    /* 旧：失联红 / 未定位白 / 弱黄闪 / 正常绿。新：红 / 浅灰 / 红 / 墨色。 */
    if (!alive) {
        return SB_TONE_RED;
    }
    if (!valid) {
        return SB_TONE_TRACK;
    }
    if (q <= 1 || sats < 5 || hdop > 20) {
        return SB_TONE_RED;
    }
    return SB_TONE_INK;
}

static sb_tone_t sb_ble_tone(bool radio_on, bool connected)
{
    /* 左簇的五个图标**位置固定**：蓝牙关着也占位（浅灰），不整颗消失，
     * 否则其余图标会跟着左移，和环心/骑行页的对齐口径对不上。 */
    if (!radio_on) {
        return SB_TONE_TRACK;
    }
    /* 旧：诊断异常红闪 10 s。新：红色常亮（不再有动画）。 */
    if (myvendor_diag_ble_unhealthy_age_ms() < SB_BLE_ALERT_MS) {
        return SB_TONE_RED;
    }
    return connected ? SB_TONE_INK : SB_TONE_TRACK;
}

/** @brief 传感器图标：**始终占位**；就绪墨色，未配对/未就绪浅灰。 */
static void sb_sensor_tones(sb_tone_t tone[SB_SLOT_N])
{
    static const uint8_t slot_of_kind[MYVENDOR_SYS_SENSOR_KIND_N] = {
        SB_HR, SB_CAD, SB_PWR,
    };
    myvendor_devctl_sensor_rec_t recs[MYVENDOR_DEVCTL_SENSOR_REC_MAX];
    myvendor_sys_sensor_ui_t ui;
    size_t n = 0;
    size_t i;
    uint8_t k;
    bool want[MYVENDOR_SYS_SENSOR_KIND_N];
    bool sensor_on = myvendor_devctl_sensor_get();

    for (k = 0; k < MYVENDOR_SYS_SENSOR_KIND_N; k++) {
        want[k] = false;
    }

    if (sensor_on &&
        myvendor_devctl_sensor_recs_get(recs, MYVENDOR_DEVCTL_SENSOR_REC_MAX,
                                        &n) == 0) {
        for (i = 0; i < n; i++) {
            if (recs[i].autorc && recs[i].kind < MYVENDOR_SYS_SENSOR_KIND_N) {
                want[recs[i].kind] = true;
            }
        }
    }

    myvendor_sys_sensor_ui_get(&ui);

    for (k = 0; k < MYVENDOR_SYS_SENSOR_KIND_N; k++) {
        const uint8_t slot = slot_of_kind[k];
        bool ready = (ui.slot[k].link == MYVENDOR_SYS_SENSOR_LINK_READY);

        /* 关功能/未配对也画浅灰占位（设计口径：五个图标常驻，靠颜色区分）。 */
        tone[slot] = (sensor_on && want[k] && ready) ? SB_TONE_INK
                                                     : SB_TONE_TRACK;
    }
}

static void sb_time_update(void)
{
    time_t now = time(NULL);
    int hour = -1;
    int min = -1;

    if (now >= 1704067200) {
        if (!myvendor_devctl_tz_hms((long)now, &hour, &min)) {
            hour = -1;
            min = -1;
        }
    }

    if (hour < 0) {
        myvendor_sys_gnss_t cfix;
        bool got = myvendor_sys_onboard_gnss_get(&cfix);

        if (!got) {
            got = myvendor_sys_phone_gnss_get(&cfix);
        }

        if (got && cfix.utc_sec >= 1704067200u) {
            time_t utc = (time_t)cfix.utc_sec;
            struct timeval tv;

            tv.tv_sec = utc;
            tv.tv_usec = 0;
            (void)settimeofday(&tv, NULL);
            if (!myvendor_devctl_tz_hms((long)utc, &hour, &min)) {
                hour = -1;
                min = -1;
            }
        }
    }

    if (hour < 0) {
        (void)bicycle_gpx_sim_get_wall_hms(&hour, &min);
    }

    if (hour < 0) {
        hour = 0;
        min = 0;
    }

    {
        const uint16_t hm = (uint16_t)(hour * 60 + min);

        if (hm != s_last_time_hm) {
            sb_text_metrics_t m;

            lv_snprintf(s_time_buf, sizeof(s_time_buf), "%02d:%02d", hour, min);
            lv_label_set_text(s_time, s_time_buf);
            sb_text_measure(s_time_buf, &m);
            /* 首字有 side bearing 时把 label 反向外移 ⇒ 墨迹恒定从 x=12 起。 */
            lv_obj_set_x(s_time, SB_TIME_X - m.ink_x0);
            s_time_ink_w = m.ink_w;
            s_last_time_hm = hm;
        }
    }
}

static void sb_battery_update(void)
{
    const int inner = SB_BATT_BODY_W - 2;   /* 只扣 1px 边框：填充贴边框 */
    int pct = s_battery_pct;
    int w;

    if (pct < 0) {
        w = 0;
    } else {
        if (pct > 100) {
            pct = 100;
        }
        w = inner * pct / 100;
        if (w < 1 && pct > 0) {
            w = 1;
        }
    }

    /* 自绘电池：只改填充宽度；颜色由 sb_slot_apply(SB_BAT) 按 tone 上。 */
    if (s_batt_fill != NULL) {
        lv_obj_set_width(s_batt_fill, w);
    }
}

static void status_bar_refresh(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();
    sb_tone_t tone[SB_SLOT_N];
    bool dot_on[SB_SLOT_N];
    bool ble;
    bool rec, nav, paused;
    int time_w;

    if (s_time == NULL) {
        return;
    }

    bicycle_status_bar_set_battery_pct(s_batt_demo >= 0 ? s_batt_demo
                                                        : myvendor_sys_battery_percent());

    sb_palette_apply(false);
    sb_time_update();
    sb_battery_update();

    for (int k = 0; k < SB_SLOT_N; k++) {
        tone[k] = SB_TONE_HIDE;
        dot_on[k] = false;
    }

    /* 左簇：GPS 恒在；BLE 关电台才隐藏；心率/功率/踏频看配对。 */
    tone[SB_GPS] = sb_gps_tone(rt);
    dot_on[SB_GPS] = rt && rt->gnss_valid;

    ble = myvendor_devctl_radio_get();
    tone[SB_BLE] = sb_ble_tone(ble, myvendor_sys_phone_ble_connected());
    dot_on[SB_BLE] = myvendor_sys_phone_ble_connected();

    sb_sensor_tones(tone);

    /* REC：记录中红、暂停灰；未记录隐藏（旧 PAUSE 药丸并入 REC 颜色）。
     * R1-S4a：真实暂停路径（map_page_pause_ride）会把 runtime.recording 置 false
     * —— 那表示"计时暂停"，不是"没有骑行"。REC 的可见性必须同时看 paused，
     * 否则暂停后圆点直接消失，设计里"暂停 = 灰点"就永远走不到。 */
    paused = helm_shell_paused();
    rec = rt && (rt->recording || paused);

    /* NAV：导航中蓝，未导航隐藏。 */
    nav = map_page_nav_active(live_map_page_instance());
    tone[SB_NAV] = nav ? SB_TONE_BLUE : SB_TONE_HIDE;

    /* 电量：<20% 红；未知 = 浅灰；图标字形按 20/60 分级。 */
    if (s_battery_pct < 0) {
        tone[SB_BAT] = SB_TONE_TRACK;
    } else if (s_battery_pct < 20) {
        tone[SB_BAT] = SB_TONE_RED;
    } else {
        tone[SB_BAT] = SB_TONE_INK;
    }

    /* 布局：先把本拍数据层的可见性取出来算两簇，再统一上色。 */
    time_w = s_time_ink_w;

    sb_layout(time_w,
              tone[SB_BLE] != SB_TONE_HIDE,
              tone[SB_HR] != SB_TONE_HIDE,
              tone[SB_PWR] != SB_TONE_HIDE,
              tone[SB_CAD] != SB_TONE_HIDE,
              rec, nav);

    for (int k = 0; k < SB_SLOT_N; k++) {
        sb_slot_apply(k, tone[k], s_shown[k], dot_on[k]);
    }

    /* 电量只有图标：颜色在 sb_slot_apply 里按 tone 上（<20% 红）。 */
    sb_rec_apply(rec, paused);
}

static void status_bar_timer_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    status_bar_refresh();
}

/* ── 公开 API ───────────────────────────────────────────────────────── */

void bicycle_status_bar_set_battery_pct(int pct)
{
    if (pct < 0 || pct > 100) {
        s_battery_pct = -1;
    } else {
        s_battery_pct = pct;
    }
}

/* ── 仅模拟器巡页：固定电量 ─────────────────────────────────────────────
 * `myvendor_sys_battery_percent()` 在模拟器里是固定 76% 的服务桩，切不到
 * 低电态；巡页脚本用它注入 <20% / 45% / 100% 三档（每 1 s 的轮询会读
 * 这里的覆盖值）。板端没有任何调用者，-1 即恢复服务值。 */
void bicycle_status_bar_demo_battery(int pct);
void bicycle_status_bar_demo_battery(int pct)
{
    s_batt_demo = (pct < 0 || pct > 100) ? -1 : pct;
}

static lv_obj_t * sb_make_bg(lv_obj_t * parent, int x, int y, int w, int h,
                             lv_color_t color, lv_opa_t opa, int radius)
{
    lv_obj_t * obj = lv_obj_create(parent);

    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t * sb_make_icon(lv_obj_t * parent, int k)
{
    lv_obj_t * ico;

    if (k == SB_BAT) {
        /* 电量是自绘电池（体+极+填充），没有字形对象。 */
        return NULL;
    }

    ico = hfi_create_px(parent, s_slot[k].id, SB_ICON_PX,
                        0, 0, 0x000000);

    if (ico == NULL) {
        return NULL;
    }
    /* 显式钉住 16 px 原生字形：模拟器链接的是自己那份旧的 helm_icons.c
     * （hfi_create_px 还没有 16 px 档，会落到 20 px），板端本就是 16 px；
     * 两边都按同一份 `helm_icons_16.c` 出字，避免"模拟器量 20 px、板端跑
     * 16 px"的口径漂移。这里不是缩放，只是选字体 + 盒尺寸。 */
    lv_obj_set_style_text_font(ico, &helm_icons_16, 0);
    lv_obj_set_size(ico, SB_ICON_PX, SB_ICON_PX);
    lv_obj_set_style_text_color(ico, helm_color(HELM_COLOR_TRACK), 0);
    sb_hide(ico);
    return ico;
}

static lv_obj_t * sb_make_dot(lv_obj_t * parent)
{
    lv_obj_t * dot = sb_make_bg(parent, 0, SB_DOT_Y, SB_DOT_D, SB_DOT_D,
                                helm_color(HELM_COLOR_TRACK), LV_OPA_COVER,
                                SB_DOT_D / 2);

    sb_hide(dot);
    return dot;
}

void bicycle_status_bar_init(void)
{
    lv_obj_t * cont = lv_pm_status_bar_cont();
    lv_color_t paper;
    lv_color_t ink;
    lv_color_t red;

    if (cont == NULL || s_bg != NULL) {
        return;
    }

    s_font = helm_font_status();
    (void)eta9184_start();

    paper = helm_color(HELM_COLOR_PAPER);
    ink = helm_color(HELM_COLOR_INK);
    red = helm_color(HELM_COLOR_GPS_DEAD);

    /* ① 纸底 + 发丝线：整幅自建，容器被 apply_theme 刷黑也盖得住。 */
    s_bg = sb_make_bg(cont, 0, 0, SB_W, SB_H, paper, LV_OPA_COVER, 0);
    s_hair = sb_make_bg(s_bg, 0, SB_HAIR_Y, SB_W, 1,
                        helm_color(HELM_COLOR_HAIR), LV_OPA_COVER, 0);

    /* ② 时间：12 px，墨迹 x0=2。 */
    s_time = lv_label_create(s_bg);
    lv_obj_remove_style_all(s_time);
    lv_label_set_long_mode(s_time, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_time, s_time_buf);
    lv_obj_set_style_text_font(s_time, s_font, 0);
    lv_obj_set_style_text_color(s_time, ink, 0);
    lv_obj_set_style_text_align(s_time, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_size(s_time, SB_TIME_BOX_W, SB_TIME_BOX_H);
    lv_obj_set_pos(s_time, SB_TIME_X, SB_TIME_Y);

    /* ③ 左簇图标（GPS/BLE 带连接点）+ NAV 图标；电量是自绘电池，单独建。 */
    for (int k = 0; k < SB_SLOT_N; k++) {
        s_slot[k].ico = sb_make_icon(s_bg, k);
        if (s_slot[k].ico != NULL && s_slot[k].dot_dx >= 0) {
            s_slot[k].dot = sb_make_dot(s_bg);
        }
    }

    /* ③b 电量：自绘电池（体 15×9 空洞 + 1px 边、填充按比例、右极 2×4）。
     * 用字形的话 Material 电池只有 14×6，用户反馈上下太窄。 */
    s_batt_body = sb_make_bg(s_bg, 0, SB_BATT_BODY_Y, SB_BATT_BODY_W,
                             SB_BATT_BODY_H, paper, LV_OPA_TRANSP, 2);
    if (s_batt_body != NULL) {
        lv_obj_set_style_border_width(s_batt_body, 1, 0);
        lv_obj_set_style_border_color(s_batt_body, ink, 0);
        lv_obj_set_style_border_opa(s_batt_body, LV_OPA_COVER, 0);
        /* 填充放进内容区（子对象 0,0 已在 1px 边框内）：左/上不再多出那条缝，
         * 高度撑满内容区（体高 − 2×边框），宽度由 sb_battery_update 按电量设。 */
        s_batt_fill = sb_make_bg(s_batt_body, 0, 0, 0, SB_BATT_BODY_H - 2,
                                 ink, LV_OPA_COVER, 1);
    }
    /* 右极 y：与 sb_slot_place() 的电池分支**同一公式**（体/极高度居中），
     * 两处都改常量即可，别写死。 */
    s_batt_nub = sb_make_bg(s_bg, 0,
                            SB_BATT_BODY_Y +
                                (SB_BATT_BODY_H - SB_BATT_NUB_H) / 2,
                            SB_BATT_NUB_W, SB_BATT_NUB_H, ink, LV_OPA_COVER, 1);
    sb_hide(s_batt_body);
    sb_hide(s_batt_nub);

    /* ④ REC 圆点（记录中红、暂停灰、未记录隐藏）。 */
    s_rec = sb_make_bg(s_bg, -SB_REC_D, SB_REC_Y, SB_REC_D, SB_REC_D, red,
                       LV_OPA_COVER, SB_REC_D / 2);
    sb_hide(s_rec);

    /* 电量是自绘电池：位置由右簇布局（sb_slot_place）负责，宽度由
     * sb_battery_update() 按百分比设。 */

    s_last_time_hm = 0xFFFFu;
    s_batt_tone_last = SB_TONE_NONE;
    s_rec_last = 0;
    s_pal_sig = 0;
    s_has_layout = false;

    status_bar_refresh();
    s_timer = lv_timer_create(status_bar_timer_cb, BICYCLE_STATUS_BAR_POLL_MS,
                              NULL);
}

void bicycle_status_bar_refresh(void)
{
    s_last_time_hm = 0xFFFFu;
    status_bar_refresh();
}
