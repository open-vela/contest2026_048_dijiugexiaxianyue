/**
 * @file bicycle_ui_ctl.c
 * @brief 自行车 UI — ui_ctl。
 */

#include "bicycle_ui_ctl.h"

#include "helm_font.h"
#include "helm_idle.h"
#include "helm_icon.h"   /* HELM_ICO_BELL/INBOX：通知横幅左侧图标 */
#include "helm_pwr.h"
#include "lv_pm_disp.h"
#include "lv_pm_i18n.h"
#include "lv_pm_overlay.h"
#include "map_page.h"
#include "myvendor_bicycle_ctl.h"
#include "myvendor_identity.h"
#include "myvendor_sys.h"
#include "bicycle_runtime.h"
#include "vmap/vmap_config.h"
#include "vmap/vmap_style.h"
#include "bicycle_config.h"
#include "bicycle_demo.h"
#include "bicycle_gpx_sim.h"
#if VMAP_ROUTE_ENABLE
#include "vmap/vmap_route_trip.h"
#include "vmap/vmap_route_log.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_MYVENDOR_LCD_DISP) && CONFIG_MYVENDOR_LCD_DISP
#include "myvendor_lcd_disp.h"
#endif

#ifndef BICYCLE_PHONE_NOTIF_MS
#define BICYCLE_PHONE_NOTIF_MS 3000u
#endif

#ifndef BICYCLE_UI_CTL_STYLE_REFR_SUPPRESS_MS
#define BICYCLE_UI_CTL_STYLE_REFR_SUPPRESS_MS 5000u
#endif

/**
 * @brief 自行车 ui ctl sync state。
 */
void bicycle_ui_ctl_sync_state(void)
{
    const char * style = vmap_style_name(vmap_style_get_id());
    const char * locale = lv_pm_i18n_locale_name(lv_pm_i18n_get());
    map_page_t * page = live_map_page_instance();

    myvendor_bicycle_ctl_state_set(MYVENDOR_BICYCLE_CTL_STATE_MAP_STYLE, style);
    myvendor_bicycle_ctl_state_set(MYVENDOR_BICYCLE_CTL_STATE_UI_LOCALE, locale);
    if (page) {
        map_page_sync_zoom_state(page);
    }
}

/**
 * @brief 自行车 ui ctl ready。
 */
void bicycle_ui_ctl_ready(void)
{
    myvendor_bicycle_ctl_ui_ready(true);
}

/**
 * @brief 自行车 ui ctl shutdown。
 */
void bicycle_ui_ctl_shutdown(void)
{
    myvendor_bicycle_ctl_ui_ready(false);
}

static void bicycle_ui_ctl_before_bulk_refr(void)
{
#if defined(CONFIG_MYVENDOR_LCD_DISP) && CONFIG_MYVENDOR_LCD_DISP
    myvendor_lcd_disp_suppress_recover_ms(BICYCLE_UI_CTL_STYLE_REFR_SUPPRESS_MS);
#endif
}

static void bicycle_ui_ctl_handle_style_set(map_page_t * page, const char * name)
{
    if (page == NULL || name == NULL || name[0] == '\0') {
        return;
    }

    bicycle_ui_ctl_before_bulk_refr();

    if (map_page_set_style_by_name(page, name)) {
        bicycle_ui_ctl_sync_state();
    }
}

static void bicycle_ui_ctl_handle_locale_set(const char * name)
{
    if (name == NULL || name[0] == '\0') {
        return;
    }

    if (lv_pm_i18n_set_by_name(name)) {
        myvendor_bicycle_ctl_state_set(MYVENDOR_BICYCLE_CTL_STATE_UI_LOCALE, name);
    }
}

static void bicycle_ui_ctl_handle_notify_show(const char * arg)
{
    char buf[MYVENDOR_BICYCLE_CTL_ARG_MAX];
    char * title;
    char * body;
    char * icon;

    if (arg == NULL || arg[0] == '\0') {
        lv_pm_notify_show_icon("测试通知", "这是一条测试通知", HELM_ICO_BELL, 0);
        return;
    }

    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    title = buf;
    body = strchr(buf, '|');
    if (body == NULL) {
        lv_pm_notify_show_icon(NULL, buf, HELM_ICO_INBOX, 0);
        return;
    }

    *body++ = '\0';
    icon = strchr(body, '|');
    if (icon != NULL) {
        *icon++ = '\0';
    }

    if (title[0] == '\0') {
        title = NULL;
    }
    if (body[0] == '\0' && title != NULL) {
        body = title;
        title = NULL;
    }

    lv_pm_notify_show_ex(title, body,
        (icon != NULL && icon[0] != '\0') ? icon : NULL, 0);
}

