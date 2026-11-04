/*
 * route_stub.c — 宿主机上用最小实现替代 vmap_route.c 的路线对象。
 *
 * 只实现 vmap_route_nav.c 直接调用的那 11 个 API，语义与真实实现一致：
 *   - total_m 由调用方传入（真实实现是 corridor 各段 length_cm 之和）；
 *   - set_progress_match / effective_along_m 逐字复刻 vmap_route.c:1319-1397；
 *   - 其余（plan/apply/clear/is_active/points/maneuvers）只保留导航匹配需要的字段。
 *
 * 刻意**不**实现真正的 Dijkstra/网络：回放只需要一条给定的折线。
 */

#include "route_stub.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "vmap_geo.h"

struct vmap_route {
    vmap_route_pt_t * pts;
    uint32_t pt_count;
    double total_m;
    bool active;
    vmap_route_maneuver_t * mans;
    uint32_t man_count;
    bool progress_valid;
    uint32_t progress_seg;
    double anchor_lon;
    double anchor_lat;
    double progress_lon;
    double progress_lat;
};

vmap_route_t * route_stub_create(void)
{
    return (vmap_route_t *)calloc(1, sizeof(vmap_route_t));
}

void route_stub_destroy(vmap_route_t * r)
{
    if (!r) {
        return;
    }
    free(r->pts);
    free(r->mans);
    free(r);
}

bool route_stub_set(vmap_route_t * r, const vmap_route_pt_t * pts,
    uint32_t n, double total_m)
{
    if (!r || !pts || n < 2u) {
        return false;
    }
    free(r->pts);
    r->pts = (vmap_route_pt_t *)malloc((size_t)n * sizeof(vmap_route_pt_t));
    if (!r->pts) {
        return false;
    }
    memcpy(r->pts, pts, (size_t)n * sizeof(vmap_route_pt_t));
    r->pt_count = n;
    r->total_m = total_m;
    r->active = true;
    r->progress_valid = false;
    r->progress_seg = 0;
    return true;
}

uint32_t route_stub_progress_seg(const vmap_route_t * r)
{
    return r ? r->progress_seg : 0u;
}

/* ---- vmap_route_nav.c 依赖的 API ---- */

bool vmap_route_is_active(const vmap_route_t * route)
{
    return route && route->active;
}

uint32_t vmap_route_point_count(const vmap_route_t * route)
{
    return route ? route->pt_count : 0u;
}

const vmap_route_pt_t * vmap_route_points(const vmap_route_t * route)
{
    return route ? route->pts : NULL;
}

double vmap_route_total_length_m(const vmap_route_t * route)
{
    return route ? route->total_m : 0.0;
}

uint32_t vmap_route_maneuver_count(const vmap_route_t * route)
{
    return route ? route->man_count : 0u;
}

const vmap_route_maneuver_t * vmap_route_maneuvers(const vmap_route_t * route)
{
    return route ? route->mans : NULL;
}

void vmap_route_set_progress_match(vmap_route_t * route, double lon, double lat,
    uint32_t seg, double seg_t)
{
    if (!route || !vmap_route_is_active(route)
        || seg + 1u >= route->pt_count) {
        return;
    }

    if (seg_t < 0.0) {
        seg_t = 0.0;
    } else if (seg_t > 1.0) {
        seg_t = 1.0;
    }

    route->progress_valid = true;
    route->anchor_lon = lon;
    route->anchor_lat = lat;
    route->progress_seg = seg;
    route->progress_lon = route->pts[seg].lon
        + (route->pts[seg + 1u].lon - route->pts[seg].lon) * seg_t;
    route->progress_lat = route->pts[seg].lat
        + (route->pts[seg + 1u].lat - route->pts[seg].lat) * seg_t;
}

double vmap_route_effective_along_m(const vmap_route_t * route,
    double polyline_along_m)
{
    double connector_m;

    if (!route || !route->progress_valid) {
        return polyline_along_m;
    }

    connector_m = vmap_geo_haversine_m(route->anchor_lon, route->anchor_lat,
        route->progress_lon, route->progress_lat);
    if (connector_m > 2.0 && polyline_along_m < connector_m + 5.0) {
        return polyline_along_m - connector_m;
    }
    return polyline_along_m;
}

void vmap_route_apply(vmap_route_t * route, const vmap_route_pt_t * pts,
    uint32_t pt_count, double total_m,
    const vmap_route_maneuver_t * maneuvers, uint32_t maneuver_count)
{
    (void)maneuvers;
    (void)maneuver_count;
    route_stub_set(route, pts, pt_count, total_m);
}

void vmap_route_clear(vmap_route_t * route)
{
    if (!route) {
        return;
    }
    route->active = false;
    route->progress_valid = false;
    route->pt_count = 0;
}

bool vmap_route_plan(vmap_route_t * route, double from_lon, double from_lat,
    double to_lon, double to_lat)
{
    (void)from_lon;
    (void)from_lat;
    (void)to_lon;
    (void)to_lat;
    return route != NULL;
}
