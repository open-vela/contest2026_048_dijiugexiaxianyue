/**
 * @file helm_font.c
 * @brief 菜单/标题用 MiSans 4bpp 点阵。缺字回退苹方位图，避免矢量字。
 */

#include "helm_font.h"

#include <stdbool.h>

static lv_font_t s_lab;
static lv_font_t s_title;
static lv_font_t s_val;
static bool s_ready;

static void helm_font_init(void)
{
    if (s_ready) {
        return;
    }

    s_lab = helm_mism4_12;
    s_lab.fallback = &helm_ui_12;
    s_title = helm_mism4_15;
    s_title.fallback = &helm_ui_15;
    s_val = helm_mism4_22;
    s_val.fallback = &helm_ui_22;
    s_ready = true;
}

const lv_font_t * helm_font_lab(void)
{
    helm_font_init();
    return &s_lab;
}

const lv_font_t * helm_font_title(void)
{
    helm_font_init();
    return &s_title;
}

const lv_font_t * helm_font_val(void)
{
    helm_font_init();
    return &s_val;
}
