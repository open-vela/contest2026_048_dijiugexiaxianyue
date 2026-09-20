/**
 * @file vmap_view.c
 * @brief 地图视图：瓦片泵 + 画布叠层（底图 → 导航线 → 标签 → REC 线 → REC 数字）。
 */

#include "vmap_view.h"

#include "vmap_config.h"
#include "vmap_label.h"
#include "vmap_render.h"
#include "vmap_tile_cache.h"
#include "vmap_tile_system.h"
#include "vmap_alloc.h"
#if VMAP_TRACK_ENABLE
#include "vmap_track.h"
#endif
#if VMAP_ROUTE_ENABLE
#include "vmap_route.h"
#endif
#if defined(CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE) && CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE
#include "bicycle_inval_probe.h"
#endif
#include "bicycle_c_debug.h"
#include <myvendor_mtp_lfs.h>
#include "myvendor_devctl.h"
#include "myvendor_watchdog.h"
#include "sf32lb_dvfs.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#define VMAP_PAN_MIN_STEP_PX  1

static void render_begin(vmap_view_t * view, bool reset_view, bool force_clear);

static void dvfs_map_busy(bool busy)
{
    if (busy) {
        sf32lb_dvfs_hold(SF32LB_DVFS_HOLD_MAP);
    } else {
        sf32lb_dvfs_release(SF32LB_DVFS_HOLD_MAP);
    }
}

bool vmap_boot_ll_ok(double lon, double lat)
{
    if (!isfinite(lon) || !isfinite(lat)) {
        return false;
    }

    if (fabs(lon) < 0.001 && fabs(lat) < 0.001) {
        return false;
    }

    if (lon < VMAP_POS_LON_MIN || lon > VMAP_POS_LON_MAX ||
        lat < VMAP_POS_LAT_MIN || lat > VMAP_POS_LAT_MAX) {
        return false;
    }

    return true;
}

void vmap_boot_lonlat(double *lon, double *lat)
{
    int32_t lon_e7 = 0;
    int32_t lat_e7 = 0;
    double last_lon;
    double last_lat;

    if (lon == NULL || lat == NULL) {
        return;
    }

    if (myvendor_devctl_last_pos_get(&lon_e7, &lat_e7)) {
        last_lon = (double)lon_e7 / 10000000.0;
        last_lat = (double)lat_e7 / 10000000.0;
        if (vmap_boot_ll_ok(last_lon, last_lat)) {
            *lon = last_lon;
            *lat = last_lat;
            return;
        }
    }

    *lon = VMAP_DEFAULT_LON;
    *lat = VMAP_DEFAULT_LAT;
}

struct vmap_view {
    lv_obj_t * canvas;
    lv_obj_t * arrow_layer;
    lv_obj_t * arrow;
    void * cbuf;
    int32_t w;
    int32_t h;
    int32_t view_w;
    int32_t view_h;
    int32_t margin;
    uint32_t stride;
    double lon;
    double lat;
    double render_lon;
    double render_lat;
    double pan_dx;
    double pan_dy;
    int zoom;
    double scale;
    bool follow_mode;
    int32_t last_arrow_angle10;
    int32_t last_pan_px;
    int32_t last_pan_py;
    vmap_tile_cache_t * cache;
    vmap_point_t * scratch;
    vmap_label_cand_t * labels;
    vmap_label_retained_t * label_retained;
    vmap_label_collector_t label_col;
    const lv_font_t * font;
    uint32_t map_rev;
    uint32_t render_rev;
    int32_t pump_tx;
    int32_t pump_ty;
    int32_t pump_tx_min;
    int32_t pump_ty_min;
    int32_t pump_tx_max;
    int32_t pump_ty_max;
    int pump_cx;
    int pump_cy;
    uint8_t pump_phase;
    uint8_t restamp_pending;
    uint8_t pump_need_region;
    uint8_t pump_dirty_n;
    int paint_zoom;
    double paint_scale;
    int32_t pump_dirty_x1[VMAP_RENDER_DIRTY_MAX];
    int32_t pump_dirty_y1[VMAP_RENDER_DIRTY_MAX];
    int32_t pump_dirty_x2[VMAP_RENDER_DIRTY_MAX];
    int32_t pump_dirty_y2[VMAP_RENDER_DIRTY_MAX];
    struct vmap_track * track;
#if VMAP_ROUTE_ENABLE
    struct vmap_route * route;
#endif
};

static void tile_range(int32_t center_x, int32_t center_y, int32_t canvas_w,
    int32_t canvas_h, double scale, int32_t ring,
    int32_t * tx_min, int32_t * ty_min, int32_t * tx_max, int32_t * ty_max)
{
    const int32_t tile_size = 256;
    double half_w = (canvas_w / 2.0) / scale;
    double half_h = (canvas_h / 2.0) / scale;
    double pad = (double)ring * tile_size;
    double left = (double)center_x - half_w - pad;
    double top = (double)center_y - half_h - pad;
    double right = (double)center_x + half_w + pad;
    double bottom = (double)center_y + half_h + pad;

    *tx_min = (int32_t)(left / tile_size) - (left < 0 ? 1 : 0);
    *ty_min = (int32_t)(top / tile_size) - (top < 0 ? 1 : 0);
    *tx_max = (int32_t)(right / tile_size);
    *ty_max = (int32_t)(bottom / tile_size);
}

