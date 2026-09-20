/**
 * @file helm_palette.h
 * @brief Helm One 半透半反配色（对齐 00_doc/UI/MCU 线稿）。
 *
 * 线稿 hex 已按 RGB565 量化。半透半反色域窄、暗饱和色会塌成黑/灰，
 * helm_color() 在进 LVGL 前做一次提亮/提纯，让屏上观感靠近 HTML。
 *
 * 户外半透半反：不要整屏 #FFFFFF。SCR 是场地底（反光仍够），PAPER 是
 * 卡片/数字面（暖纸）。SCR 用浅暖灰，半反屏中灰会发脏。
 * 卡片不描黑框。NAV 是橙色强调（速度 / 选中），不是导航蓝。
 */

#ifndef HELM_PALETTE_H
#define HELM_PALETTE_H

#include "lvgl/lvgl.h"

#define HELM_COLOR_SCR       0xE4DCD0
#define HELM_COLOR_PAPER     0xFBF6EE
#define HELM_COLOR_INK       0x000000
#define HELM_COLOR_HAIR      0xD4C8B8
#define HELM_COLOR_NAV       0xFF6E12
#define HELM_COLOR_NAV_FILL  0xFFC890
#define HELM_COLOR_HR        0xC60000
#define HELM_COLOR_HR_FILL   0xFFD7D6
#define HELM_COLOR_CAD       0x007900
#define HELM_COLOR_CAD_FILL  0xCEF3CE
#define HELM_COLOR_OK        0x00B400
#define HELM_COLOR_OK_FILL   0x3CB83C
#define HELM_COLOR_PAUSE_PILL 0xE6B400
#define HELM_COLOR_PWR       0x9C4100
#define HELM_COLOR_PWR_FILL  0xFFE3B5
#define HELM_COLOR_CLIMB     0x086163
#define HELM_COLOR_CLIMB_FILL 0xD6F3F7
/* Spark area: mid-luma / high chroma. HTML --*-fill pastels wash to white
 * on this panel (same class as pause-dock #CEF3CE). Skip helm_color(). */
#define HELM_COLOR_NAV_SPARK   0xE88830
#define HELM_COLOR_HR_SPARK    0xF07878
#define HELM_COLOR_CAD_SPARK   0x58C060
#define HELM_COLOR_PWR_SPARK   0xE09038
#define HELM_COLOR_CLIMB_SPARK 0x52BCC4
/**
 * @name GPX 导航海拔剖面分色
 * @brief 按 |坡度| 三档跳色，已骑过路段灰色；填充与描边成对，不做渐变。
 * @{
 */
#define HELM_COLOR_ELEV_FLAT      0x58C060 /**< 平坦填充（绿）。 */
#define HELM_COLOR_ELEV_FLAT_INK  0x007900 /**< 平坦脊线。 */
#define HELM_COLOR_ELEV_MID       0xE6B400 /**< 中等起伏填充（黄）。 */
#define HELM_COLOR_ELEV_MID_INK   0x9C4100 /**< 中等起伏脊线。 */
#define HELM_COLOR_ELEV_STEEP     0xE05050 /**< 陡变填充（红）。 */
#define HELM_COLOR_ELEV_STEEP_INK 0xC60000 /**< 陡变脊线。 */
#define HELM_COLOR_ELEV_DONE      0xB0A898 /**< 已骑过路段填充（灰）。 */
#define HELM_COLOR_ELEV_DONE_INK  0x6E6860 /**< 已骑过路段脊线。 */
/** @} */
/**
 * @name 时速 / 心率三档跳色
 * @brief 时速 / 心率数字三档跳色：低绿、中亮黄、高红。
 * @details 低档复用 `HELM_COLOR_CAD` / `HELM_COLOR_CAD_SPARK`，
 *          高档复用 `HELM_COLOR_HR` / `HELM_COLOR_HR_SPARK`。
 *          时速切点 18 / 32 km/h，心率切点 120 / 150 bpm。
 *          仅「当前时速」和有效心率跳色；均速 / 极速仍用 `HELM_COLOR_NAV`。
 * @{
 */
