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

#include "bicycle_runtime.h"
#include "bicycle_ride_gpx.h"
#include "companion_bridge.h"
#include "helm_font.h"
#include "helm_icon.h"
#include "helm_idle.h"
#include "helm_menu.h"
#include "helm_pwr.h"
#include "helm_palette.h"
#include "helm_widget.h"
#include "lv_pm_overlay.h"
#include "myvendor_devctl.h"
#include "myvendor_sound.h"
#include "Vendor/Board/lv_port/lv_port_buttons.h"
#include "vmap/vmap_alloc.h"
#include "vmap/vmap_config.h"
#include "vmap/vmap_format.h"
#include "lvgl/src/draw/lv_draw_buf.h"
#include "lvgl/src/draw/lv_draw_label.h"
#include "lvgl/src/widgets/canvas/lv_canvas.h"
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
#ifndef HELM_HERO_SPIN_MS
#define HELM_HERO_SPIN_MS 80u
#endif
#define HELM_SPARK_PTS 30
/** @brief 海拔剖面均匀采样点数（段数 = N−1）。 */
#define HELM_CLIMB_PROF_N 60
/** @brief 海拔剖面「平坦」档：|坡度|低于此百分数为绿。 */
#define HELM_CLIMB_GRADE_FLAT  3u
/** @brief 海拔剖面「陡」档：|坡度|大于等于此百分数为红；两档之间为黄。 */
#define HELM_CLIMB_GRADE_STEEP 8u
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
#ifndef HELM_PAGE_HR_ENABLE
#  define HELM_PAGE_HR_ENABLE 0
#endif
#define HELM_CLIMB_REC_MS  (30u * 60u * 1000u)
#define HELM_CLIMB_REC_PERIOD_MS  (HELM_CLIMB_REC_MS / HELM_CLIMB_PROF_N)
#define HELM_SPARK_MAX 9
#define HELM_SPEED_WRAP_H  80
#define HELM_SPEED_HEAD_H  22
#define HELM_CLIMB_CHART_H (HELM_MAP_BODY_H - HELM_SPEED_HEAD_H - HELM_ROW2_H \
                            - 2 * HELM_INSET - 2 * HELM_GAP)
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
    HELM_PAGE_STANDBY = 0,
    HELM_PAGE_DATA,      /**< 概览：时间 / 里程 / 均速 / 均心率。 */
    HELM_PAGE_DATA_HR,   /**< 心率：当前 / 均心 / 最大 / 踏频。 */
    HELM_PAGE_DATA_SPD,  /**< 速度：均速 / 极速 / 功率 / 爬升。 */
    HELM_PAGE_MAP,
    HELM_PAGE_TURN,
    HELM_PAGE_CLIMB,
} helm_page_id_t;