static void vmap_view_apply_canvas_pos(vmap_view_t * view, double dx, double dy)
{
    lv_coord_t px;
    lv_coord_t py;

    if (!view || !view->canvas) {
        return;
    }

    px = (lv_coord_t)lround(dx);
    py = (lv_coord_t)lround(dy);

    if (px == view->last_pan_px && py == view->last_pan_py) {
        return;
    }

    /*
     * Moving a canvas invalidates the complete visible map. Coalesce sub-pixel
     * GNSS movement; the centered arrow still updates normally.
     */
    if (view->last_pan_px != INT32_MIN && view->last_pan_py != INT32_MIN
        && abs((int)px - view->last_pan_px) < VMAP_PAN_MIN_STEP_PX
        && abs((int)py - view->last_pan_py) < VMAP_PAN_MIN_STEP_PX) {
        return;
    }

    view->pan_dx = (double)px;
    view->pan_dy = (double)py;
    view->last_pan_px = px;
    view->last_pan_py = py;
    view->map_rev++;
    vmap_label_collector_set_focus(&view->label_col,
        view->w / 2 + px, view->h / 2 + py);
#if defined(CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE) && CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE
    bicycle_inval_probe_tag("vmap_pan");
#endif
    /* lv_obj_set_pos already invalidates old + new areas; do not inval again. */
    lv_obj_set_pos(view->canvas,
        (lv_coord_t)(-view->margin - px),
        (lv_coord_t)(-view->margin - py));
}

static bool vmap_view_geo_to_viewport_impl(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y, bool check_visible)
{
    int base_x = 0;
    int base_y = 0;
    int geo_x = 0;
    int geo_y = 0;
    double cx;
    double cy;
    int32_t sx;
    int32_t sy;
    const int32_t pad = 24;

    if (!view) {
        return false;
    }

    vmap_latlong_to_pixel(view->render_lat, view->render_lon, view->zoom, &base_x, &base_y);
    vmap_latlong_to_pixel(lat, lon, view->zoom, &geo_x, &geo_y);

    cx = (geo_x - base_x) * view->scale + (double)view->w / 2.0;
    cy = (geo_y - base_y) * view->scale + (double)view->h / 2.0;
    sx = (int32_t)lround(cx - (double)view->margin - view->pan_dx);
    sy = (int32_t)lround(cy - (double)view->margin - view->pan_dy);

    if (out_x) {
        *out_x = sx;
    }
    if (out_y) {
        *out_y = sy;
    }

    if (!check_visible) {
        return true;
    }

    return sx >= -pad && sy >= -pad
        && sx <= view->view_w + pad && sy <= view->view_h + pad;
}

/**
 * @brief 经纬度 → 视口像素（含 follow pan）。
 */
bool vmap_view_geo_to_viewport(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y)
{
    return vmap_view_geo_to_viewport_impl(view, lon, lat, out_x, out_y, true);
}

/**
 * @brief 经纬度 → 视口像素，不做可见性裁剪。
 */
bool vmap_view_geo_to_viewport_raw(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y)
{
    return vmap_view_geo_to_viewport_impl(view, lon, lat, out_x, out_y, false);
}

/**
 * @brief 经纬度 → 画布整数像素。
 */
bool vmap_view_geo_to_canvas_raw(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y)
{
    int base_x = 0;
    int base_y = 0;
    int geo_x = 0;
    int geo_y = 0;
    double cx;
    double cy;

    if (!view) {
        return false;
    }

    vmap_latlong_to_pixel(view->render_lat, view->render_lon, view->zoom, &base_x, &base_y);
    vmap_latlong_to_pixel(lat, lon, view->zoom, &geo_x, &geo_y);

    cx = (geo_x - base_x) * view->scale + (double)view->w / 2.0;
    cy = (geo_y - base_y) * view->scale + (double)view->h / 2.0;

    if (out_x) {
        *out_x = (int32_t)lround(cx);
    }
    if (out_y) {
        *out_y = (int32_t)lround(cy);
    }
    return true;
}

bool vmap_view_geo_to_canvas_f(const vmap_view_t * view, double lon, double lat,
    double * out_x, double * out_y)
{
    vmap_view_proj_t proj;

    if (!view) {
        return false;
    }

    vmap_view_proj_begin(view, &proj);
    vmap_view_proj_point_f(&proj, lon, lat, out_x, out_y);
    return true;
}

void vmap_view_proj_begin(const vmap_view_t * view, vmap_view_proj_t * proj)
{
    if (!proj) {
        return;
    }
    if (!view) {
        memset(proj, 0, sizeof(*proj));
        return;
    }

    proj->zoom = view->zoom;
    proj->scale = view->scale;
    proj->half_w = (double)view->w / 2.0;
    proj->half_h = (double)view->h / 2.0;
    vmap_latlong_to_pixel(view->render_lat, view->render_lon, view->zoom,
        &proj->base_x, &proj->base_y);
}

void vmap_view_proj_point(const vmap_view_proj_t * proj, double lon, double lat,
    int32_t * out_x, int32_t * out_y)
{
    int geo_x = 0;
    int geo_y = 0;
    double cx;
    double cy;

    if (!proj) {
        return;
    }

    vmap_latlong_to_pixel(lat, lon, proj->zoom, &geo_x, &geo_y);
    cx = (geo_x - proj->base_x) * proj->scale + proj->half_w;
    cy = (geo_y - proj->base_y) * proj->scale + proj->half_h;
    if (out_x) {
        *out_x = (int32_t)lround(cx);
    }
    if (out_y) {
        *out_y = (int32_t)lround(cy);
    }
}

void vmap_view_proj_point_f(const vmap_view_proj_t * proj, double lon, double lat,
    double * out_x, double * out_y)
{
    double geo_x = 0.0;
    double geo_y = 0.0;

    if (!proj) {
        return;
    }

    vmap_latlong_to_pixel_d(lat, lon, proj->zoom, &geo_x, &geo_y);
    if (out_x) {
        *out_x = (geo_x - (double)proj->base_x) * proj->scale + proj->half_w;
    }
    if (out_y) {
        *out_y = (geo_y - (double)proj->base_y) * proj->scale + proj->half_h;
    }
}

/**
 * @brief retained 画布宽高（含 overscan）。
 * @return 请求的值。
 */
