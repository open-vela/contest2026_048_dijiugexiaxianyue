/*
 * vmap 像素核心的宿主差分测试。
 *
 * 用途：证明对 vmap_render.c 的优化（按行裁剪并集 / 32 位成对写 / 圆盘去 sqrt /
 * 多边形填充的逐行增量推进）在**像素上逐位不变**。它是 README.md §3 那套说法的
 * 载体 —— 不靠保证，靠跑。
 *
 * 原理
 * ----
 * 本文件被**拼在 vmap_render.c 之后**编成同一个编译单元，所以能直接调用它的
 * static 函数（cover_span_fp / fill_polygon_fp / fill_disc_aa_fp / blend_span /
 * fill_run_u16 ...）。配 -ffunction-sections -Wl,--gc-sections 把不需要的
 * 外部符号（vmap_style_current 等）整段丢掉，只需 verify/stubs_host.c 里那几个
 * 与 EPIC 搬运有关的桩。
 *
 * 两个二进制（改动前 / 改动后）吃同一批随机输入，比对整块画布的字节与逐用例
 * 哈希。刻意设计：
 *   - 画布宽取 67（奇数）：走成对写的"奇数起点 / 奇数尾"分支；
 *   - 裁剪矩形一半概率**互相重叠**：专抓"像素被混合两次"；
 *   - fill_run_u16 的行指针带 0~3 的偏移：专抓"按地址对齐"这个坑
 *     （vmap_render_scroll() 会把行指针本身偏移 dx 再交进来）。
 *
 * 另外还有一组**多边形专项**（`T_POLY_CASES`，见 `t_poly_suite()`）：随机用例的
 * y 量程压不到"跨几十行"的边（AA 那条只有 ±3.5 px、最多盖 8 行），而"每条边只算
 * 一次斜率、之后逐行推进"这类改动的错**恰恰只在跨行时才露出来**，所以它用确定性
 * 构造补齐三件事：跨满画布的边、n 超过实现里状态数组容量的多边形、以及半开区间
 * `[yi, yj)` 的边界（顶点正好落在扫描行下界与中心）。
 *
 * 怎么跑
 * ------
 * 在 openvela 根目录下（桩与两个假头**已入库**，所以这一段现在能照抄）：
 *
 *   V=vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/src/vmap
 *   R=vendor/my_vendor
 *   REL=boards/sf32lb52/my_vendor/ui/bicycle/src/vmap/vmap_render.c
 *   H=vendor/my_vendor/docs/map/perf/verify/harness.c
 *   ST=vendor/my_vendor/docs/map/perf/verify/stubs_host.c
 *   FA=vendor/my_vendor/docs/map/perf/verify/fake          # 挡 nuttx/config.h、board_malloc.h
 *
 *   # 基线版本要用**具体的 commit**，不要用 HEAD：
 *   # 一旦这次优化被提交，HEAD 里就已经是改动后的代码，对比就没意义了。
 *   # 而且必须是"搬运后端接进来**之后**"的提交（见下），否则会比两件事。
 *   BASE=<改动前的 commit>
 *
 *   FLAGS="-x c - -I$FA -I$V -O1 -ffunction-sections -fdata-sections -Wl,--gc-sections -lm -DVMAP_RENDER_PRIM=0"
 *   ( git -C $R show $BASE:$REL; cat $V/vmap_blit.c; cat $ST; cat $H ) | gcc $FLAGS -o /tmp/vmap_old
 *   ( cat $V/vmap_render.c;        cat $V/vmap_blit.c; cat $ST; cat $H ) | gcc $FLAGS -o /tmp/vmap_new
 *   diff <(/tmp/vmap_old) <(/tmp/vmap_new) && echo "IDENTICAL"
 *
 * `-DVMAP_RENDER_PRIM=0` 是必须的：`vmap_render.c` 里的图元级周期埋点要读 DWT
 * （宿主机上没有这块寄存器）。
 *
 * ⚠ 基线要选在 `0be501d0`（scroll/clear 改走 `vmap_blit_*`）**之后** —— 更早的
 * 提交也能跑，但那比的是两件事（scroll 重构 + 搬运接入），要接受这一点才用。
 *
 * ⚠ 它只覆盖**像素原语**（cover_span/fill_polygon/圆盘/blend/scroll/clear）——
 * 瓦片与矢量那条路（`vmap_render_draw_tile` 及其层函数）没被覆盖，见
 * ../GPU_ACCEL.md §2.2 关于旋转改造的验证说明。
 *
 * 要更严格就换 -O 级别各跑一遍（UB 会在不同 -O 下露出不同结果），
 * 并加大用例数 / 换种子：
 *
 *   -O0 / -O1 / -O2
 *   -DCONFIG_MYVENDOR_VMAP_RENDER_AA=0 -Wno-error=implicit-function-declaration
 *                                       # 非 AA 路径（GCC 14 下 `box_may_write`
 *                                       # 的隐式声明会变成 error，是**改动前就有**
 *                                       # 的问题，用这个开关降回 warning）
 *   -DT_CASES=20000 -DT_POLY_CASES=3000 -DT_SEED0=0xdeadbeefu
 *   -DT_CANVAS_W=101 -DT_CANVAS_H=301    # 高画布：跨几百行的边
 *
 * 2026-09-25 实测（14 种配置 × 2 万个随机用例 + 每组 2000~3000 个多边形专项
 * ≈ 30 万例）**全部 IDENTICAL**：-O0/-O1/-O2 × AA=1/0 × 两个种子，外加
 * 101×301 高画布两档。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 画布尺寸可覆盖：换成高画布（-DT_CANVAS_H=301）能把"跨几百行的边"也压到，
 * 而逐行增量推进的错在长累积下更容易露出来。 */
