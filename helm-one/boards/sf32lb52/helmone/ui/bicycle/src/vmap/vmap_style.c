/**
 * @file vmap_style.c
 * @brief vmap style 模块。
 */

#include "vmap_style.h"

#include "vmap_format.h"
#include <string.h>

#define RS(r, g, b, w) { (uint8_t)(r), (uint8_t)(g), (uint8_t)(b), (uint8_t)(w), 0, 0, 0 }
#define RSC(r, g, b, w, cr, cg, cb) \
    { (uint8_t)(r), (uint8_t)(g), (uint8_t)(b), (uint8_t)(w), \
      (uint8_t)(cr), (uint8_t)(cg), (uint8_t)(cb) }

/*
 * 调色板口径：`docs/map/STYLE.md` §2（唯一权威）。四份设备盘对应：
 *   classic ← paper（纸面，默认）   nav ← bright（冷白）
 *   outdoor ← terrain（白天户外）   night ← night（夜间）
 *
 * 道路一律"壳 + 芯"：芯色 r/g/b，壳色 cr/cg/cb；w 是 1/10 px 的芯宽（§3）。
 * 等级靠宽度阶梯分层，不靠七种颜色 —— 芯色只有"琥珀（主干）→ 近白（支路）"两族。
 */
static const vmap_style_t g_vmap_styles[VMAP_STYLE_COUNT] = {
    [VMAP_STYLE_CLASSIC] = {
        .name = "classic",
        /* paper：暖白纸面 #F4F1E8 */
        .bg_r = 244, .bg_g = 241, .bg_b = 232,
        .forest_r = 203, .forest_g = 224, .forest_b = 188,       /* #CBE0BC */
        .land_res_r = 239, .land_res_g = 234, .land_res_b = 222, /* #EFEADE land1 */
        /* land2/3 在结构里共用一个槽（下面 §2 的 land2/land3 色差 1~2 级）；
         * 设备 z14 两者都不画（§5.4），取 land3 #EFECE4。 */
        .land_com_r = 239, .land_com_g = 236, .land_com_b = 228,
        .land_edu_r = 249, .land_edu_g = 242, .land_edu_b = 218, /* #F9F2DA land4 */
        .land_park_r = 216, .land_park_g = 232, .land_park_b = 202, /* #D8E8CA land5 */
        .water_r = 169, .water_g = 208, .water_b = 232,          /* #A9D0E8 */
        /* 遗留的全局壳色：新路径按 class 取 cr/cg/cb，这里只做兜底。 */
        .road_case_r = 202, .road_case_g = 196, .road_case_b = 182,
        .road_case_extra = 1,
        .waterway_r = 147, .waterway_g = 194, .waterway_b = 218, /* #93C2DA */
        .waterway_w = 2,
        .road = {
            [0] = RS(0, 0, 0, 0),
            /* 芯 / 壳 / 芯宽（1/10 px，§3） */
            [VMAP_ROAD_MOTORWAY]   = RSC(247, 200, 115, 42, 206, 143,  51), /* #F7C873 / #CE8F33 */
            [VMAP_ROAD_PRIMARY]    = RSC(251, 220, 156, 36, 198, 160,  84), /* #FBDC9C / #C6A054 */
            [VMAP_ROAD_SECONDARY]  = RSC(254, 253, 249, 30, 183, 176, 161), /* #FEFDF9 / #B7B0A1 */
            /* 支路壳：很浅（比底色深 10~20 级），1× 下能看出边界，放大也不会
             * 变成"米色带白线"（WP-FIX3 ② 的判据；起点 #E4DFD2）。 */
            [VMAP_ROAD_TERTIARY]   = RSC(254, 253, 249, 26, 211, 203, 184), /* #FEFDF9 / #D3CBB8 */
            [VMAP_ROAD_RESIDENTIAL]= RSC(254, 253, 249, 22, 217, 210, 192), /* #FEFDF9 / #D9D2C0 */
            [VMAP_ROAD_SERVICE]    = RSC(254, 253, 249, 17, 226, 220, 207), /* #FEFDF9 / #E2DCCF */
            [VMAP_ROAD_PATH]       = RSC(169, 124,  74, 14, 169, 124,  74), /* #A97C4A，无壳 */
        },
        .track_speed_gradient = true,
        .track_r = 0, .track_g = 168, .track_b = 220,
        .track_line_w = 4,
        .km_badge_r = 20, .km_badge_g = 20, .km_badge_b = 20,
        .lbl_place = 0x2E2C29,
        .lbl_water = 0x3D7EA6,
        .lbl_poi   = 0x3F7048,
        .lbl_road  = 0x514C45,
        .lbl_halo  = 0xFFFFFF,
        .lbl_halo_opa = 235,                                     /* 92% */
        .ui_root_bg = 0xE4DCD0,
        .ui_status_bg = 0xF5F5F5,
        .ui_status_opa = 255,
        .ui_title_text = 0x101010,
        .ui_accent = 0xFF931E,
        .ui_rec = 0xFF4040,
        .ui_panel_bg = 0xF0F0F0,
        .ui_panel_opa = 255,
        .ui_panel_border = 0xFF931E,
        .ui_panel_border_opa = 128,
        .ui_speed_text = 0xFF931E,
        .ui_heading_text = 0x101010,
        .ui_dist_text = 0x666666,
        .ui_toast_bg = 0xEEEEEE,
        .ui_toast_opa = 255,
        .ui_toast_text = 0x101010,
        .ui_navi_arrow_dark = true,
    },
    [VMAP_STYLE_NAV] = {
        .name = "nav",
        /* bright：冷白 #F6F7F9 */
        .bg_r = 246, .bg_g = 247, .bg_b = 249,
        .forest_r = 211, .forest_g = 231, .forest_b = 196,       /* #D3E7C4 */
        .land_res_r = 238, .land_res_g = 241, .land_res_b = 244, /* #EEF1F4 land1 */
        .land_com_r = 238, .land_com_g = 241, .land_com_b = 243, /* #EEF1F3 land3 */
        .land_edu_r = 251, .land_edu_g = 243, .land_edu_b = 222, /* #FBF3DE land4 */
        .land_park_r = 223, .land_park_g = 237, .land_park_b = 214, /* #DFEDD6 land5 */
        .water_r = 169, .water_g = 212, .water_b = 240,          /* #A9D4F0 */
        .road_case_r = 207, .road_case_g = 213, .road_case_b = 222,
        .road_case_extra = 1,
        .waterway_r = 143, .waterway_g = 196, .waterway_b = 228, /* #8FC4E4 */
        .waterway_w = 2,
        .road = {
            [0] = RS(0, 0, 0, 0),
            [VMAP_ROAD_MOTORWAY]   = RSC(248, 197,  86, 42, 208, 148,  37), /* #F8C556 / #D09425 */
            [VMAP_ROAD_PRIMARY]    = RSC(251, 217, 138, 36, 196, 160,  80), /* #FBD98A / #C4A050 */
            [VMAP_ROAD_SECONDARY]  = RSC(255, 255, 255, 30, 188, 196, 207), /* #FFFFFF / #BCC4CF */
            [VMAP_ROAD_TERTIARY]   = RSC(255, 255, 255, 26, 198, 205, 215), /* #FFFFFF / #C6CDD7 */
            [VMAP_ROAD_RESIDENTIAL]= RSC(255, 255, 255, 22, 207, 213, 222), /* #FFFFFF / #CFD5DE */
            [VMAP_ROAD_SERVICE]    = RSC(255, 255, 255, 17, 219, 224, 230), /* #FFFFFF / #DBE0E6 */
            [VMAP_ROAD_PATH]       = RSC(143, 160, 174, 14, 143, 160, 174), /* #8FA0AE，无壳 */
        },
        .track_speed_gradient = false,
        .track_r = 220, .track_g = 40, .track_b = 40,
        .track_line_w = 4,
        .km_badge_r = 24, .km_badge_g = 24, .km_badge_b = 28,
        .lbl_place = 0x1F2933,
        .lbl_water = 0x2F7BA8,
        .lbl_poi   = 0x3C7048,
        .lbl_road  = 0x4A5560,
        .lbl_halo  = 0xFFFFFF,
        .lbl_halo_opa = 240,                                     /* 94% */
        .ui_root_bg = 0x101014,
        .ui_status_bg = 0x222228,
        .ui_status_opa = 255,
        .ui_title_text = 0xFFFFFF,
        .ui_accent = 0xFF931E,
        .ui_rec = 0xFF4040,
        .ui_panel_bg = 0x101010,
        .ui_panel_opa = 255,
        .ui_panel_border = 0x404048,
        .ui_panel_border_opa = 255,
        .ui_speed_text = 0xFFFFFF,
        .ui_heading_text = 0xCCCCCC,
        .ui_dist_text = 0xFFFFFF,
        .ui_toast_bg = 0x101010,
        .ui_toast_opa = 255,
        .ui_toast_text = 0xFFFFFF,
        .ui_navi_arrow_dark = false,
    },
    [VMAP_STYLE_OUTDOOR] = {
        .name = "outdoor",
        /*
         * terrain：白天户外盘（STYLE.md §2 的 terrain 列）。
         * 暖底 + 更重的绿 + 棕步道：和 classic 的纸面区分开，又不像 night 那样暗。
         * 之前 outdoor 和 night 共用一份夜间盘，两个主题在地图区只差 2px ——
         * 这里把 outdoor 换回真正的白天户外配色，night 用下面的夜间盘。
         */
        .bg_r = 243, .bg_g = 240, .bg_b = 226,                  /* #F3F0E2 */
        .forest_r = 194, .forest_g = 220, .forest_b = 174,       /* #C2DCAE */
        .land_res_r = 237, .land_res_g = 232, .land_res_b = 214, /* #EDE8D6 land1 */
        .land_com_r = 233, .land_com_g = 231, .land_com_b = 218, /* #E9E7DA land3 */
        .land_edu_r = 247, .land_edu_g = 239, .land_edu_b = 208, /* #F7EFD0 land4 */
        .land_park_r = 210, .land_park_g = 229, .land_park_b = 190, /* #D2E5BE land5 */
        .water_r = 166, .water_g = 207, .water_b = 228,          /* #A6CFE4 */
        .road_case_r = 202, .road_case_g = 196, .road_case_b = 182,
        .road_case_extra = 1,
        .waterway_r = 136, .waterway_g = 187, .waterway_b = 212, /* #88BBD4 */
        .waterway_w = 2,
        .road = {
            [0] = RS(0, 0, 0, 0),
            [VMAP_ROAD_MOTORWAY]   = RSC(242, 192, 112, 42, 192, 142,  56), /* #F2C070 / #C08E38 */
            [VMAP_ROAD_PRIMARY]    = RSC(247, 220, 156, 36, 192, 164,  92), /* #F7DC9C / #C0A45C */
            [VMAP_ROAD_SECONDARY]  = RSC(253, 251, 244, 30, 181, 175, 156), /* #FDFBF4 / #B5AF9C */
            [VMAP_ROAD_TERTIARY]   = RSC(253, 251, 244, 26, 220, 214, 196), /* #FDFBF4 / #DCD6C4 */
            [VMAP_ROAD_RESIDENTIAL]= RSC(253, 251, 244, 22, 224, 219, 203), /* #FDFBF4 / #E0DBCB */
            [VMAP_ROAD_SERVICE]    = RSC(253, 251, 244, 17, 229, 225, 211), /* #FDFBF4 / #E5E1D3 */
            [VMAP_ROAD_PATH]       = RSC(158, 110,  66, 14, 158, 110,  66), /* #9E6E42，无壳 */
        },
        .track_speed_gradient = true,
        .track_r = 0, .track_g = 168, .track_b = 220,
        .track_line_w = 4,
        .km_badge_r = 20, .km_badge_g = 20, .km_badge_b = 20,
        .lbl_place = 0x2B2A24,
        .lbl_water = 0x33708F,
        .lbl_poi   = 0x3A6B3C,
        .lbl_road  = 0x4E493F,
        .lbl_halo  = 0xFFFFFF,
        .lbl_halo_opa = 230,                                     /* 90% */
        .ui_root_bg = 0xE4DCD0,
        .ui_status_bg = 0xF5F5F5,
        .ui_status_opa = 255,
        .ui_title_text = 0x101010,
        .ui_accent = 0xFF931E,
        .ui_rec = 0xFF4040,
        .ui_panel_bg = 0xF0F0F0,
        .ui_panel_opa = 255,
        .ui_panel_border = 0xFF931E,
        .ui_panel_border_opa = 128,
        .ui_speed_text = 0xFF931E,
        .ui_heading_text = 0x101010,
        .ui_dist_text = 0x666666,
        .ui_toast_bg = 0xEEEEEE,
        .ui_toast_opa = 255,
        .ui_toast_text = 0x101010,
        .ui_navi_arrow_dark = true,
    },
    [VMAP_STYLE_NIGHT] = {
        .name = "night",
        /*
         * night：夜间盘（STYLE.md §2 的 night 列）。原来的 OUTDOOR 就是这一份；
         * 2026-10-29 拆出独立枚举后，`bicycle_pm_theme.c` 把 pm 主题 night 映射到这里。
         */
        .bg_r = 20, .bg_g = 22, .bg_b = 26,                      /* #14161A */
        .forest_r = 22, .forest_g = 36, .forest_b = 26,          /* #16241A */
        .land_res_r = 26, .land_res_g = 29, .land_res_b = 34,    /* #1A1D22 land1 */
        .land_com_r = 25, .land_com_g = 28, .land_com_b = 32,    /* #191C20 land3 */
        .land_edu_r = 35, .land_edu_g = 32, .land_edu_b = 25,    /* #232019 land4 */
        .land_park_r = 23, .land_park_g = 37, .land_park_b = 28, /* #17251C land5 */
        .water_r = 22, .water_g = 48, .water_b = 63,             /* #16303F */
        .road_case_r = 33, .road_case_g = 37, .road_case_b = 42,
        .road_case_extra = 0,
        .waterway_r = 42, .waterway_g = 85, .waterway_b = 108,   /* #2A556C */
        .waterway_w = 2,
        .road = {
            [0] = RS(0, 0, 0, 0),
            [VMAP_ROAD_MOTORWAY]   = RSC(192, 138,  58, 42, 110,  78,  30), /* #C08A3A / #6E4E1E */
            [VMAP_ROAD_PRIMARY]    = RSC(156, 123,  60, 36,  94,  74,  34), /* #9C7B3C / #5E4A22 */
            [VMAP_ROAD_SECONDARY]  = RSC( 74,  80,  88, 30,  38,  42,  48), /* #4A5058 / #262A30 */
            [VMAP_ROAD_TERTIARY]   = RSC( 67,  73,  80, 26,  36,  40,  45), /* #434950 / #24282D */
            [VMAP_ROAD_RESIDENTIAL]= RSC( 60,  66,  73, 22,  33,  37,  42), /* #3C4249 / #21252A */
            [VMAP_ROAD_SERVICE]    = RSC( 52,  58,  65, 17,  30,  34,  39), /* #343A41 / #1E2227 */
            [VMAP_ROAD_PATH]       = RSC( 90, 107,  82, 14,  90, 107,  82), /* #5A6B52，无壳 */
        },
        /*
         * 夜间轨迹线用**红色**（与 `nav` 那份同色），不用测速渐变：用户 2026-09-27 实测
         * "夜间导航轨迹线和路线一样了、看不清" —— 渐变低段是青蓝，正好和蓝色的路线撞色。
         * 路线仍是蓝色（`vmap_route.c` 的 route rgb565），轨迹红/路线蓝＝与日光导航一致的
         * 分工。要恢复测速色就把 track_speed_gradient 打开（并解决撞色）。
         */
        .track_speed_gradient = false,
        .track_r = 220, .track_g = 40, .track_b = 40,
        .track_line_w = 4,
        .km_badge_r = 255, .km_badge_g = 255, .km_badge_b = 255,
        .lbl_place = 0xE6EAF0,
        .lbl_water = 0x7FB6D8,
        .lbl_poi   = 0x8FC79A,
        .lbl_road  = 0xC3C9D2,
        .lbl_halo  = 0x0C0E12,
        .lbl_halo_opa = 230,                                     /* 90% */
        .ui_root_bg = 0x08080A,
        .ui_status_bg = 0x101014,
        .ui_status_opa = 255,
        .ui_title_text = 0xFFFFFF,
        .ui_accent = 0xFFCC00,
        .ui_rec = 0xFF5050,
        .ui_panel_bg = 0x101014,
        .ui_panel_opa = 255,
        .ui_panel_border = 0xFFCC00,
        .ui_panel_border_opa = 200,
        .ui_speed_text = 0xFFCC00,
        .ui_heading_text = 0xFFFFFF,
        .ui_dist_text = 0xFFFFFF,
        .ui_toast_bg = 0x101014,
        .ui_toast_opa = 255,
        .ui_toast_text = 0xFFFFFF,
        .ui_navi_arrow_dark = false,
    },
};

