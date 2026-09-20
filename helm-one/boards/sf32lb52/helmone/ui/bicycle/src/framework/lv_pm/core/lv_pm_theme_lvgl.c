/**
 * @file lv_pm_theme_lvgl.c
 * @brief lv_pm 页面管理 — theme_lvgl。
 */

#include "../include/lv_pm_theme_def.h"

#include "lvgl/lvgl.h"

static lv_theme_t * s_pm_theme;
static lv_style_t s_style_page;
static lv_style_t s_style_cont;
static lv_style_t s_style_text;
static lv_style_t s_style_focus;
static lv_style_t s_style_bar_main;
static lv_style_t s_style_bar_ind;
static bool s_styles_inited;

static void pm_styles_update(const lv_pm_theme_def_t * def)
{
    const lv_pm_theme_colors_t * c = &def->colors;
    const lv_font_t * font = def->fonts.body;

    if (!s_styles_inited) {
        lv_style_init(&s_style_page);
        lv_style_init(&s_style_cont);
        lv_style_init(&s_style_text);
        lv_style_init(&s_style_focus);
        lv_style_init(&s_style_bar_main);
        lv_style_init(&s_style_bar_ind);
        s_styles_inited = true;
    }

    lv_style_set_bg_color(&s_style_page, c->page_bg);
    lv_style_set_bg_opa(&s_style_page, LV_OPA_COVER);
    lv_style_set_text_color(&s_style_page, c->text_primary);
    lv_style_set_border_width(&s_style_page, 0);
    lv_style_set_radius(&s_style_page, 0);
    lv_style_set_shadow_width(&s_style_page, 0);
    if (font != NULL) {
        lv_style_set_text_font(&s_style_page, font);
    }

    lv_style_set_border_width(&s_style_cont, 0);
    lv_style_set_radius(&s_style_cont, 0);
    lv_style_set_shadow_width(&s_style_cont, 0);
    lv_style_set_pad_all(&s_style_cont, 0);

    lv_style_set_text_color(&s_style_text, c->text_primary);
    if (font != NULL) {
        lv_style_set_text_font(&s_style_text, font);
    }

    lv_style_set_outline_width(&s_style_focus, 2);
    lv_style_set_outline_opa(&s_style_focus, LV_OPA_COVER);
    lv_style_set_outline_color(&s_style_focus, c->focus);

    lv_style_set_bg_color(&s_style_bar_main, c->panel_bg);
    lv_style_set_bg_opa(&s_style_bar_main, c->panel_bg_opa);
    lv_style_set_radius(&s_style_bar_main, 4);

    lv_style_set_bg_color(&s_style_bar_ind, c->accent);
    lv_style_set_bg_opa(&s_style_bar_ind, LV_OPA_COVER);
    lv_style_set_radius(&s_style_bar_ind, 4);
}

static void pm_lvgl_apply_cb(lv_theme_t * th, lv_obj_t * obj)
{
    LV_UNUSED(th);

    /* Overlay only — never remove_style_all (see EPD lv_port_theme.c). */
    if (lv_obj_check_type(obj, &lv_obj_class)) {
        lv_obj_add_style(obj, &s_style_cont, LV_STATE_DEFAULT);
        lv_obj_add_style(obj, &s_style_focus, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
        lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
        return;
    }

    if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_obj_add_style(obj, &s_style_text, LV_STATE_DEFAULT);
        return;
    }

    if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_add_style(obj, &s_style_cont, LV_STATE_DEFAULT);
        lv_obj_add_style(obj, &s_style_text, LV_STATE_DEFAULT);
        lv_obj_add_style(obj, &s_style_focus,
                         LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY | LV_STATE_PRESSED);
        return;
    }

    if (lv_obj_check_type(obj, &lv_bar_class)) {
        lv_obj_add_style(obj, &s_style_bar_main, LV_PART_MAIN);
        lv_obj_add_style(obj, &s_style_bar_ind, LV_PART_INDICATOR);
        return;
    }
}

static void pm_theme_ensure(lv_display_t * disp)
{
    lv_theme_t * base;

    if (s_pm_theme != NULL) {
        return;
    }

    s_pm_theme = lv_theme_create();
    if (s_pm_theme == NULL) {
        return;
    }

    base = disp != NULL ? lv_display_get_theme(disp) : NULL;
    if (base != NULL) {
        lv_theme_copy(s_pm_theme, base);
        lv_theme_set_parent(s_pm_theme, base);
    }

    lv_theme_set_apply_cb(s_pm_theme, pm_lvgl_apply_cb);
}

/**
 * @brief lv_pm theme lvgl attach page。
 */
void lv_pm_theme_lvgl_attach_page(lv_obj_t * page)
{
    if (page == NULL) {
        return;
    }

    lv_obj_add_style(page, &s_style_page, LV_STATE_DEFAULT);
}

/**
 * @brief lv_pm theme lvgl attach focus。
 */
void lv_pm_theme_lvgl_attach_focus(lv_obj_t * obj)
{
    if (obj == NULL) {
        return;
    }

    lv_obj_add_style(obj, &s_style_focus, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
}

/**
 * @brief lv_pm theme lvgl bind。
 */
void lv_pm_theme_lvgl_bind(const lv_pm_theme_def_t * def)
{
    lv_display_t * disp;

    if (def == NULL) {
        return;
    }

    disp = lv_display_get_default();
    pm_theme_ensure(disp);
    pm_styles_update(def);

    if (s_pm_theme == NULL) {
        return;
    }

    if (disp != NULL) {
        lv_display_set_theme(disp, s_pm_theme);
    }

    lv_obj_report_style_change(NULL);
}
