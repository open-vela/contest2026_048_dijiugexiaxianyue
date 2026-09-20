/**
 * @file vmap_render.c
 * @brief vmap render 模块。
 */

#include "vmap_render.h"

#include "vmap_config.h"
#include "vmap_format.h"
#include "vmap_style.h"
#include <math.h>
#include <string.h>

#define RGB565(r, g, b) VMAP_STYLE_RGB565((r), (g), (b))

#define ROAD_STYLE_NUM VMAP_STYLE_ROAD_CLASSES

#if VMAP_RENDER_AA
#define VMAP_FP_SHIFT 8
#define VMAP_FP_ONE   (1 << VMAP_FP_SHIFT)
#endif

static const vmap_style_road_t * road_style_for(int cls)
{
    const vmap_style_t * style = vmap_style_current();

    if (cls < 1 || cls >= ROAD_STYLE_NUM) {
        cls = VMAP_ROAD_RESIDENTIAL;
    }

    return &style->road[cls];
}

void vmap_render_clip_reset(vmap_canvas_t * c)
{
    if (c) {
        c->clip_n = 0;
    }
}

void vmap_render_clip_add(vmap_canvas_t * c, int32_t x1, int32_t y1,
    int32_t x2, int32_t y2)
{
    uint8_t i;

    if (!c || c->clip_n >= VMAP_RENDER_DIRTY_MAX || x1 >= x2 || y1 >= y2) {
        return;
    }
    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 > c->w) {
        x2 = c->w;
    }
    if (y2 > c->h) {
        y2 = c->h;
    }
    if (x1 >= x2 || y1 >= y2) {
        return;
    }

    i = c->clip_n;
    c->clip_x1[i] = x1;
    c->clip_y1[i] = y1;
    c->clip_x2[i] = x2;
    c->clip_y2[i] = y2;
    c->clip_n++;
}

static bool canvas_in_clip(const vmap_canvas_t * c, int32_t x, int32_t y)
{
    uint8_t i;

    if (!c || c->clip_n == 0) {
        return true;
    }
    for (i = 0; i < c->clip_n; i++) {
        if (x >= c->clip_x1[i] && x < c->clip_x2[i]
            && y >= c->clip_y1[i] && y < c->clip_y2[i]) {
            return true;
        }
    }
    return false;
}

static void put_pixel(vmap_canvas_t * c, int32_t x, int32_t y, uint16_t color)
{
    uint16_t * row;

    if (!c || !c->buf) {
        return;
    }
    if ((uint32_t)x >= (uint32_t)c->w || (uint32_t)y >= (uint32_t)c->h) {
        return;
    }
    if (!canvas_in_clip(c, x, y)) {
        return;
    }

    row = (uint16_t *)((uint8_t *)c->buf + (size_t)y * c->stride);
    row[x] = color;
}

#if VMAP_RENDER_AA
static uint16_t mix_rgb565(uint16_t bg, uint16_t fg, uint8_t mix)
{
    uint32_t mix5;
    uint32_t bg32;
    uint32_t fg32;
    uint32_t r;
    uint32_t g;
    uint32_t b;

    if (mix >= 255) {
        return fg;
    }
    if (mix == 0) {
        return bg;
    }

    mix5 = ((uint32_t)mix + 4U) >> 3;
    bg32 = bg;
    fg32 = fg;
    r = ((bg32 & 0xF800U) + (((fg32 & 0xF800U) - (bg32 & 0xF800U)) * mix5 >> 5))
        & 0xF800U;
    g = ((bg32 & 0x07E0U) + (((fg32 & 0x07E0U) - (bg32 & 0x07E0U)) * mix5 >> 5))
        & 0x07E0U;
    b = ((bg32 & 0x001FU) + (((fg32 & 0x001FU) - (bg32 & 0x001FU)) * mix5 >> 5))
        & 0x001FU;
    return (uint16_t)(r | g | b);
}

static void blend_pixel(vmap_canvas_t * c, int32_t x, int32_t y, uint16_t color,
    uint8_t alpha)
{
    uint16_t * row;

    if (!c || !c->buf || alpha == 0) {
        return;
    }
    if ((uint32_t)x >= (uint32_t)c->w || (uint32_t)y >= (uint32_t)c->h) {
        return;
    }
    if (!canvas_in_clip(c, x, y)) {
        return;
    }
    if (alpha >= 255) {
        put_pixel(c, x, y, color);
        return;
    }

    row = (uint16_t *)((uint8_t *)c->buf + (size_t)y * c->stride);
    row[x] = mix_rgb565(row[x], color, alpha);
}

typedef struct {
    int32_t x;
    int32_t y;
} vmap_fp_point_t;

static void cover_span_fp(vmap_canvas_t * c, int32_t y, int32_t x0_fp,
    int32_t x1_fp, uint16_t color)
{
    int32_t x;
    int32_t x_last;

    if (x0_fp > x1_fp) {
        int32_t t = x0_fp;
        x0_fp = x1_fp;
        x1_fp = t;
    }

    x = x0_fp >> VMAP_FP_SHIFT;
    x_last = (x1_fp + VMAP_FP_ONE - 1) >> VMAP_FP_SHIFT;

    for (; x <= x_last; x++) {
        const int32_t px0_fp = x << VMAP_FP_SHIFT;
        const int32_t px1_fp = px0_fp + VMAP_FP_ONE;
        const int32_t a_fp = x0_fp > px0_fp ? x0_fp : px0_fp;
        const int32_t b_fp = x1_fp < px1_fp ? x1_fp : px1_fp;
        const int32_t cov_fp = b_fp - a_fp;
        uint8_t alpha;

        if (cov_fp <= 0) {
            continue;
        }
        if (cov_fp >= VMAP_FP_ONE) {
            put_pixel(c, x, y, color);
            continue;
        }

        alpha = (uint8_t)(((uint32_t)cov_fp * 255U) >> VMAP_FP_SHIFT);
        blend_pixel(c, x, y, color, alpha);
    }
}