void vmap_view_get_buffer_size(const vmap_view_t * view, int32_t * w, int32_t * h)
{
    if (!view) {
        return;
    }
    if (w) {
        *w = view->w;
    }
    if (h) {
        *h = view->h;
    }
}

/**
 * @brief 画布坐标点是否可安全描边。
 */
bool vmap_view_canvas_point_sane(const vmap_view_t * view, int32_t x, int32_t y)
{
    int32_t cw = 0;
    int32_t ch = 0;

    if (!view) {
        return false;
    }

    vmap_view_get_buffer_size(view, &cw, &ch);
    if (cw < 1 || ch < 1) {
        return false;
    }

    return x >= -cw * 4 && x <= cw * 5 && y >= -ch * 4 && y <= ch * 5;
}

/**
 * @brief 画布点是否落在可视窗口外扩 pad 内。
 */
bool vmap_view_canvas_point_in_pad(const vmap_view_t * view, int32_t x, int32_t y,
    int32_t pad)
{
    int32_t cw = 0;
    int32_t ch = 0;

    if (!view || pad < 0 || !vmap_view_canvas_point_sane(view, x, y)) {
        return false;
    }

    vmap_view_get_buffer_size(view, &cw, &ch);
    return x >= -pad && y >= -pad && x <= cw + pad && y <= ch + pad;
}

/**
 * @brief 画布点是否落在可视窗口内。
 */
bool vmap_view_canvas_point_visible(const vmap_view_t * view, int32_t x, int32_t y)
{
    return vmap_view_canvas_point_in_pad(view, x, y, 48);
}

/**
 * @brief 取出当前 RGB565 画布描述。
 */
bool vmap_view_get_canvas(const vmap_view_t * view, vmap_canvas_t * canvas)
{
    if (!view || !canvas || !view->cbuf) {
        return false;
    }

    canvas->buf = (uint16_t *)view->cbuf;
    canvas->w = view->w;
    canvas->h = view->h;
    canvas->stride = view->stride;
    vmap_render_clip_reset(canvas);
    return true;
}

void vmap_view_invalidate_canvas_area(const vmap_view_t * view,
    int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    lv_area_t area;
    lv_area_t coords;

    if (!view || !view->canvas || x1 > x2 || y1 > y2) {
        return;
    }

    if (x2 < 0 || y2 < 0 || x1 >= view->w || y1 >= view->h) {
        return;
    }

    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 >= view->w) {
        x2 = view->w - 1;
    }
    if (y2 >= view->h) {
        y2 = view->h - 1;
    }

    lv_obj_get_coords(view->canvas, &coords);
    area.x1 = coords.x1 + x1;
    area.y1 = coords.y1 + y1;
    area.x2 = coords.x1 + x2;
    area.y2 = coords.y1 + y2;
    lv_obj_invalidate_area(view->canvas, &area);
}

#if VMAP_TRACK_ENABLE
/**
 * @brief 绑定 REC 轨迹叠层。
 */
void vmap_view_attach_track(vmap_view_t * view, vmap_track_t * track)
{
    if (view) {
        view->track = track;
    }
}

#if VMAP_ROUTE_ENABLE
/**
 * @brief 把规划路线绑到本视图。
 */
void vmap_view_attach_route(vmap_view_t * view, vmap_route_t * route)
{
    if (view) {
        view->route = route;
        if (route) {
            vmap_route_bind_view(route, view);
        }
    }
}
#endif
#endif

/**
 * @brief 创建带 overscan 的 RGB565 大画布；canvas 隐藏到首帧完成。
 * @return 视图；失败为 NULL。
 */
vmap_view_t * vmap_view_create(lv_obj_t * parent, int32_t view_w, int32_t view_h,
    int32_t margin)
{
    vmap_view_t * view = (vmap_view_t *)calloc(1, sizeof(*view));
    if (!view) {
        return NULL;
    }

    view->view_w = view_w;
    view->view_h = view_h;
    view->margin = margin;
    view->w = view_w + margin * 2;
    view->h = view_h + margin * 2;
    view->zoom = VMAP_DEFAULT_ZOOM;
    view->scale = VMAP_DEFAULT_SCALE;
    vmap_boot_lonlat(&view->lon, &view->lat);
    view->render_lon = view->lon;
    view->render_lat = view->lat;
    view->follow_mode = true;
    view->last_arrow_angle10 = INT32_MIN;
    view->last_pan_px = INT32_MIN;
    view->last_pan_py = INT32_MIN;
    view->paint_zoom = -1;

    view->stride = lv_draw_buf_width_to_stride(view->w, LV_COLOR_FORMAT_RGB565);
    view->cbuf = vmap_malloc((size_t)view->stride * (size_t)view->h);
    if (!view->cbuf) {
        free(view);
        return NULL;
    }

    memset(view->cbuf, 0, (size_t)view->stride * (size_t)view->h);

    view->scratch = (vmap_point_t *)vmap_malloc(sizeof(vmap_point_t) * VMAP_SCRATCH_CAP);
    view->labels = (vmap_label_cand_t *)vmap_malloc(sizeof(vmap_label_cand_t) * VMAP_LABEL_CAP);
    view->label_retained = (vmap_label_retained_t *)vmap_malloc(
        sizeof(vmap_label_retained_t) * VMAP_LABEL_RETAIN_CAP);
    view->cache = vmap_tile_cache_create();
    if (!view->scratch || !view->labels || !view->label_retained || !view->cache) {
        vmap_view_destroy(view);
        return NULL;
    }

    vmap_label_collector_init(&view->label_col, view->labels, VMAP_LABEL_CAP,
        view->label_retained, VMAP_LABEL_RETAIN_CAP,
        view->w, view->h, view->view_w, view->view_h, view->margin);

    view->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(view->canvas, view->cbuf, view->w, view->h,
        LV_COLOR_FORMAT_RGB565);
    lv_obj_remove_style_all(view->canvas);
    lv_obj_set_size(view->canvas, view->w, view->h);
    lv_obj_set_pos(view->canvas, -margin, -margin);
    /* 首帧 render_finish 前不要把 PSRAM 噪声送到 LCD。 */
    lv_obj_add_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);

    vmap_view_set_tile_dir(view, VMAP_TILE_DIR);
    return view;
}