typedef struct {
    map_page_t * map;
    lv_obj_t * standby;
    lv_obj_t * data;
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
    lv_obj_t * nav_ban_ico;
    lv_obj_t * nav_ban_m;
    lv_obj_t * nav_ban_rd;
    lv_obj_t * speed_big;
    lv_obj_t * speed_unit;
    lv_obj_t * standby_hint;
    lv_obj_t * hero;
    lv_obj_t * hero_mode;
    lv_obj_t * gps_val;
    lv_obj_t * last_dist;
    lv_obj_t * data_speed;
    lv_obj_t * data_unit;
    lv_obj_t * data_mark;
    lv_obj_t * data_title;
    lv_obj_t * data_hero_key;
    lv_obj_t * cell_lab[4];
    lv_obj_t * cell_val[4];
    lv_obj_t * cell_box[4];
    lv_obj_t * turn_ico;
    lv_obj_t * turn_dist;
    lv_obj_t * turn_sub;
    lv_obj_t * climb_mark;
    lv_obj_t * climb_alt;
    lv_obj_t * climb_gain;
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

static helm_shell_t s_sh;
static uint32_t s_still_since;
static uint32_t s_move_since;
static uint32_t s_autopause_hold_since;
/** 搜星扫描圈上次真正触发重绘的时刻（见 HELM_HERO_SPIN_MS）。 */
static uint32_t s_hero_spin_redraw_ms;
static int16_t s_rec_ele[HELM_CLIMB_PROF_N];
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

static lv_obj_t * helm_pane_of(helm_page_id_t id)
{
    switch (id) {
    case HELM_PAGE_STANDBY:
        return s_sh.standby;
    case HELM_PAGE_DATA:
    case HELM_PAGE_DATA_HR:
    case HELM_PAGE_DATA_SPD:
        return s_sh.data;
    case HELM_PAGE_TURN:
        return s_sh.turn;
    case HELM_PAGE_CLIMB:
        return s_sh.climb;
    default:
        return NULL;
    }
}

static bool helm_page_is_data(helm_page_id_t id)
{
    return id == HELM_PAGE_DATA || id == HELM_PAGE_DATA_HR
        || id == HELM_PAGE_DATA_SPD;
}

static void helm_pane_y(void * var, int32_t v)
{
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

static void helm_pane_snap(lv_obj_t * out, lv_obj_t * in)
{
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

/** 待机翻页：只位移，不改 opa、不 stagger。 */
static void helm_pane_slide(lv_obj_t * out, lv_obj_t * in, int dir)
{
    lv_anim_t ay;
    lv_coord_t dy = (dir >= 0) ? HELM_VIEW_PX : -HELM_VIEW_PX;

    helm_pane_reset(out);
    helm_pane_reset(in);

    if (out && out != in) {
        lv_anim_init(&ay);
        lv_anim_set_var(&ay, out);
        lv_anim_set_values(&ay, 0, -dy);
        lv_anim_set_time(&ay, HELM_VIEW_MS);
        lv_anim_set_path_cb(&ay, lv_anim_path_ease_in);
        lv_anim_set_exec_cb(&ay, helm_pane_y);
        lv_anim_set_ready_cb(&ay, helm_pane_hide_ready);
        lv_anim_start(&ay);
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

static void helm_fmt_speed(char * buf, size_t n, float kph)
{
    int v = (int)(kph * 10.0f + 0.5f);

    if (v < 0) {
        v = 0;
    }

    lv_snprintf(buf, n, "%d.%d", v / 10, v % 10);
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

static void helm_fmt_km(char * buf, size_t n, float km)
{
    int d10 = (int)(km * 10.0f + 0.5f);

    if (d10 < 0) {
        d10 = 0;
    }

    lv_snprintf(buf, n, "%d.%d", d10 / 10, d10 % 10);
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
    if (obj) {
        lv_obj_set_style_text_color(obj, helm_color(hex), 0);
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

static void helm_paint_hr_pair(lv_obj_t * num, lv_obj_t * unit)
{
    uint32_t ink = helm_live_hr_ink();

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

    if (!rt->recording && !s_sh.paused) {
        return false;
    }

    minute = (uint32_t)(rt->session_elapsed_ms / HELM_HIST_PERIOD_MS);
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
    lv_draw_triangle_dsc_t tri_dsc;
    lv_color_t fill;
    lv_area_t coords;
    int32_t full_h;
    int32_t i;

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
    lv_draw_triangle_dsc_init(&tri_dsc);
    tri_dsc.grad.dir = LV_GRAD_DIR_VER;
    tri_dsc.grad.stops_count = 2;

    for (i = 0; i < draw_line_dsc->point_cnt - 1; i++) {
        lv_point_precise_t p1 = draw_line_dsc->points[i];
        lv_point_precise_t p2 = draw_line_dsc->points[i + 1];
        lv_draw_rect_dsc_t rect_dsc;
        lv_area_t rect_area;
        int32_t y_min;
        int32_t y_max;
        int32_t fract_upper;
        int32_t fract_lower;

        if (p1.x == LV_DRAW_LINE_POINT_NONE || p1.y == LV_DRAW_LINE_POINT_NONE ||
            p2.x == LV_DRAW_LINE_POINT_NONE || p2.y == LV_DRAW_LINE_POINT_NONE) {
            continue;
        }

        y_min = (int32_t)(p1.y < p2.y ? p1.y : p2.y);
        y_max = (int32_t)(p1.y > p2.y ? p1.y : p2.y);
        fract_upper = (y_min - coords.y1) * 255 / full_h;
        fract_lower = (y_max - coords.y1) * 255 / full_h;
        if (fract_upper < 0) {
            fract_upper = 0;
        } else if (fract_upper > 255) {
            fract_upper = 255;
        }

        if (fract_lower < 0) {
            fract_lower = 0;
        } else if (fract_lower > 255) {
            fract_lower = 255;
        }

        tri_dsc.p[0] = p1;
        tri_dsc.p[1] = p2;
        tri_dsc.p[2].x = p1.y < p2.y ? p1.x : p2.x;
        tri_dsc.p[2].y = (lv_value_precise_t)y_max;
        tri_dsc.grad.stops[0].color = fill;
        tri_dsc.grad.stops[0].opa = (lv_opa_t)(255 - fract_upper);
        tri_dsc.grad.stops[0].frac = 0;
        tri_dsc.grad.stops[1].color = fill;
        tri_dsc.grad.stops[1].opa = (lv_opa_t)(255 - fract_lower);
        tri_dsc.grad.stops[1].frac = 255;
        lv_draw_triangle(base_dsc->layer, &tri_dsc);

        if ((int32_t)p2.x <= (int32_t)p1.x) {
            continue;
        }

        lv_draw_rect_dsc_init(&rect_dsc);
        rect_dsc.bg_grad.dir = LV_GRAD_DIR_VER;
        rect_dsc.bg_grad.stops_count = 2;
        rect_dsc.bg_grad.stops[0].color = fill;
        rect_dsc.bg_grad.stops[0].frac = 0;
        rect_dsc.bg_grad.stops[0].opa = (lv_opa_t)(255 - fract_lower);
        rect_dsc.bg_grad.stops[1].color = fill;
        rect_dsc.bg_grad.stops[1].frac = 255;
        rect_dsc.bg_grad.stops[1].opa = 0;
        rect_area.x1 = (int32_t)p1.x;
        rect_area.x2 = (int32_t)p2.x - 1;
        rect_area.y1 = y_max;
        rect_area.y2 = coords.y2;
        lv_draw_rect(base_dsc->layer, &rect_dsc, &rect_area);
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
static void helm_spark_bind(lv_obj_t * cell, lv_obj_t * lab)
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
    lv_obj_set_size(chart, lv_pct(100), lv_pct(100));
    lv_obj_align(chart, LV_ALIGN_TOP_LEFT, 0, 0);
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

    slot = (uint32_t)(rt->session_elapsed_ms / HELM_CLIMB_REC_PERIOD_MS);
    if (s_rec_ele_n > 0 && slot == s_rec_ele_slot) {
        s_rec_ele[s_rec_ele_n - 1u] = alt;
        return;
    }

    s_rec_ele_slot = slot;
    if (s_rec_ele_n >= HELM_CLIMB_PROF_N) {
        memmove(&s_rec_ele[0], &s_rec_ele[1],
                (size_t)(HELM_CLIMB_PROF_N - 1u) * sizeof(s_rec_ele[0]));
        s_rec_ele_n = (uint8_t)(HELM_CLIMB_PROF_N - 1u);
    }

    s_rec_ele[s_rec_ele_n++] = alt;
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

/**
 * @brief 隐藏导航剖面画布和竖线（非导航、无数据或回退现场绘制时）。
 */
static void helm_climb_canvas_hide(void)
{
    if (s_sh.climb_canvas) {
        lv_obj_add_flag(s_sh.climb_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_sh.climb_cursor) {
        lv_obj_add_flag(s_sh.climb_cursor, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief 把当前位置竖线放到画布对应 x。
 * @details 坐标仍来自隐藏的 `lv_chart`（`get_point_pos_by_id`），不重绘剖面。
 */
static void helm_climb_cursor_place(void)
{
    lv_point_t p;

    if (s_sh.climb_cursor == NULL || s_sh.climb_chart == NULL ||
        s_sh.climb_ser == NULL || s_sh.climb_n < 2u) {
        return;
    }
    lv_obj_update_layout(s_sh.climb_chart);
    lv_chart_get_point_pos_by_id(s_sh.climb_chart, s_sh.climb_ser,
        (uint32_t)s_sh.climb_now, &p);
    lv_obj_set_x(s_sh.climb_cursor, p.x);
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
    return true;
}

/**
 * @brief 只标脏当前位置竖线附近一条窄带（非导航「经过海拔」用）。
 * @param x 点在图表内的相对 x。
 */
static void helm_climb_inval_cursor_x(int32_t x)
{
    lv_area_t a;

    if (s_sh.climb_chart == NULL) {
        return;
    }
    lv_obj_get_coords(s_sh.climb_chart, &a);
    a.x1 += x - 2;
    a.x2 = a.x1 + 5;
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
        helm_climb_inval_cursor_x(prev_p.x);
        helm_climb_inval_cursor_x(p.x);
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
        helm_label_set(s_sh.climb_title, nav ? "路线海拔" : "经过海拔");
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
    if (nav && n >= 2u) {
        uint32_t step_cm = (fp_total_cm > 0u) ? (fp_total_cm / (n - 1u)) : 0u;

        if (step_cm < 1u) {
            step_cm = 1u;
        }
        for (i = 0; i + 1u < n; i++) {
            s_climb_zone[i] = helm_climb_grade_zone(s_climb_eles[i],
                s_climb_eles[i + 1u], step_cm);
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
    if (s_sh.climb_mark) {
        if (remain_gain >= 0) {
            lv_snprintf(buf, sizeof(buf), "%ldm", (long)remain_gain);
        } else {
            lv_snprintf(buf, sizeof(buf), "%u/%u",
                        (unsigned)page_i + 1u, (unsigned)page_n);
        }
        helm_label_set(s_sh.climb_mark, buf);
    }

    if (s_sh.climb_gain) {
        lv_snprintf(buf, sizeof(buf), "%d", rt ? (int)(rt->gain_m + 0.5f) : 0);
        helm_label_set(s_sh.climb_gain, buf);
    }

    if (s_sh.climb_alt) {
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
            lv_snprintf(buf, sizeof(buf), "%d", alt);
            helm_label_set(s_sh.climb_alt, buf);
        } else {
            helm_label_set(s_sh.climb_alt, "--");
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

/** @brief 录制、暂停或导航中：用骑行翻页（数据/地图/转向/爬升）。 */
static bool helm_in_session(void)
{
    return helm_riding() || helm_nav_on();
}

static uint8_t * helm_active_idx(void)
{
    return helm_in_session() ? &s_sh.ride_idx : &s_sh.home_idx;
}

static uint8_t helm_page_count(void)
{
    uint8_t n;

    if (!helm_in_session()) {
        n = 5u;
    } else {
        n = helm_nav_on() ? 6u : 5u;
    }
#if !HELM_PAGE_HR_ENABLE
    n -= 1u;
#endif
    return n;
}

static helm_page_id_t helm_page_at(uint8_t idx)
{
    bool nav = helm_nav_on();

    if (!helm_in_session()) {
        static const helm_page_id_t idle[] = {
            HELM_PAGE_STANDBY, HELM_PAGE_DATA,
#if HELM_PAGE_HR_ENABLE
            HELM_PAGE_DATA_HR,
#endif
            HELM_PAGE_DATA_SPD, HELM_PAGE_MAP
        };
        return idle[idx % (uint8_t)(sizeof(idle) / sizeof(idle[0]))];
    }

    if (nav) {
        static const helm_page_id_t n[] = {
            HELM_PAGE_DATA,
#if HELM_PAGE_HR_ENABLE
            HELM_PAGE_DATA_HR,
#endif
            HELM_PAGE_DATA_SPD, HELM_PAGE_MAP, HELM_PAGE_TURN, HELM_PAGE_CLIMB
        };
        return n[idx % (uint8_t)(sizeof(n) / sizeof(n[0]))];
    }

    {
        static const helm_page_id_t r[] = {
            HELM_PAGE_DATA,
#if HELM_PAGE_HR_ENABLE
            HELM_PAGE_DATA_HR,
#endif
            HELM_PAGE_DATA_SPD, HELM_PAGE_MAP, HELM_PAGE_CLIMB
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
        /* 待机地图 → 骑行地图；其余去掉待机页后对齐。 */
#if HELM_PAGE_HR_ENABLE
        if (s_sh.home_idx >= 4u) {
            s_sh.ride_idx = 3u;
#else
        if (s_sh.home_idx >= 3u) {
            s_sh.ride_idx = 2u;
#endif
        } else if (s_sh.home_idx > 0u) {
            s_sh.ride_idx = (uint8_t)(s_sh.home_idx - 1u);
        } else {
            s_sh.ride_idx = 0u;
        }
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
#if HELM_PAGE_HR_ENABLE
        if (s_sh.home_idx >= 4u) {
            s_sh.ride_idx = 3u;
#else
        if (s_sh.home_idx >= 3u) {
            s_sh.ride_idx = 2u;
#endif
        } else if (s_sh.home_idx > 0u) {
            s_sh.ride_idx = (uint8_t)(s_sh.home_idx - 1u);
        } else {
            s_sh.ride_idx = 0u;
        }
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
    lv_pm_notify_show("骑行", "未保存", 1500);
}

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
        lab[3] = "均心率";
        lv_snprintf(val[3], 16, "%s",
                    helm_dash(rt && rt->hr_avg_bpm > 0u, tmp, sizeof(tmp), "%u",
                              rt ? (int)rt->hr_avg_bpm : 0));
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

static void helm_fill_rot(uint8_t ri, const char ** la, char va[16],
                          const char ** lb, char vb[16])
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();
    char tmp[16];
    float avg = helm_avg_kph(rt);
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

            lv_snprintf(vb, 16, "%d.%d%%", g10 / 10, (g10 < 0 ? -g10 : g10) % 10);
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
    map_on = (id == HELM_PAGE_MAP);
    rot_on = (id == HELM_PAGE_MAP || id == HELM_PAGE_TURN ||
              id == HELM_PAGE_CLIMB);
    nav_ban = map_on && map_page_nav_active(s_sh.map);
    rot_was = (s_sh.rotbar != NULL) &&
              !lv_obj_has_flag(s_sh.rotbar, LV_OBJ_FLAG_HIDDEN);
    pause_was = (s_sh.pause_dock != NULL) &&
                !lv_obj_has_flag(s_sh.pause_dock, LV_OBJ_FLAG_HIDDEN);

    /* 只改可见标志，不在 KEY 这一拍里 catchup/重绘地图。 */
    map_page_set_map_ui_visible(s_sh.map, map_on);

    if (anim) {
        lv_obj_t * panes[4];
        lv_obj_t * po = helm_pane_of(prev);
        lv_obj_t * pi = helm_pane_of(id);
        unsigned i;

        panes[0] = s_sh.standby;
        panes[1] = s_sh.data;
        panes[2] = s_sh.turn;
        panes[3] = s_sh.climb;
        if (po == pi && po != NULL) {
            helm_pane_nudge(po, s_sh.view_dir);
        } else if (helm_riding()) {
            helm_pane_snap(po, pi);
            helm_riding_view_enter(s_sh.view_dir);
        } else {
            helm_pane_slide(po, pi, s_sh.view_dir);
        }

        for (i = 0; i < 4; i++) {
            if (panes[i] && panes[i] != po && panes[i] != pi) {
                helm_pane_reset(panes[i]);
                lv_obj_add_flag(panes[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        helm_pane_reset(s_sh.standby);
        helm_pane_reset(s_sh.data);
        helm_pane_reset(s_sh.turn);
        helm_pane_reset(s_sh.climb);
        helm_show(s_sh.standby, id == HELM_PAGE_STANDBY);
        helm_show(s_sh.data, helm_page_is_data(id));
        helm_show(s_sh.turn, id == HELM_PAGE_TURN);
        helm_show(s_sh.climb, id == HELM_PAGE_CLIMB);
    }
    helm_show(s_sh.rotbar, rot_on);
    helm_show(s_sh.nav_ban, nav_ban);
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
    helm_show(s_sh.subbar, id != HELM_PAGE_STANDBY);
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
    const char * labs[4];
    char vals[4][16];
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

    if (helm_page_is_data(id)) {
        uint8_t gi = (id == HELM_PAGE_DATA_HR) ? 1u
                   : (id == HELM_PAGE_DATA_SPD) ? 2u : 0u;
        const char * title = (gi == 1u) ? "心率" : (gi == 2u) ? "速度" : "骑行";

        helm_label_set(s_sh.data_title, title);

        if (gi == 1u) {
            if (rt && rt->hr_valid) {
                lv_snprintf(buf, sizeof(buf), "%u", (unsigned)rt->hr_bpm);
            } else {
                lv_snprintf(buf, sizeof(buf), "--");
            }

            helm_label_set(s_sh.data_speed, buf);
            helm_label_set(s_sh.data_unit, "bpm");
            helm_label_set(s_sh.data_hero_key, "心率");
            helm_paint_hr_pair(s_sh.data_speed, s_sh.data_unit);
        } else {
            helm_fmt_speed(buf, sizeof(buf), rt ? rt->speed_kph : 0);
            helm_label_set(s_sh.data_speed, buf);
            helm_label_set(s_sh.data_unit, "km/h");
            helm_label_set(s_sh.data_hero_key, "速度");
            helm_paint_speed_pair(s_sh.data_speed, s_sh.data_unit);
        }

        lv_snprintf(buf, sizeof(buf), "%u/%u",
                    (unsigned)*helm_active_idx() + 1u, (unsigned)n);
        helm_label_set(s_sh.data_mark, buf);

        helm_fill_group(gi, labs, vals);
        for (i = 0; i < 4; i++) {
            uint32_t fill = helm_tone_fill(labs[i]);
            bool spark = (fill != HELM_COLOR_PAPER);
            uint32_t ink = helm_metric_ink(labs[i]);

            helm_label_set(s_sh.cell_lab[i], labs[i]);
            helm_set_text_hex(s_sh.cell_lab[i], helm_metric_ink(labs[i]));

            if (spark && !helm_lab_eq(labs[i], "心率")) {
                ink = HELM_COLOR_INK;
            }
            helm_label_set(s_sh.cell_val[i], vals[i]);
            helm_set_text_hex(s_sh.cell_val[i], ink);
        }
    }

    if (id == HELM_PAGE_CLIMB) {
        helm_climb_tick(rt, *helm_active_idx(), n);
    }

    if (id == HELM_PAGE_TURN && s_sh.turn_dist && s_sh.turn_sub && s_sh.turn_ico) {
        char ico[8];
        char dist[16];
        char sub[32];

        if (map_page_nav_turn_info(s_sh.map, ico, sizeof(ico), dist, sizeof(dist),
                                   sub, sizeof(sub))) {
            helm_label_set(s_sh.turn_ico, ico);
            helm_label_set(s_sh.turn_dist, dist);
            helm_label_set(s_sh.turn_sub, sub);
        } else {
            helm_label_set(s_sh.turn_ico, "↑");
            helm_label_set(s_sh.turn_dist, "--");
            helm_label_set(s_sh.turn_sub, "转向");
        }
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

        {
            char nav_st[24];

            if (map_page_nav_status_text(s_sh.map, nav_st, sizeof(nav_st))) {
                helm_label_set(s_sh.sub_right, nav_st);
            } else {
                lv_snprintf(buf, sizeof(buf), "LAP %u",
                            rt ? (unsigned)rt->lap_count : 1u);
                helm_label_set(s_sh.sub_right, buf);
            }
        }
    }

#if VMAP_ROUTE_ENABLE
    if (helm_obj_shown(s_sh.nav_ban)) {
        static const char spin[] = "|/-\\";
        bool arrived = map_page_nav_arrived(s_sh.map);
        bool replanning = map_page_nav_replanning(s_sh.map) && !arrived;
        bool warn = rt && rt->nav_off_route_m > map_page_nav_off_warn_m(s_sh.map)
                    && !arrived && !replanning;
        uint32_t bg = arrived ? HELM_COLOR_CAD_FILL :
                      ((warn || replanning) ? HELM_COLOR_WARN
                                            : HELM_COLOR_NAV_FILL);
        uint32_t fg = arrived ? HELM_COLOR_CAD :
                      ((warn || replanning) ? HELM_COLOR_INK
                                            : HELM_COLOR_NAV);
        char ico[8];
        char dist[16];
        char sub[32];
        char dest[32];

        if (!arrived) {
            s_sh.arrive_dismissed = false;
        }

        if (map_page_nav_turn_info(s_sh.map, ico, sizeof(ico), dist, sizeof(dist),
                                   sub, sizeof(sub))) {
            helm_label_set(s_sh.nav_ban_ico, ico);
            if (sub[0] != '\0') {
                lv_snprintf(line, sizeof(line), "%s · %s", dist, sub);
                helm_label_set(s_sh.nav_ban_m, line);
            } else {
                helm_label_set(s_sh.nav_ban_m, dist);
            }
        } else {
            helm_label_set(s_sh.nav_ban_ico, "↑");
            helm_label_set(s_sh.nav_ban_m, "--");
            sub[0] = '\0';
        }

        if (map_page_nav_status_text(s_sh.map, dest, sizeof(dest))
            && dest[0] != '\0') {
            lv_snprintf(line, sizeof(line), "去 %s", dest);
            helm_label_set(s_sh.nav_ban_rd, line);
        } else {
            helm_label_set(s_sh.nav_ban_rd, "");
        }

        lv_obj_set_style_bg_color(s_sh.nav_ban, helm_color(bg), 0);
        lv_obj_set_style_border_width(s_sh.nav_ban, 0, 0);
        if (s_sh.nav_ban_ico) {
            lv_obj_set_style_text_color(s_sh.nav_ban_ico, helm_color(fg), 0);
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
            lv_obj_set_style_text_color(s_sh.nav_ban_m, helm_color(fg), 0);
            if (arrived) {
                helm_label_set(s_sh.nav_ban_m, "已到达");
            } else if (replanning) {
                helm_label_set(s_sh.nav_ban_m, "正在重新规划路线");
            } else if (warn) {
                lv_snprintf(line, sizeof(line), "偏航 %d m",
                            (int)(rt->nav_off_route_m + 0.5));
                helm_label_set(s_sh.nav_ban_m, line);
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
    helm_shell_toggle_ride();
}

void helm_shell_on_nav_started(void)
{
    if (!s_sh.attached) {
        return;
    }

    /* 导航不自动开 REC。切到骑行地图页。 */
#if HELM_PAGE_HR_ENABLE
    s_sh.ride_idx = 3;
#else
    s_sh.ride_idx = 2;
#endif
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
    if (id == HELM_PAGE_MAP || id == HELM_PAGE_TURN ||
        id == HELM_PAGE_CLIMB) {
        s_sh.rot_idx = (uint8_t)((s_sh.rot_idx + 1u) % 5u);
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

static lv_obj_t * helm_metric_cell(lv_obj_t * parent, int idx)
{
    lv_obj_t * cell = lv_obj_create(parent);

    lv_obj_remove_style_all(cell);
    helm_grow_x(cell);
    lv_obj_set_height(cell, lv_pct(100));
    helm_style_card(cell);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cell, 4, 0);

    s_sh.cell_box[idx] = cell;
    s_sh.cell_lab[idx] = helm_lab(cell, s_sh.font_lab, HELM_COLOR_INK, "--");
    s_sh.cell_val[idx] = helm_lab(cell, s_sh.font_quad, HELM_COLOR_INK, "--");
    helm_spark_bind(cell, s_sh.cell_lab[idx]);
    return cell;
}

static lv_obj_t * helm_metric_row(lv_obj_t * parent)
{
    lv_obj_t * row = lv_obj_create(parent);

    lv_obj_remove_style_all(row);
    helm_grow_y(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, HELM_GAP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
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

static void helm_build_standby(lv_obj_t * root)
{
    lv_obj_t * hero;
    lv_obj_t * row;
    lv_obj_t * hint;

    s_sh.standby = helm_overlay(root, 0, HELM_PAGE_H);

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
}

static void helm_build_data(lv_obj_t * root)
{
    lv_obj_t * wrap;
    lv_obj_t * quad;

    s_sh.data = helm_overlay(root, HELM_SUBBAR_H,
                             HELM_PAGE_H - HELM_SUBBAR_H);
    helm_speed_head(s_sh.data, "骑行", &s_sh.data_title, &s_sh.data_mark);

    wrap = lv_obj_create(s_sh.data);
    lv_obj_remove_style_all(wrap);
    helm_style_card(wrap);
    lv_obj_set_size(wrap, lv_pct(100), HELM_SPEED_WRAP_H);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    s_sh.data_speed = helm_lab(wrap, s_sh.font_speed_ride, HELM_COLOR_NAV, "0.0");
    lv_obj_set_width(s_sh.data_speed, lv_pct(100));
    lv_obj_set_style_text_align(s_sh.data_speed, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(s_sh.data_speed, -1, 0);
    s_sh.data_unit = helm_lab(wrap, s_sh.font_lab, HELM_COLOR_NAV, "km/h");
    lv_obj_set_width(s_sh.data_unit, lv_pct(100));
    lv_obj_set_style_text_align(s_sh.data_unit, LV_TEXT_ALIGN_CENTER, 0);
    {
        lv_obj_t * key = helm_lab(wrap, s_sh.font_lab, HELM_COLOR_INK, "速度");

        lv_obj_add_flag(key, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_FLOATING);
        s_sh.data_hero_key = key;
        helm_spark_bind(wrap, key);
        if (s_sh.data_speed) {
            lv_obj_move_foreground(s_sh.data_speed);
        }

        if (s_sh.data_unit) {
            lv_obj_move_foreground(s_sh.data_unit);
        }
    }

    quad = lv_obj_create(s_sh.data);
    lv_obj_remove_style_all(quad);
    helm_grow_y(quad);
    lv_obj_set_flex_flow(quad, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(quad, HELM_GAP, 0);
    {
        lv_obj_t * r0 = helm_metric_row(quad);
        lv_obj_t * r1 = helm_metric_row(quad);

        helm_metric_cell(r0, 0);
        helm_metric_cell(r0, 1);
        helm_metric_cell(r1, 2);
        helm_metric_cell(r1, 3);
    }
}

static void helm_build_turn(lv_obj_t * root)
{
    s_sh.turn = helm_overlay(root, HELM_SUBBAR_H, HELM_MAP_BODY_H);
    lv_obj_set_flex_align(s_sh.turn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    s_sh.turn_ico = helm_lab(s_sh.turn, s_sh.font_mark, HELM_COLOR_NAV, "↑");
    lv_obj_set_style_transform_pivot_x(s_sh.turn_ico, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_sh.turn_ico, LV_PCT(50), 0);
    lv_obj_set_style_transform_scale(s_sh.turn_ico, 336, 0);
    lv_obj_add_flag(s_sh.turn_ico, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    s_sh.turn_dist = helm_lab(s_sh.turn, s_sh.font_quad, HELM_COLOR_NAV, "--");
    /* 转向页的副标题也是路名，同样不能走点阵字库（缺字见上面 nav_ban_rd）。 */
    s_sh.turn_sub = helm_lab(s_sh.turn, helm_font_sys(15, s_sh.font_lab),
                             HELM_COLOR_NAV, "转向");
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
    lv_draw_triangle_dsc_t tri_dsc;
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

    fill = helm_color_snap(HELM_COLOR_CLIMB_FILL);
    lv_draw_triangle_dsc_init(&tri_dsc);
    tri_dsc.grad.dir = LV_GRAD_DIR_VER;
    tri_dsc.grad.stops_count = 2;
    tri_dsc.grad.stops[0].color = fill;
    tri_dsc.grad.stops[0].opa = LV_OPA_50;
    tri_dsc.grad.stops[0].frac = 0;
    tri_dsc.grad.stops[1].color = fill;
    tri_dsc.grad.stops[1].opa = LV_OPA_0;
    tri_dsc.grad.stops[1].frac = 255;

    for (i = 0; i < draw_line_dsc->point_cnt - 1; i++) {
        lv_point_precise_t p1 = draw_line_dsc->points[i];
        lv_point_precise_t p2 = draw_line_dsc->points[i + 1];
        lv_draw_rect_dsc_t rect_dsc;
        lv_area_t rect_area;

        if (p1.x == LV_DRAW_LINE_POINT_NONE || p1.y == LV_DRAW_LINE_POINT_NONE ||
            p2.x == LV_DRAW_LINE_POINT_NONE || p2.y == LV_DRAW_LINE_POINT_NONE) {
            continue;
        }
        if (!helm_climb_seg_in_clip(&base_dsc->layer->_clip_area, p1, p2,
                coords.y2, 2)) {
            continue;
        }

        tri_dsc.p[0] = p1;
        tri_dsc.p[1] = p2;
        tri_dsc.p[2].x = p2.x;
        tri_dsc.p[2].y = coords.y2;
        lv_draw_triangle(base_dsc->layer, &tri_dsc);

        lv_draw_rect_dsc_init(&rect_dsc);
        rect_dsc.bg_color = fill;
        rect_dsc.bg_opa = LV_OPA_20;
        rect_dsc.bg_grad.dir = LV_GRAD_DIR_VER;
        rect_dsc.bg_grad.stops_count = 2;
        rect_dsc.bg_grad.stops[0].color = fill;
        rect_dsc.bg_grad.stops[0].opa = LV_OPA_40;
        rect_dsc.bg_grad.stops[0].frac = 0;
        rect_dsc.bg_grad.stops[1].color = fill;
        rect_dsc.bg_grad.stops[1].opa = LV_OPA_0;
        rect_dsc.bg_grad.stops[1].frac = 255;
        rect_area.x1 = (int32_t)(p1.x < p2.x ? p1.x : p2.x);
        rect_area.x2 = (int32_t)(p1.x > p2.x ? p1.x : p2.x);
        rect_area.y1 = (int32_t)(p1.y > p2.y ? p1.y : p2.y);
        rect_area.y2 = coords.y2;
        if (rect_area.x2 > rect_area.x1 && rect_area.y2 > rect_area.y1) {
            lv_draw_rect(base_dsc->layer, &rect_dsc, &rect_area);
        }
    }
}

/**
 * @brief 绘制当前位置竖线。导航用黑色，经过海拔用青绿。
 * @param e LV_EVENT_DRAW_POST。
 * @details 画布可见时竖线由 `climb_cursor` 对象承担，本回调直接返回。
 */
static void helm_climb_draw(lv_event_t * e)
{
    lv_layer_t * layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_point_t p;
    lv_color_t c;
    uint32_t id;
    uint32_t n;
    int32_t x;

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
    x = coords.x1 + p.x;
    c = helm_color_snap(s_sh.climb_nav ? HELM_COLOR_INK : HELM_COLOR_CLIMB);
    {
        lv_draw_rect_dsc_t rd;
        lv_area_t bar;

        lv_draw_rect_dsc_init(&rd);
        rd.bg_color = c;
        rd.bg_opa = LV_OPA_COVER;
        bar.x1 = x;
        bar.x2 = x + 1;
        bar.y1 = coords.y1;
        bar.y2 = coords.y2;
        lv_draw_rect(layer, &rd, &bar);
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
    lv_obj_t * row;
    lv_obj_t * dummy;
    lv_obj_t * card;
    lv_obj_t * chart;

    s_sh.climb = helm_overlay(root, HELM_SUBBAR_H, HELM_MAP_BODY_H);
    helm_speed_head(s_sh.climb, "爬升", NULL, &s_sh.climb_mark);

    row = lv_obj_create(s_sh.climb);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), HELM_ROW2_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, HELM_GAP, 0);

    helm_col(row, &dummy, &s_sh.climb_alt, s_sh.font_quad);
    lv_label_set_text_static(dummy, "海拔");
    lv_obj_set_style_text_color(s_sh.climb_alt, helm_color(HELM_COLOR_INK), 0);
    helm_spark_bind(lv_obj_get_parent(s_sh.climb_alt), dummy);

    helm_col(row, &dummy, &s_sh.climb_gain, s_sh.font_quad);
    lv_label_set_text_static(dummy, "累计爬升");
    lv_obj_set_style_text_color(s_sh.climb_gain, helm_color(HELM_COLOR_INK), 0);
    lv_label_set_text_static(s_sh.climb_gain, "--");
    helm_spark_bind(lv_obj_get_parent(s_sh.climb_gain), dummy);

    card = lv_obj_create(s_sh.climb);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, lv_pct(100), HELM_CLIMB_CHART_H);
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

    s_sh.climb_cursor = lv_obj_create(card);
    lv_obj_remove_style_all(s_sh.climb_cursor);
    lv_obj_remove_flag(s_sh.climb_cursor, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_sh.climb_cursor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_sh.climb_cursor, 2, lv_pct(100));
    lv_obj_set_style_bg_color(s_sh.climb_cursor, helm_color_snap(HELM_COLOR_INK), 0);
    lv_obj_set_style_bg_opa(s_sh.climb_cursor, LV_OPA_COVER, 0);
    lv_obj_align(s_sh.climb_cursor, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(s_sh.climb_cursor, LV_OBJ_FLAG_HIDDEN);

    s_sh.climb_title = helm_lab(card, s_sh.font_lab, HELM_COLOR_CLIMB, "海拔剖面");
    lv_obj_align(s_sh.climb_title, LV_ALIGN_TOP_LEFT, 8, 6);
    lv_obj_move_foreground(s_sh.climb_title);
    lv_obj_move_foreground(s_sh.climb_cursor);
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
    s_sh.sub_right = helm_lab(s_sh.subbar, s_sh.font_lab, HELM_COLOR_INK, "LAP 1");

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
    helm_spark_bind(s_sh.rot_cell[1], s_sh.rot_lab[1]);
    helm_spark_bind(s_sh.rot_cell[2], s_sh.rot_lab[2]);
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
    lv_obj_set_size(s_sh.nav_ban, PAGE_HOR_RES, HELM_NAV_BAN_H);
    lv_obj_add_flag(s_sh.nav_ban, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(s_sh.nav_ban, 0, HELM_SUBBAR_H);
    lv_obj_set_style_bg_color(s_sh.nav_ban, helm_color(HELM_COLOR_NAV_FILL), 0);
    lv_obj_set_style_bg_opa(s_sh.nav_ban, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_sh.nav_ban, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sh.nav_ban, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(s_sh.nav_ban, 8, 0);
    lv_obj_set_style_pad_gap(s_sh.nav_ban, 8, 0);
    lv_obj_add_flag(s_sh.nav_ban, LV_OBJ_FLAG_HIDDEN);
    s_sh.nav_ban_ico = helm_lab(s_sh.nav_ban, s_sh.font_val, HELM_COLOR_NAV, "↑");
    lv_obj_set_style_text_font(s_sh.nav_ban_ico, s_sh.font_val, 0);
    lv_obj_set_width(s_sh.nav_ban_ico, 36);
    lv_obj_set_style_text_align(s_sh.nav_ban_ico, LV_TEXT_ALIGN_CENTER, 0);
    col = lv_obj_create(s_sh.nav_ban);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    helm_grow_x(col);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    s_sh.nav_ban_m = helm_lab(col, helm_font_sys(22, s_sh.font_val),
                              HELM_COLOR_NAV, "--");
    /* 路名必须走 FreeType：点阵字库（helm_mism4_*）只收了 UI 文案那 486 个字，
     * 真实路名（「金陵路」的 金/陵 都在缺字之列）会整块显示不出来。
     * 地图上的路名一直用 myvendor_system_font_get() 所以正常，横幅这里漏了。 */
    s_sh.nav_ban_rd = helm_lab(col, helm_font_sys(15, s_sh.font_title),
                               HELM_COLOR_INK, "");
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
    helm_pane_reset(s_sh.turn);
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

    s_sh.map = map;
    s_sh.font_lab = helm_font_lab();
    s_sh.font_title = helm_font_title();
    s_sh.font_val = helm_font_val();
    s_sh.font_quad = helm_font_quad();
    s_sh.font_rot = helm_font_rot_speed();
    s_sh.font_speed = helm_font_speed();
    s_sh.font_speed_ride = helm_font_speed_ride();
    s_sh.font_mark = helm_font_mark();

    helm_build_chrome(root);
    helm_build_standby(root);
    helm_build_data(root);
    helm_build_turn(root);
    helm_build_climb(root);
    helm_build_pause(root);
    helm_build_save(root);
    helm_build_arrive(root);
    helm_hist_reset();
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
    s_sh.rot_timer = lv_timer_create(helm_rot_cb, HELM_ROT_MS, NULL);
    s_sh.ui_timer = lv_timer_create(helm_ui_cb, HELM_UI_MS, NULL);
}
