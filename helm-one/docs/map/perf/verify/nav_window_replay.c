/*
 * nav_window_replay.c — 宿主回放：vmap_route_nav_update() 的"缓存段号 + 窗口搜索"
 * 在 AHEAD=64 与 AHEAD=32（BACK 均为 8）下的逐点选段差异。
 *
 * 目的
 * ----
 * 设备端 `vmap_route_nav_update()` 每 500 ms 一次、单次 17 ms（WP-8 `[gpx] navupd=`），
 * 其中窗口扫描 `hint-8 … hint+64` 共 73 段是最贵的一段。把 AHEAD 降到 32 只剩 41 段，
 * 理论上单次耗时减半；但窗口变窄可能选到"错的段"。本程序用真实 GPX 轨迹逐点回放，
 * 证明（或否证）两个窗口的选段完全一致。
 *
 * 忠实度
 * ------
 * 下面 geo_* 三个函数与 nav_update() 的窗口搜索是
 * `boards/sf32lb52/my_vendor/ui/bicycle/src/vmap/vmap_geo.c` 与 `vmap_route_nav.c`
 * 的逐行复刻（含 110540 m/° 的扁率近似、8 阶 cos 多项式、严格 `<` 的并列取靠前段、
 * 连续性惩罚常数、acquire 分支的 stride/192/96、effective_along 的 connector 修正）。
 * 没有用 haversine 替换 point_to_seg —— 两个窗口必须吃**同一套**判据，否则比对无意义。
 *
 * 输入
 * ----
 * 真实轨迹：mkfs/lfs/mtp/record/TRK_20260908_073948.gpx（870 点，6.04 km）
 *           mkfs/lfs/mtp/record/TRK_20260908_174312.gpx（1446 点，7.83 km）
 * 每个场景把轨迹点序列当"路线折线"，把（可能变换过的）轨迹点序列当"骑行轨迹"。
 * 路线/轨迹的变换（前半程、抽稀、加密、偏移、缺口、折返）在场景表里写清楚，
 * 标 SYNTH 的是由真实轨迹加工出来的压力变体，不是原始样本。
 *
 * 编译（宿主机，无板级依赖）
 *   gcc -O2 -std=c11 -Wall -Wextra -o /tmp/nav_window_replay nav_window_replay.c -lm
 * 运行
 *   /tmp/nav_window_replay <repo>/boards/sf32lb52/my_vendor/mkfs/lfs/mtp/record [csv-out]
 * 退出码
 *   0 = 所有场景逐点选段完全一致；1 = 有不一致（明细打在 stdout/CSV）。
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ 常量 */

/* vmap_route_nav.c:18-25 逐字复制 */
#define VMAP_NAV_MATCH_BACK_SEGS     8u
#define VMAP_NAV_MATCH_AHEAD_SEGS   64u
#define VMAP_NAV_MATCH_ACQUIRE_MAX 192u
#define VMAP_NAV_MATCH_ACQUIRE_HALF 96u
#define VMAP_NAV_MATCH_BACK_TOL_M   40.0
#define VMAP_NAV_MATCH_AHEAD_TOL_M 200.0
#define VMAP_NAV_MATCH_MAX_PENALTY 100.0
#define VMAP_NAV_MATCH_PROGRESS_W    2.0

/* vmap_config.h */
#define VMAP_ROUTE_ARRIVE_M 45.0
#define VMAP_NAV_ARRIVE_NEAR_M 300.0

#define MAX_PTS 65536u

/* ------------------------------------------------------------------ 几何 */

/* vmap_geo.c:71-79 逐字复制：cos 的 8 阶多项式（FP32 核上省 FP64 超越函数）。 */
static double geo_cos_deg(double deg)
{
    const double x = deg * (M_PI / 180.0);
    const double x2 = x * x;

    return 1.0 - x2 * 0.5 + x2 * x2 * (1.0 / 24.0)
        - x2 * x2 * x2 * (1.0 / 720.0)
        + x2 * x2 * x2 * x2 * (1.0 / 40320.0);
}