static void bicycle_ui_ctl_handle_bottom_show(const char * arg)
{
    if (arg == NULL || arg[0] == '\0') {
        lv_pm_bottom_show("这是一条测试提示", 0);
        return;
    }

    lv_pm_bottom_show(arg, 0);
}

static void bicycle_ui_ctl_handle_map_zoom(map_page_t * page, const char * arg)
{
    if (page == NULL) {
        lv_pm_bottom_show("地图未就绪，无法缩放", 0);
        return;
    }

    if (!map_page_apply_zoom_cmd(page, arg)) {
        lv_pm_bottom_show("缩放参数无效", 0);
        return;
    }
}

static void bicycle_ui_ctl_handle_map_rot(map_page_t * page, const char * arg)
{
    if (page == NULL) {
        lv_pm_bottom_show("地图未就绪，无法切换朝向", 0);
        return;
    }

    if (!map_page_apply_rot_cmd(page, arg)) {
        lv_pm_bottom_show("朝向参数无效", 0);
        return;
    }

    lv_pm_bottom_show(strcmp(arg, "on") == 0 ? "航向朝上：较耗性能，画面可能变慢"
                                             : "已切回正北朝上", 0);
}

#if VMAP_ROUTE_ENABLE
/**
 * @brief 自行车 ui ctl parse nav。
 */
static bool bicycle_ui_ctl_parse_nav(const char * arg, double * from_lon,
    double * from_lat, double * to_lon, double * to_lat)
{
    char buf[MYVENDOR_BICYCLE_CTL_ARG_MAX];
    char * tokens[4];
    char * save = NULL;
    char * p;
    int count = 0;

    if (!arg || arg[0] == '\0' || !from_lon || !from_lat || !to_lon || !to_lat) {
        return false;
    }

    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    for (p = strtok_r(buf, ",", &save); p != NULL && count < 4;
         p = strtok_r(NULL, ",", &save)) {
        tokens[count++] = p;
    }

    if (count == 2) {
        bicycle_gnss_fix_t fix;

        if (!bicycle_runtime_poll_fix(&fix) || !fix.valid) {
            VMAP_NAV_WARN("ui: no GNSS fix for nav start");
            lv_pm_bottom_show("未定位，无法开始导航", 0);
            return false;
        }
        *from_lon = fix.longitude;
        *from_lat = fix.latitude;
        *to_lon = atof(tokens[0]);
        *to_lat = atof(tokens[1]);
        return true;
    }

    if (count == 4) {
        *from_lon = atof(tokens[0]);
        *from_lat = atof(tokens[1]);
        *to_lon = atof(tokens[2]);
        *to_lat = atof(tokens[3]);
        return true;
    }

    return false;
}

/**
 * @brief 自行车 ui ctl handle nav plan。
 */
static void bicycle_ui_ctl_handle_nav_plan(map_page_t * page, const char * arg)
{
    double from_lon;
    double from_lat;
    double to_lon;
    double to_lat;

    if (!bicycle_ui_ctl_parse_nav(arg, &from_lon, &from_lat, &to_lon, &to_lat)) {
        VMAP_NAV_WARN("ui: bad coords \"%s\"", arg ? arg : "");
        lv_pm_bottom_show("坐标格式无效", 0);
        return;
    }

    VMAP_NAV_LOG("ui: plan frozen start %.6f,%.6f -> %.6f,%.6f",
        from_lon, from_lat, to_lon, to_lat);
    if (page == NULL || !map_page_nav_plan(page, from_lon, from_lat, to_lon, to_lat)) {
        VMAP_NAV_WARN("ui: map_page_nav_plan failed page=%p", (void *)page);
        lv_pm_bottom_show("路线规划失败", 0);
        return;
    }
}

/**
 * @brief 自行车 ui ctl handle nav plan sim。
 */