static void fill_polygon_fp(vmap_canvas_t * c, const vmap_fp_point_t * pts,
    int32_t n, uint16_t color)
{
    int32_t min_y;
    int32_t max_y;
    int32_t y;

    if (n < 3) {
        return;
    }

    min_y = pts[0].y >> VMAP_FP_SHIFT;
    max_y = pts[0].y >> VMAP_FP_SHIFT;
    for (int32_t i = 1; i < n; i++) {
        int32_t py = pts[i].y >> VMAP_FP_SHIFT;
        if (py < min_y) {
            min_y = py;
        }
        if (py > max_y) {
            max_y = py;
        }
    }
    if (min_y < 0) {
        min_y = 0;
    }
    if (max_y > c->h - 1) {
        max_y = c->h - 1;
    }

    for (y = min_y; y <= max_y; y++) {
        int32_t xs_fp[64];
        int32_t cnt = 0;
        const int32_t y_scan_fp = (y << VMAP_FP_SHIFT) + (VMAP_FP_ONE / 2);

        for (int32_t i = 0, j = n - 1; i < n; j = i++) {
            const int32_t yi_fp = pts[i].y;
            const int32_t yj_fp = pts[j].y;

            if ((yi_fp <= y_scan_fp && yj_fp > y_scan_fp)
                || (yj_fp <= y_scan_fp && yi_fp > y_scan_fp)) {
                const int32_t xi_fp = pts[i].x;
                const int32_t xj_fp = pts[j].x;
                const int32_t x_fp = (int32_t)((int64_t)(y_scan_fp - yi_fp)
                    * (xj_fp - xi_fp) / (yj_fp - yi_fp) + xi_fp);

                if (cnt < (int32_t)(sizeof(xs_fp) / sizeof(xs_fp[0]))) {
                    xs_fp[cnt++] = x_fp;
                }
            }
        }

        for (int32_t a = 1; a < cnt; a++) {
            int32_t v = xs_fp[a];
            int32_t b = a - 1;

            while (b >= 0 && xs_fp[b] > v) {
                xs_fp[b + 1] = xs_fp[b];
                b--;
            }
            xs_fp[b + 1] = v;
        }
        for (int32_t a = 0; a + 1 < cnt; a += 2) {
            cover_span_fp(c, y, xs_fp[a], xs_fp[a + 1], color);
        }
    }
}
#endif /* VMAP_RENDER_AA */

static void blend_span(vmap_canvas_t * c, int32_t x0, int32_t x1, int32_t y,
    uint16_t color)
{
    if ((uint32_t)y >= (uint32_t)c->h) {
        return;
    }
    if (x0 > x1) {
        int32_t t = x0;
        x0 = x1;
        x1 = t;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (x1 > c->w - 1) {
        x1 = c->w - 1;
    }
    if (x0 > x1) {
        return;
    }
    if (c->clip_n > 0) {
        uint8_t i;

        for (i = 0; i < c->clip_n; i++) {
            int32_t xa;
            int32_t xb;
            uint16_t * row;

            if (y < c->clip_y1[i] || y >= c->clip_y2[i]) {
                continue;
            }
            xa = x0 > c->clip_x1[i] ? x0 : c->clip_x1[i];
            xb = x1 < (c->clip_x2[i] - 1) ? x1 : (c->clip_x2[i] - 1);
            if (xa > xb) {
                continue;
            }
            row = (uint16_t *)((uint8_t *)c->buf + (size_t)y * c->stride);
            for (int32_t x = xa; x <= xb; x++) {
                row[x] = color;
            }
        }
        return;
    }
    {
        uint16_t * row = (uint16_t *)((uint8_t *)c->buf + (size_t)y * c->stride);
        for (int32_t x = x0; x <= x1; x++) {
            row[x] = color;
        }
    }
}

#if VMAP_RENDER_AA
static void fill_disc_aa_fp(vmap_canvas_t * c, int32_t cx_fp, int32_t cy_fp,
    int32_t r_fp, uint16_t color)
{
    const int64_t outer2 = (int64_t)r_fp * (int64_t)r_fp;
    int32_t y0;
    int32_t y1;
    int32_t y;

    if (r_fp < VMAP_FP_ONE / 4) {
        blend_pixel(c, cx_fp >> VMAP_FP_SHIFT, cy_fp >> VMAP_FP_SHIFT, color, 255);
        return;
    }

    y0 = (cy_fp - r_fp) >> VMAP_FP_SHIFT;
    y1 = (cy_fp + r_fp + VMAP_FP_ONE - 1) >> VMAP_FP_SHIFT;
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 > c->h - 1) {
        y1 = c->h - 1;
    }

    for (y = y0; y <= y1; y++) {
        const int32_t y_mid_fp = (y << VMAP_FP_SHIFT) + (VMAP_FP_ONE / 2);
        const int32_t dy_fp = y_mid_fp - cy_fp;
        const int64_t dy2 = (int64_t)dy_fp * (int64_t)dy_fp;
        int32_t dx_fp;

        if (dy2 >= outer2) {
            continue;
        }

        dx_fp = (int32_t)sqrt((double)(outer2 - dy2));
        cover_span_fp(c, y, cx_fp - dx_fp, cx_fp + dx_fp, color);
    }
}

static void fill_disc_aa(vmap_canvas_t * c, int32_t cx, int32_t cy, int32_t r,
    uint16_t color)
{
    const int32_t r_fp = (r << VMAP_FP_SHIFT) + (VMAP_FP_ONE / 2);

    if (r < 1) {
        blend_pixel(c, cx, cy, color, 255);
        return;
    }

    fill_disc_aa_fp(c, cx << VMAP_FP_SHIFT, cy << VMAP_FP_SHIFT, r_fp, color);
}
#endif /* VMAP_RENDER_AA */

