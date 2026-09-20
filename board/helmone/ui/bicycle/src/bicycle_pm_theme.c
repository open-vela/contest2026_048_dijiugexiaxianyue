/**
 * @file bicycle_pm_theme.c
 * @brief 自行车 UI — pm_theme。
 */

#include "lv_pm_theme_def.h"

#include "vmap/vmap_style.h"

static vmap_style_id_t pm_theme_to_vmap(lv_pm_theme_id_t id)
{
    /* pm: 0=classic, 1=outdoor — vmap outdoor enum is 2. */
    if (id == 1u) {
        return VMAP_STYLE_OUTDOOR;
    }

    return VMAP_STYLE_CLASSIC;
}

/**
 * @brief 自行车 pm theme on activate。
 */
void bicycle_pm_theme_on_activate(lv_pm_theme_id_t id, void * user_data)
{
    (void)user_data;
    vmap_style_set_id(pm_theme_to_vmap(id));
}
