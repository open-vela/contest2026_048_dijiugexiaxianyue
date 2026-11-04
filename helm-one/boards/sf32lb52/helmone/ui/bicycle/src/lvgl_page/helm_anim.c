/**
 * @file helm_anim.c
 * @brief 统一小动效入口 + 动画挂起闸门。
 */

#include "helm_anim.h"

#include <stddef.h>

/** @brief 挂起深度：待机与菜单/MTP 覆盖可以嵌套，避免一方恢复时把另一方解除。 */
static int s_suspend_depth;

void helm_anim_suspend(bool on)
{
    if (on) {
        if (s_suspend_depth < 1000) {
            s_suspend_depth++;
        }
    } else if (s_suspend_depth > 0) {
        s_suspend_depth--;
    }
}

bool helm_anim_is_suspended(void)
{
    return s_suspend_depth > 0;
}

void helm_anim_run(lv_obj_t *obj, lv_anim_exec_xcb_t exec, int32_t from, int32_t to,
                   uint32_t ms, lv_anim_path_cb_t path,
                   lv_anim_completed_cb_t done)
{
    lv_anim_t a;

    if (obj == NULL || exec == NULL) {
        return;
    }

    /* LVGL 只在 start 时清理同 var+exec 的动画；带 delay 的两条会短暂并存。
     * UI 动效都是"从当前值接着走"，所以这里显式先删。 */
    lv_anim_delete(obj, exec);

    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, (path != NULL) ? path : lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, exec);
    if (done != NULL) {
        lv_anim_set_completed_cb(&a, done);
    }
    lv_anim_start(&a);
}

void helm_anim_stop(lv_obj_t *obj, lv_anim_exec_xcb_t exec)
{
    if (obj != NULL) {
        lv_anim_delete(obj, exec);
    }
}
