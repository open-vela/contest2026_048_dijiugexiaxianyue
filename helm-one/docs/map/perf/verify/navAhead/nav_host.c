/*
 * nav_host.c — 把**真实的** vmap_route_nav.c 编到宿主机上，用 GPX 轨迹逐点回放。
 *
 * 编译期通过 VMAP_NAV_SRC 指向某个版本的 vmap_route_nav.c：
 *   - nav64: 原始源码（VMAP_NAV_MATCH_AHEAD_SEGS = 64）
 *   - nav32: 由 build.sh 用 sed 生成的副本，仅把该宏改成 32
 * 两个二进制吃同一批路线/轨迹，输出逐点 seg/along/off，`diff` 即结论。
 * 这样比对的是**固件源码本身**，不是手抄的等价实现（避免"抄错所以等价/不等价"）。
 *
 * 场景构造与 nav_window_replay.c 一致；输出 CSV 可直接与该文件的结果互校。
 *
 * 用法: nav64|nav32 <gpx-dir> <csv-out>
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "route_stub.h"
#include "vmap_geo.h"

#ifndef VMAP_NAV_SRC
#error "compile with -DVMAP_NAV_SRC=\".../vmap_route_nav.c\""
#endif
#include VMAP_NAV_SRC

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_PTS 65536u

typedef struct {
    double lon;
    double lat;
} pt_t;

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

static uint32_t densify(const pt_t * src, uint32_t n, pt_t * dst, uint32_t cap,
    double step_m)
{
    uint32_t m = 0;

    for (uint32_t i = 0; i + 1u < n; i++) {
        const double d = vmap_geo_haversine_m(src[i].lon, src[i].lat,
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

static void offset_track(pt_t * p, uint32_t n, double meters)
{
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t a = i > 0u ? i - 1u : 0u;
        const uint32_t b = i + 1u < n ? i + 1u : n - 1u;
        const double latr = p[i].lat * M_PI / 180.0;
        const double kx = 111320.0 * cos(latr);
        double ux = (p[b].lon - p[a].lon) * kx;
        double uy = (p[b].lat - p[a].lat) * 110540.0;
        const double len = sqrt(ux * ux + uy * uy);

        if (len < 1e-9) {
            continue;
        }
        ux /= len;
        uy /= len;
        p[i].lon += (-uy * meters) / kx;
        p[i].lat += (ux * meters) / 110540.0;
    }
}

static uint32_t cut_gap(pt_t * p, uint32_t n, double center_m, double gap_m)
{
    double * cum = (double *)malloc((size_t)n * sizeof(double));
    uint32_t m = 0;

    if (!cum) {
        return n;
    }
    cum[0] = 0.0;
    for (uint32_t i = 1; i < n; i++) {
        cum[i] = cum[i - 1] + vmap_geo_haversine_m(p[i - 1].lon, p[i - 1].lat,
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

static double poly_total(const vmap_route_pt_t * p, uint32_t n)
{
    double t = 0.0;

    for (uint32_t i = 1; i < n; i++) {
        t += vmap_geo_haversine_m(p[i - 1u].lon, p[i - 1u].lat, p[i].lon,
            p[i].lat);
    }
    return t;
}

/* 跑一个场景：route 折线 + trace 点序列，输出 nav 每拍选中的段与进度。 */
static void run(const char * name, const pt_t * rp, uint32_t rn,
    const pt_t * tp, uint32_t tn, FILE * csv)
{
    vmap_route_pt_t * rpt = (vmap_route_pt_t *)malloc(sizeof(*rpt) * rn);
    vmap_route_t * route = route_stub_create();
    double total;
    vmap_route_nav_t * nav;

    for (uint32_t i = 0; i < rn; i++) {
        rpt[i].lon = rp[i].lon;
        rpt[i].lat = rp[i].lat;
        rpt[i].ele_m = 0;
    }
    total = poly_total(rpt, rn);
    route_stub_set(route, rpt, rn, total);
    nav = vmap_route_nav_create(route);
    vmap_route_nav_apply(nav, rpt, rn, total, NULL, 0u);

    for (uint32_t i = 0; i < tn; i++) {
        const vmap_route_nav_state_t * st;

        vmap_route_nav_update(nav, tp[i].lon, tp[i].lat, 0.0f, 15.0f);
        st = vmap_route_nav_state(nav);
        fprintf(csv, "%s,%u,%.7f,%.7f,%u,%.3f,%.3f,%d\n", name, i, tp[i].lon,
            tp[i].lat, route_stub_progress_seg(route), st->along_m,
            st->off_route_m, st->arrived ? 1 : 0);
    }

    vmap_route_nav_destroy(nav);
    route_stub_destroy(route);
    free(rpt);
}