static void bicycle_ui_ctl_handle_nav_plan_sim(map_page_t * page, const char * arg)
{
    double from_lon;
    double from_lat;
    double to_lon;
    double to_lat;

    if (!bicycle_ui_ctl_parse_nav(arg, &from_lon, &from_lat, &to_lon, &to_lat)) {
        VMAP_NAV_WARN("ui: bad sim coords \"%s\"", arg ? arg : "");
        lv_pm_bottom_show("模拟导航坐标无效", 0);
        return;
    }

    VMAP_NAV_LOG("ui: sim plan %.6f,%.6f -> %.6f,%.6f",
        from_lon, from_lat, to_lon, to_lat);
    if (page == NULL
        || !map_page_nav_plan_sim(page, from_lon, from_lat, to_lon, to_lat)) {
        VMAP_NAV_WARN("ui: map_page_nav_plan_sim failed page=%p", (void *)page);
        lv_pm_bottom_show("模拟路线规划失败", 0);
        return;
    }
}

static void bicycle_ui_ctl_handle_nav_stop(map_page_t * page)
{
    VMAP_NAV_LOG("ui: nav stop");
    map_page_nav_stop(page);
    lv_pm_bottom_show("导航已结束", 0);
}

static void bicycle_ui_ctl_handle_nav_sim_speed(const char * arg)
{
    char msg[48];
    float kph;

    if (!arg || arg[0] == '\0') {
        snprintf(msg, sizeof(msg), "模拟速度：%.0f km/h",
            bicycle_config_nav_sim_speed_kph());
        lv_pm_bottom_show(msg, 0);
        return;
    }

    kph = (float)atof(arg);
    if (kph <= 0.0f) {
        lv_pm_bottom_show("模拟速度无效", 0);
        return;
    }

    bicycle_config_set_nav_sim_speed_kph(kph);
    if (bicycle_route_sim_active()) {
        bicycle_route_sim_set_speed(bicycle_config_nav_sim_speed_kph());
    }
    snprintf(msg, sizeof(msg), "模拟速度：%.0f km/h",
        bicycle_config_nav_sim_speed_kph());
    lv_pm_bottom_show(msg, 0);
}

/**
 * @brief 取 s[n] 起一个完整 UTF-8 字符的字节数。
 * @return 字符长度（1..4）；非法首字节、续字节不完整/不是 10xxxxxx 返回 0。
 * @note  s[n] 必须非 NUL（调用方已保证）。按 RFC 3629 收紧范围检查：
 *        拒绝过长编码（C0/C1、E0 A0 之前、F0 90 之前）、UTF-16 代理区
 *        （ED A0..ED BF）以及超出 U+10FFFF 的 F4 90.. 和 F5..FF 首字节。
 */
static size_t bicycle_ui_ctl_utf8_len(const char * s, size_t n)
{
    const unsigned char c = (unsigned char)s[n];
    size_t len;
    size_t i;

    if (c < 0x80u) {
        return 1u;
    }
    if (c < 0xC2u) {
        return 0u;              /* 续字节或过长 2 字节编码 */
    }
    if (c < 0xE0u) {
        len = 2u;
    } else if (c < 0xF0u) {
        len = 3u;
    } else if (c < 0xF5u) {
        len = 4u;
    } else {
        return 0u;              /* F5..FF 超出 Unicode 范围 */
    }

    for (i = 1u; i < len; i++) {
        const unsigned char cc = (unsigned char)s[n + i];

        if (cc == '\0' || (cc & 0xC0u) != 0x80u) {
            return 0u;
        }
    }

    if (len == 3u) {
        const unsigned char c1 = (unsigned char)s[n + 1u];

        if (c == 0xE0u && c1 < 0xA0u) {
            return 0u;          /* 过长 3 字节 */
        }
        if (c == 0xEDu && c1 >= 0xA0u) {
            return 0u;          /* UTF-16 代理区 */
        }
    } else if (len == 4u) {
        const unsigned char c1 = (unsigned char)s[n + 1u];

        if (c == 0xF0u && c1 < 0x90u) {
            return 0u;          /* 过长 4 字节 */
        }
        if (c == 0xF4u && c1 >= 0x90u) {
            return 0u;          /* > U+10FFFF */
        }
    }

    return len;
}

/**
 * @brief 解出已校验的 UTF-8 字符码位（len 来自 bicycle_ui_ctl_utf8_len()）。
 */