#define HELM_COLOR_YEL       0xFFDB00 /**< 中档数字/标签（亮黄）。 */
#define HELM_COLOR_YEL_SPARK 0xE8C430 /**< 中档走势填充。 */
/** @} */
#define HELM_COLOR_WARN      0xFFDB00
#define HELM_COLOR_GPS       0x31D36B
#define HELM_COLOR_GPS_DEAD  0xF75152
#define HELM_COLOR_BATT_MID  0xFFDB00
#define HELM_COLOR_BAR       0x000000
#define HELM_RADIUS          6
#define HELM_INSET           6
#define HELM_GAP             4
#define HELM_LIST_ROW_H      56
#define HELM_STATUS_H        24
#define HELM_PAGE_H          296
#define HELM_SUBBAR_H        22
#define HELM_ROTBAR_H        80
#define HELM_ROW2_H          80
#define HELM_MHEAD_H         32
#define HELM_NAV_BAN_H       58
#define HELM_MAP_BODY_H      (HELM_PAGE_H - HELM_SUBBAR_H - HELM_ROTBAR_H)
#define HELM_HAIR_W          1
/* index.html .dock: left/right/bottom 6; padding 8 10 6; border 2; radius 6.
 * Auto height ≈ head 18 + sumg 8+28+8+28 + keys 8+16 + pad 14 + border 4 = 132. */
#define HELM_DOCK_INSET      6
#define HELM_DOCK_BORDER_W   0
#define HELM_DOCK_H          148
#define HELM_DOCK_HEAD_H     20
#define HELM_DOCK_SUM_MT     8
#define HELM_DOCK_ROW_GAP    8
#define HELM_DOCK_COL_GAP    6
#define HELM_DOCK_CELL_H     32
#define HELM_DOCK_ICO_SUM    22
#define HELM_DOCK_ICO_HEAD   18
#define HELM_DOCK_KEY_MT     8
#define HELM_DOCK_KEY_H      16

static inline uint8_t helm_snap565(uint8_t c, uint8_t bits)
{
    unsigned maxv = (1u << bits) - 1u;
    unsigned q = ((unsigned)c * maxv + 127u) / 255u;

    return (uint8_t)((q * 255u + maxv / 2u) / maxv);
}

static inline lv_color_t helm_color(uint32_t hex)
{
    int r = (int)((hex >> 16) & 0xff);
    int g = (int)((hex >> 8) & 0xff);
    int b = (int)(hex & 0xff);
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int chroma = mx - mn;
    int lum = (r * 3 + g * 6 + b) / 10;

    if (r > 180 && g > 140 && b < 40) {
        /* Yellow #FFDB00 washes to white on the panel — drop luma. */
        r = (r * 82) / 100;
        g = (g * 75) / 100;
    } else if (chroma >= 40 && lum < 150 && mx > 0) {
        /* Dark navy/red/green collapse on transflective — lift same hue. */
        int add = ((150 - lum) * 6) / 10;

        r += (r * add) / mx;
        g += (g * add) / mx;
        b += (b * add) / mx;
    } else if (chroma >= 16 && lum > 165) {
        /* Pale fills wash to white — punch greens harder. */
        int k = (g > r && g > b) ? 4 : 2;

        r += ((r - lum) * k) / 5;
        g += ((g - lum) * k) / 5;
        b += ((b - lum) * k) / 5;
    }

    if (r < 0) {
        r = 0;
    } else if (r > 255) {
        r = 255;
    }

    if (g < 0) {
        g = 0;
    } else if (g > 255) {
        g = 255;
    }

    if (b < 0) {
        b = 0;
    } else if (b > 255) {
        b = 255;
    }

    return lv_color_make(helm_snap565((uint8_t)r, 5),
                         helm_snap565((uint8_t)g, 6),
                         helm_snap565((uint8_t)b, 5));
}

/* RGB565 only — no pale-punch / dark-lift. Spark strokes stay dark on fill. */
static inline lv_color_t helm_color_snap(uint32_t hex)
{
    return lv_color_make(helm_snap565((uint8_t)((hex >> 16) & 0xff), 5),
                         helm_snap565((uint8_t)((hex >> 8) & 0xff), 6),
                         helm_snap565((uint8_t)(hex & 0xff), 5));
}

#endif /* HELM_PALETTE_H */
