/**
 * @file vmap_geo.c
 * @brief vmap geo 模块。
 */

#include "vmap_geo.h"

#include <math.h>

/**
 * @brief vmap geo haversine m。
 */
double vmap_geo_haversine_m(double lon1, double lat1, double lon2, double lat2)
{
    const double r = 6371000.0;
    const double dlat = (lat2 - lat1) * M_PI / 180.0;
    const double dlon = (lon2 - lon1) * M_PI / 180.0;
    const double lat1r = lat1 * M_PI / 180.0;
    const double lat2r = lat2 * M_PI / 180.0;
    const double a = sin(dlat / 2.0) * sin(dlat / 2.0)
        + cos(lat1r) * cos(lat2r) * sin(dlon / 2.0) * sin(dlon / 2.0);

    return 2.0 * r * asin(fmin(1.0, sqrt(a)));
}

/**
 * @brief vmap geo bearing deg。
 */
float vmap_geo_bearing_deg(double lon1, double lat1, double lon2, double lat2)
{
    const double lat1r = lat1 * M_PI / 180.0;
    const double lat2r = lat2 * M_PI / 180.0;
    const double dlon = (lon2 - lon1) * M_PI / 180.0;
    const double y = sin(dlon) * cos(lat2r);
    const double x = cos(lat1r) * sin(lat2r)
        - sin(lat1r) * cos(lat2r) * cos(dlon);
    float brg = (float)(atan2(y, x) * 180.0 / M_PI);

    if (brg < 0.0f) {
        brg += 360.0f;
    }
    return brg;
}

/**
 * @brief vmap geo angle delta deg。
 */
float vmap_geo_angle_delta_deg(float from_deg, float to_deg)
{
    float d = to_deg - from_deg;

    while (d > 180.0f) {
        d -= 360.0f;
    }
    while (d < -180.0f) {
        d += 360.0f;
    }
    return d;
}

/**
 * @brief 局部等距圆柱平面的经度尺度：cos(lat) 的**多项式近似**。
 *
 * @details 为什么不用 `cos()`：`vmap_geo_point_to_seg_m()` 在导航更新里是**逐段**
 *          调用的（一条 2000 段的路线每 0.5 s 扫一遍）。这颗核只有 FP32，
 *          **双精度超越函数是软件实现**（一次 ~5 µs），于是"每段 3 次 haversine +
 *          2 次 bearing"= 每段 15~20 次 FP64 三角函数 ⇒ 实测**一次投影 195 ms**
 *          （2026-10-28 `[gpx] navupd=`）。8 阶展开在 |lat| ≤ 80° 时绝对误差
 *          < 1e-5（余项 (80π/180)^10/10! ≈ 7.8e-6），对 35~50 m 的偏航判据完全够用。
 */
static double vmap_geo_cos_deg(double deg)
{
    const double x = deg * (M_PI / 180.0);
    const double x2 = x * x;

    return 1.0 - x2 * 0.5 + x2 * x2 * (1.0 / 24.0)
        - x2 * x2 * x2 * (1.0 / 720.0)
        + x2 * x2 * x2 * x2 * (1.0 / 40320.0);
}

#define VMAP_GEO_M_PER_DEG_LAT 110540.0

/**
 * @brief vmap geo point to seg m。
 *
 * @note 用局部等距圆柱平面算距离与投影参数。纬向用 `110540 m/°`，比球面模型
 *       （R=6371 km ⇒ 111195 m/°）低约 0.59%。**已测样本**：22~45°N 的单段距离
 *       误差 0.59~0.62%；高纬会更大（79°N、段长 400 m 时 0.91%）⇒ 没有全域上界，
 *       但在产品纬度带可以按 0.7% 估。这是刻意的扁率近似，不是 bug。
 *       **不要再改回 haversine + bearing**：那是逐段 FP64 三角函数的开销，
 *       见上面 cos 近似的说明。
 */
double vmap_geo_point_to_seg_m(double px, double py,
    double ax, double ay, double bx, double by, double * t_out)
{
    const double kx = 111320.0 * vmap_geo_cos_deg(ay);
    const double vx = (bx - ax) * kx;
    const double vy = (by - ay) * VMAP_GEO_M_PER_DEG_LAT;
    const double wx = (px - ax) * kx;
    const double wy = (py - ay) * VMAP_GEO_M_PER_DEG_LAT;
    const double seg2 = vx * vx + vy * vy;
    double t = 0.0;

    if (seg2 > 1e-6) {
        t = (wx * vx + wy * vy) / seg2;
        if (t < 0.0) {
            t = 0.0;
        } else if (t > 1.0) {
            t = 1.0;
        }
    }

    if (t_out) {
        *t_out = t;
    }

    {
        const double dx = wx - vx * t;
        const double dy = wy - vy * t;

        return sqrt(dx * dx + dy * dy);
    }
}

#if 0   /* 旧的逐段 haversine+bearing 实现（保留作对照） */
double vmap_geo_point_to_seg_m_slow(double px, double py,
    double ax, double ay, double bx, double by, double * t_out)
{
    const double seg_m = vmap_geo_haversine_m(ax, ay, bx, by);
    float brg = vmap_geo_bearing_deg(ax, ay, bx, by);
    float brg_p = vmap_geo_bearing_deg(ax, ay, px, py);
    const double ap_m = vmap_geo_haversine_m(ax, ay, px, py);
    double t = 0.0;

    if (seg_m < 0.5) {
        if (t_out) {
            *t_out = 0.0;
        }
        return ap_m;
    }

    {
        const double along = ap_m * cos((brg_p - brg) * M_PI / 180.0);

        t = along / seg_m;
        if (t < 0.0) {
            t = 0.0;
        } else if (t > 1.0) {
            t = 1.0;
        }
    }

    if (t_out) {
        *t_out = t;
    }

    {
        const double mid_lon = ax + (bx - ax) * t;
        const double mid_lat = ay + (by - ay) * t;

        return vmap_geo_haversine_m(px, py, mid_lon, mid_lat);
    }
}
#endif  /* 旧的逐段 haversine+bearing 实现 */

/**
 * @brief vmap geo project polyline m。
 */
double vmap_geo_project_polyline_m(const double * lons, const double * lats,
    uint32_t n, double lon, double lat, uint32_t * seg_index, double * t_on_seg)
{
    double best_d = 1e18;
    uint32_t best_i = 0;
    double best_t = 0.0;

    if (!lons || !lats || n < 2) {
        if (seg_index) {
            *seg_index = 0;
        }
        if (t_on_seg) {
            *t_on_seg = 0.0;
        }
        return 1e18;
    }

    for (uint32_t i = 0; i + 1 < n; i++) {
        double t = 0.0;
        const double d = vmap_geo_point_to_seg_m(lon, lat,
            lons[i], lats[i], lons[i + 1], lats[i + 1], &t);

        if (d < best_d) {
            best_d = d;
            best_i = i;
            best_t = t;
        }
    }

    if (seg_index) {
        *seg_index = best_i;
    }
    if (t_on_seg) {
        *t_on_seg = best_t;
    }
    return best_d;
}
