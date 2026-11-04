/**
 * @file vmap_style.h
 * @brief vmap style 模块。
 */

#ifndef VMAP_STYLE_H
#define VMAP_STYLE_H

#include "vmap_format.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VMAP_STYLE_ROAD_CLASSES 8

/*
 * 板端只有 z14 瓦片（`docs/map/STYLE.md` §7「z14 单层数据」），放大只是把 z14
 * 几何按 `scale` 线性放大 —— 于是"当前 zoom = 14 + log2(scale)"。可见性判定
 * 按 STYLE.md §3 的"从哪个 zoom 开始画"折成 **scale 阈值**（2^(z-14)）：
 *   道路  z6 / z9 / z11 / z12.5 / z13.5 / z14.5
 *         ⇒ scale 0.004 / 0.031 / 0.125 / 0.354 / 0.707 / 1.414
 *   用地  z12.5 / z15 ⇒ scale 0.354 / 2.0
 * 设备默认 scale = 1.69（VMAP_DEFAULT_SCALE）⇒ service 已出、商务/工业不出。
 */
#define VMAP_STYLE_ZOOM 14u

/** @brief scale × 1000（整数比较用；scale 是 z14 瓦片的线性倍率）。 */
static inline int32_t vmap_style_scale_milli(float scale)
{
    if (!(scale > 0.0f)) {
        return 1000;
    }

    return (int32_t)(scale * 1000.0f + 0.5f);
}

typedef enum {
    VMAP_STYLE_CLASSIC = 0,
    VMAP_STYLE_NAV,
    /** @brief 白天户外盘（terrain）：暖底、绿更重、棕步道（STYLE.md §2 terrain）。 */
    VMAP_STYLE_OUTDOOR,
    /** @brief 夜间盘（night）：深色底 + 深色道路（STYLE.md §2 night）。 */
    VMAP_STYLE_NIGHT,
    VMAP_STYLE_COUNT
} vmap_style_id_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    /** @brief 芯宽，**1/10 设备像素**（z14 1× 基准，STYLE.md §3）。
     *
     * 原来这里是整像素，表达不了 4.2 / 3.6 / 3.0 / 2.6 / 2.2 / 1.7 / 1.4 的
     * 宽度阶梯（整像素会把相邻两级压成同一级）。唯一读取点是
     * `vmap_render.c` 的 `road_core_px()`，用整数换算成设备像素，不引入浮点。
     */
    uint8_t w;
    uint8_t cr;     /**< 壳色 r（STYLE.md §2「路壳」；path 无壳，等于芯色） */
    uint8_t cg;     /**< 壳色 g */
    uint8_t cb;     /**< 壳色 b */
} vmap_style_road_t;

/**
 * @brief 该等级道路在当前缩放下是否绘制（STYLE.md §3 最后一列）。
 * @param cls  道路等级（VMAP_ROAD_*）。
 * @param scale z14 瓦片的线性倍率（`vmap_view_set_scale`）。
 * @return true 绘制。
 */
static inline bool vmap_style_road_visible(uint8_t cls, float scale)
{
    static const uint16_t k_min_scale_milli[VMAP_STYLE_ROAD_CLASSES] = {
        0u, 4u, 31u, 125u, 354u, 707u, 1414u, 707u,
    };

    if (cls == 0u || cls >= VMAP_STYLE_ROAD_CLASSES) {
        return false;
    }

    return vmap_style_scale_milli(scale) >= (int32_t)k_min_scale_milli[cls];
}

/**
 * @brief 该用地类型在当前缩放下是否绘制（STYLE.md §5.4）。
 * @param attr  VMAP_LAND_*。
 * @param scale z14 瓦片的线性倍率。
 * @return true 绘制。
 */
static inline bool vmap_style_land_visible(uint8_t attr, float scale)
{
    switch (attr) {
    case VMAP_LAND_COMMERCIAL:
    case VMAP_LAND_INDUSTRIAL:
        /* z15 起：设备默认 scale 1.69 < 2.0 ⇒ 商务/工业默认不出现。 */
        return vmap_style_scale_milli(scale) >= 2000;
    case VMAP_LAND_RESIDENTIAL:
    case VMAP_LAND_EDU:
    case VMAP_LAND_PARK:
        return vmap_style_scale_milli(scale) >= 354;      /* z12.5 */
    default:
        /* 未知类型保持旧行为（按住宅色画出来），不要静默丢数据。 */
        return true;
    }
}