void vmap_render_fill_disc(vmap_canvas_t * c, int32_t cx, int32_t cy, int32_t r,
    uint16_t color)
{
    int32_t r2;

#if VMAP_RENDER_AA
    fill_disc_aa(c, cx, cy, r, color);
    return;
#endif

    if (r < 1) {
        put_pixel(c, cx, cy, color);
        return;
    }

    r2 = r * r;
    for (int32_t dy = -r; dy <= r; dy++) {
        int32_t y = cy + dy;
        int32_t yy;
        int32_t dx_max;

        if ((uint32_t)y >= (uint32_t)c->h) {
            continue;
        }

        yy = dy * dy;
        dx_max = 0;
        while (((dx_max + 1) * (dx_max + 1) + yy) <= r2) {
            dx_max++;
        }
        blend_span(c, cx - dx_max, cx + dx_max, y, color);
    }
}

static void fill_polygon(vmap_canvas_t * c, const vmap_point_t * pts, int32_t n,
    uint16_t color)
{
    int32_t min_y;
    int32_t max_y;
    int32_t y;

    if (n < 3) {
        return;
    }

    min_y = pts[0].y;
    max_y = pts[0].y;
    for (int32_t i = 1; i < n; i++) {
        if (pts[i].y < min_y) {
            min_y = pts[i].y;
        }
        if (pts[i].y > max_y) {
            max_y = pts[i].y;
        }
    }
    if (min_y < 0) {
        min_y = 0;
    }
    if (max_y > c->h - 1) {
        max_y = c->h - 1;
    }

    for (y = min_y; y <= max_y; y++) {
#if VMAP_RENDER_AA
        int32_t xs_fp[64];
#else
        int32_t xs[64];
#endif
        int32_t cnt = 0;

        for (int32_t i = 0, j = n - 1; i < n; j = i++) {
            int32_t yi = pts[i].y;
            int32_t yj = pts[j].y;

            if ((yi <= y && yj > y) || (yj <= y && yi > y)) {
                int32_t xi = pts[i].x;
                int32_t xj = pts[j].x;
#if VMAP_RENDER_AA
                int32_t x_fp = (int32_t)((int64_t)xi * VMAP_FP_ONE
                    + (int64_t)(y - yi) * (xj - xi) * VMAP_FP_ONE / (yj - yi));

                if (cnt < (int32_t)(sizeof(xs_fp) / sizeof(xs_fp[0]))) {
                    xs_fp[cnt++] = x_fp;
                }
#else
                int32_t x = xi + (int32_t)((int64_t)(y - yi) * (xj - xi) / (yj - yi));

                if (cnt < (int32_t)(sizeof(xs) / sizeof(xs[0]))) {
                    xs[cnt++] = x;
                }
#endif
            }
        }

#if VMAP_RENDER_AA
        for (int32_t a = 1; a < cnt; a++) {
            int32_t v = xs_fp[a];
            int32_t b = a - 1;

            while (b >= 0 && xs_fp[b] > v) {
                xs_fp[b + 1] = xs_fp[b];
                b--;
            }
            xs_fp[b + 1] = v;
        }
        for (int32_t a = 0; a + 1 < cnt; a += 2) {
            cover_span_fp(c, y, xs_fp[a], xs_fp[a + 1], color);
        }
#else
        for (int32_t a = 1; a < cnt; a++) {
            int32_t v = xs[a];
            int32_t b = a - 1;
            while (b >= 0 && xs[b] > v) {
                xs[b + 1] = xs[b];
                b--;
            }
            xs[b + 1] = v;
        }
        for (int32_t a = 0; a + 1 < cnt; a += 2) {
            blend_span(c, xs[a], xs[a + 1], y, color);
        }
#endif
    }
}

#if VMAP_RENDER_AA
static void draw_thick_line_aa(vmap_canvas_t * c, int32_t x0, int32_t y0,
    int32_t x1, int32_t y1, int32_t width, uint16_t color)
{
    double dx;
    double dy;
    double len;
    double nx;
    double ny;
    double hw;
    vmap_fp_point_t quad[4];

    dx = (double)(x1 - x0);
    dy = (double)(y1 - y0);
    len = sqrt(dx * dx + dy * dy);
    if (len < 1e-6) {
        vmap_render_fill_disc(c, x0, y0, (width + 1) / 2, color);
        return;
    }

    nx = -dy / len;
    ny = dx / len;
    hw = (double)width * 0.5;

    quad[0].x = (int32_t)lround((x0 + nx * hw) * (double)VMAP_FP_ONE);
    quad[0].y = (int32_t)lround((y0 + ny * hw) * (double)VMAP_FP_ONE);
    quad[1].x = (int32_t)lround((x1 + nx * hw) * (double)VMAP_FP_ONE);
    quad[1].y = (int32_t)lround((y1 + ny * hw) * (double)VMAP_FP_ONE);
    quad[2].x = (int32_t)lround((x1 - nx * hw) * (double)VMAP_FP_ONE);
    quad[2].y = (int32_t)lround((y1 - ny * hw) * (double)VMAP_FP_ONE);
    quad[3].x = (int32_t)lround((x0 - nx * hw) * (double)VMAP_FP_ONE);
    quad[3].y = (int32_t)lround((y0 - ny * hw) * (double)VMAP_FP_ONE);
    fill_polygon_fp(c, quad, 4, color);
}
#endif /* VMAP_RENDER_AA */