#define GEO_M_PER_DEG_LAT 110540.0

/* vmap_geo.c:93-123 逐字复制 */
static double geo_point_to_seg_m(double px, double py,
    double ax, double ay, double bx, double by, double * t_out)
{
    const double kx = 111320.0 * geo_cos_deg(ay);
    const double vx = (bx - ax) * kx;
    const double vy = (by - ay) * GEO_M_PER_DEG_LAT;
    const double wx = (px - ax) * kx;
    const double wy = (py - ay) * GEO_M_PER_DEG_LAT;
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

/* vmap_geo.c:13-24 逐字复制 */
static double geo_haversine_m(double lon1, double lat1, double lon2, double lat2)
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

/* ------------------------------------------------------------------ 数据 */

typedef struct {
    double lon;
    double lat;
} pt_t;

typedef struct {
    pt_t * p;
    uint32_t n;
    double * cum;   /* cum[i] = 从 0 到点 i 的折线长（m），i < n */
    double total;   /* cum[n-1] */
} poly_t;

typedef struct {
    /* 与 struct vmap_route_nav 里参与匹配的字段一一对应 */
    uint32_t track_seg_hint;
    double track_seg_start_m;
    bool track_hint_valid;
    double along_m;      /* state.along_m（effective，上一点的） */
    bool active;         /* state.active */
    /* route 侧 progress_*（vmap_route_set_progress_match 写的） */
    double anchor_lon, anchor_lat;
    double progress_lon, progress_lat;
    bool progress_valid;
    uint32_t progress_seg;
    /* 输出 */
    double off_m, remain_m;
    bool arrived;
    uint32_t seg;
} nav_t;

typedef struct {
    uint32_t seg;
    double along;
    double off;
    double remain;
    bool arrived;
} nav_out_t;

/* ------------------------------------------------------------------ 路线 */

static bool poly_build(poly_t * r, const pt_t * src, uint32_t n)
{
    if (n < 2) {
        return false;
    }
    r->p = (pt_t *)malloc((size_t)n * sizeof(pt_t));
    r->cum = (double *)malloc((size_t)n * sizeof(double));
    if (!r->p || !r->cum) {
        return false;
    }
    memcpy(r->p, src, (size_t)n * sizeof(pt_t));
    r->n = n;
    r->cum[0] = 0.0;
    for (uint32_t i = 1; i < n; i++) {
        r->cum[i] = r->cum[i - 1]
            + geo_haversine_m(src[i - 1].lon, src[i - 1].lat, src[i].lon, src[i].lat);
    }
    r->total = r->cum[n - 1];
    return true;
}

static void poly_free(poly_t * r)
{
    free(r->p);
    free(r->cum);
    memset(r, 0, sizeof(*r));
}

static void nav_reset(nav_t * nav)
{
    memset(nav, 0, sizeof(*nav));
    nav->track_seg_hint = 0;
    nav->track_hint_valid = false;
}

/*
 * vmap_route_nav_update() 的匹配部分（vmap_route_nav.c:474-584）逐行复刻，
 * 只把宏常量换成 ahead/back 参数，其余（含 acc 回退、并列取靠前、惩罚封顶）不动。
 */
static void nav_update(nav_t * nav, const poly_t * r, double lon, double lat,
    uint32_t ahead, uint32_t back, uint64_t * seg_evals)
{
    const pt_t * pts = r->p;
    const uint32_t n = r->n;
    uint32_t seg = 0;
    double best_d = 1e18;
    double best_score = 1e18;
    double best_along = 0.0;
    double best_seg_start_m = 0.0;
    double best_t = 0.0;
    const bool continuity = nav->track_hint_valid && nav->active;
    const double prev_along = nav->along_m;
    double acc;
    uint32_t hint;
    uint32_t start;
    uint32_t end;
    bool acquire_all;

    acquire_all = !nav->track_hint_valid;
    hint = nav->track_seg_hint;
    if (!acquire_all && hint + 1u >= n) {
        hint = n - 2u;
    }

    if (acquire_all) {
        uint32_t stride = 1u;
        uint32_t coarse = 0;
        double coarse_d = 1e18;
        double coarse_acc = 0.0;
        double run = 0.0;
        const uint32_t last_seg = n - 2u;

        if (last_seg + 1u > VMAP_NAV_MATCH_ACQUIRE_MAX) {
            stride = (last_seg + VMAP_NAV_MATCH_ACQUIRE_MAX)
                / VMAP_NAV_MATCH_ACQUIRE_MAX;
        }
        for (uint32_t i = 0; i <= last_seg; i++) {
            if ((i % stride) == 0u || i == last_seg) {
                double tloc = 0.0;
                const double d = geo_point_to_seg_m(lon, lat,
                    pts[i].lon, pts[i].lat, pts[i + 1u].lon, pts[i + 1u].lat,
                    &tloc);

                (*seg_evals)++;
                if (d < coarse_d) {
                    coarse_d = d;
                    coarse = i;
                    coarse_acc = run;
                }
            }
            run += geo_haversine_m(pts[i].lon, pts[i].lat,
                pts[i + 1u].lon, pts[i + 1u].lat);
        }
        hint = coarse;
        start = hint > VMAP_NAV_MATCH_ACQUIRE_HALF
            ? hint - VMAP_NAV_MATCH_ACQUIRE_HALF : 0u;
        end = hint + VMAP_NAV_MATCH_ACQUIRE_HALF;
        if (end > last_seg) {
            end = last_seg;
        }
        acc = coarse_acc;
    } else {
        start = hint > back ? hint - back : 0u;
        end = hint + ahead;
        if (end + 1u >= n) {
            end = n - 2u;
        }
        acc = nav->track_seg_start_m;
    }

    for (uint32_t i = hint; i > start; i--) {
        acc -= geo_haversine_m(pts[i - 1u].lon, pts[i - 1u].lat,
            pts[i].lon, pts[i].lat);
    }

    for (uint32_t i = start; i <= end; i++) {
        double tloc = 0.0;
        const double d = geo_point_to_seg_m(lon, lat,
            pts[i].lon, pts[i].lat, pts[i + 1u].lon, pts[i + 1u].lat, &tloc);
        const double seg_m = geo_haversine_m(pts[i].lon, pts[i].lat,
            pts[i + 1u].lon, pts[i + 1u].lat);
        const double candidate_along = acc + seg_m * tloc;
        double score = d;

        (*seg_evals)++;
        if (continuity) {
            const double delta = candidate_along - prev_along;
            double excess = 0.0;

            if (delta < -VMAP_NAV_MATCH_BACK_TOL_M) {
                excess = -delta - VMAP_NAV_MATCH_BACK_TOL_M;
            } else if (delta > VMAP_NAV_MATCH_AHEAD_TOL_M) {
                excess = delta - VMAP_NAV_MATCH_AHEAD_TOL_M;
            }
            score += fmin(VMAP_NAV_MATCH_MAX_PENALTY, excess * VMAP_NAV_MATCH_PROGRESS_W);
        }

        if (score < best_score) {
            best_score = score;
            best_d = d;
            seg = i;
            best_seg_start_m = acc;
            best_t = tloc;
            best_along = candidate_along;
        }
        acc += seg_m;
    }

    nav->track_seg_hint = seg;
    nav->track_seg_start_m = best_seg_start_m;
    nav->track_hint_valid = true;
    nav->off_m = best_d;

    /* vmap_route_set_progress_match()：anchor 就是这次查询点 */
    nav->anchor_lon = lon;
    nav->anchor_lat = lat;
    nav->progress_seg = seg;
    nav->progress_lon = pts[seg].lon + (pts[seg + 1u].lon - pts[seg].lon) * best_t;
    nav->progress_lat = pts[seg].lat + (pts[seg + 1u].lat - pts[seg].lat) * best_t;
    nav->progress_valid = true;

    /* vmap_route_effective_along_m() 逐字复刻 */
    {
        double along = best_along;

        if (nav->progress_valid) {
            const double connector_m = geo_haversine_m(nav->anchor_lon,
                nav->anchor_lat, nav->progress_lon, nav->progress_lat);

            if (connector_m > 2.0 && best_along < connector_m + 5.0) {
                along = best_along - connector_m;
            }
        }
        nav->along_m = along;
    }

    nav->active = true;
    nav->seg = seg;
    nav->remain_m = fmax(0.0, r->total - nav->along_m);
    nav->arrived = nav->remain_m <= VMAP_ROUTE_ARRIVE_M;
    if (geo_haversine_m(lon, lat, pts[n - 1u].lon, pts[n - 1u].lat)
        <= (VMAP_NAV_ARRIVE_NEAR_M * 0.25)) {
        nav->arrived = true;
    }
}

static nav_out_t nav_step(nav_t * nav, const poly_t * r, double lon, double lat,
    uint32_t ahead, uint32_t back, uint64_t * seg_evals)
{
    nav_update(nav, r, lon, lat, ahead, back, seg_evals);
    {
        nav_out_t o;

        o.seg = nav->seg;
        o.along = nav->along_m;
        o.off = nav->off_m;
        o.remain = nav->remain_m;
        o.arrived = nav->arrived;
        return o;
    }
}

/* ------------------------------------------------------------------ GPX */

static uint32_t gpx_load(const char * path, pt_t * out, uint32_t cap)
{
    FILE * f = fopen(path, "rb");
    char * buf;
    long sz;
    const char * s;
    uint32_t n = 0;

    if (!f) {
        fprintf(stderr, "gpx: open %s failed\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)sz + 1u);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(buf);
        return 0;
    }
    buf[sz] = '\0';
    fclose(f);

    s = buf;
    while ((s = strstr(s, "<trkpt")) != NULL) {
        const char * lat = strstr(s, "lat=\"");
        const char * lon = strstr(s, "lon=\"");
        const char * end = strchr(s, '>');

        if (!lat || !lon || !end || lat > end || lon > end) {
            break;
        }
        if (n < cap) {
            out[n].lat = atof(lat + 5);
            out[n].lon = atof(lon + 5);
            n++;
        }
        s = end;
    }
    free(buf);
    return n;
}

/* ------------------------------------------------------------------ 变换 */

static uint32_t copy_range(pt_t * dst, const pt_t * src, uint32_t a, uint32_t b)
{
    uint32_t n = 0;

    for (uint32_t i = a; i < b; i++) {
        dst[n++] = src[i];
    }
    return n;
}

static uint32_t reverse_pts(pt_t * p, uint32_t n)
{
    for (uint32_t i = 0; i < n / 2u; i++) {
        pt_t t = p[i];

        p[i] = p[n - 1u - i];
        p[n - 1u - i] = t;
    }
    return n;
}

static uint32_t decimate(pt_t * p, uint32_t n, uint32_t step)
{
    uint32_t m = 0;

    for (uint32_t i = 0; i < n; i += step) {
        p[m++] = p[i];
    }
    if (m >= 2u) {
        p[m - 1u] = p[n - 1u];
    }
    return m;
}

/* 在折线上按弧长线性插值加密，使每段 <= step_m（模拟 OSM 密集顶点） */
static uint32_t densify(const pt_t * src, uint32_t n, pt_t * dst, uint32_t cap,
    double step_m)
{
    uint32_t m = 0;

    for (uint32_t i = 0; i + 1u < n; i++) {
        const double d = geo_haversine_m(src[i].lon, src[i].lat,
            src[i + 1u].lon, src[i + 1u].lat);
        const uint32_t k = (uint32_t)fmax(1.0, ceil(d / step_m));

        for (uint32_t j = 0; j < k && m < cap; j++) {
            const double t = (double)j / (double)k;

            dst[m].lon = src[i].lon + (src[i + 1u].lon - src[i].lon) * t;
            dst[m].lat = src[i].lat + (src[i + 1u].lat - src[i].lat) * t;
            m++;
        }
    }
    if (m < cap) {
        dst[m++] = src[n - 1u];
    }
    return m;
}

/* 轨迹整体向右侧平移 meters（用与 point_to_seg 同一套平面换算） */
static void offset_track(pt_t * p, uint32_t n, double meters)
{
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t a = i > 0u ? i - 1u : 0u;
        const uint32_t b = i + 1u < n ? i + 1u : n - 1u;
        const double kx = 111320.0 * geo_cos_deg(p[i].lat);
        double ux = (p[b].lon - p[a].lon) * kx;
        double uy = (p[b].lat - p[a].lat) * GEO_M_PER_DEG_LAT;
        const double len = sqrt(ux * ux + uy * uy);

        if (len < 1e-9) {
            continue;
        }
        ux /= len;
        uy /= len;
        /* 右法向 (-uy, ux) */
        p[i].lon += (-uy * meters) / kx;
        p[i].lat += (ux * meters) / GEO_M_PER_DEG_LAT;
    }
}

/*
 * 删掉轨迹上一段连续点，使"沿路缺口"≈ gap_m（模拟 GNSS 失锁 / 抄近路）。
 * center_m 是缺口中心在轨迹累计弧长上的位置；返回剩余点数。
 */
static uint32_t cut_gap(pt_t * p, uint32_t n, double center_m, double gap_m)
{
    double * cum = (double *)malloc((size_t)n * sizeof(double));
    uint32_t m = 0;

    if (!cum) {
        return n;
    }
    cum[0] = 0.0;
    for (uint32_t i = 1; i < n; i++) {
        cum[i] = cum[i - 1] + geo_haversine_m(p[i - 1].lon, p[i - 1].lat,
            p[i].lon, p[i].lat);
    }
    {
        const double a = center_m - gap_m * 0.5;
        const double b = center_m + gap_m * 0.5;

        for (uint32_t i = 0; i < n; i++) {
            if (cum[i] >= a && cum[i] <= b && i + 1u < n) {
                continue;
            }
            p[m++] = p[i];
        }
    }
    free(cum);
    return m;
}

/* ------------------------------------------------------------------ 比对 */

typedef struct {
    uint32_t n;
    uint32_t diff;
    uint32_t max_dseg;
    uint32_t hist[8];
    double max_dalong;
    double max_doff;
    uint32_t arrived_diff;
    uint32_t back_steps64;
    uint32_t back_steps32;
    uint32_t far_jumps64;   /* along 前跳 > 200 m 的次数（超出口径） */
    uint32_t far_jumps32;
    double max_back_m64;
    double max_back_m32;
    uint32_t first_diff;
    nav_out_t f64, f32;
    double f_lon, f_lat;
} cmp_t;

static void run_scenario(const char * name, const poly_t * route,
    const pt_t * trace, uint32_t tn, bool synth, cmp_t * out, FILE * csv)
{
    nav_t a, b;
    uint64_t evals64 = 0, evals32 = 0;
    nav_out_t prev64 = { 0 }, prev32 = { 0 };

    memset(out, 0, sizeof(*out));
    out->n = tn;
    out->first_diff = UINT32_MAX;
    nav_reset(&a);
    nav_reset(&b);

    for (uint32_t i = 0; i < tn; i++) {
        const nav_out_t o64 = nav_step(&a, route, trace[i].lon, trace[i].lat,
            VMAP_NAV_MATCH_AHEAD_SEGS, VMAP_NAV_MATCH_BACK_SEGS, &evals64);
        const nav_out_t o32 = nav_step(&b, route, trace[i].lon, trace[i].lat,
            32u, VMAP_NAV_MATCH_BACK_SEGS, &evals32);

        if (o64.seg != o32.seg) {
            const uint32_t d = o64.seg > o32.seg ? o64.seg - o32.seg
                                                 : o32.seg - o64.seg;

            out->diff++;
            if (d > out->max_dseg) {
                out->max_dseg = d;
            }
            out->hist[d < 8u ? d : 7u]++;
            if (out->first_diff == UINT32_MAX) {
                out->first_diff = i;
                out->f64 = o64;
                out->f32 = o32;
                out->f_lon = trace[i].lon;
                out->f_lat = trace[i].lat;
            }
        }
        {
            const double da = fabs(o64.along - o32.along);
            const double dof = fabs(o64.off - o32.off);

            if (da > out->max_dalong) {
                out->max_dalong = da;
            }
            if (dof > out->max_doff) {
                out->max_doff = dof;
            }
        }
        if (o64.arrived != o32.arrived) {
            out->arrived_diff++;
        }
        if (i > 0u) {
            const double d64 = o64.along - prev64.along;
            const double d32 = o32.along - prev32.along;

            if (d64 < -40.0) {
                out->back_steps64++;
                if (-d64 > out->max_back_m64) {
                    out->max_back_m64 = -d64;
                }
            }
            if (d32 < -40.0) {
                out->back_steps32++;
                if (-d32 > out->max_back_m32) {
                    out->max_back_m32 = -d32;
                }
            }
            if (d64 > 200.0) {
                out->far_jumps64++;
            }
            if (d32 > 200.0) {
                out->far_jumps32++;
            }
        }
        prev64 = o64;
        prev32 = o32;

        if (csv) {
            fprintf(csv, "%s,%u,%.7f,%.7f,%u,%.3f,%.3f,%u,%.3f,%.3f,%d\n",
                name, i, trace[i].lon, trace[i].lat, o64.seg, o64.along,
                o64.off, o32.seg, o32.along, o32.off,
                o64.seg != o32.seg ? 1 : 0);
        }
    }

    printf("%-22s %-5s %5u %5u %6.1f %6.1f %5u %8.1f %8u %6.3f %4u %4u %4u %4u %6.1f %6.1f\n",
        name, synth ? "SYN" : "REAL", route->n, tn,
        (double)evals64 / (double)tn, (double)evals32 / (double)tn,
        out->diff, out->max_dalong, out->max_dseg,
        (double)out->max_dseg * (route->total / (double)(route->n - 1u)),
        out->arrived_diff, out->back_steps64, out->back_steps32,
        out->far_jumps32, out->max_back_m64, out->max_back_m32);

    if (out->diff) {
        printf("    first diff @%u (%.7f,%.7f): seg64=%u seg32=%u "
               "along64=%.2f along32=%.2f off64=%.2f off32=%.2f\n",
            out->first_diff, out->f_lon, out->f_lat,
            out->f64.seg, out->f32.seg, out->f64.along, out->f32.along,
            out->f64.off, out->f32.off);
        printf("    |dseg| hist 1/2/3/4/5/6/7+: %u/%u/%u/%u/%u/%u/%u  "
               "max|dseg|=%u (~%.0f m)  max|dAlong|=%.1f m\n",
            out->hist[1], out->hist[2], out->hist[3], out->hist[4],
            out->hist[5], out->hist[6], out->hist[7], out->max_dseg,
            (double)out->max_dseg * (route->total / (double)(route->n - 1u)),
            out->max_dalong);
    }
}

int main(int argc, char ** argv)
{
    const char * dir;
    char path_a[1024], path_b[1024];
    pt_t *A, *B, *r, *t;
    poly_t route;
    cmp_t c;
    FILE * csv = NULL;
    int diffs = 0;
    uint32_t na, nb;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <gpx-dir> [csv-out]\n", argv[0]);
        return 2;
    }
    dir = argv[1];
    snprintf(path_a, sizeof(path_a), "%s/TRK_20260908_073948.gpx", dir);
    snprintf(path_b, sizeof(path_b), "%s/TRK_20260908_174312.gpx", dir);

    A = (pt_t *)malloc(sizeof(pt_t) * MAX_PTS);
    B = (pt_t *)malloc(sizeof(pt_t) * MAX_PTS);
    r = (pt_t *)malloc(sizeof(pt_t) * MAX_PTS);
    t = (pt_t *)malloc(sizeof(pt_t) * MAX_PTS);
    if (!A || !B || !r || !t) {
        return 2;
    }
    na = gpx_load(path_a, A, MAX_PTS);
    nb = gpx_load(path_b, B, MAX_PTS);
    if (na < 100u || nb < 100u) {
        fprintf(stderr, "gpx: too few points A=%u B=%u\n", na, nb);
        return 2;
    }
    printf("samples: A=%s (%u pts)  B=%s (%u pts)\n",
        "TRK_20260908_073948.gpx", na, "TRK_20260908_174312.gpx", nb);

    if (argc >= 3) {
        csv = fopen(argv[2], "w");
        if (csv) {
            fprintf(csv, "scenario,idx,lon,lat,seg64,along64,off64,seg32,along32,off32,diff\n");
        }
    }

    printf("\n%-22s %-5s %5s %5s %6s %6s %5s %8s %8s %6s %4s %4s %4s %4s %6s %6s\n",
        "scenario", "kind", "rtpts", "trpts", "eval64", "eval32", "diff",
        "maxdAln", "maxdSeg", "~dm", "arr", "bk64", "bk32", "jmp32",
        "mbk64", "mbk32");

    /* ---- 真实样本：前半程当路线、整条当轨迹（任务指定的方案 A） ---- */
    {
        uint32_t m = copy_range(r, A, 0, na / 2u);
        poly_build(&route, r, m);
        run_scenario("A-half", &route, A, na, false, &c, csv);
        diffs += (int)c.diff;
        poly_free(&route);
    }
    {
        uint32_t m = copy_range(r, B, 0, nb / 2u);
        poly_build(&route, r, m);
        run_scenario("B-half", &route, B, nb, false, &c, csv);
        diffs += (int)c.diff;
        poly_free(&route);
    }
    /* ---- 真实样本：整条路线 vs 整条轨迹 ---- */
    {
        poly_build(&route, A, na);
        run_scenario("A-full", &route, A, na, false, &c, csv);
        diffs += (int)c.diff;
        poly_free(&route);
    }
    {
        poly_build(&route, B, nb);
        run_scenario("B-full", &route, B, nb, false, &c, csv);
        diffs += (int)c.diff;
        poly_free(&route);
    }
    /* ---- 真实样本交叉：一趟的路线 + 另一趟的轨迹（规划 vs 实际骑行） ---- */
    {
        poly_build(&route, A, na);
        run_scenario("A-route_B-trace", &route, B, nb, false, &c, csv);
        diffs += (int)c.diff;
        poly_free(&route);
    }
    {
        poly_build(&route, B, nb);
        run_scenario("B-route_A-trace", &route, A, na, false, &c, csv);
        diffs += (int)c.diff;
        poly_free(&route);
    }
    /* ---- 真实样本：轨迹整体右移 15 m（GPS 偏置/骑在路侧） ---- */
    {
        uint32_t m = copy_range(t, A, 0, na);

        offset_track(t, m, 15.0);
        poly_build(&route, A, na);
        run_scenario("A-full-offset15", &route, t, m, true, &c, csv);
        diffs += (int)c.diff;
        poly_free(&route);
    }

    /* ---- 构造变体：折返路线（同一段路去+回），窗口宽度的真正风险面 ---- */
    {
        pt_t * ob = (pt_t *)malloc(sizeof(pt_t) * MAX_PTS);   /* 原始折返 */
        pt_t * dec = (pt_t *)malloc(sizeof(pt_t) * MAX_PTS);  /* 抽稀版 */
        pt_t * off = (pt_t *)malloc(sizeof(pt_t) * MAX_PTS);  /* 偏移轨迹 */
        pt_t * tr = (pt_t *)malloc(sizeof(pt_t) * MAX_PTS);   /* 带缺口轨迹 */
        const uint32_t half = na / 2u;
        uint32_t m = copy_range(ob, A, 0, half);
        poly_t rt;
        double half_m;

        memcpy(ob + m, A, sizeof(pt_t) * (half - 1u));
        m += half - 1u;
        reverse_pts(ob + half, m - half);   /* 回程 = 去程逆序 */

        poly_build(&rt, ob, m);
        half_m = rt.cum[half - 1u];
        run_scenario("outback", &rt, ob, m, true, &c, csv);
        diffs += (int)c.diff;

        /* 加密到 3 / 5 / 10 m 段（真实 VGRF 边: p10=11.3 m, min=0.32 m） */
        {
            const double steps[3] = { 3.0, 5.0, 10.0 };
            const char * names[3] = { "outback-dense3", "outback-dense5",
                                      "outback-dense10" };

            for (int k = 0; k < 3; k++) {
                const uint32_t md = densify(ob, m, r, MAX_PTS, steps[k]);

                poly_build(&rt, r, md);
                run_scenario(names[k], &rt, ob, m, true, &c, csv);
                diffs += (int)c.diff;
                poly_free(&rt);
            }
        }
        /* 抽稀到 4 倍（段更长，类似 VGRF 的节点间距） */
        {
            const uint32_t mk = decimate(dec, m, 4u);

            memcpy(dec + mk, ob + mk, 0);   /* no-op, clarity */
            poly_build(&rt, dec, mk);
            run_scenario("outback-decim4", &rt, ob, m, true, &c, csv);
            diffs += (int)c.diff;
            poly_free(&rt);
        }
        /* 轨迹右偏 20 m：并列/平行判定的鲁棒性 */
        {
            memcpy(off, ob, sizeof(pt_t) * m);
            offset_track(off, m, 20.0);
            poly_build(&rt, ob, m);
            run_scenario("outback-offset20", &rt, off, m, true, &c, csv);
            diffs += (int)c.diff;
            poly_free(&rt);
        }
        /* 折返点附近 GNSS 缺口：150/300/600 m（失锁或抄近路） */
        {
            const double gaps[3] = { 150.0, 300.0, 600.0 };
            const char * names[3] = { "outback-gap150", "outback-gap300",
                                      "outback-gap600" };

            for (int g = 0; g < 3; g++) {
                uint32_t mg = copy_range(tr, ob, 0, m);

                mg = cut_gap(tr, mg, half_m, gaps[g]);
                poly_build(&rt, ob, m);
                run_scenario(names[g], &rt, tr, mg, true, &c, csv);
                diffs += (int)c.diff;
                poly_free(&rt);
            }
        }
        /* 直路上 300 m 缺口（无折返，纯追赶） */
        {
            const uint32_t mh = copy_range(r, A, 0, na / 2u);
            uint32_t mg = copy_range(tr, A, 0, na);

            poly_build(&rt, r, mh);
            {
                const double mid = rt.cum[mh / 2u];

                mg = cut_gap(tr, mg, mid, 300.0);
                run_scenario("half-gap300", &rt, tr, mg, true, &c, csv);
                diffs += (int)c.diff;
            }
            poly_free(&rt);
        }
        free(ob);
        free(dec);
        free(off);
        free(tr);
    }

    if (csv) {
        fclose(csv);
    }

    printf("\nTOTAL differing points across REAL scenarios and SYNTH variants: %d\n",
        diffs);
    printf("%s\n", diffs == 0
        ? "RESULT: AHEAD=32 与 AHEAD=64 逐点选段完全一致"
        : "RESULT: 存在不一致，AHEAD=32 不可等价替换");

    free(A);
    free(B);
    free(r);
    free(t);
    return diffs == 0 ? 0 : 1;
}
