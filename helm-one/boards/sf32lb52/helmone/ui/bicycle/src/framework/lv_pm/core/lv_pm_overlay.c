/**
 * @file lv_pm_overlay.c
 * @brief lv_pm 页面管理 — overlay。
 */

#include "../include/lv_pm_overlay.h"
#include "../include/lv_pm_bar.h"
#include "../include/lv_pm_disp.h"

#include "helm_font.h"

#include <unistd.h>

#ifndef LV_PM_NOTIFY_ANIM_MS
#define LV_PM_NOTIFY_ANIM_MS 320
#endif
#ifndef LV_PM_NOTIFY_DEFAULT_MS
#define LV_PM_NOTIFY_DEFAULT_MS 3200
#endif
#ifndef LV_PM_BOTTOM_DEFAULT_MS
#define LV_PM_BOTTOM_DEFAULT_MS 2600
#endif
#ifndef LV_PM_BOTTOM_Y_OFS
#define LV_PM_BOTTOM_Y_OFS (lv_pm_ver_res() / 5)
#endif
#ifndef LV_PM_OVERLAY_FONT_PX
#define LV_PM_OVERLAY_FONT_PX 14
#endif
#ifndef LV_PM_BOTTOM_CARD_MIN_H
#define LV_PM_BOTTOM_CARD_MIN_H 44
#endif
#ifndef LV_PM_OVERLAY_BG_OPA
#define LV_PM_OVERLAY_BG_OPA 200
#endif
#ifndef LV_PM_NOTIFY_CARD_MARGIN
#define LV_PM_NOTIFY_CARD_MARGIN 16
#endif
#ifndef LV_PM_BOTTOM_MAX_W
#define LV_PM_BOTTOM_MAX_W (lv_pm_hor_res() - 32)
#endif
#ifndef LV_PM_NOTIFY_GAP_BELOW_BAR
#define LV_PM_NOTIFY_GAP_BELOW_BAR 2
#endif
#ifndef LV_PM_NOTIFY_ICON_PX
#define LV_PM_NOTIFY_ICON_PX 32
#endif

#if LV_PM_USE_OVERLAY

typedef struct {
    lv_obj_t * card;
    lv_obj_t * icon;
    lv_obj_t * text_col;
    lv_obj_t * title;
    lv_obj_t * body;
    lv_timer_t * timer;
    lv_coord_t shown_y;
    lv_coord_t hidden_y;
    bool showing;
} pm_notify_t;

typedef struct {
    lv_obj_t * card;
    lv_obj_t * label;
    lv_timer_t * timer;
    bool showing;
} pm_bottom_t;

static pm_notify_t s_notify;
static pm_bottom_t s_bottom;

static void overlay_anim_y(void * var, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)v, 0);
}

static lv_font_t * overlay_font(void)
{
    /* 量产：任意通知走系统 TTF；工厂：只用 4bpp C 字模。 */
    return (lv_font_t *)helm_font_sys(LV_PM_OVERLAY_FONT_PX, helm_font_title());
}

static void overlay_style_label_fixed(lv_obj_t * lbl, lv_font_t * font)
{
    if (font != NULL) {
        lv_obj_set_style_text_font(lbl, font, 0);
    }
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
}