void vmap_render_draw_thick_line(vmap_canvas_t * c, int32_t x0, int32_t y0, int32_t x1,
    int32_t y1, int32_t width, uint16_t color)
{
#if VMAP_RENDER_AA
    if (width < 1) {
        width = 1;
    }

    if (x0 == x1 && y0 == y1) {
        vmap_render_fill_disc(c, x0, y0, (width + 1) / 2, color);
        return;
    }

    draw_thick_line_aa(c, x0, y0, x1, y1, width, color);
#else
    double dx;
    double dy;
    double len;
    double nx;
    double ny;
    double hw;
    vmap_point_t quad[4];

    if (width < 1) {
        width = 1;
    }

    if (x0 == x1 && y0 == y1) {
        vmap_render_fill_disc(c, x0, y0, (width + 1) / 2, color);
        return;
    }

    dx = (double)(x1 - x0);
    dy = (double)(y1 - y0);
    len = sqrt(dx * dx + dy * dy);
    if (len < 1e-6) {
        vmap_render_fill_disc(c, x0, y0, (width + 1) / 2, color);
        return;
    }

    nx = -dy / len;
    ny = dx / len;
    hw = (double)width * 0.5;

    quad[0].x = (int32_t)lround(x0 + nx * hw);
    quad[0].y = (int32_t)lround(y0 + ny * hw);
    quad[1].x = (int32_t)lround(x1 + nx * hw);
    quad[1].y = (int32_t)lround(y1 + ny * hw);
    quad[2].x = (int32_t)lround(x1 - nx * hw);
    quad[2].y = (int32_t)lround(y1 - ny * hw);
    quad[3].x = (int32_t)lround(x0 - nx * hw);
    quad[3].y = (int32_t)lround(y0 - ny * hw);
    fill_polygon(c, quad, 4, color);
#endif
}

void vmap_render_fill_disc_f(vmap_canvas_t * c, double cx, double cy,
    double radius, uint16_t color)
{
#if VMAP_RENDER_AA
    int32_t r_fp;

    if (!c) {
        return;
    }
    if (radius < 0.35) {
        radius = 0.35;
    }
    r_fp = (int32_t)lround(radius * (double)VMAP_FP_ONE + (double)(VMAP_FP_ONE / 2));
    fill_disc_aa_fp(c, (int32_t)lround(cx * (double)VMAP_FP_ONE),
        (int32_t)lround(cy * (double)VMAP_FP_ONE), r_fp, color);
#else
    vmap_render_fill_disc(c, (int32_t)lround(cx), (int32_t)lround(cy),
        (int32_t)lround(radius), color);
#endif
}

void vmap_render_draw_thick_line_f(vmap_canvas_t * c, double x0, double y0,
    double x1, double y1, double width, uint16_t color)
{
#if VMAP_RENDER_AA
    double dx;
    double dy;
    double len;
    double nx;
    double ny;
    double hw;
    vmap_fp_point_t quad[4];

    if (!c) {
        return;
    }
    if (width < 1.0) {
        width = 1.0;
    }

    dx = x1 - x0;
    dy = y1 - y0;
    len = sqrt(dx * dx + dy * dy);
    if (len < 0.20) {
        vmap_render_fill_disc_f(c, x0, y0, width * 0.5, color);
        return;
    }

    nx = -dy / len;
    ny = dx / len;
    hw = width * 0.5;

    quad[0].x = (int32_t)lround((x0 + nx * hw) * (double)VMAP_FP_ONE);
    quad[0].y = (int32_t)lround((y0 + ny * hw) * (double)VMAP_FP_ONE);
    quad[1].x = (int32_t)lround((x1 + nx * hw) * (double)VMAP_FP_ONE);
    quad[1].y = (int32_t)lround((y1 + ny * hw) * (double)VMAP_FP_ONE);
    quad[2].x = (int32_t)lround((x1 - nx * hw) * (double)VMAP_FP_ONE);
    quad[2].y = (int32_t)lround((y1 - ny * hw) * (double)VMAP_FP_ONE);
    quad[3].x = (int32_t)lround((x0 - nx * hw) * (double)VMAP_FP_ONE);
    quad[3].y = (int32_t)lround((y0 - ny * hw) * (double)VMAP_FP_ONE);
    fill_polygon_fp(c, quad, 4, color);
#else
    vmap_render_draw_thick_line(c, (int32_t)lround(x0), (int32_t)lround(y0),
        (int32_t)lround(x1), (int32_t)lround(y1), (int32_t)lround(width), color);
#endif
}

#if VMAP_RENDER_AA
static void ribbon_offset_vertex(const vmap_point_t * pts, int32_t n, int32_t i,
    double hw, double * lx, double * ly, double * rx, double * ry)
{
    double n0x;
    double n0y;
    double n1x;
    double n1y;
    double mx;
    double my;
    double mlen;

    if (i == 0) {
        const double dx = (double)(pts[1].x - pts[0].x);
        const double dy = (double)(pts[1].y - pts[0].y);
        const double len = hypot(dx, dy);

        if (len < 1e-6) {
            n0x = 0.0;
            n0y = 1.0;
        } else {
            n0x = -dy / len;
            n0y = dx / len;
        }
        mx = n0x;
        my = n0y;
        mlen = hw;
    } else if (i == n - 1) {
        const double dx = (double)(pts[n - 1].x - pts[n - 2].x);
        const double dy = (double)(pts[n - 1].y - pts[n - 2].y);
        const double len = hypot(dx, dy);

        if (len < 1e-6) {
            n0x = 0.0;
            n0y = 1.0;
        } else {
            n0x = -dy / len;
            n0y = dx / len;
        }
        mx = n0x;
        my = n0y;
        mlen = hw;
    } else {
        const double dx0 = (double)(pts[i].x - pts[i - 1].x);
        const double dy0 = (double)(pts[i].y - pts[i - 1].y);
        const double dx1 = (double)(pts[i + 1].x - pts[i].x);
        const double dy1 = (double)(pts[i + 1].y - pts[i].y);
        const double len0 = hypot(dx0, dy0);
        const double len1 = hypot(dx1, dy1);
        double dot;

        if (len0 < 1e-6 || len1 < 1e-6) {
            mx = 0.0;
            my = 1.0;
            mlen = hw;
            goto out;
        }

        n0x = -dy0 / len0;
        n0y = dx0 / len0;
        n1x = -dy1 / len1;
        n1y = dx1 / len1;
        mx = n0x + n1x;
        my = n0y + n1y;
        mlen = hypot(mx, my);
        if (mlen < 1e-6) {
            mx = n0x;
            my = n0y;
            mlen = hw;
            goto out;
        }

        dot = (mx / mlen) * n0x + (my / mlen) * n0y;
        mx /= mlen;
        my /= mlen;
        if (fabs(dot) > 1e-4) {
            mlen = hw / dot;
            if (mlen > hw * 2.5) {
                mlen = hw * 2.5;
            } else if (mlen < -hw * 2.5) {
                mlen = -hw * 2.5;
            }
        } else {
            mlen = hw;
        }
    }

out:
    *lx = (double)pts[i].x + mx * mlen;
    *ly = (double)pts[i].y + my * mlen;
    *rx = (double)pts[i].x - mx * mlen;
    *ry = (double)pts[i].y - my * mlen;
}
#endif /* VMAP_RENDER_AA */

