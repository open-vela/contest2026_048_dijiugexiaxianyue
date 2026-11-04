/**
 * @file bicycle_mtp_ui.h
 * @brief MTP/USB 传输页面 UI 辅助。
 */

#ifndef BICYCLE_MTP_UI_H
#define BICYCLE_MTP_UI_H

#include <myvendor_mtp.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bicycle_mtp_ui {
    bool leaving;
    bool host_attached_sample;
    uint32_t host_attached_stable_since;
    bool need_off_before_enter;
    bool off_sample;
    uint32_t off_stable_since;
    bool storage_reload_pending;
    bool storage_released;
    bool transfer_active_seen;
    myvendor_mtp_plug_t last_plug;
} bicycle_mtp_ui_t;

/**
 * @brief 自行车 mtp ui init。
 * @return 0 成功，负 errno 失败。
 */
void bicycle_mtp_ui_init(bicycle_mtp_ui_t * ui);
/**
 * @brief 自行车 mtp ui run。
 */
void bicycle_mtp_ui_run(bicycle_mtp_ui_t * ui);

/**
 * @brief 当前是否有 MTP「存储占用」在替主界面兜着 cover。
 *
 * @return true = 主界面的 cover 是**正当的**（手机还在、瓦片存储被占用），
 *         此时 map_page 的 stale-cover 兜底不应强行 resume。
 */
bool bicycle_mtp_ui_storage_held(void);

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_MTP_UI_H */