static uint32_t bicycle_ui_ctl_utf8_cp(const char * s, size_t len)
{
    const unsigned char * p = (const unsigned char *)s;

    switch (len) {
    case 2u:
        return ((uint32_t)(p[0] & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
    case 3u:
        return ((uint32_t)(p[0] & 0x0Fu) << 12) |
               ((uint32_t)(p[1] & 0x3Fu) << 6) | (uint32_t)(p[2] & 0x3Fu);
    case 4u:
        return ((uint32_t)(p[0] & 0x07u) << 18) |
               ((uint32_t)(p[1] & 0x3Fu) << 12) |
               ((uint32_t)(p[2] & 0x3Fu) << 6) | (uint32_t)(p[3] & 0x3Fu);
    default:
        return (uint32_t)p[0];
    }
}

/**
 * @brief 按底部 toast 的实际字体宽度截断文本，并保留完整 UTF-8 字符。
 * @param max_w     可用像素宽；非法/不完整序列截到上一个完整字符。
 * @param max_bytes 可用字节数（不含结尾 NUL），避免超出 toast 缓冲区。
 * @note  lv_text_get_width() 只声明在私有头 lv_text_private.h，且第 4 参是
 *        lv_text_attributes_t*（非 letter_space）；这里改用公开的
 *        lv_font_get_glyph_width() 逐字累加 adv_w（含 kerning），
 *        与它 letter_space=0 时的算法一致。
 */
static size_t bicycle_ui_ctl_fit_utf8_width(const char * s, int32_t max_w,
    size_t max_bytes)
{
    const lv_font_t * font = helm_font_sys(14, helm_font_title());
    size_t n = 0u;
    int32_t w = 0;

    if (s == NULL || font == NULL || max_w <= 0) {
        return 0u;
    }

    while (s[n] != '\0') {
        const size_t len = bicycle_ui_ctl_utf8_len(s, n);
        uint32_t cp;
        uint32_t next = 0u;
        size_t next_len;
        int32_t adv;

        if (len == 0u || n + len > max_bytes) {
            break;
        }

        cp = bicycle_ui_ctl_utf8_cp(s + n, len);
        next_len = bicycle_ui_ctl_utf8_len(s, n + len);
        if (next_len != 0u) {
            next = bicycle_ui_ctl_utf8_cp(s + n + len, next_len);
        }

        adv = (int32_t)lv_font_get_glyph_width(font, cp, next);
        if (adv > max_w - w) {
            break;
        }

        w += adv;
        n += len;
    }

    return n;
}

/**
 * @brief 从 n（必须是字符边界）退回上一个 UTF-8 字符边界。
 */
static size_t bicycle_ui_ctl_utf8_prev(const char * s, size_t n)
{
    while (n > 0u) {
        n--;
        if (((unsigned char)s[n] & 0xC0u) != 0x80u) {
            break;
        }
    }

    return n;
}

static void bicycle_ui_ctl_handle_gnss_sim(map_page_t * page, const char * arg)
{
    char path[MYVENDOR_BICYCLE_CTL_ARG_MAX];
    char toast[128];
    const char * p;
    const char * rest;
    bool reverse = false;
    float kph = 0.0f;
    float skip_km = 0.0f;
    int ret;
    char * end = NULL;

    if (arg == NULL || arg[0] == '\0' || strcmp(arg, "stop") == 0) {
        (void)map_page_gnss_sim(page, NULL, false, 0.0f, 0.0f);
        lv_pm_bottom_show("模拟轨迹已停止", 0);
        return;
    }

    p = arg;
    if (strncmp(p, "rev|", 4) == 0) {
        reverse = true;
        p += 4;
    } else if (strncmp(p, "fwd|", 4) == 0) {
        p += 4;
    }

    rest = strchr(p, '|');
    if (rest != NULL) {
        size_t n = (size_t)(rest - p);

        if (n >= sizeof(path)) {
            n = sizeof(path) - 1u;
        }
        memcpy(path, p, n);
        path[n] = '\0';
        kph = (float)strtod(rest + 1, &end);
        if (end == rest + 1) {
            kph = 0.0f;
        }
        if (end != NULL && *end == '|') {
            skip_km = (float)strtod(end + 1, NULL);
            if (skip_km > 0.0f) {
                skip_km = skip_km / 1000.0f;
            }
        }
    } else {
        lv_strlcpy(path, p, sizeof(path));
    }

    if (path[0] == '\0') {
        lv_pm_bottom_show("未指定 GPX 路径", 0);
        return;
    }

    ret = map_page_gnss_sim(page, path, reverse, kph, skip_km);
    if (ret != 0) {
        snprintf(toast, sizeof(toast), "GPX 轨迹打开失败 (%d)", ret);
        lv_pm_bottom_show(toast, 0);
        return;
    }

    {
        const char * base = strrchr(path, '/');
        const lv_font_t * font = helm_font_sys(14, helm_font_title());
        const char * prefix = reverse ? "反向回放" : "顺序回放";
        const int32_t line_w = lv_pm_hor_res() - 64;
        double at_m = bicycle_gpx_sim_prefix_m();
        char tail[24];
        int32_t avail;
        size_t room;
        size_t n;

        base = (base != NULL && base[1] != '\0') ? base + 1 : path;
        if (at_m >= 50.0) {
            snprintf(tail, sizeof(tail), " @%.1fkm", at_m / 1000.0);
        } else {
            tail[0] = '\0';
        }

        /* 先用像素宽做粗截：单行 176px，两行 352px，留 16px 折行余量。 */
        avail = 2 * line_w - 16;
        if (font != NULL) {
            lv_point_t sz;

            lv_text_get_size(&sz, prefix, font, 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            avail -= (int32_t)sz.x;
            lv_text_get_size(&sz, " ", font, 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            avail -= (int32_t)sz.x;
            if (tail[0] != '\0') {
                lv_text_get_size(&sz, tail, font, 0, 0, LV_COORD_MAX,
                                 LV_TEXT_FLAG_NONE);
                avail -= (int32_t)sz.x;
            }
        }

        /* 文件名再受 toast 缓冲余量限制，避免 snprintf 在 UTF-8 字符中间截断。 */
        room = sizeof(toast) - 1u - strlen(prefix) - 1u - strlen(tail);
        n = bicycle_ui_ctl_fit_utf8_width(base, avail, room);

        /* 粗截只按总像素宽，遇到空格/换行仍可能排出第三行；这里用 LVGL
         * 自己的折行结果验证高度，超两行就回退一个字符再试。 */
        for (;;) {
            lv_point_t sz;

            snprintf(toast, sizeof(toast), "%s %.*s%s", prefix, (int)n, base,
                     tail);
            if (font == NULL) {
                break;
            }
            lv_text_get_size(&sz, toast, font, 0, 0, line_w,
                             LV_TEXT_FLAG_NONE);
            if (sz.y <= 2 * lv_font_get_line_height(font)) {
                break;
            }
            if (n == 0u) {
                /* 连前缀+后缀都排不下：退到只显示前缀，保证 ≤2 行。 */
                snprintf(toast, sizeof(toast), "%s", prefix);
                break;
            }
            n = bicycle_ui_ctl_utf8_prev(base, n);
        }
        lv_pm_bottom_show(toast, 0);
    }
}

/**
 * @brief 自行车 ui ctl parse trip。
 */
static bool bicycle_ui_ctl_parse_trip(const char * arg,
    vmap_route_waypoint_t * out, uint32_t * out_count)
{
    char buf[MYVENDOR_BICYCLE_CTL_ARG_MAX];
    char * save = NULL;
    char * seg;
    uint32_t n = 0;

    if (!arg || !out || !out_count) {
        return false;
    }

    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    seg = strtok_r(buf, ";", &save);
    while (seg != NULL && n < VMAP_ROUTE_TRIP_MAX_WAYPOINTS) {
        char * comma = strchr(seg, ',');

        if (!comma) {
            return false;
        }
        *comma = '\0';
        out[n].lon = atof(seg);
        out[n].lat = atof(comma + 1);
        n++;
        seg = strtok_r(NULL, ";", &save);
    }

    if (n == 0) {
        return false;
    }

    *out_count = n;
    return true;
}

static bool bicycle_ui_ctl_trip_from_bridge(vmap_route_waypoint_t * out,
    uint32_t * out_count)
{
    int32_t lat[MYVENDOR_SYS_NAV_MAX_PTS];
    int32_t lon[MYVENDOR_SYS_NAV_MAX_PTS];
    uint8_t n = 0;
    uint8_t i;

    if (!out || !out_count) {
        return false;
    }
    if (!myvendor_sys_nav_take(lat, lon, &n) || n == 0) {
        return false;
    }

    for (i = 0; i < n && i < VMAP_ROUTE_TRIP_MAX_WAYPOINTS; i++) {
        out[i].lat = (double)lat[i] / 10000000.0;
        out[i].lon = (double)lon[i] / 10000000.0;
    }
    *out_count = i;
    return i > 0;
}

static void bicycle_ui_ctl_handle_nav_trip(map_page_t * page, const char * arg)
{
    vmap_route_waypoint_t wps[VMAP_ROUTE_TRIP_MAX_WAYPOINTS];
    uint32_t count = 0;
    bool ok;

    if (arg == NULL || arg[0] == '\0') {
        ok = bicycle_ui_ctl_trip_from_bridge(wps, &count);
    } else {
        ok = bicycle_ui_ctl_parse_trip(arg, wps, &count);
    }

    if (!ok) {
        lv_pm_bottom_show("途经点坐标无效", 0);
        return;
    }

    if (page == NULL || !map_page_nav_trip_plan(page, wps, count)) {
        lv_pm_bottom_show("途经点路线规划失败", 0);
        return;
    }
}
#endif

static void bicycle_ui_ctl_handle_ride_start(map_page_t * page)
{
    map_page_set_recording(page, true);
#if VMAP_ROUTE_ENABLE
    {
        vmap_route_waypoint_t wps[VMAP_ROUTE_TRIP_MAX_WAYPOINTS];
        uint32_t count = 0;

        if (bicycle_ui_ctl_trip_from_bridge(wps, &count)) {
            if (page == NULL || !map_page_nav_trip_plan(page, wps, count)) {
                lv_pm_bottom_show("导航启动失败", 0);
                return;
            }
        }
    }
#endif
    lv_pm_bottom_show("骑行已开始", 0);
}

static void bicycle_ui_ctl_handle_ride_stop(map_page_t * page)
{
    bicycle_runtime_save_last_pos();
    map_page_set_recording(page, false);
#if VMAP_ROUTE_ENABLE
    map_page_nav_stop(page);
#endif
    lv_pm_bottom_show("骑行已结束", 0);
}

static void bicycle_ui_ctl_handle_sensor_set(const char * arg)
{
    unsigned hr = 0;
    unsigned cad = 0;
    unsigned pwr = 0;
    int n;

    if (arg == NULL || arg[0] == '\0' || strcmp(arg, "off") == 0 ||
        strcmp(arg, "0") == 0) {
        bicycle_runtime_update_sensors(0, false, 0, false, 0, false);
        lv_pm_bottom_show("传感器已关闭", 0);
        return;
    }

    n = sscanf(arg, "%u,%u,%u", &hr, &cad, &pwr);
    if (n < 1) {
        lv_pm_bottom_show("传感器参数无效", 0);
        return;
    }

    bicycle_runtime_update_sensors((uint16_t)hr, n >= 1 && hr > 0,
        (uint16_t)cad, n >= 2 && cad > 0, (uint16_t)pwr, n >= 3 && pwr > 0);
}

/**
 * @brief 演示提示文案：优先当前单场景的中文说明，其次模式/步骤状态。
 */
static const char * bicycle_ui_ctl_demo_desc(void)
{
    const char * desc = bicycle_demo_scene_current_desc();

    if (desc == NULL || desc[0] == '\0') {
        desc = bicycle_demo_status();
    }
    if (desc == NULL || desc[0] == '\0') {
        desc = "已开启";
    }

    return desc;
}

/**
 * @brief 单场景切换成功后的 toast（当前场景中文说明；说明缺失时退回状态文案）。
 */
static void bicycle_ui_ctl_demo_scene_toast(void)
{
    /* bicycle_demo_status() 的输出最长 47 字节（内部 buf[48]），
     * 前缀 "演示：" 9 字节 ⇒ msg[96] 足够，不会截断。 */
    char msg[96];

    snprintf(msg, sizeof(msg), "演示：%s", bicycle_ui_ctl_demo_desc());
    lv_pm_bottom_show(msg, 0);
}

/**
 * @brief 演示模式开关 / 单场景切换 / 循环演示模式（`bicycle_nsh demo` / 菜单）。
 * @param arg "on"|"off"|"toggle"（空 = 开）；"run:<场景名>"|"next"|"prev" 单场景；
 *            "mode:<模式名>"|"start:<模式名>" 循环演示（random/linear/minghu/full）。
 */
static void bicycle_ui_ctl_handle_demo(const char * arg)
{
    char msg[96];
    int rc;

    /* 循环演示模式：mode:<名>/start:<名>（NSH `demo start|mode|on <name>`）。
     * 名字表在 bicycle_demo.h，未知名字提示后直接返回，别落到末尾的通用 toast。 */
    if (arg != NULL && (strncmp(arg, "mode:", 5) == 0 ||
                        strncmp(arg, "start:", 6) == 0)) {
        const char * name = strchr(arg, ':') + 1;
        int mode = bicycle_demo_mode_from_name(name);

        if (mode < 0) {
            lv_pm_bottom_show("未知演示模式", 0);
            return;
        }

        rc = bicycle_demo_start_mode((bicycle_demo_mode_t)mode);
        if (rc != BICYCLE_DEMO_OK) {
            lv_pm_bottom_show("演示模式启动失败", 0);
            return;
        }

        snprintf(msg, sizeof(msg), "演示：%s", bicycle_ui_ctl_demo_desc());
        lv_pm_bottom_show(msg, 0);
        return;
    }

    /* 单场景：run:<name> 由 demo 表查找；未知名字要提示，不能静默。 */
    if (arg != NULL && strncmp(arg, "run:", 4) == 0) {
        if (arg[4] == '\0') {
            lv_pm_bottom_show("未知演示场景", 0);
            return;
        }

        rc = bicycle_demo_scene_run(arg + 4);
        if (rc == 0) {
            bicycle_ui_ctl_demo_scene_toast();
        } else if (rc == BICYCLE_DEMO_SCENE_ERR_UNKNOWN) {
            lv_pm_bottom_show("未知演示场景", 0);
        } else {
            lv_pm_bottom_show("演示场景启动失败", 0);
        }
        return;
    }

    if (arg != NULL && (strcmp(arg, "next") == 0 || strcmp(arg, "prev") == 0)) {
        rc = bicycle_demo_scene_step(strcmp(arg, "next") == 0 ? 1 : -1);
        if (rc == 0) {
            bicycle_ui_ctl_demo_scene_toast();
        } else {
            lv_pm_bottom_show("演示场景启动失败", 0);
        }
        return;
    }

    if (arg != NULL && strcmp(arg, "off") == 0) {
        bicycle_demo_stop();
        lv_pm_bottom_show("演示模式已停止", 0);
        return;
    }

    if (arg != NULL && strcmp(arg, "toggle") == 0) {
        bicycle_demo_toggle();
    } else if (!bicycle_demo_active()) {
        rc = bicycle_demo_start();

        if (rc != BICYCLE_DEMO_OK) {
            lv_pm_bottom_show(bicycle_demo_error_text(rc), 0);
            return;
        }
    }

    snprintf(msg, sizeof(msg), "演示：%s", bicycle_ui_ctl_demo_desc());
    lv_pm_bottom_show(msg, 0);
}

static void bicycle_ui_ctl_handle_msg(map_page_t * page,
                                      const myvendor_bicycle_ctl_msg_t * msg)
{
    if (msg == NULL) {
        return;
    }

    switch ((myvendor_bicycle_ctl_op_t)msg->op) {
    case MYVENDOR_BICYCLE_CTL_OP_NOTIFY_SHOW:
        bicycle_ui_ctl_handle_notify_show(msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_BOTTOM_SHOW:
        bicycle_ui_ctl_handle_bottom_show(msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_MAP_ZOOM:
        bicycle_ui_ctl_handle_map_zoom(page, msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_MAP_ROT:
        bicycle_ui_ctl_handle_map_rot(page, msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_RIDE_START:
        bicycle_ui_ctl_handle_ride_start(page);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_RIDE_STOP:
        bicycle_ui_ctl_handle_ride_stop(page);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_SENSOR_SET:
        bicycle_ui_ctl_handle_sensor_set(msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_GNSS_SIM:
        bicycle_ui_ctl_handle_gnss_sim(page, msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_DEMO:
        bicycle_ui_ctl_handle_demo(msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_IDLE_ENTER:
        helm_idle_force_enter();
        return;

    case MYVENDOR_BICYCLE_CTL_OP_IDLE_WAKE:
        helm_idle_wake();
        return;

    case MYVENDOR_BICYCLE_CTL_OP_IDLE_HOUR:
        helm_idle_force_hour();
        return;

    case MYVENDOR_BICYCLE_CTL_OP_IDLE_STAY:
        helm_idle_stay(msg->arg[0] != '0');
        return;

#if VMAP_ROUTE_ENABLE
    case MYVENDOR_BICYCLE_CTL_OP_NAV_PLAN:
        bicycle_ui_ctl_handle_nav_plan(page, msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_NAV_PLAN_SIM:
        bicycle_ui_ctl_handle_nav_plan_sim(page, msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_NAV_TRIP:
        bicycle_ui_ctl_handle_nav_trip(page, msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_NAV_STOP:
        bicycle_ui_ctl_handle_nav_stop(page);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_NAV_SIM_SPEED:
        bicycle_ui_ctl_handle_nav_sim_speed(msg->arg);
        return;
#endif

    default:
        break;
    }

    if (page == NULL) {
        return;
    }

    switch ((myvendor_bicycle_ctl_op_t)msg->op) {
    case MYVENDOR_BICYCLE_CTL_OP_STYLE_SET:
        bicycle_ui_ctl_handle_style_set(page, msg->arg);
        break;

    case MYVENDOR_BICYCLE_CTL_OP_STYLE_CYCLE:
        bicycle_ui_ctl_before_bulk_refr();
        if (map_page_cycle_style(page)) {
            bicycle_ui_ctl_sync_state();
        }
        break;

    case MYVENDOR_BICYCLE_CTL_OP_UI_REDRAW:
        bicycle_ui_ctl_before_bulk_refr();
        map_page_redraw(page);
        break;

    case MYVENDOR_BICYCLE_CTL_OP_LOCALE_SET:
        bicycle_ui_ctl_handle_locale_set(msg->arg);
        break;

    default:
        break;
    }
}

/**
 * @brief 关机或陀螺仪静止休眠时不弹 App 横幅。
 * @details 0xFF17 已写入最近通知；这里只丢掉待弹队列，避免盖住时钟/关机页。
 */
static bool bicycle_ui_phone_banner_quiet(void)
{
    return helm_pwr_is_offing() || helm_pwr_dialog_open() ||
           helm_idle_is_sleeping();
}

/**
 * @brief 抽空 ctl 队列，并弹出下一条手机通知横幅。
 */
void bicycle_ui_ctl_poll(map_page_t * page)
{
    myvendor_bicycle_ctl_msg_t msg;
    myvendor_sys_notif_t ntf;

    while (myvendor_bicycle_ctl_take(&msg)) {
        bicycle_ui_ctl_handle_msg(page, &msg);
    }

    if (myvendor_is_factory() || bicycle_ui_phone_banner_quiet()) {
        while (myvendor_sys_notif_take(&ntf)) {
        }

        return;
    }

    if (lv_pm_notify_is_showing()) {
        return;
    }

    if (!myvendor_sys_notif_take(&ntf)) {
        return;
    }

    /* 防御（板级，2026-09-24）：**弹窗限速 1.5 s**。
     *
     * 现场：微信**通话**通知是常驻的、手机侧持续下发，UI 每抽到一条就重弹一次
     * `lv_pm_notify_show_ex()`（modal 反复 show/hide）⇒ 整块 UI 卡死。
     * bridge 侧已做"同键合并 + 弹窗限流"（`companion_bridge_notif_post()`），
     * 这里在 UI 侧再兜一道：太密的直接丢弃（该条仍在 inbox 列表里）。 */
    {
        static uint32_t s_last_show_ms;
        uint32_t t = (uint32_t)lv_tick_get();

        if (s_last_show_ms != 0u && (uint32_t)(t - s_last_show_ms) < 1500u) {
            return;
        }

        s_last_show_ms = t ? t : 1u;
    }

    lv_pm_notify_show_ex(ntf.title[0] != '\0' ? ntf.title : "通知",
                         ntf.body[0] != '\0' ? ntf.body : ntf.title,
                         ntf.icon[0] != '\0' ? ntf.icon : NULL,
                         BICYCLE_PHONE_NOTIF_MS);
}