#ifndef T_CANVAS_W
#define T_CANVAS_W 67               /* 奇数宽：会走成对写的"奇数起点/奇数尾"分支 */
#endif
#ifndef T_CANVAS_H
#define T_CANVAS_H 41
#endif
#ifndef T_CASES
#define T_CASES    4000
#endif
#ifndef T_SEED0
#define T_SEED0    0x1234567u
#endif
#ifndef T_POLY_CASES
#define T_POLY_CASES 600
#endif

static uint16_t g_fb[T_CANVAS_H * T_CANVAS_W];
static uint32_t g_rng;

static uint32_t t_rnd(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 9;
}

static int32_t t_range(int32_t lo, int32_t hi)
{
    if (hi <= lo) {
        return lo;
    }
    return lo + (int32_t)(t_rnd() % (uint32_t)(hi - lo + 1));
}

static uint32_t t_hash(const void * p, size_t bytes)
{
    const uint8_t * b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    size_t i;

    for (i = 0; i < bytes; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

static void t_reset(void)
{
    int i;

    for (i = 0; i < T_CANVAS_H * T_CANVAS_W; i++) {
        g_fb[i] = (uint16_t)t_rnd();
    }
}

static void t_canvas(vmap_canvas_t * c, int nclip)
{
    int i;

    memset(c, 0, sizeof(*c));
    c->buf = g_fb;
    c->w = T_CANVAS_W;
    c->h = T_CANVAS_H;
    c->stride = T_CANVAS_W * 2;
    c->clip_n = 0;
    (void)nclip;
}

/* 加 n 个裁剪矩形；有一半概率让它们互相重叠，专门盯"像素被混合两次"这类错。 */
static void t_clips(vmap_canvas_t * c, int n, int allow_overlap)
{
    int i;

    for (i = 0; i < n && i < VMAP_RENDER_DIRTY_MAX; i++) {
        int32_t x1 = t_range(-6, T_CANVAS_W + 4);
        int32_t y1 = t_range(-6, T_CANVAS_H + 4);
        int32_t x2 = allow_overlap ? x1 + t_range(1, 24) : x1 + t_range(1, 12);
        int32_t y2 = allow_overlap ? y1 + t_range(1, 20) : y1 + t_range(1, 10);

        vmap_render_clip_add(c, x1, y1, x2, y2);
    }
}

/* ——— 多边形专项（确定性构造）—————————————————————————————
 *
 * 随机用例的 y 量程压不到"跨几十行"的边：AA 那条只有 ±900 fp ≈ ±3.5 px，整条
 * 多边形最多盖 8 行；而"每条边只算一次斜率、之后逐行增量推进"这类改动的错**恰恰
 * 只在跨行时才会露出来**（首行播种是对的、后面越推越偏）。所以这一组补齐三件事：
 *
 *   ① 跨满画布的边（y 量程开到 ±3 个画布高，斜率从"极陡"到"极缓"都有）；
 *   ② n 超过实现里状态数组容量的多边形（走"超过上限照旧逐行算"的那条支路，
 *      同时也是"第 64 条之后的边仍用原式"这条等价性的证据）；
 *   ③ 半开区间 [yi, yj) 的边界：顶点正好落在扫描行的**下界**与**中心**上
 *      （`yi <= y_scan && yj > y_scan` 的等号是唯一会"差一个扫描行"的地方）。
 *
 * 刻意全是确定性构造（不含随机分支），所以它像一个真值表：任何一条走错都会让
 * 两个二进制的哈希对不上。
 */
static uint32_t t_poly_suite(void)
{
    static const int32_t sub[5] = { 0, 1, 127, 128, 255 };
    uint32_t acc = 0;
    int t;

    for (t = 0; t < T_POLY_CASES; t++) {
        vmap_canvas_t c;
        int nc = (t % 5 == 0) ? 0 : t_range(0, 4);

        g_rng = T_SEED0 ^ (0x9e3779b9u + (uint32_t)t * 2654435761u);
        t_reset();
        t_canvas(&c, nc);
        t_clips(&c, nc, (t % 3) == 0);

        switch (t % 6) {
        case 0:                                  /* 满幅整数多边形 */
        {
            vmap_point_t pts[24];
            int n = t_range(3, 24);
            int i;

            for (i = 0; i < n; i++) {
                pts[i].x = t_range(-2 * T_CANVAS_W, 3 * T_CANVAS_W);
                pts[i].y = t_range(-2 * T_CANVAS_H, 3 * T_CANVAS_H);
            }
            fill_polygon(&c, pts, n, (uint16_t)t_rnd());
            break;
        }
        case 1:                                  /* n 超过边状态数组的容量 */
        {
            vmap_point_t pts[96];
            int n = t_range(65, 96);
            int i;

            for (i = 0; i < n; i++) {
                pts[i].x = t_range(-2 * T_CANVAS_W, 3 * T_CANVAS_W);
                pts[i].y = t_range(-T_CANVAS_H, 2 * T_CANVAS_H);
            }
            fill_polygon(&c, pts, n, (uint16_t)t_rnd());
            break;
        }
        case 2:                                  /* 顶点落在整数行（半开区间下界） */
        {
            vmap_point_t pts[16];
            int n = t_range(3, 16);
            int i;

            for (i = 0; i < n; i++) {
                pts[i].x = t_range(-3 * T_CANVAS_W, 4 * T_CANVAS_W);
                pts[i].y = t_range(-4, T_CANVAS_H + 3);   /* 整行号，含界外 */
            }
            fill_polygon(&c, pts, n, (uint16_t)t_rnd());
#if VMAP_RENDER_AA
            {
                vmap_fp_point_t fp[16];

                for (i = 0; i < n; i++) {
                    fp[i].x = pts[i].x * 256;
                    fp[i].y = pts[i].y * 256;     /* 正好落在扫描行的下界 */
                }
                fill_polygon_fp(&c, fp, n, (uint16_t)t_rnd());
            }
#endif
            break;
        }
        case 3:                                  /* 顶点落在扫描行中心（等号边界） */
        {
            vmap_point_t pts[16];
            int n = t_range(3, 16);
            int i;

            for (i = 0; i < n; i++) {
                pts[i].x = t_range(-2 * T_CANVAS_W, 3 * T_CANVAS_W);
                pts[i].y = t_range(-4, T_CANVAS_H + 3);
            }
            fill_polygon(&c, pts, n, (uint16_t)t_rnd());
#if VMAP_RENDER_AA
            {
                vmap_fp_point_t fp[16];

                for (i = 0; i < n; i++) {
                    fp[i].x = pts[i].x * 256 + 128;
                    fp[i].y = pts[i].y * 256 + 128;   /* == y_scan */
                }
                fill_polygon_fp(&c, fp, n, (uint16_t)t_rnd());
            }
#endif
            break;
        }
        case 4:                                  /* 水平边 / 重复点 / 零长度边 */
        {
            vmap_point_t pts[16];
            int n = 0;

            pts[n].x = t_range(0, T_CANVAS_W); pts[n].y = t_range(0, T_CANVAS_H - 1); n++;
            pts[n] = pts[n - 1];                                   /* 重复点 */
            n++;
            pts[n].x = pts[n - 1].x + t_range(1, 30); pts[n].y = pts[n - 1].y; n++;
            pts[n].x = pts[n - 1].x; pts[n].y = t_range(0, T_CANVAS_H - 1); n++;
            pts[n].x = t_range(-10, T_CANVAS_W + 10); pts[n].y = pts[n - 1].y; n++;
            pts[n].x = t_range(-10, T_CANVAS_W + 10); pts[n].y = pts[0].y; n++;
            fill_polygon(&c, pts, n, (uint16_t)t_rnd());
#if VMAP_RENDER_AA
            {
                vmap_fp_point_t fp[16];
                int i;

                for (i = 0; i < n; i++) {
                    fp[i].x = pts[i].x * 256;
                    fp[i].y = pts[i].y * 256 + sub[t_range(0, 4)];
                }
                fill_polygon_fp(&c, fp, n, (uint16_t)t_rnd());
            }
#endif
            break;
        }
        default:                                 /* 细长条：dy = ±1、dx 很大 */
        {
            vmap_point_t pts[8];
            int32_t y0 = t_range(-3, T_CANVAS_H + 2);
            int32_t y1 = y0 + t_range(-1, 1);
            int32_t x0 = t_range(-4 * T_CANVAS_W, 5 * T_CANVAS_W);
            int32_t x1 = t_range(-4 * T_CANVAS_W, 5 * T_CANVAS_W);

            pts[0].x = x0; pts[0].y = y0;
            pts[1].x = x1; pts[1].y = y1;
            pts[2].x = x1; pts[2].y = y1 + t_range(1, T_CANVAS_H);
            pts[3].x = x0; pts[3].y = pts[2].y;
            fill_polygon(&c, pts, 4, (uint16_t)t_rnd());
#if VMAP_RENDER_AA
            {
                vmap_fp_point_t fp[4];
                int i;

                for (i = 0; i < 4; i++) {
                    fp[i].x = pts[i].x * 256;
                    fp[i].y = pts[i].y * 256 + sub[t_range(0, 4)];
                }
                fill_polygon_fp(&c, fp, 4, (uint16_t)t_rnd());
            }
#endif
            break;
        }
        }

        acc = acc * 31u + t_hash(g_fb, sizeof(g_fb));
    }

    printf("TOTAL2 polycases=%d acc=%08x\n", T_POLY_CASES, (unsigned)acc);
    return acc;
}

int main(int argc, char ** argv)
{
    uint32_t acc = 0;
    int verbose = (argc > 1 && argv[1][0] == 'v');
    int k;

    acc = t_poly_suite();

    for (k = 0; k < T_CASES; k++) {
        vmap_canvas_t c;
        uint32_t h;
        int kind = k % 14;
        int nc = (k % 5 == 0) ? 0 : t_range(0, 4);
        int ov = (k % 3) == 0;

        g_rng = T_SEED0 + (uint32_t)k * 2654435761u;
        t_reset();

        switch (kind) {
#if VMAP_RENDER_AA
        case 0:                                  /* cover_span_fp */
        case 1:
            t_canvas(&c, nc);
            t_clips(&c, nc, ov);
            cover_span_fp(&c, t_range(-3, T_CANVAS_H + 3),
                t_range(-4000, 4000), t_range(-4000, 4000),
                (uint16_t)t_rnd());
            break;
        case 2:                                  /* 窄跨度：不足一个像素 */
            t_canvas(&c, nc);
            t_clips(&c, nc, ov);
            {
                int32_t x0 = t_range(-500, 500);
                cover_span_fp(&c, t_range(0, T_CANVAS_H - 1), x0, x0 + t_range(0, 240),
                    (uint16_t)t_rnd());
            }
            break;
        case 3:                                  /* fill_polygon_fp */
            t_canvas(&c, nc);
            t_clips(&c, nc, ov);
            {
                vmap_fp_point_t pts[18];
                int n = t_range(3, 18);
                int i;
                for (i = 0; i < n; i++) {
                    pts[i].x = t_range(-1200, 1200);
                    pts[i].y = t_range(-900, 900);
                }
                fill_polygon_fp(&c, pts, n, (uint16_t)t_rnd());
            }
            break;
        case 4:                                  /* fill_disc_aa_fp */
        case 5:
            t_canvas(&c, nc);
            t_clips(&c, nc, ov);
            fill_disc_aa_fp(&c, t_range(-2000, 2000), t_range(-2000, 2000),
                t_range(1, 3000), (uint16_t)t_rnd());
            break;
        case 6:                                  /* vmap_render_fill_disc（整半径） */
            t_canvas(&c, nc);
            t_clips(&c, nc, ov);
            vmap_render_fill_disc(&c, t_range(-8, T_CANVAS_W + 8),
                t_range(-8, T_CANVAS_H + 8), t_range(0, 14), (uint16_t)t_rnd());
            break;
        case 7:                                  /* 大半径圆盘 */
            t_canvas(&c, nc);
            t_clips(&c, nc, ov);
            fill_disc_aa_fp(&c, t_range(0, T_CANVAS_W) << 8, t_range(0, T_CANVAS_H) << 8,
                t_range(3000, 12000), (uint16_t)t_rnd());
            break;
#else
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
            t_canvas(&c, nc);
            t_clips(&c, nc, ov);
            {
                vmap_point_t pts[18];
                int n = t_range(3, 18);
                int i;
                for (i = 0; i < n; i++) {
                    pts[i].x = t_range(-60, T_CANVAS_W + 60);
                    pts[i].y = t_range(-40, T_CANVAS_H + 40);
                }
                fill_polygon(&c, pts, n, (uint16_t)t_rnd());
            }
            break;
#endif
        case 8:                                  /* blend_span */
        case 9:
            t_canvas(&c, nc);
            t_clips(&c, nc, ov);
            blend_span(&c, t_range(-40, T_CANVAS_W + 40),
                t_range(-40, T_CANVAS_W + 40), t_range(-3, T_CANVAS_H + 3),
                (uint16_t)t_rnd());
            break;
        case 10:                                 /* fill_run_u16：含奇数对齐 */
            t_canvas(&c, nc);
            t_clips(&c, nc, ov);
            {
                int32_t y = t_range(0, T_CANVAS_H - 1);
                int32_t off = t_range(0, 3);
                uint16_t * row = (uint16_t *)((uint8_t *)g_fb + (size_t)y * c.stride)
                    + off;
                /* render_fill_span() 在 2026-09-25 的"矩形+两条边"改造里删掉了
                 * （无调用者，补边改走 vmap_blit_fill），它只是 fill_run_u16 的
                 * 薄包装 —— 直接测底下这层，奇数对齐这个坑照抓。 */
                fill_run_u16(row, t_range(-2, 20), t_range(1, T_CANVAS_W - off),
                    (uint16_t)t_rnd());
            }
            break;
        case 12:                                 /* vmap_render_clear（整块清） */
            t_canvas(&c, nc);
            vmap_render_clear(&c, (uint16_t)t_rnd());
            break;
        case 13:                                 /* vmap_render_scroll（块拷贝+补边） */
            t_canvas(&c, nc);
            {
                /* dx/dy 覆盖：0、±1（奇偶对齐分界）、随机、以及超出画布
                 * （会退化成整块清屏）几种。 */
                static const int32_t probe[] = { 0, 1, -1, 2, -2, 3, -3, 117, -117, 9999 };
                int32_t dx = probe[t_range(0, 9)];
                int32_t dy = probe[t_range(0, 9)];
                vmap_render_scroll(&c, dx, dy, (uint16_t)t_rnd());
            }
            break;
        default:                                 /* 混合序列，盯交互 */
            t_canvas(&c, nc);
            t_clips(&c, nc, ov);
            {
                int i;
                for (i = 0; i < 6; i++) {
                    if ((i & 1) == 0) {
                        blend_span(&c, t_range(-20, T_CANVAS_W + 20),
                            t_range(-20, T_CANVAS_W + 20), t_range(-2, T_CANVAS_H + 2),
                            (uint16_t)t_rnd());
                    } else {
#if VMAP_RENDER_AA
                        cover_span_fp(&c, t_range(-2, T_CANVAS_H + 2),
                            t_range(-3000, 3000), t_range(-3000, 3000),
                            (uint16_t)t_rnd());
#endif
                    }
                }
            }
            break;
        }

        h = t_hash(g_fb, sizeof(g_fb));
        acc = acc * 31u + h;
        if (verbose) {
            printf("case=%d kind=%d clip=%d hash=%08x\n", k, kind, c.clip_n,
                (unsigned)h);
        }
    }

    printf("TOTAL cases=%d acc=%08x\n", T_CASES, (unsigned)acc);

#ifdef T_GATE
    /* box_may_write（bbox 早退的闸门）真值表：判"写不到"时，盒内必须**没有任何**
     * 像素可写 —— 暴力核对。判错就会漏画，所以这条必须在。
     * 只有带这个闸门的版本才编（旧基线没有该函数），因此单独一段、不进 acc。 */
    {
        int g;
        int bad = 0;

        for (g = 0; g < T_CASES; g++) {
            vmap_canvas_t c;
            int nc = (g % 5 == 0) ? 0 : t_range(0, 4);
            int ov = (g % 3) == 0;
            int32_t x1;
            int32_t y1;
            int32_t x2;
            int32_t y2;
            int32_t px;
            int32_t py;

            g_rng = T_SEED0 ^ 0x5bf03635u + (uint32_t)g * 2654435761u;
            t_canvas(&c, nc);
            t_clips(&c, nc, ov);

            x1 = t_range(-30, T_CANVAS_W + 30);
            y1 = t_range(-30, T_CANVAS_H + 30);
            x2 = x1 + t_range(-5, 40);
            y2 = y1 + t_range(-5, 30);

            if (box_may_write(&c, x1, y1, x2, y2)) {
                continue;
            }
            for (py = y1; py <= y2; py++) {
                for (px = x1; px <= x2; px++) {
                    if ((uint32_t)px >= (uint32_t)T_CANVAS_W
                        || (uint32_t)py >= (uint32_t)T_CANVAS_H) {
                        continue;
                    }
                    if (canvas_in_clip(&c, px, py)) {
                        printf("GATE-BUG box=(%d,%d)-(%d,%d) px=(%d,%d)\n",
                            (int)x1, (int)y1, (int)x2, (int)y2, (int)px, (int)py);
                        bad++;
                    }
                }
            }
        }
        printf("GATE cases=%d bad=%d\n", T_CASES, bad);
        if (bad != 0) {
            return 1;
        }
    }
#endif
    return 0;
}