static void overlay_style_label_fit(lv_obj_t * lbl, lv_font_t * font)
{
    if (font != NULL) {
        lv_obj_set_style_text_font(lbl, font, 0);
    }
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(lbl, LV_PM_BOTTOM_MAX_W - 20, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
}

static lv_coord_t overlay_notify_card_w(void)
{
    return lv_pm_hor_res() - LV_PM_NOTIFY_CARD_MARGIN;
}

static void overlay_notify_layout_card(void)
{
    if (s_notify.card == NULL) {
        return;
    }

    lv_obj_set_width(s_notify.card, overlay_notify_card_w());
    lv_obj_add_flag(s_notify.card, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_translate_x(s_notify.card, 0, 0);
    lv_obj_set_style_transform_pivot_x(s_notify.card, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(s_notify.card, 0, 0);
    lv_obj_update_layout(s_notify.card);
    lv_obj_align(s_notify.card, LV_ALIGN_TOP_MID, 0, 0);
}

static void overlay_style_notify_card(lv_obj_t * card)
{
    lv_font_t * font = overlay_font();

    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, overlay_notify_card_w());
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_PM_OVERLAY_BG_OPA, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_set_style_shadow_width(card, 8, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, 8, 0);
    if (font != NULL) {
        lv_obj_set_style_text_font(card, font, 0);
    }
}

static void overlay_style_bottom_card(lv_obj_t * card)
{
    lv_font_t * font = overlay_font();

    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_SIZE_CONTENT);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, LV_PM_BOTTOM_CARD_MIN_H, 0);
    lv_obj_set_style_max_width(card, LV_PM_BOTTOM_MAX_W, 0);
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_PM_OVERLAY_BG_OPA, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_hor(card, 16, 0);
    lv_obj_set_style_pad_ver(card, 10, 0);
    lv_obj_set_style_shadow_width(card, 8, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    if (font != NULL) {
        lv_obj_set_style_text_font(card, font, 0);
    }
}

static void overlay_apply_chrome(void)
{
    lv_font_t * font = overlay_font();

    if (s_notify.card != NULL) {
        overlay_style_notify_card(s_notify.card);
    }
    if (s_notify.title != NULL) {
        overlay_style_label_fixed(s_notify.title, font);
    }
    if (s_notify.body != NULL) {
        overlay_style_label_fixed(s_notify.body, font);
    }
    if (s_bottom.card != NULL) {
        overlay_style_bottom_card(s_bottom.card);
    }
    if (s_bottom.label != NULL) {
        overlay_style_label_fit(s_bottom.label, font);
    }
}

static void overlay_run_slide(lv_obj_t * card, lv_coord_t from_y, lv_coord_t to_y,
                              uint32_t time_ms, lv_anim_ready_cb_t ready_cb)
{
    lv_anim_t a;

    lv_anim_delete(card, overlay_anim_y);
    lv_obj_set_style_translate_y(card, from_y, 0);
    lv_obj_set_style_opa(card, LV_OPA_COVER, 0);

    lv_anim_init(&a);
    lv_anim_set_var(&a, card);
    lv_anim_set_exec_cb(&a, overlay_anim_y);
    lv_anim_set_values(&a, from_y, to_y);
    lv_anim_set_time(&a, time_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (ready_cb != NULL) {
        lv_anim_set_ready_cb(&a, ready_cb);
    }
    lv_anim_start(&a);
}

static void overlay_raise_status_chrome(void)
{
#if LV_PM_USE_STA_BAR
    lv_obj_t * chrome = lv_pm_status_bar_cont();

    if (chrome != NULL && !lv_obj_has_flag(chrome, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(chrome);
    }
#endif
}

static void overlay_bring_top(lv_obj_t * card)
{
    if (card != NULL) {
        lv_obj_move_foreground(card);
    }
}

static void notify_hide_ready(lv_anim_t * a)
{
    lv_obj_t * card = (lv_obj_t *)a->var;

    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_translate_y(card, s_notify.hidden_y, 0);
    lv_obj_set_style_opa(card, LV_OPA_COVER, 0);
    s_notify.showing = false;
}

static void bottom_hide_now(void)
{
    if (s_bottom.card == NULL) {
        return;
    }

    lv_anim_delete(s_bottom.card, overlay_anim_y);
    lv_obj_set_style_translate_y(s_bottom.card, 0, 0);
    lv_obj_set_style_opa(s_bottom.card, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_bottom.card, LV_OBJ_FLAG_HIDDEN);
    s_bottom.showing = false;
}

static void notify_timer_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    lv_pm_notify_dismiss();
}

static void bottom_timer_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    lv_pm_bottom_dismiss();
}

static void notify_stop_timer(void)
{
    if (s_notify.timer != NULL) {
        lv_timer_pause(s_notify.timer);
    }
}

static void bottom_stop_timer(void)
{
    if (s_bottom.timer != NULL) {
        lv_timer_pause(s_bottom.timer);
    }
}

static void notify_update_geometry(void)
{
    const lv_coord_t bar_h = lv_pm_status_bar_height();

    s_notify.shown_y = bar_h + LV_PM_NOTIFY_GAP_BELOW_BAR;
    s_notify.hidden_y = -(lv_coord_t)(lv_pm_ver_res() / 2);
}

/**
 * @brief lv_pm overlay apply theme。
 */
void lv_pm_overlay_apply_theme(const lv_pm_theme_def_t * def)
{
    LV_UNUSED(def);
    overlay_apply_chrome();
}

/**
 * @brief lv_pm overlay init。
 * @return 0 成功，负 errno 失败。
 */
void lv_pm_overlay_init(lv_obj_t * parent)
{
    if (parent == NULL || s_notify.card != NULL) {
        return;
    }

    notify_update_geometry();

    s_notify.card = lv_obj_create(parent);
    overlay_style_notify_card(s_notify.card);
    overlay_notify_layout_card();
    lv_obj_set_style_translate_y(s_notify.card, s_notify.hidden_y, 0);
    lv_obj_add_flag(s_notify.card, LV_OBJ_FLAG_HIDDEN);

    s_notify.icon = lv_img_create(s_notify.card);
    lv_obj_set_size(s_notify.icon, LV_PM_NOTIFY_ICON_PX, LV_PM_NOTIFY_ICON_PX);
    lv_obj_add_flag(s_notify.icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_notify.icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_notify.text_col = lv_obj_create(s_notify.card);
    lv_obj_remove_style_all(s_notify.text_col);
    lv_obj_set_flex_grow(s_notify.text_col, 1);
    lv_obj_set_width(s_notify.text_col, LV_PCT(100));
    lv_obj_set_height(s_notify.text_col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_notify.text_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_notify.text_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_notify.text_col, 2, 0);
    lv_obj_clear_flag(s_notify.text_col, LV_OBJ_FLAG_SCROLLABLE);

    s_notify.title = lv_label_create(s_notify.text_col);
    lv_label_set_text(s_notify.title, "");
    overlay_style_label_fixed(s_notify.title, overlay_font());

    s_notify.body = lv_label_create(s_notify.text_col);
    lv_label_set_text(s_notify.body, "");
    overlay_style_label_fixed(s_notify.body, overlay_font());

    s_notify.timer = lv_timer_create(notify_timer_cb, LV_PM_NOTIFY_DEFAULT_MS, NULL);
    lv_timer_pause(s_notify.timer);

    s_bottom.card = lv_obj_create(parent);
    overlay_style_bottom_card(s_bottom.card);
    lv_obj_align(s_bottom.card, LV_ALIGN_CENTER, 0, LV_PM_BOTTOM_Y_OFS);
    lv_obj_add_flag(s_bottom.card, LV_OBJ_FLAG_HIDDEN);

    s_bottom.label = lv_label_create(s_bottom.card);
    lv_label_set_text(s_bottom.label, "");
    overlay_style_label_fit(s_bottom.label, overlay_font());

    s_bottom.timer = lv_timer_create(bottom_timer_cb, LV_PM_BOTTOM_DEFAULT_MS, NULL);
    lv_timer_pause(s_bottom.timer);

    overlay_raise_status_chrome();
}

/**
 * @brief lv_pm notify show。
 */
bool lv_pm_notify_show(const char * title, const char * text, uint32_t duration_ms)
{
    return lv_pm_notify_show_ex(title, text, NULL, duration_ms);
}

/**
 * @brief lv_pm notify show ex。
 */
bool lv_pm_notify_show_ex(const char * title, const char * text,
                          const char * icon_path, uint32_t duration_ms)
{
    bool have_icon = false;

    if (s_notify.card == NULL || text == NULL) {
        return false;
    }

    notify_update_geometry();
    overlay_apply_chrome();

    if (icon_path != NULL && icon_path[0] != '\0' && s_notify.icon != NULL &&
        access(icon_path, R_OK) == 0) {
        lv_img_set_src(s_notify.icon, icon_path);
        lv_obj_clear_flag(s_notify.icon, LV_OBJ_FLAG_HIDDEN);
        have_icon = true;
    } else if (s_notify.icon != NULL) {
        lv_obj_add_flag(s_notify.icon, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_notify.title != NULL) {
        lv_obj_set_style_text_align(s_notify.title,
            have_icon ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
    }
    if (s_notify.body != NULL) {
        lv_obj_set_style_text_align(s_notify.body,
            have_icon ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
    }

    if (title != NULL && title[0] != '\0') {
        lv_label_set_text(s_notify.title, title);
        lv_obj_clear_flag(s_notify.title, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_notify.title, "");
        lv_obj_add_flag(s_notify.title, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(s_notify.body, text);
    lv_obj_update_layout(s_notify.card);
    overlay_notify_layout_card();

    notify_stop_timer();
    lv_obj_clear_flag(s_notify.card, LV_OBJ_FLAG_HIDDEN);
    overlay_bring_top(s_notify.card);
    overlay_raise_status_chrome();

    overlay_run_slide(s_notify.card, s_notify.hidden_y, s_notify.shown_y,
                      LV_PM_NOTIFY_ANIM_MS, NULL);
    s_notify.showing = true;

    if (duration_ms == 0) {
        duration_ms = LV_PM_NOTIFY_DEFAULT_MS;
    }

    lv_timer_set_period(s_notify.timer, duration_ms);
    lv_timer_reset(s_notify.timer);
    lv_timer_resume(s_notify.timer);
    overlay_raise_status_chrome();
    return true;
}

/**
 * @brief lv_pm notify dismiss。
 */
void lv_pm_notify_dismiss(void)
{
    if (s_notify.card == NULL || !s_notify.showing) {
        return;
    }

    notify_stop_timer();
    s_notify.showing = false;
    overlay_run_slide(s_notify.card, lv_obj_get_style_translate_y(s_notify.card, 0),
                      s_notify.hidden_y, LV_PM_NOTIFY_ANIM_MS, notify_hide_ready);
}

bool lv_pm_notify_is_showing(void)
{
    return s_notify.showing;
}

/**
 * @brief lv_pm bottom show。
 */
bool lv_pm_bottom_show(const char * text, uint32_t duration_ms)
{
    if (s_bottom.card == NULL || text == NULL) {
        return false;
    }

    overlay_apply_chrome();
    lv_label_set_text(s_bottom.label, text);
    lv_obj_update_layout(s_bottom.card);
    lv_obj_align(s_bottom.card, LV_ALIGN_CENTER, 0, LV_PM_BOTTOM_Y_OFS);

    bottom_stop_timer();
    lv_anim_delete(s_bottom.card, overlay_anim_y);
    lv_obj_set_style_translate_y(s_bottom.card, 0, 0);
    lv_obj_set_style_opa(s_bottom.card, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_bottom.card, LV_OBJ_FLAG_HIDDEN);
    overlay_bring_top(s_bottom.card);
    s_bottom.showing = true;

    if (duration_ms == 0) {
        duration_ms = LV_PM_BOTTOM_DEFAULT_MS;
    }

    lv_timer_set_period(s_bottom.timer, duration_ms);
    lv_timer_reset(s_bottom.timer);
    lv_timer_resume(s_bottom.timer);
    return true;
}

/**
 * @brief lv_pm bottom dismiss。
 */
void lv_pm_bottom_dismiss(void)
{
    if (s_bottom.card == NULL || !s_bottom.showing) {
        return;
    }

    bottom_stop_timer();
    bottom_hide_now();
}

#endif /* LV_PM_USE_OVERLAY */