static vmap_style_id_t g_vmap_style_id = VMAP_STYLE_CLASSIC;
static vmap_style_changed_cb_t g_vmap_style_cb;
static void * g_vmap_style_cb_user;

/* =====================================================================
 * 日间盘"压深"（2026-11-01 用户："vmap 的日间配色颜色可以加深一些些，
 * 现在在屏幕上显示有些淡"）。
 *
 * 上面那张表是**设计稿色**（docs/map/STYLE.md §2 的权威值，不在这里改）；
 * 板端半透半反屏会把浅色洗淡，所以在**取用**时套一个统一的压深：
 *   - 底色/林地/用地/水/路壳/水道：压 10%；
 *   - 道路芯色（近白）：只压 6% —— 压得和底色一样多会把"路比地亮"的层级
 *     一起压平，反而更难看出路网。
 * 只动画布色：标签、轨迹、里程角标、UI 色都不碰（那些是"要读的字"）。
 * 夜间盘（VMAP_STYLE_NIGHT）不压。
 * ===================================================================== */
#define VMAP_DAY_DEEPEN_PCT       10u
#define VMAP_DAY_CORE_DEEPEN_PCT   6u
/** 半透半反屏同时会**去饱和**，所以压深时顺手把色度提一点（%）。 */
#define VMAP_DAY_SAT_PCT         108u