/**
 * @brief 销毁视图：释放画布与瓦片缓存。
 */
void vmap_view_destroy(vmap_view_t * view)
{
    if (!view) {
        return;
    }

    view->track = NULL;

    if (view->canvas) {
        lv_obj_delete(view->canvas);
        view->canvas = NULL;
    }
    if (view->arrow) {
        lv_obj_delete(view->arrow);
    }
    if (view->cbuf) {
        vmap_free(view->cbuf);
    }
    if (view->scratch) {
        vmap_free(view->scratch);
    }
    if (view->labels) {
        vmap_free(view->labels);
    }
    if (view->label_retained) {
        vmap_free(view->label_retained);
    }
    vmap_tile_cache_destroy(view->cache);
    free(view);
}

/**
 * @brief 底层 LVGL canvas 对象。
 */
lv_obj_t * vmap_view_get_obj(const vmap_view_t * view)
{
    return view ? view->canvas : NULL;
}

/**
 * @brief 设置地图目录（网格 .vpk 根）。
 */
void vmap_view_set_tile_dir(vmap_view_t * view, const char * dir)
{
    if (view && view->cache) {
        vmap_tile_cache_set_dir(view->cache, dir);
    }
}

/**
 * @brief 设置中心与缩放；不打开 .vpk。
 */
void vmap_view_set_view(vmap_view_t * view, double lon, double lat, int zoom)
{
    if (!view) {
        return;
    }
    view->lon = lon;
    view->lat = lat;
    view->render_lon = lon;
    view->render_lat = lat;
    view->zoom = zoom;
    /*
     * set_view is an explicit coordinate-system jump (route review/zoom/boot),
     * not a follow rebase.  Never scroll pixels from the previous projection.
     */
    view->paint_zoom = -1;
    if (view->cache) {
        vmap_tile_cache_set_zoom(view->cache, zoom);
    }
}

/**
 * @brief 打开当前经纬度所在 cell 的 region pack。
 */
bool vmap_view_ensure_region(vmap_view_t * view, double lon, double lat)
{
    if (!view || !view->cache) {
        return false;
    }
    return vmap_tile_cache_ensure_region(view->cache, lon, lat);
}

/**
 * @brief 解析瓦片 LRU / region slot 缓存。
 */
vmap_tile_cache_t * vmap_view_tile_cache(vmap_view_t * view)
{
    return view ? view->cache : NULL;
}

/**
 * @brief 当前瓦片 zoom。
 */
int vmap_view_get_zoom(const vmap_view_t * view)
{
    return view ? view->zoom : 0;
}

/**
 * @brief 当前显示倍率。
 */
double vmap_view_get_scale(const vmap_view_t * view)
{
    return view ? view->scale : 1.0;
}

/**
 * @brief 当前跟车中心（经纬度）。
 */
void vmap_view_get_center(const vmap_view_t * view, double * lon, double * lat)
{
    if (!view) {
        if (lon) {
            *lon = 0.0;
        }
        if (lat) {
            *lat = 0.0;
        }
        return;
    }
    if (lon) {
        *lon = view->lon;
    }
    if (lat) {
        *lat = view->lat;
    }
}

/**
 * @brief 改显示倍率。
 */
void vmap_view_set_scale(vmap_view_t * view, double scale)
{
    if (!view) {
        return;
    }
    if (scale < 0.25) {
        scale = 0.25;
    } else if (scale > 8.0) {
        scale = 8.0;
    }
    view->scale = scale;
}

/**
 * @brief 路名标签字体。
 */
void vmap_view_set_font(vmap_view_t * view, const lv_font_t * font)
{
    if (view) {
        view->font = font;
    }
}

