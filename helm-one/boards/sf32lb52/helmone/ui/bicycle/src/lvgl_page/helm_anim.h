/**
 * @file helm_anim.h
 * @brief 统一小动效入口 + "屏幕不可见"动画闸门。
 *
 * @details 本板上的动效预算只够**局部对象**（见 docs/ui_epic_performance_audit
 *          与 docs/map/perf）。这个头文件只做两件事：
 *            1. 起动画前先删同一对象、同一 exec 的旧动画，避免衔接时两条并存；
 *            2. 提供 suspend 计数，**熄屏/待机**时让所有 exec 回调早退，
 *               不产生 invalidate（对象上的动画本身不必逐条删除）。
 *               ⚠ 菜单/MTP 等"覆盖层"仍在正常显示和交互，不能挂起：
 *               通用控件（开关旋钮等）也要继续动。
 *
 * @note 所有新增动画的 exec 回调第一行都应检查 `helm_anim_is_suspended()`；
 *       回调返回前不要修改对象状态，否则动画时间仍会推进（只是不重绘）。
 */
#ifndef HELM_ANIM_H
#define HELM_ANIM_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 屏幕不可见（熄屏/待机）时挂起/恢复动效；可嵌套，必须成对调用。 */
void helm_anim_suspend(bool on);

/** @brief 当前是否挂起。exec 回调首行检查。 */
bool helm_anim_is_suspended(void);

/**
 * @brief 起一条对象动画，自动替换同 var + exec 的旧动画。
 * @param path  NULL = lv_anim_path_ease_out（UI 动效默认曲线）。
 * @param done  NULL = 无完成回调。
 */
void helm_anim_run(lv_obj_t *obj, lv_anim_exec_xcb_t exec, int32_t from, int32_t to,
                   uint32_t ms, lv_anim_path_cb_t path,
                   lv_anim_completed_cb_t done);

/** @brief 只停这个对象上指定的 exec 动画（不动其它动画）。 */
void helm_anim_stop(lv_obj_t *obj, lv_anim_exec_xcb_t exec);

#ifdef __cplusplus
}
#endif

#endif /* HELM_ANIM_H */