static uint8_t vmap_day_clamp(int v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return (uint8_t)v;
}

/** @brief 压深 + 轻微提色度（先按比例压，再以自身亮度为中心放大色差）。 */
static void vmap_day_tone(uint8_t * r, uint8_t * g, uint8_t * b, unsigned pct)
{
    int rr = ((int)*r * (int)(100u - pct)) / 100;
    int gg = ((int)*g * (int)(100u - pct)) / 100;
    int bb = ((int)*b * (int)(100u - pct)) / 100;
    int lum = (rr * 3 + gg * 6 + bb) / 10;

    rr = lum + (rr - lum) * (int)VMAP_DAY_SAT_PCT / 100;
    gg = lum + (gg - lum) * (int)VMAP_DAY_SAT_PCT / 100;
    bb = lum + (bb - lum) * (int)VMAP_DAY_SAT_PCT / 100;
    *r = vmap_day_clamp(rr);
    *g = vmap_day_clamp(gg);
    *b = vmap_day_clamp(bb);
}

static void vmap_style_deepen(vmap_style_t * s)
{
    unsigned i;

#define VMAP_TONE(r, g, b, pct) (vmap_day_tone(&s->r, &s->g, &s->b, pct))
    VMAP_TONE(bg_r, bg_g, bg_b, VMAP_DAY_DEEPEN_PCT);
    VMAP_TONE(forest_r, forest_g, forest_b, VMAP_DAY_DEEPEN_PCT);
    VMAP_TONE(land_res_r, land_res_g, land_res_b, VMAP_DAY_DEEPEN_PCT);
    VMAP_TONE(land_com_r, land_com_g, land_com_b, VMAP_DAY_DEEPEN_PCT);
    VMAP_TONE(land_edu_r, land_edu_g, land_edu_b, VMAP_DAY_DEEPEN_PCT);
    VMAP_TONE(land_park_r, land_park_g, land_park_b, VMAP_DAY_DEEPEN_PCT);
    VMAP_TONE(water_r, water_g, water_b, VMAP_DAY_DEEPEN_PCT);
    VMAP_TONE(road_case_r, road_case_g, road_case_b, VMAP_DAY_DEEPEN_PCT);
    VMAP_TONE(waterway_r, waterway_g, waterway_b, VMAP_DAY_DEEPEN_PCT);
#undef VMAP_TONE

    for (i = 1; i < (unsigned)VMAP_STYLE_ROAD_CLASSES; i++) {
        vmap_day_tone(&s->road[i].r, &s->road[i].g, &s->road[i].b,
                      VMAP_DAY_CORE_DEEPEN_PCT);
        vmap_day_tone(&s->road[i].cr, &s->road[i].cg, &s->road[i].cb,
                      VMAP_DAY_DEEPEN_PCT);
    }
}