static double vmap_clamp_d(double v, double lo, double hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static void vmap_follow_log(const vmap_view_t * view, double lon, double lat)
{
    static uint32_t s_last;

    if (!view) {
        return;
    }
    if (s_last != 0u && lv_tick_elaps(s_last) < 2000u) {
        return;
    }
    s_last = lv_tick_get();
    syslog(LOG_NOTICE, "[map] follow pan=%.0f,%.0f lon=%.5f lat=%.5f",
        view->pan_dx, view->pan_dy, lon, lat);
}

/**
 * @brief 跟车：改中心经纬度。未越 overscan 只平移 canvas。
 */
void vmap_view_set_center(vmap_view_t * view, double lon, double lat)
{
    int now_x = 0;
    int now_y = 0;
    int base_x = 0;
    int base_y = 0;
    double dx;
    double dy;
    double limit;

    if (!view) {
        return;
    }

    vmap_latlong_to_pixel(lat, lon, view->zoom, &now_x, &now_y);
    vmap_latlong_to_pixel(view->render_lat, view->render_lon, view->zoom, &base_x, &base_y);

    dx = (now_x - base_x) * view->scale;
    dy = (now_y - base_y) * view->scale;
    view->lon = lon;
    view->lat = lat;

    /*
     * Follow must not decode tiles. vmap_tile_cache_warm() is a blocking
     * LFS parse (~150ms) on this thread; doing it every GNSS tick once pan
     * crossed the old prefetch threshold pinned bicycle_ui at ~80% idle~10%.
     * New tiles load in vmap_view_render_pump() when the canvas rebases.
     */
    limit = (double)view->margin - (double)VMAP_PAN_GUARD;

    if (dx >= -limit && dx <= limit && dy >= -limit && dy <= limit) {
        bicycle_c_debug_map_soft_pan();
        vmap_view_apply_canvas_pos(view, dx, dy);
        vmap_follow_log(view, lon, lat);
        return;
    }

    /* Soft-pan even when tiles cannot be reloaded (BLE LFS hold / in-flight pump). */
    if (view->pump_phase != 0 || myvendor_mtp_lfs_quiesce()) {
        bicycle_c_debug_map_soft_pan();
        vmap_view_apply_canvas_pos(view,
            vmap_clamp_d(dx, -limit, limit),
            vmap_clamp_d(dy, -limit, limit));
        vmap_follow_log(view, lon, lat);
        return;
    }

    bicycle_c_debug_map_full_render();
    vmap_view_render(view);
}

/**
 * @brief 绑定定位箭头所在 LVGL 层。
 */
void vmap_view_bind_arrow_layer(vmap_view_t * view, lv_obj_t * layer)
{
    if (view) {
        view->arrow_layer = layer;
    }
}

/**
 * @brief 设置箭头图源。
 */
void vmap_view_set_arrow_image(vmap_view_t * view, const void * src)
{
    lv_coord_t aw;
    lv_coord_t ah;

    if (!view || !view->arrow_layer || !src) {
        return;
    }

    if (!view->arrow) {
        view->arrow = lv_img_create(view->arrow_layer);
        lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_img_set_src(view->arrow, src);
    lv_obj_update_layout(view->arrow_layer);
    lv_obj_update_layout(view->arrow);
    aw = lv_obj_get_width(view->arrow);
    ah = lv_obj_get_height(view->arrow);
    lv_img_set_pivot(view->arrow, aw / 2, ah / 2);
    view->last_arrow_angle10 = INT32_MIN;
    /* 新建 img 默认在父对象 (0,0)。跟随模式立刻居中，避免钉在左上角。 */
    if (view->follow_mode) {
        lv_obj_align(view->arrow, LV_ALIGN_CENTER, 0, 0);
        lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief 跟随模式：箭头钉在视口中心。
 */
void vmap_view_set_follow_mode(vmap_view_t * view, bool enable)
{
    if (view) {
        view->follow_mode = enable;
    }
}

/**
 * @brief 按经纬度与航向摆箭头。
 */
void vmap_view_update_arrow(vmap_view_t * view, double lon, double lat, float course_deg)
{
    int32_t sx;
    int32_t sy;
    int32_t angle10;

    if (!view || !view->arrow) {
        return;
    }

    angle10 = (int32_t)(course_deg * 10.0f);
    vmap_label_collector_set_focus(&view->label_col,
        view->w / 2 + (int32_t)lround(view->pan_dx),
        view->h / 2 + (int32_t)lround(view->pan_dy));

    if (view->follow_mode) {
        const bool hidden = lv_obj_has_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);

        if (angle10 == view->last_arrow_angle10 && !hidden) {
            return;
        }
        view->last_arrow_angle10 = angle10;
        lv_obj_align(view->arrow, LV_ALIGN_CENTER, 0, 0);
        lv_img_set_angle(view->arrow, angle10);
        lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(view->arrow);
        return;
    }

    if (!vmap_view_geo_to_viewport(view, lon, lat, &sx, &sy)) {
        lv_obj_add_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (angle10 == view->last_arrow_angle10
        && sx == lv_obj_get_x(view->arrow) + lv_obj_get_width(view->arrow) / 2
        && sy == lv_obj_get_y(view->arrow) + lv_obj_get_height(view->arrow) / 2) {
        return;
    }

    view->last_arrow_angle10 = angle10;
    lv_obj_set_pos(view->arrow,
        sx - lv_obj_get_width(view->arrow) / 2,
        sy - lv_obj_get_height(view->arrow) / 2);
    lv_img_set_angle(view->arrow, angle10);
    lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(view->arrow);
}

/**
 * @brief 显示或隐藏定位箭头。
 */
void vmap_view_set_arrow_visible(vmap_view_t * view, bool visible)
{
    if (!view || !view->arrow) {
        return;
    }

    if (visible) {
        lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 把箭头提到 map_area 最前。
 */
void vmap_view_bring_arrow_to_front(vmap_view_t * view)
{
    if (view && view->arrow) {
        lv_obj_move_foreground(view->arrow);
    }
}

/**
 * @brief 丢弃瓦片索引/arena 并重绘。
 * @return 0 成功，负 errno 失败。
 */
void vmap_view_reload_storage(vmap_view_t * view)
{
    if (!view || !view->cache || myvendor_mtp_lfs_quiesce()) {
        return;
    }

    vmap_view_set_tile_dir(view, VMAP_TILE_DIR);
    vmap_tile_cache_set_zoom(view->cache, view->zoom);
    render_begin(view, true, true);
    vmap_view_bring_arrow_to_front(view);
}

/**
 * @brief 关闭已打开的地图分片句柄。
 */
void vmap_view_release_storage(vmap_view_t * view)
{
    if (!view || !view->cache) {
        return;
    }
    vmap_tile_cache_release_lfs(view->cache);
    /* 句柄已关，半帧 rebase 不能再解算；MTP reload 会 force_clear 重画。 */
    if (view->pump_phase != 0) {
        view->pump_phase = 0;
        view->pump_need_region = 0;
        view->pump_dirty_n = 0;
        view->restamp_pending = 0;
        dvfs_map_busy(false);
        syslog(LOG_NOTICE, "[map] abort pump (lfs released)");
    }
}

static void view_canvas(const vmap_view_t * view, vmap_canvas_t * canvas,
    bool clip_dirty)
{
    uint8_t i;

    canvas->buf = (uint16_t *)view->cbuf;
    canvas->w = view->w;
    canvas->h = view->h;
    canvas->stride = view->stride;
    vmap_render_clip_reset(canvas);
    if (!clip_dirty) {
        return;
    }
    for (i = 0; i < view->pump_dirty_n; i++) {
        vmap_render_clip_add(canvas,
            view->pump_dirty_x1[i], view->pump_dirty_y1[i],
            view->pump_dirty_x2[i], view->pump_dirty_y2[i]);
    }
}

static void pump_dirty_add(vmap_view_t * view, int32_t x1, int32_t y1,
    int32_t x2, int32_t y2)
{
    uint8_t i;

    if (!view || x1 >= x2 || y1 >= y2) {
        return;
    }

    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 > view->w) {
        x2 = view->w;
    }
    if (y2 > view->h) {
        y2 = view->h;
    }
    if (x1 >= x2 || y1 >= y2 || view->pump_dirty_n >= VMAP_RENDER_DIRTY_MAX) {
        return;
    }

    i = view->pump_dirty_n;
    view->pump_dirty_x1[i] = x1;
    view->pump_dirty_y1[i] = y1;
    view->pump_dirty_x2[i] = x2;
    view->pump_dirty_y2[i] = y2;
    view->pump_dirty_n++;
}

static void pump_dirty_from_scroll(vmap_view_t * view, int32_t sx, int32_t sy)
{
    const int32_t pad = (int32_t)VMAP_RENDER_DIRTY_PAD;

    view->pump_dirty_n = 0;
    if (sx > 0) {
        pump_dirty_add(view, view->w - sx - pad, 0, view->w, view->h);
    } else if (sx < 0) {
        pump_dirty_add(view, 0, 0, -sx + pad, view->h);
    }
    if (sy > 0) {
        pump_dirty_add(view, 0, view->h - sy - pad, view->w, view->h);
    } else if (sy < 0) {
        pump_dirty_add(view, 0, 0, view->w, -sy + pad);
    }
}

static bool pump_rects_overlap(int32_t ax1, int32_t ay1, int32_t ax2, int32_t ay2,
    int32_t bx1, int32_t by1, int32_t bx2, int32_t by2)
{
    return ax1 < bx2 && ax2 > bx1 && ay1 < by2 && ay2 > by1;
}

static bool pump_tile_dirty(const vmap_view_t * view, int32_t tx, int32_t ty)
{
    const int32_t tile_size = 256;
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
    uint8_t i;

    if (!view || view->pump_dirty_n == 0) {
        return true;
    }

    x1 = (int32_t)lround((tx * tile_size - view->pump_cx) * view->scale
        + view->w / 2.0);
    y1 = (int32_t)lround((ty * tile_size - view->pump_cy) * view->scale
        + view->h / 2.0);
    x2 = (int32_t)lround(x1 + 256.0 * view->scale);
    y2 = (int32_t)lround(y1 + 256.0 * view->scale);

    for (i = 0; i < view->pump_dirty_n; i++) {
        if (pump_rects_overlap(x1, y1, x2, y2,
                view->pump_dirty_x1[i], view->pump_dirty_y1[i],
                view->pump_dirty_x2[i], view->pump_dirty_y2[i])) {
            return true;
        }
    }
    return false;
}

static bool pump_advance(vmap_view_t * view)
{
    view->pump_ty--;
    if (view->pump_ty < view->pump_ty_min) {
        view->pump_tx--;
        view->pump_ty = view->pump_ty_max;
    }
    return view->pump_tx >= view->pump_tx_min;
}

static void render_begin(vmap_view_t * view, bool reset_view, bool force_clear)
{
    vmap_canvas_t canvas;
    const double origin_lon = reset_view ? view->lon : view->render_lon;
    const double origin_lat = reset_view ? view->lat : view->render_lat;
    int32_t sx;
    int32_t sy;
    bool scrolled = false;

    /* fopen/.vpk index belongs on the first pump tick, not GNSS/REC/set_view. */

    dvfs_map_busy(true);
    view->pump_dirty_n = 0;
    sx = (view->last_pan_px == INT32_MIN) ? (int32_t)lround(view->pan_dx)
                                         : view->last_pan_px;
    sy = (view->last_pan_py == INT32_MIN) ? (int32_t)lround(view->pan_dy)
                                         : view->last_pan_py;

    if (reset_view) {
        /*
         * A hidden page can jump much farther than the last on-screen pan.
         * Derive the pixel migration from the two geographic origins instead
         * of trusting last_pan_px.  Otherwise a 1 km jump with a 14 px old pan
         * scrolls only 14 px, then marks the whole interior as the new origin.
         */
        if (!force_clear && view->render_rev > 0
            && view->paint_zoom == view->zoom
            && view->paint_scale == view->scale) {
            int old_x = 0;
            int old_y = 0;
            int new_x = 0;
            int new_y = 0;

            vmap_latlong_to_pixel(view->render_lat, view->render_lon,
                view->zoom, &old_x, &old_y);
            vmap_latlong_to_pixel(origin_lat, origin_lon,
                view->zoom, &new_x, &new_y);
            sx = (int32_t)lround((new_x - old_x) * view->scale);
            sy = (int32_t)lround((new_y - old_y) * view->scale);
        }

        bool can_scroll = !force_clear && view->render_rev > 0
            && view->paint_zoom == view->zoom
            && view->paint_scale == view->scale
            && (sx != 0 || sy != 0)
            && sx > -view->w && sx < view->w
            && sy > -view->h && sy < view->h;

        view->render_lon = origin_lon;
        view->render_lat = origin_lat;
        view_canvas(view, &canvas, false);
        if (can_scroll) {
            vmap_render_scroll(&canvas, sx, sy, vmap_style_bg565());
            pump_dirty_from_scroll(view, sx, sy);
            vmap_label_retained_scroll(&view->label_col, sx, sy,
                VMAP_RENDER_DIRTY_MAX, &view->pump_dirty_n,
                view->pump_dirty_x1, view->pump_dirty_y1,
                view->pump_dirty_x2, view->pump_dirty_y2);
            scrolled = true;
            lv_obj_invalidate(view->canvas);
        } else {
            vmap_render_clear(&canvas, vmap_style_bg565());
            vmap_label_retained_reset(&view->label_col);
        }

        view->pan_dx = 0.0;
        view->pan_dy = 0.0;
        view->last_pan_px = INT32_MIN;
        view->last_pan_py = INT32_MIN;
        vmap_view_apply_canvas_pos(view, 0.0, 0.0);
    } else {
        /* keep-pan redraw repaints every base tile, erasing all baked labels. */
        vmap_label_retained_reset(&view->label_col);
    }

    vmap_label_collector_reset(&view->label_col);
    view->label_col.clip_n = 0;
    vmap_label_collector_set_focus(&view->label_col,
        view->w / 2 + (int32_t)lround(view->pan_dx),
        view->h / 2 + (int32_t)lround(view->pan_dy));

    vmap_latlong_to_pixel(origin_lat, origin_lon, view->zoom,
        &view->pump_cx, &view->pump_cy);
    tile_range(view->pump_cx, view->pump_cy, view->w, view->h, view->scale, 1,
        &view->pump_tx_min, &view->pump_ty_min,
        &view->pump_tx_max, &view->pump_ty_max);
    view->pump_tx = view->pump_tx_max;
    view->pump_ty = view->pump_ty_max;
    view->pump_phase = 1;
    view->pump_need_region = 1;

    if (scrolled) {
        syslog(LOG_NOTICE,
            "[map] rebase scroll=%d,%d dirty=%u labels=%d z=%d",
            (int)sx, (int)sy, (unsigned)view->pump_dirty_n,
            (int)view->label_col.retained_count, view->zoom);
    }
}

static bool render_next_tile(vmap_view_t * view)
{
    const int32_t tile_size = 256;
    const double scale = view->scale;
    vmap_canvas_t canvas;
    vmap_tile_t tile;
    float base_x;
    float base_y;
    int32_t tx;
    int32_t ty;
    int32_t ix1;
    int32_t iy1;
    int32_t ix2;
    int32_t iy2;

    if (view->pump_need_region) {
        view->pump_need_region = 0;
        if (view->cache) {
            myvendor_watchdog_ui_beat();
            myvendor_watchdog_busy_pump();
            vmap_tile_cache_ensure_region(view->cache,
                view->render_lon, view->render_lat);
            myvendor_watchdog_ui_beat();
        }
        return true;
    }

    while (view->pump_tx >= view->pump_tx_min) {
        tx = view->pump_tx;
        ty = view->pump_ty;

        if (!pump_tile_dirty(view, tx, ty)) {
            if (view->font
                && vmap_tile_cache_cached(view->cache, view->zoom, (int)tx, (int)ty)
                && vmap_tile_cache_get(view->cache, view->zoom, (int)tx, (int)ty,
                    &tile)) {
                base_x = (float)((tx * tile_size - view->pump_cx) * scale
                    + view->w / 2.0);
                base_y = (float)((ty * tile_size - view->pump_cy) * scale
                    + view->h / 2.0);
                vmap_label_collector_add_tile(&view->label_col, &tile,
                    base_x, base_y, (float)scale);
            }
            if (!pump_advance(view)) {
                return false;
            }
            continue;
        }

        if (!vmap_tile_cache_cached(view->cache, view->zoom, (int)tx, (int)ty)) {
            if (!vmap_tile_cache_warm(view->cache, view->zoom,
                    (int)tx, (int)ty)) {
                /* 邻格 heap 暂时放不下：跳过，继续画已在 PSRAM 的瓦片。 */
                if (!pump_advance(view)) {
                    return false;
                }
                continue;
            }
            return true;
        }

        view_canvas(view, &canvas, true);
        base_x = (float)((tx * tile_size - view->pump_cx) * scale
            + view->w / 2.0);
        base_y = (float)((ty * tile_size - view->pump_cy) * scale
            + view->h / 2.0);

        if (vmap_tile_cache_get(view->cache, view->zoom, (int)tx, (int)ty,
                &tile)) {
            vmap_render_draw_tile(&canvas, &tile, base_x, base_y, (float)scale,
                view->scratch, VMAP_SCRATCH_CAP);
            if (view->font) {
                vmap_label_collector_add_tile(&view->label_col, &tile,
                    base_x, base_y, (float)scale);
            }

            ix1 = (int32_t)floor((double)base_x);
            iy1 = (int32_t)floor((double)base_y);
            ix2 = (int32_t)ceil((double)base_x + 256.0 * scale);
            iy2 = (int32_t)ceil((double)base_y + 256.0 * scale);
            if (view->pump_dirty_n == 0) {
                vmap_view_invalidate_canvas_area(view, ix1, iy1, ix2, iy2);
            } else {
                uint8_t di;

                for (di = 0; di < view->pump_dirty_n; di++) {
                    int32_t vx1 = ix1 > view->pump_dirty_x1[di]
                        ? ix1 : view->pump_dirty_x1[di];
                    int32_t vy1 = iy1 > view->pump_dirty_y1[di]
                        ? iy1 : view->pump_dirty_y1[di];
                    int32_t vx2 = ix2 < (view->pump_dirty_x2[di] - 1)
                        ? ix2 : (view->pump_dirty_x2[di] - 1);
                    int32_t vy2 = iy2 < (view->pump_dirty_y2[di] - 1)
                        ? iy2 : (view->pump_dirty_y2[di] - 1);

                    if (vx1 <= vx2 && vy1 <= vy2) {
                        vmap_view_invalidate_canvas_area(view, vx1, vy1, vx2, vy2);
                    }
                }
            }
        }

        if (!pump_advance(view)) {
            return false;
        }
        return true;
    }

    return false;
}

/*
 * Canvas stack, bottom → top:
 *   map tiles (already in buffer) → nav → labels → REC line → REC numbers.
 * clip_dirty: follow-rebase only restamps the new edge, so interior ink is not
 * redrawn fat. Labels use the same dirty rects.
 */
static void stamp_overlays(vmap_view_t * view, bool labels)
{
    vmap_canvas_t canvas;
    lv_area_t label_drawn;
    bool labels_drawn = false;

    if (!view || !view->canvas || !view->cbuf) {
        return;
    }

    view_canvas(view, &canvas, true);

#if VMAP_ROUTE_ENABLE
    if (view->route) {
        vmap_route_paint_to_canvas(view->route, &canvas, view);
    }
#endif

    if (labels && view->font && view->label_col.count > 0) {
        labels_drawn = vmap_label_paint_to_canvas(
            view->canvas, &view->label_col, view->font,
            view->scale, view->pump_dirty_n,
            view->pump_dirty_x1, view->pump_dirty_y1,
            view->pump_dirty_x2, view->pump_dirty_y2, &label_drawn);
    }

#if VMAP_TRACK_ENABLE
    if (view->track) {
        if (labels_drawn && canvas.clip_n > 0u) {
            int32_t x1 = label_drawn.x1;
            int32_t y1 = label_drawn.y1;
            int32_t x2 = label_drawn.x2 + 1;
            int32_t y2 = label_drawn.y2 + 1;
            uint8_t i;

            /*
             * New labels may be outside the tile dirty edge. Expand the REC
             * clip to their union so track ink is restored above those words.
             */
            for (i = 0; i < canvas.clip_n; i++) {
                if (canvas.clip_x1[i] < x1) {
                    x1 = canvas.clip_x1[i];
                }
                if (canvas.clip_y1[i] < y1) {
                    y1 = canvas.clip_y1[i];
                }
                if (canvas.clip_x2[i] > x2) {
                    x2 = canvas.clip_x2[i];
                }
                if (canvas.clip_y2[i] > y2) {
                    y2 = canvas.clip_y2[i];
                }
            }
            vmap_render_clip_reset(&canvas);
            vmap_render_clip_add(&canvas, x1, y1, x2, y2);
        }
        vmap_track_paint_to_canvas(view->track, &canvas);
    }
#endif
}

void vmap_view_stamp_nav_then_track(vmap_view_t * view)
{
    stamp_overlays(view, false);
    if (view && view->canvas) {
        lv_obj_invalidate(view->canvas);
    }
}

static void render_finish(vmap_view_t * view)
{
    stamp_overlays(view, true);

#if defined(CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE) && CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE
    bicycle_inval_probe_tag("vmap_render");
#endif
    view->render_rev++;
    view->paint_zoom = view->zoom;
    view->paint_scale = view->scale;
    view->pump_phase = 0;
    view->pump_need_region = 0;
    view->pump_dirty_n = 0;
    lv_obj_clear_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);

    if (view->restamp_pending && !myvendor_mtp_lfs_quiesce()) {
        view->restamp_pending = 0;
        lv_obj_invalidate(view->canvas);
        render_begin(view, false, false);
        return;
    }
    dvfs_map_busy(false);
    lv_obj_invalidate(view->canvas);
    vmap_view_bring_arrow_to_front(view);
}

/**
 * @brief 开始一帧栅格化（只 begin，由 pump 画完）。
 */
void vmap_view_render(vmap_view_t * view)
{
    if (!view || !view->canvas || !view->cbuf || myvendor_mtp_lfs_quiesce()) {
        return;
    }

    view->restamp_pending = 0;
    render_begin(view, true, false);
}

void vmap_view_redraw_keep_pan(vmap_view_t * view)
{
    if (!view || !view->canvas || !view->cbuf || myvendor_mtp_lfs_quiesce()) {
        return;
    }
    if (view->render_rev == 0) {
        return;
    }
    if (view->pump_phase != 0) {
        view->restamp_pending = 1;
        syslog(LOG_NOTICE, "[map] restamp queued (pump busy)");
        return;
    }

    view->restamp_pending = 0;
    render_begin(view, false, false);
    syslog(LOG_NOTICE, "[map] restamp keep_pan z=%d tiles tx=%d..%d ty=%d..%d",
        view->zoom, (int)view->pump_tx_min, (int)view->pump_tx_max,
        (int)view->pump_ty_min, (int)view->pump_ty_max);
}

bool vmap_view_render_pump(vmap_view_t * view, unsigned max_tiles)
{
    unsigned n;
    uint32_t t0;

    if (!view || !view->canvas || !view->cbuf || myvendor_mtp_lfs_quiesce()) {
        return true;
    }

    if (view->pump_phase == 0) {
        return true;
    }

    if (max_tiles == 0) {
        max_tiles = VMAP_RENDER_PUMP_MAX_TILES;
    }

    t0 = lv_tick_get();
    myvendor_watchdog_ui_beat();
    for (n = 0; n < max_tiles && view->pump_phase != 0; n++) {
        if (n > 0 && lv_tick_elaps(t0) >= VMAP_RENDER_PUMP_BUDGET_MS) {
            break;
        }
        if (!render_next_tile(view)) {
            render_finish(view);
            break;
        }
        if (lv_tick_elaps(t0) >= VMAP_RENDER_PUMP_BUDGET_MS) {
            break;
        }
    }

    return view->pump_phase == 0;
}

bool vmap_view_render_busy(const vmap_view_t * view)
{
    return view != NULL && view->pump_phase != 0;
}

/**
 * @brief 地图存储世代（MTP reload 后递增）。
 */
uint32_t vmap_view_map_revision(const vmap_view_t * view)
{
    return view ? view->map_rev : 0;
}

/**
 * @brief 已完成的渲染帧计数；0 表示从未 render_finish。
 */
uint32_t vmap_view_render_revision(const vmap_view_t * view)
{
    return view ? view->render_rev : 0;
}