typedef struct {
    const char * name;

    /* Map canvas (RGB888 → rasterizer converts to RGB565). */
    uint8_t bg_r;
    uint8_t bg_g;
    uint8_t bg_b;
    uint8_t forest_r;
    uint8_t forest_g;
    uint8_t forest_b;
    uint8_t land_res_r;
    uint8_t land_res_g;
    uint8_t land_res_b;
    uint8_t land_com_r;
    uint8_t land_com_g;
    uint8_t land_com_b;
    uint8_t land_edu_r;
    uint8_t land_edu_g;
    uint8_t land_edu_b;
    uint8_t land_park_r;
    uint8_t land_park_g;
    uint8_t land_park_b;
    uint8_t water_r;
    uint8_t water_g;
    uint8_t water_b;
    uint8_t road_case_r;
    uint8_t road_case_g;
    uint8_t road_case_b;
    uint8_t road_case_extra;
    uint8_t waterway_r;
    uint8_t waterway_g;
    uint8_t waterway_b;
    uint8_t waterway_w;
    vmap_style_road_t road[VMAP_STYLE_ROAD_CLASSES];

    /* Track overlay on map canvas, above labels. */
    bool track_speed_gradient;
    uint8_t track_r;
    uint8_t track_g;
    uint8_t track_b;
    uint8_t track_line_w;
    uint8_t km_badge_r;
    uint8_t km_badge_g;
    uint8_t km_badge_b;

    /* Map labels (0xRRGGBB). */
    uint32_t lbl_place;
    uint32_t lbl_water;
    uint32_t lbl_poi;
    uint32_t lbl_road;
    /** @brief 标签描边 halo 色（STYLE.md §2「描边 halo」：日光白、夜间近黑）。 */
    uint32_t lbl_halo;
    /** @brief 描边不透明度（0–255）。 */
    uint8_t  lbl_halo_opa;

    /* LVGL chrome (0xRRGGBB, opa 0–255). */
    uint32_t ui_root_bg;
    uint32_t ui_status_bg;
    uint8_t  ui_status_opa;
    uint32_t ui_title_text;
    uint32_t ui_accent;
    uint32_t ui_rec;
    uint32_t ui_panel_bg;
    uint8_t  ui_panel_opa;
    uint32_t ui_panel_border;
    uint8_t  ui_panel_border_opa;
    uint32_t ui_speed_text;
    uint32_t ui_heading_text;
    uint32_t ui_dist_text;
    uint32_t ui_toast_bg;
    uint8_t  ui_toast_opa;
    uint32_t ui_toast_text;
    bool     ui_navi_arrow_dark;
} vmap_style_t;

#define VMAP_STYLE_RGB565(r, g, b) \
    ((uint16_t)(((((uint16_t)(r)) & 0xF8u) << 8) | \
                ((((uint16_t)(g)) & 0xFCu) << 3) | \
                (((uint16_t)(b)) >> 3)))

/**
 * @brief vmap style rgb565。
 */
uint16_t vmap_style_rgb565(uint8_t r, uint8_t g, uint8_t b);
/**
 * @brief 当前主题背景 RGB565。
 */
uint16_t vmap_style_bg565(void);

/**
 * @brief vmap style count。
 * @return 请求的值。
 */
unsigned vmap_style_count(void);
/**
 * @brief vmap style get id。
 * @return 请求的值。
 */
vmap_style_id_t vmap_style_get_id(void);
/**
 * @brief vmap style current。
 */
const vmap_style_t * vmap_style_current(void);
/**
 * @brief vmap style get。
 * @return 请求的值。
 */
const vmap_style_t * vmap_style_get(vmap_style_id_t id);
/**
 * @brief vmap style name。
 */
const char * vmap_style_name(vmap_style_id_t id);

/**
 * @brief vmap style set id。
 * @return 请求的值。
 */
bool vmap_style_set_id(vmap_style_id_t id);
/**
 * @brief vmap style set。
 */
bool vmap_style_set(vmap_style_id_t id);
/**
 * @brief vmap style set next。
 */
bool vmap_style_set_next(void);
/**
 * @brief vmap style set by name。
 */
bool vmap_style_set_by_name(const char * name);

typedef void (*vmap_style_changed_cb_t)(vmap_style_id_t id, void * user_data);
/**
 * @brief vmap style set changed cb。
 */
void vmap_style_set_changed_cb(vmap_style_changed_cb_t cb, void * user_data);
/** @deprecated 请优先使用 lv_pm_theme_set + lv_pm_set_theme_changed。 */

#ifdef __cplusplus
}
#endif

#endif /* VMAP_STYLE_H */
