/**
 * @file bicycle_pm_theme.c
 * @brief 自行车 UI — pm_theme。
 */

#include "lv_pm_theme_def.h"

#include "vmap/vmap_style.h"

static vmap_style_id_t pm_theme_to_vmap(lv_pm_theme_id_t id)
{
    /* pm 主题三档（lv_pm_theme_def.h）：classic=0 / outdoor=1 / night=2。
     * 地图盘：classic→paper、outdoor→terrain（白天户外）、night→night（夜间）。
     * 之前 outdoor 和 night 都映射到 VMAP_STYLE_OUTDOOR（那一份是夜间盘），
     * 两个主题在地图区只差 2px；2026-10-29 拆出 VMAP_STYLE_NIGHT 后各归各的。
     * 注意：vmap 的枚举值（OUTDOOR=2 / NIGHT=3）跟 pm 的 id 不是一回事。 */
    if (id == 1u) {
        return VMAP_STYLE_OUTDOOR;
    }
    if (id == 2u) {
        return VMAP_STYLE_NIGHT;
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