/** @brief 压深后的有效表（首次取用时从设计表生成一次；夜间盘原样拷贝）。 */
static vmap_style_t s_style_eff[VMAP_STYLE_COUNT];
static bool s_style_eff_ready;

static const vmap_style_t * vmap_style_eff(vmap_style_id_t id)
{
    if (!s_style_eff_ready) {
        unsigned i;

        for (i = 0; i < (unsigned)VMAP_STYLE_COUNT; i++) {
            s_style_eff[i] = g_vmap_styles[i];
            if (i != (unsigned)VMAP_STYLE_NIGHT) {
                vmap_style_deepen(&s_style_eff[i]);
            }
        }
        s_style_eff_ready = true;
    }

    if ((unsigned)id >= (unsigned)VMAP_STYLE_COUNT) {
        id = VMAP_STYLE_CLASSIC;
    }
    return &s_style_eff[id];
}

/**
 * @brief vmap style rgb565。
 */
uint16_t vmap_style_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return VMAP_STYLE_RGB565(r, g, b);
}

/**
 * @brief 当前主题背景 RGB565。
 */
uint16_t vmap_style_bg565(void)
{
    const vmap_style_t * s = vmap_style_current();

    return vmap_style_rgb565(s->bg_r, s->bg_g, s->bg_b);
}