#if VMAP_RENDER_AA
static void stroke_ribbon_aa_chunk(vmap_canvas_t * c, const vmap_point_t * pts,
    int32_t n, int32_t width, uint16_t color, bool cap_start, bool cap_end)
{
    vmap_fp_point_t left[VMAP_RIBBON_CHUNK_MAX];
    vmap_fp_point_t right[VMAP_RIBBON_CHUNK_MAX];
    vmap_fp_point_t poly[VMAP_RIBBON_CHUNK_MAX * 2];
    const double hw = (double)width * 0.5;
    int32_t pn;
    int32_t i;

    if (!c || !pts || n < 2 || width < 1 || n > VMAP_RIBBON_CHUNK_MAX) {
        return;
    }

    for (i = 0; i < n; i++) {
        double lx;
        double ly;
        double rx;
        double ry;

        ribbon_offset_vertex(pts, n, i, hw, &lx, &ly, &rx, &ry);
        left[i].x = (int32_t)lround(lx * (double)VMAP_FP_ONE);
        left[i].y = (int32_t)lround(ly * (double)VMAP_FP_ONE);
        right[i].x = (int32_t)lround(rx * (double)VMAP_FP_ONE);
        right[i].y = (int32_t)lround(ry * (double)VMAP_FP_ONE);
    }

    pn = 0;
    for (i = 0; i < n; i++) {
        poly[pn++] = left[i];
    }
    for (i = n - 1; i >= 0; i--) {
        poly[pn++] = right[i];
    }

    fill_polygon_fp(c, poly, pn, color);

    {
        const int32_t join_r = (width + 1) / 2;

        if (cap_start) {
            vmap_render_fill_disc(c, pts[0].x, pts[0].y, join_r, color);
        }
        if (cap_end) {
            vmap_render_fill_disc(c, pts[n - 1].x, pts[n - 1].y, join_r, color);
        }
    }
}
#endif /* VMAP_RENDER_AA */

void vmap_render_stroke_ribbon(vmap_canvas_t * c, const vmap_point_t * pts,
    int32_t n, int32_t width, uint16_t color)
{
#if VMAP_RENDER_AA
    int32_t start;

    if (!c || !pts || n < 2 || width < 1) {
        return;
    }

    if (n <= VMAP_RIBBON_CHUNK_MAX) {
        stroke_ribbon_aa_chunk(c, pts, n, width, color, true, true);
        return;
    }

    start = 0;
    while (start < n - 1) {
        int32_t end = start + VMAP_RIBBON_CHUNK_MAX - 1;
        bool cap_start;
        bool cap_end;

        if (end >= n - 1) {
            end = n - 1;
        }

        cap_start = (start == 0);
        cap_end = (end == n - 1);
        stroke_ribbon_aa_chunk(c, pts + start, end - start + 1, width, color,
            cap_start, cap_end);

        if (end >= n - 1) {
            break;
        }

        start = end;
    }
#else
    int32_t i;

    if (!c || !pts || n < 2 || width < 1) {
        return;
    }

    for (i = 1; i < n; i++) {
        vmap_render_draw_thick_line(c, pts[i - 1].x, pts[i - 1].y,
            pts[i].x, pts[i].y, width, color);
    }

    {
        const int32_t join_r = (width + 1) / 2;
        vmap_render_fill_disc(c, pts[0].x, pts[0].y, join_r, color);
        vmap_render_fill_disc(c, pts[n - 1].x, pts[n - 1].y, join_r, color);
    }
#endif
}

static void stroke_polygon(vmap_canvas_t * c, const vmap_point_t * pts, int32_t n,
    int32_t width, uint16_t color)
{
    int32_t join_r;
    int32_t i;

    if (n < 2 || width < 1) {
        return;
    }

    join_r = (width + 1) / 2;

    for (i = 0; i < n; i++) {
        int32_t j = (i + 1) % n;
        vmap_render_draw_thick_line(c, pts[i].x, pts[i].y, pts[j].x, pts[j].y, width, color);
    }

    for (i = 0; i < n; i++) {
        vmap_render_fill_disc(c, pts[i].x, pts[i].y, join_r, color);
    }
}

static double pt_seg_dist2(double ax, double ay, double bx, double by, double cx,
    double cy)
{
    const double dx = cx - ax;
    const double dy = cy - ay;
    const double len2 = dx * dx + dy * dy;
    double t;
    double px;
    double py;
    double ex;
    double ey;

    if (len2 < 1e-6) {
        ex = bx - ax;
        ey = by - ay;
        return ex * ex + ey * ey;
    }

    t = ((bx - ax) * dx + (by - ay) * dy) / len2;
    if (t < 0.0) {
        t = 0.0;
    } else if (t > 1.0) {
        t = 1.0;
    }

    px = ax + t * dx;
    py = ay + t * dy;
    ex = bx - px;
    ey = by - py;
    return ex * ex + ey * ey;
}

