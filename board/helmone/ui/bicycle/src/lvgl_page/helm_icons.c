/**
 * @file helm_icons.c
 * @brief 图标字体的 UTF-8/名称映射与小工具。
 */

#include "helm_icons.h"

typedef struct {
    uint32_t cp;       /**< 重映射后的私有区码点 */
    const char *name;  /**< 中文名 */
    const char *sym;   /**< Material Symbols 源名 */
} icon_meta_t;

/* 顺序必须与 helm_icons.h 的 helm_icon_t 一致 */
static const icon_meta_t s_meta[HELM_ICON_COUNT] = {
    { 0xE001, "骑行",   "directions_bike" },
    { 0xE002, "导航",   "navigation"      },
    { 0xE003, "记录",   "bar_chart"       },
    { 0xE004, "设置",   "settings"        },
    { 0xE005, "GPS",    "gps_fixed"       },
    { 0xE006, "电量",   "battery_horiz_075" },
    { 0xE007, "时间",   "schedule"        },
    { 0xE008, "显示",   "brightness_6"    },
    { 0xE009, "传感器", "sensors"         },
    { 0xE00A, "声音",   "volume_up"       },
    { 0xE00B, "信息",   "info"            },
    { 0xE00C, "右箭头", "chevron_right"   },
    { 0xE00D, "返回",   "arrow_back"      },
    { 0xE00E, "心率",   "monitor_heart"   },
    { 0xE00F, "速度",   "speed"           },
    { 0xE010, "海拔",   "terrain"         },
    { 0xE011, "蓝牙",   "bluetooth"       },
    { 0xE012, "心率",   "favorite"        },
    { 0xE013, "功率",   "bolt"            },
    { 0xE014, "踏频",   "autorenew"       },
    { 0xE015, "电量空", "battery_horiz_000" },
    { 0xE016, "电量半", "battery_horiz_050" },
};

static char s_utf8[4];

const char *helm_icon_text(helm_icon_t id)
{
    uint32_t cp;

    if (id < 0 || id >= HELM_ICON_COUNT) {
        id = HELM_ICON_INFO;
    }
    cp = s_meta[id].cp;
    /* PUA 0xE000..0xF8FF 都是 3 字节 UTF-8 */
    s_utf8[0] = (char)(0xE0u | (cp >> 12));
    s_utf8[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    s_utf8[2] = (char)(0x80u | (cp & 0x3Fu));
    s_utf8[3] = '\0';
    return s_utf8;
}

uint32_t helm_icon_codepoint(helm_icon_t id)
{
    if (id < 0 || id >= HELM_ICON_COUNT) {
        id = HELM_ICON_INFO;
    }
    return s_meta[id].cp;
}

const char *helm_icon_name(helm_icon_t id)
{
    if (id < 0 || id >= HELM_ICON_COUNT) {
        id = HELM_ICON_INFO;
    }
    return s_meta[id].name;
}

const char *helm_icon_symbol(helm_icon_t id)
{
    if (id < 0 || id >= HELM_ICON_COUNT) {
        id = HELM_ICON_INFO;
    }
    return s_meta[id].sym;
}

lv_obj_t *hfi_create_px(lv_obj_t *parent, helm_icon_t id, int px, int cx,
                        int cy, uint32_t hex)
{
    lv_obj_t *l = lv_label_create(parent);
    const lv_font_t *font;
    int box;

    if (px <= 14) {
        font = &helm_icons_14;
        box = 14;
    } else if (px >= 26) {
        font = &helm_icons_26;
        box = 26;
    } else if (px >= 16) {
        /* 状态栏 16px（用户：图标清晰但 14px 偏小）；15..25 都落这一档。 */
        font = &helm_icons_16;
        box = 16;
    } else {
        font = &helm_icons_20;
        box = 20;
    }

    lv_label_set_text(l, helm_icon_text(id));
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, box);
    lv_obj_set_height(l, box);
    lv_obj_set_pos(l, cx - box / 2, cy - box / 2);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    return l;
}

lv_obj_t *hfi_create(lv_obj_t *parent, helm_icon_t id, int cx, int cy,
                           uint32_t hex)
{
    return hfi_create_px(parent, id, 20, cx, cy, hex);
}

void hfi_set(lv_obj_t *icon, helm_icon_t id)
{
    if (icon != NULL) {
        lv_label_set_text(icon, helm_icon_text(id));
    }
}
