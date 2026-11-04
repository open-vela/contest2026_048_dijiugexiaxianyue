/**
 * @file helm_icons.h
 * @brief Helm 图标字体（Material Symbols Rounded，单色）的 C 侧入口。
 *
 * 字形来自 `helm_icons_14/20/26.c`（由 tools/gen_helm_icons.sh 生成），码点重映射到
 * 私有区 0xE001..0xE016，枚举顺序与生成脚本一一对应。
 *
 * 设计约束（见 evidence/WP-HELM/R1/ICONS.md）：
 *   - 统一 20px 字面、24×24 设计网格、单色（颜色由 text color 决定）；
 *   - 所有图标同一字体、同一基线，线宽/端点/光学尺寸由字体保证一致；
 *   - 图标只做形状，不承载正文；纸色符号只放在深色块上。
 */

#ifndef HELM_ICONS_H
#define HELM_ICONS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(helm_icons_14);
LV_FONT_DECLARE(helm_icons_16);
LV_FONT_DECLARE(helm_icons_20);
LV_FONT_DECLARE(helm_icons_26);

typedef enum {
    HELM_ICON_BIKE = 0,   /**< 骑行   directions_bike  U+E001 */
    HELM_ICON_NAV,        /**< 导航   navigation       U+E002 */
    HELM_ICON_RECORD,     /**< 记录   bar_chart        U+E003 */
    HELM_ICON_SETTINGS,   /**< 设置   settings         U+E004 */
    HELM_ICON_GPS,        /**< GPS    gps_fixed        U+E005 */
    HELM_ICON_BATTERY,    /**< 电量   battery_horiz_075 U+E006 */
    HELM_ICON_CLOCK,      /**< 时间   schedule         U+E007 */
    HELM_ICON_DISPLAY,    /**< 显示   brightness_6     U+E008 */
    HELM_ICON_SENSOR,     /**< 传感器 sensors          U+E009 */
    HELM_ICON_SOUND,      /**< 声音   volume_up        U+E00A */
    HELM_ICON_INFO,       /**< 信息   info             U+E00B */
    HELM_ICON_CHEVRON,    /**< 右箭头 chevron_right    U+E00C */
    HELM_ICON_BACK,       /**< 返回   arrow_back       U+E00D */
    HELM_ICON_HEART,      /**< 心率   monitor_heart    U+E00E */
    HELM_ICON_SPEED,      /**< 速度   speed            U+E00F */
    HELM_ICON_TERRAIN,    /**< 海拔   terrain          U+E010 */
    HELM_ICON_BLE,        /**< 蓝牙   bluetooth        U+E011 */
    HELM_ICON_HR,         /**< 心率   favorite         U+E012 */
    HELM_ICON_POWER,      /**< 功率   bolt             U+E013 */
    HELM_ICON_CADENCE,    /**< 踏频   autorenew        U+E014 */
    HELM_ICON_BATT_EMPTY, /**< 电量0  battery_horiz_000 U+E015 */
    HELM_ICON_BATT_HALF,  /**< 电量5  battery_horiz_050 U+E016 */
    HELM_ICON_COUNT
} helm_icon_t;

/** 图标字形的 UTF-8 文本（静态缓冲，set_text 会拷贝）。 */
const char *helm_icon_text(helm_icon_t id);

/** 图标码点（ICONS.md / 图标表用）。 */
uint32_t helm_icon_codepoint(helm_icon_t id);

/** 中文名（图标表 / 图标全家福用）。 */
const char *helm_icon_name(helm_icon_t id);

/** 英文源名（Material Symbols 名称，ICONS.md 用）。 */
const char *helm_icon_symbol(helm_icon_t id);

/**
 * @brief 在 (cx, cy) 画一个 20×20 图标（居中），返回 label。
 *
 * label 固定 20×20、文本居中；同一字体同一基线 ⇒ 视觉重心一致。
 * 颜色改 `lv_obj_set_style_text_color()` 即可（单色字形）。
 */
lv_obj_t *hfi_create(lv_obj_t *parent, helm_icon_t id, int cx, int cy,
                           uint32_t hex);

/**
 * @brief 指定字面尺寸的图标（14 / 20 / 26px）。
 *
 * px <= 14 用 `helm_icons_14.c`（状态栏原生 14px，清晰、无缩放），
 * px >= 26 用 `helm_icons_26.c`（“图标与 30px 文字字面等高”的对比方案），
 * 其余用默认 20px。三份位图都来自同一源字体。
 */
lv_obj_t *hfi_create_px(lv_obj_t *parent, helm_icon_t id, int px, int cx,
                        int cy, uint32_t hex);

/** 换字形（不重建对象，只 set_text）。 */
void hfi_set(lv_obj_t *icon, helm_icon_t id);

#ifdef __cplusplus
}
#endif

#endif /* HELM_ICONS_H */