/**
 * @brief Douglas-Peucker 简化折线。
 * @param pts 点数组。
 * @param n 点数。
 */
int32_t vmap_render_simplify_polyline(vmap_point_t * pts, int32_t n,
    float min_seg_px, float max_dev_px)
{
    const double min_seg2 = (double)min_seg_px * (double)min_seg_px;
    const double max_dev2 = (double)max_dev_px * (double)max_dev_px;
    int32_t w;

    if (n < 3) {
        return n;
    }

    w = 1;
    for (int32_t i = 1; i < n - 1; i++) {
        const double dev2 = pt_seg_dist2((double)pts[w - 1].x, (double)pts[w - 1].y,
            (double)pts[i].x, (double)pts[i].y,
            (double)pts[i + 1].x, (double)pts[i + 1].y);
        const double dx = (double)pts[i].x - (double)pts[w - 1].x;
        const double dy = (double)pts[i].y - (double)pts[w - 1].y;
        const double seg2 = dx * dx + dy * dy;

        if (dev2 < max_dev2) {
            continue;
        }

        if (seg2 < min_seg2) {
            pts[w - 1] = pts[i];
        } else {
            pts[w++] = pts[i];
        }
    }

    {
        const double dx = (double)pts[n - 1].x - (double)pts[w - 1].x;
        const double dy = (double)pts[n - 1].y - (double)pts[w - 1].y;

        if (dx * dx + dy * dy < min_seg2 && w >= 2) {
            pts[w - 1] = pts[n - 1];
        } else {
            pts[w++] = pts[n - 1];
        }
    }

    return w;
}

#if VMAP_ROAD_SIMPLIFY
static int32_t simplify_road_polyline(vmap_point_t * pts, int32_t n,
    float min_seg_px, float max_dev_px)
{
    return vmap_render_simplify_polyline(pts, n, min_seg_px, max_dev_px);
}
#endif /* VMAP_ROAD_SIMPLIFY */

/**
 * Skip area:highway leftovers: a closed ring whose interior is much wider
 * than a schematic stroke (roundabouts stay — they are compact).
 */
static bool road_ring_is_area(const vmap_point_t * pts, int32_t n)
{
    int32_t i;
    int32_t dx;
    int32_t dy;
    double area2;
    double peri;
    double compactness;
    double mean_w;

    if (!pts || n < 5) {
        return false;
    }

    dx = pts[0].x - pts[n - 1].x;
    dy = pts[0].y - pts[n - 1].y;
    if (dx * dx + dy * dy > 16) {
        return false;
    }

    area2 = 0.0;
    peri = 0.0;
    for (i = 0; i < n - 1; i++) {
        area2 += (double)pts[i].x * (double)pts[i + 1].y
            - (double)pts[i + 1].x * (double)pts[i].y;
        peri += hypot((double)(pts[i + 1].x - pts[i].x),
            (double)(pts[i + 1].y - pts[i].y));
    }

    area2 = fabs(area2);
    if (peri < 8.0) {
        return false;
    }

    compactness = (2.0 * M_PI * area2) / (peri * peri);
    mean_w = area2 / peri;
    if (compactness > 0.55) {
        return false;
    }

    return mean_w > 10.0 && compactness < 0.45;
}

static void draw_road_polyline(vmap_canvas_t * c, const vmap_point_t * pts,
    int32_t n, int32_t width, uint16_t color)
{
    const int32_t join_r = (width + 1) / 2;
    int32_t i;

    if (!c || n < 2 || width < 1) {
        return;
    }

    if (road_ring_is_area(pts, n)) {
        return;
    }

    vmap_render_stroke_ribbon(c, pts, n, width, color);

    for (i = 0; i < n; i++) {
        vmap_render_fill_disc(c, pts[i].x, pts[i].y, join_r, color);
    }
}

static void draw_road_feature_line(vmap_canvas_t * c, const vmap_feature_t * f,
    float base_x, float base_y, float u, int32_t width, uint16_t color,
    vmap_point_t * scratch, int scratch_cap)
{
    int32_t n;
    int32_t i;

    if (!f || f->point_count < 2 || width < 1 || !scratch || scratch_cap < 2) {
        return;
    }

    n = f->point_count;
    if (n > scratch_cap) {
        n = scratch_cap;
    }

    for (i = 0; i < n; i++) {
        scratch[i].x = (int32_t)lroundf(base_x + vmap_feature_x(f, (uint16_t)i) * u);
        scratch[i].y = (int32_t)lroundf(base_y + vmap_feature_y(f, (uint16_t)i) * u);
    }

#if VMAP_ROAD_SIMPLIFY
    n = simplify_road_polyline(scratch, n, VMAP_ROAD_MIN_SEG_PX,
        VMAP_ROAD_MAX_DEV_PX);
    if (n >= 4) {
        n = simplify_road_polyline(scratch, n, VMAP_ROAD_MIN_SEG_PX,
            VMAP_ROAD_MAX_DEV_PX);
    }
#endif

    draw_road_polyline(c, scratch, n, width, color);
}

