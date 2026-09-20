/**
 * @file lv_pm_themes.c
 * @brief lv_pm 页面管理 — themes。
 */

#include "lv_pm_themes.h"

const lv_pm_theme_def_t * const lv_pm_builtin_themes[] = {
    &lv_pm_theme_classic,
    &lv_pm_theme_outdoor,
};

const unsigned lv_pm_builtin_theme_count =
    (unsigned)(sizeof(lv_pm_builtin_themes) / sizeof(lv_pm_builtin_themes[0]));
