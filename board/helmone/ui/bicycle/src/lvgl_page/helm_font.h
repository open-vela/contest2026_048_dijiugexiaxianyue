/**
 * @file helm_font.h
 * @brief 菜单/标题：MiSans Medium 4bpp 点阵（缺字回退苹方位图）。
 *        通知/地图长文本：量产可用 helm_font_sys() FreeType；
 *        工厂固件只用 4bpp C 字模（helm_mism4_* + helm_ui_* 回退）。
 *        大号速度数字仍用码表位图。
 */

#ifndef HELM_FONT_H
#define HELM_FONT_H

#include <nuttx/config.h>
#include "lvgl/lvgl.h"
#include "myvendor_system_font.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(helm_ui_12);
LV_FONT_DECLARE(helm_ui_15);
LV_FONT_DECLARE(helm_ui_22);
LV_FONT_DECLARE(helm_mism4_12);
LV_FONT_DECLARE(helm_mism4_15);
LV_FONT_DECLARE(helm_mism4_22);
LV_FONT_DECLARE(helm_ui_64);
LV_FONT_DECLARE(helm_num_28);
LV_FONT_DECLARE(helm_num_32);
LV_FONT_DECLARE(helm_num_56);
LV_FONT_DECLARE(helm_num_64);
LV_FONT_DECLARE(helm_fa_16);

#define HELM_FA_CHECK    "\xEF\x80\x8C"
#define HELM_FA_XMARK    "\xEF\x80\x8D"
#define HELM_FA_CLOCK    "\xEF\x80\x97"
#define HELM_FA_PAUSE    "\xEF\x81\x8C"
#define HELM_FA_CHART    "\xEF\x88\x81"
#define HELM_FA_ROUTE    "\xEF\x93\x97"
#define HELM_FA_MOUNTAIN "\xEF\x9B\xBC"

/**
 * @brief 系统 TTF（PSRAM FreeType）。工厂固件返回 @p fallback。
 * @param px       字号。
 * @param fallback 点阵回退，只给调用方用。
 * @return TTF 或 fallback。
 * @warning **不得**把 `fallback` 写进共享 TTF 缓存槽的 `f->fallback`，
 *          否则所有调用方互相污染。
 */
static inline const lv_font_t * helm_font_sys(int32_t px, const lv_font_t * fallback)
{
#ifdef CONFIG_MYVENDOR_FACTORY_MODE
    LV_UNUSED(px);
    return fallback;
#else
    lv_font_t * f = myvendor_system_font_get(px);

    return f ? f : fallback;
#endif
}

/** @brief 事项/标签用 15px 点阵（splash TTF 未就绪时）。 */
const lv_font_t * helm_font_lab(void);
/** @brief 标题 22px 点阵。 */
const lv_font_t * helm_font_title(void);
/** @brief 数值 22px 点阵。 */
const lv_font_t * helm_font_val(void);

static inline const lv_font_t * helm_font_quad(void)
{
    return &helm_num_32;
}

static inline const lv_font_t * helm_font_rot_speed(void)
{
    return &helm_num_28;
}

static inline const lv_font_t * helm_font_speed_ride(void)
{
    return &helm_num_56;
}

static inline const lv_font_t * helm_font_speed(void)
{
    return &helm_num_64;
}

static inline const lv_font_t * helm_font_mark(void)
{
    return &helm_ui_64;
}

#ifdef __cplusplus
}
#endif

#endif /* HELM_FONT_H */