/**
 * @brief vmap style count。
 * @return 请求的值。
 */
unsigned vmap_style_count(void)
{
    return (unsigned)VMAP_STYLE_COUNT;
}

/**
 * @brief vmap style get id。
 * @return 请求的值。
 */
vmap_style_id_t vmap_style_get_id(void)
{
    return g_vmap_style_id;
}

/**
 * @brief vmap style current。
 */
const vmap_style_t * vmap_style_current(void)
{
    return vmap_style_eff(g_vmap_style_id);
}

/**
 * @brief vmap style get。
 * @return 请求的值。
 */
const vmap_style_t * vmap_style_get(vmap_style_id_t id)
{
    return vmap_style_eff(id);
}

/**
 * @brief vmap style name。
 */
const char * vmap_style_name(vmap_style_id_t id)
{
    return vmap_style_get(id)->name;
}

/**
 * @brief vmap style set changed cb。
 */
void vmap_style_set_changed_cb(vmap_style_changed_cb_t cb, void * user_data)
{
    g_vmap_style_cb = cb;
    g_vmap_style_cb_user = user_data;
}

/**
 * @brief vmap style set id。
 * @return 请求的值。
 */
bool vmap_style_set_id(vmap_style_id_t id)
{
    if ((unsigned)id >= (unsigned)VMAP_STYLE_COUNT) {
        return false;
    }

    g_vmap_style_id = id;
    return true;
}

/**
 * @brief vmap style set。
 */
bool vmap_style_set(vmap_style_id_t id)
{
    if (!vmap_style_set_id(id)) {
        return false;
    }

    if (g_vmap_style_cb != NULL) {
        g_vmap_style_cb(id, g_vmap_style_cb_user);
    }

    return true;
}

/**
 * @brief vmap style set next。
 */
bool vmap_style_set_next(void)
{
    vmap_style_id_t next = (vmap_style_id_t)(((unsigned)g_vmap_style_id + 1u)
        % (unsigned)VMAP_STYLE_COUNT);

    return vmap_style_set(next);
}

/**
 * @brief vmap style set by name。
 */
bool vmap_style_set_by_name(const char * name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    for (unsigned i = 0; i < (unsigned)VMAP_STYLE_COUNT; i++) {
        if (strcmp(g_vmap_styles[i].name, name) == 0) {
            return vmap_style_set((vmap_style_id_t)i);
        }
    }

    return false;
}