int main(int argc, char ** argv)
{
    char path_a[1024], path_b[1024];
    pt_t *A, *B, *r, *t, *dec, *ob, *off, *tr;
    FILE * csv;
    uint32_t na, nb;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <gpx-dir> <csv-out>  (built with AHEAD=%u)\n",
            argv[0], (unsigned)VMAP_NAV_MATCH_AHEAD_SEGS);
        return 2;
    }
    snprintf(path_a, sizeof(path_a), "%s/TRK_20260908_073948.gpx", argv[1]);
    snprintf(path_b, sizeof(path_b), "%s/TRK_20260908_174312.gpx", argv[1]);

    A = (pt_t *)malloc(sizeof(pt_t) * MAX_PTS);
    B = (pt_t *)malloc(sizeof(pt_t) * MAX_PTS);
    r = (pt_t *)malloc(sizeof(pt_t) * MAX_PTS);
    t = (pt_t *)malloc(sizeof(pt_t) * MAX_PTS);
    dec = (pt_t *)malloc(sizeof(pt_t) * MAX_PTS);
    ob = (pt_t *)malloc(sizeof(pt_t) * MAX_PTS);
    off = (pt_t *)malloc(sizeof(pt_t) * MAX_PTS);
    tr = (pt_t *)malloc(sizeof(pt_t) * MAX_PTS);
    if (!A || !B || !r || !t || !dec || !ob || !off || !tr) {
        return 2;
    }
    na = gpx_load(path_a, A, MAX_PTS);
    nb = gpx_load(path_b, B, MAX_PTS);
    if (na < 100u || nb < 100u) {
        fprintf(stderr, "gpx: too few points A=%u B=%u\n", na, nb);
        return 2;
    }
    csv = fopen(argv[2], "w");
    if (!csv) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 2;
    }
    fprintf(csv, "scenario,idx,lon,lat,seg,along,off,arrived\n");
    fprintf(stderr, "nav_host: AHEAD=%u A=%u pts B=%u pts\n",
        (unsigned)VMAP_NAV_MATCH_AHEAD_SEGS, na, nb);

    /* 真实样本：前半程当路线、整条当轨迹 */
    {
        const uint32_t m = copy_range(r, A, 0, na / 2u);

        run("A-half", r, m, A, na, csv);
    }
    {
        const uint32_t m = copy_range(r, B, 0, nb / 2u);

        run("B-half", r, m, B, nb, csv);
    }
    /* 真实样本：整条 vs 整条 */
    run("A-full", A, na, A, na, csv);
    run("B-full", B, nb, B, nb, csv);
    /* 真实样本交叉 */
    run("A-route_B-trace", A, na, B, nb, csv);
    run("B-route_A-trace", B, nb, A, na, csv);
    /* 真实样本 + 15 m 横向偏置 */
    {
        const uint32_t m = copy_range(t, A, 0, na);

        offset_track(t, m, 15.0);
        run("A-full-offset15", A, na, t, m, csv);
    }
    /* 折返路线（去 + 回） */
    {
        const uint32_t half = na / 2u;
        uint32_t m = copy_range(ob, A, 0, half);
        double half_m = 0.0;

        memcpy(ob + m, A, sizeof(pt_t) * (half - 1u));
        m += half - 1u;
        reverse_pts(ob + half, m - half);

        {
            vmap_route_pt_t * tmp = (vmap_route_pt_t *)malloc(sizeof(*tmp) * m);

            for (uint32_t i = 0; i < m; i++) {
                tmp[i].lon = ob[i].lon;
                tmp[i].lat = ob[i].lat;
                tmp[i].ele_m = 0;
            }
            half_m = 0.0;
            for (uint32_t i = 1; i < half; i++) {
                half_m += vmap_geo_haversine_m(tmp[i - 1u].lon,
                    tmp[i - 1u].lat, tmp[i].lon, tmp[i].lat);
            }
            free(tmp);
        }

        run("outback", ob, m, ob, m, csv);
        {
            const double steps[3] = { 3.0, 5.0, 10.0 };
            const char * names[3] = { "outback-dense3", "outback-dense5",
                                      "outback-dense10" };

            for (int k = 0; k < 3; k++) {
                const uint32_t md = densify(ob, m, r, MAX_PTS, steps[k]);

                run(names[k], r, md, ob, m, csv);
            }
        }
        {
            const uint32_t mk = decimate(dec, m, 4u);

            run("outback-decim4", dec, mk, ob, m, csv);
        }
        {
            memcpy(off, ob, sizeof(pt_t) * m);
            offset_track(off, m, 20.0);
            run("outback-offset20", ob, m, off, m, csv);
        }
        {
            const double gaps[3] = { 150.0, 300.0, 600.0 };
            const char * names[3] = { "outback-gap150", "outback-gap300",
                                      "outback-gap600" };

            for (int g = 0; g < 3; g++) {
                uint32_t mg = copy_range(tr, ob, 0, m);

                mg = cut_gap(tr, mg, half_m, gaps[g]);
                run(names[g], ob, m, tr, mg, csv);
            }
        }
        {
            const uint32_t mh = copy_range(r, A, 0, na / 2u);
            uint32_t mg = copy_range(tr, A, 0, na);
            double mid = 0.0;

            for (uint32_t i = 1; i <= mh / 2u; i++) {
                mid += vmap_geo_haversine_m(r[i - 1u].lon, r[i - 1u].lat,
                    r[i].lon, r[i].lat);
            }
            mg = cut_gap(tr, mg, mid, 300.0);
            run("half-gap300", r, mh, tr, mg, csv);
        }
    }

    fclose(csv);
    free(A);
    free(B);
    free(r);
    free(t);
    free(dec);
    free(ob);
    free(off);
    free(tr);
    return 0;
}