static void draw_feature_line(vmap_canvas_t * c, const vmap_feature_t * f,
    float base_x, float base_y, float u, int32_t width, uint16_t color)
{
    int32_t join_r;
    int32_t px;
    int32_t py;
    uint16_t i;

    if (f->point_count < 2 || width < 1) {
        return;
    }

    join_r = (width + 1) / 2;
    px = (int32_t)(base_x + vmap_feature_x(f, 0) * u);
    py = (int32_t)(base_y + vmap_feature_y(f, 0) * u);

    for (i = 1; i < f->point_count; i++) {
        int32_t nx = (int32_t)(base_x + vmap_feature_x(f, i) * u);
        int32_t ny = (int32_t)(base_y + vmap_feature_y(f, i) * u);
        vmap_render_draw_thick_line(c, px, py, nx, ny, width, color);
        px = nx;
        py = ny;
    }

    for (i = 0; i < f->point_count; i++) {
        int32_t x = (int32_t)(base_x + vmap_feature_x(f, i) * u);
        int32_t y = (int32_t)(base_y + vmap_feature_y(f, i) * u);
        vmap_render_fill_disc(c, x, y, join_r, color);
    }
}

static uint16_t land_fill_color(const vmap_style_t * style, uint8_t attr)
{
    switch (attr) {
    case VMAP_LAND_COMMERCIAL:
    case VMAP_LAND_INDUSTRIAL:
        return RGB565(style->land_com_r, style->land_com_g, style->land_com_b);
    case VMAP_LAND_EDU:
        return RGB565(style->land_edu_r, style->land_edu_g, style->land_edu_b);
    case VMAP_LAND_PARK:
        return RGB565(style->land_park_r, style->land_park_g, style->land_park_b);
    default:
        return RGB565(style->land_res_r, style->land_res_g, style->land_res_b);
    }
}

static void draw_land_layer(vmap_canvas_t * c, vmap_feature_iter_t it,
    uint16_t extent, float base_x, float base_y, float scale,
    vmap_point_t * scratch, int scratch_cap)
{
    const vmap_style_t * style = vmap_style_current();
    float u = 256.0f * scale / (float)extent;
    vmap_feature_t f;

    while (vmap_feature_iter_next(&it, &f)) {
        int32_t n;
        int32_t i;

        if (!vmap_feature_is_closed(&f) || f.point_count < 3) {
            continue;
        }
        n = f.point_count;
        if (n > scratch_cap) {
            n = scratch_cap;
        }
        for (i = 0; i < n; i++) {
            scratch[i].x = (int32_t)(base_x + vmap_feature_x(&f, (uint16_t)i) * u);
            scratch[i].y = (int32_t)(base_y + vmap_feature_y(&f, (uint16_t)i) * u);
        }
        fill_polygon(c, scratch, n, land_fill_color(style, f.attr));
    }
}

static void draw_poly_layer(vmap_canvas_t * c, vmap_feature_iter_t it,
    uint16_t extent, float base_x, float base_y, float scale, uint16_t fill_color,
    uint16_t stroke_color, int32_t stroke_w, vmap_point_t * scratch, int scratch_cap)
{
    float u = 256.0f * scale / (float)extent;
    vmap_feature_t f;

    while (vmap_feature_iter_next(&it, &f)) {
        int32_t n;
        int32_t i;

        if (!vmap_feature_is_closed(&f) || f.point_count < 3) {
            continue;
        }
        n = f.point_count;
        if (n > scratch_cap) {
            n = scratch_cap;
        }
        for (i = 0; i < n; i++) {
            scratch[i].x = (int32_t)(base_x + vmap_feature_x(&f, (uint16_t)i) * u);
            scratch[i].y = (int32_t)(base_y + vmap_feature_y(&f, (uint16_t)i) * u);
        }
        fill_polygon(c, scratch, n, fill_color);
        if (stroke_w > 0) {
            stroke_polygon(c, scratch, n, stroke_w, stroke_color);
        }
    }
}

static void draw_road_layer(vmap_canvas_t * c, const vmap_tile_t * tile,
    uint16_t extent, float base_x, float base_y, float scale,
    vmap_point_t * scratch, int scratch_cap)
{
    const vmap_style_t * style = vmap_style_current();
    float u = 256.0f * scale / (float)extent;
    int cls;
    const uint16_t case_color = RGB565(style->road_case_r, style->road_case_g,
        style->road_case_b);

    for (cls = VMAP_ROAD_PATH; cls >= VMAP_ROAD_MOTORWAY; cls--) {
        vmap_feature_iter_t it = vmap_tile_layer_iter(tile, VMAP_LAYER_ROAD);
        vmap_feature_t f;
        while (vmap_feature_iter_next(&it, &f)) {
            int raw;
            const vmap_style_road_t * st;
            int32_t case_w;

            if (f.point_count < 2 || vmap_feature_is_closed(&f)) {
                continue;
            }
            raw = f.attr;
            if (raw < 1 || raw >= ROAD_STYLE_NUM) {
                raw = VMAP_ROAD_RESIDENTIAL;
            }
            if (raw != cls) {
                continue;
            }
            st = road_style_for(raw);
            case_w = (int32_t)st->w + (int32_t)style->road_case_extra;
            if (case_w > (int32_t)st->w) {
                draw_road_feature_line(c, &f, base_x, base_y, u, case_w, case_color,
                    scratch, scratch_cap);
            }
        }
    }

    for (cls = VMAP_ROAD_PATH; cls >= VMAP_ROAD_MOTORWAY; cls--) {
        vmap_feature_iter_t it = vmap_tile_layer_iter(tile, VMAP_LAYER_ROAD);
        vmap_feature_t f;
        while (vmap_feature_iter_next(&it, &f)) {
            int raw;
            const vmap_style_road_t * st;
            uint16_t color;

            if (f.point_count < 2 || vmap_feature_is_closed(&f)) {
                continue;
            }
            raw = f.attr;
            if (raw < 1 || raw >= ROAD_STYLE_NUM) {
                raw = VMAP_ROAD_RESIDENTIAL;
            }
            if (raw != cls) {
                continue;
            }
            st = road_style_for(raw);
            color = RGB565(st->r, st->g, st->b);
            draw_road_feature_line(c, &f, base_x, base_y, u, (int32_t)st->w, color,
                scratch, scratch_cap);
        }
    }
}

static void draw_line_layer(vmap_canvas_t * c, vmap_feature_iter_t it,
    uint16_t extent, float base_x, float base_y, float scale)
{
    const vmap_style_t * style = vmap_style_current();
    float u = 256.0f * scale / (float)extent;
    const uint16_t color = RGB565(style->waterway_r, style->waterway_g,
        style->waterway_b);
    vmap_feature_t f;

    while (vmap_feature_iter_next(&it, &f)) {
        if (f.point_count < 2) {
            continue;
        }
        draw_feature_line(c, &f, base_x, base_y, u, (int32_t)style->waterway_w,
            color);
    }
}

/**
 * @brief 用纯色清画布。
 * @param c 画布。
 * @param color RGB565。
 */
void vmap_render_clear(vmap_canvas_t * c, uint16_t color)
{
    if (!c || !c->buf) {
        return;
    }

    const uint32_t pair = ((uint32_t)color << 16) | (uint32_t)color;
    const int32_t pairs = c->w / 2;
    const bool odd = (c->w & 1) != 0;

    for (int32_t y = 0; y < c->h; y++) {
        uint16_t * row = (uint16_t *)((uint8_t *)c->buf + (size_t)y * c->stride);
        uint32_t * row32 = (uint32_t *)row;
        for (int32_t x = 0; x < pairs; x++) {
            row32[x] = pair;
        }
        if (odd) {
            row[c->w - 1] = color;
        }
    }
}

static void render_fill_span(uint16_t * row, int32_t x0, int32_t x1, uint16_t color)
{
    int32_t x;

    if (!row || x0 >= x1) {
        return;
    }

    for (x = x0; x < x1; x++) {
        row[x] = color;
    }
}

void vmap_render_scroll(vmap_canvas_t * c, int32_t dx, int32_t dy, uint16_t fill)
{
    int32_t y;
    int32_t y_start;
    int32_t y_end;
    int32_t y_step;
    int32_t copy_w;

    if (!c || !c->buf) {
        return;
    }

    if (dx == 0 && dy == 0) {
        return;
    }

    if (dx >= c->w || dx <= -c->w || dy >= c->h || dy <= -c->h) {
        vmap_render_clear(c, fill);
        return;
    }

    if (dy > 0) {
        y_start = 0;
        y_end = c->h;
        y_step = 1;
    } else {
        y_start = c->h - 1;
        y_end = -1;
        y_step = -1;
    }

    copy_w = c->w - (dx >= 0 ? dx : -dx);

    for (y = y_start; y != y_end; y += y_step) {
        int32_t sy = y + dy;
        uint16_t * dst = (uint16_t *)((uint8_t *)c->buf + (size_t)y * c->stride);

        if (sy < 0 || sy >= c->h) {
            render_fill_span(dst, 0, c->w, fill);
            continue;
        }

        {
            uint16_t * src = (uint16_t *)((uint8_t *)c->buf
                + (size_t)sy * c->stride);

            if (dx == 0) {
                if (src != dst) {
                    memmove(dst, src, (size_t)c->w * sizeof(uint16_t));
                }
            } else if (dx > 0) {
                memmove(dst, src + dx, (size_t)copy_w * sizeof(uint16_t));
                render_fill_span(dst, c->w - dx, c->w, fill);
            } else {
                memmove(dst - dx, src, (size_t)copy_w * sizeof(uint16_t));
                render_fill_span(dst, 0, -dx, fill);
            }
        }
    }
}

static void fill_rect(vmap_canvas_t * c, int32_t x1, int32_t y1, int32_t x2,
    int32_t y2, uint16_t color)
{
    int32_t y;

    if (!c || x1 >= x2 || y1 >= y2) {
        return;
    }
    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 > c->w) {
        x2 = c->w;
    }
    if (y2 > c->h) {
        y2 = c->h;
    }
    if (x1 >= x2 || y1 >= y2) {
        return;
    }

    for (y = y1; y < y2; y++) {
        blend_span(c, x1, x2 - 1, y, color);
    }
}

/**
 * @brief 将瓦片绘制到画布。
 */
void vmap_render_draw_tile(vmap_canvas_t * c, const vmap_tile_t * tile,
    float base_x, float base_y, float scale, vmap_point_t * scratch, int scratch_cap)
{
    const vmap_style_t * style;
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;

    if (!c || !tile) {
        return;
    }

    /* Vector layers do not cover every pixel. Track/route ribbons are stamped
     * on top; without a bg fill, restamping leaves those pixels behind. */
    x1 = (int32_t)floorf(base_x);
    y1 = (int32_t)floorf(base_y);
    x2 = (int32_t)ceilf(base_x + 256.0f * scale);
    y2 = (int32_t)ceilf(base_y + 256.0f * scale);
    fill_rect(c, x1, y1, x2, y2, vmap_style_bg565());

    style = vmap_style_current();
    uint16_t extent = vmap_tile_extent(tile);
    const uint16_t forest = RGB565(style->forest_r, style->forest_g,
        style->forest_b);
    const uint16_t water = RGB565(style->water_r, style->water_g, style->water_b);

    draw_land_layer(c, vmap_tile_layer_iter(tile, VMAP_LAYER_LAND), extent,
        base_x, base_y, scale, scratch, scratch_cap);
    draw_poly_layer(c, vmap_tile_layer_iter(tile, VMAP_LAYER_FOREST), extent,
        base_x, base_y, scale, forest, 0, 0, scratch, scratch_cap);
    draw_poly_layer(c, vmap_tile_layer_iter(tile, VMAP_LAYER_WATER), extent,
        base_x, base_y, scale, water, 0, 0, scratch, scratch_cap);
    draw_line_layer(c, vmap_tile_layer_iter(tile, VMAP_LAYER_WATERWAY), extent,
        base_x, base_y, scale);
    draw_road_layer(c, tile, extent, base_x, base_y, scale, scratch, scratch_cap);
}
