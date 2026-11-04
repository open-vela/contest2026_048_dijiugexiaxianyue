/**
 * @file lv_draw_sifli_epic.c
 *
 */

/**
 * Copyright 2024 SiFli Technologies
 *
 * SPDX-License-Identifier: MIT
 */

/*********************
 *      INCLUDES
 *********************/

#include "lv_draw_sifli_epic.h"

#if LV_USE_SIFLI_EPIC
#include "lv_sifli_epic_utils.h"
#include "lv_sifli_epic_cfg.h"
#include "../epic_mw/epic_mw.h"
#include "../../../display/lv_display_private.h"
#include "../../../font/lv_font.h"
#include "../../../misc/lv_log.h"
#include "../../../misc/lv_math.h"
#include "../../../misc/lv_text.h"
#include <stdio.h>

#if LV_USE_NUTTX
    #include <pthread.h>
    #include <sched.h>
#endif

/*********************
 *      DEFINES
 *********************/

#define DRAW_UNIT_ID_SIFLI_EPIC 11

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**
 * Evaluate a task and set the score and preferred EPIC unit.
 * @param draw_unit Draw unit
 * @param task Draw task
 * @return 1 if task is preferred, 0 otherwise
 */
static int32_t _epic_evaluate(lv_draw_unit_t * draw_unit, lv_draw_task_t * task);

/**
 * Real evaluation body; wrapped by _epic_evaluate() for accept/reject stats.
 */
static int32_t _epic_evaluate_inner(lv_draw_unit_t * draw_unit, lv_draw_task_t * task);

/**
 * Dispatch a task to the EPIC unit.
 * @param draw_unit Draw unit
 * @param layer Layer to draw
 * @return 1 if task was dispatched, 0 otherwise
 */
static int32_t _epic_dispatch(lv_draw_unit_t * draw_unit, lv_layer_t * layer);

/**
 * Delete the EPIC draw unit.
 * @param draw_unit Draw unit
 * @return Always returns 0
 */
static int32_t _epic_delete(lv_draw_unit_t * draw_unit);

#if LV_USE_SIFLI_EPIC_DRAW_THREAD
    /**
    * Render thread callback.
    * @param ptr Pointer to draw unit
    */
    static void _epic_render_thread_cb(void * ptr);
#endif

/**
 * Wait for EPIC to finish current task (required with draw thread — see SiFli SDK).
 * @param draw_unit Draw unit
 * @return 1 on success
 */
/* 与上游 LVGL 9.6 对齐：只有在**没开 EPIC 绘制线程**时才需要"等 EPIC 完成"。
 * 本地原来是 `#if !LV_USE_SIFLI_EPIC_DRAW_THREAD`（只要有 OS 就注册）—— 当前配置下未开
 * LV_USE_SIFLI_EPIC_DRAW_THREAD，两者等价；但哪天有人开了线程化绘制，
 * 旧条件会在主线程里多发一次等待。我们不用多线程渲染 ⇒ 按上游来。 */
#if LV_USE_OS
    static int32_t _epic_wait_for_finish(lv_draw_unit_t * draw_unit);
#endif

/**
 * Execute drawing operation.
 * @param u EPIC draw unit
 */
static void _epic_execute_drawing(lv_draw_sifli_epic_unit_t * u);

#if LV_USE_SIFLI_EPIC_DRAW_THREAD
    /**
     * Hand-off queue helpers.  Single producer (dispatcher / UI thread) and
     * single consumer (render thread); the monotonic indices never wrap in
     * practice and __atomic keeps the stores ordered on a single core too.
     */
    static inline uint32_t _epic_queue_count(lv_draw_sifli_epic_unit_t * u);
    static inline bool _epic_queue_push(lv_draw_sifli_epic_unit_t * u, lv_draw_task_t * task);
    static inline lv_draw_task_t * _epic_queue_pop(lv_draw_sifli_epic_unit_t * u);
#endif

/**********************
 *  STATIC VARIABLES
 **********************/

static bool epic_draw_unit_initialized = false;
#if LV_USE_SIFLI_EPIC_DRAW_THREAD
    static uint32_t epic_dispatch_log_cnt;
    static uint32_t epic_render_start_log_cnt;
    static uint32_t epic_render_done_log_cnt;
    static bool epic_thread_start_logged;
#endif

/* Accept/reject counters per draw-task type — quantify how much work the
 * EPIC unit takes vs. how much falls back to the SW rasterizer. */
#define EPIC_STAT_TYPES 16
static uint32_t epic_stat_accept[EPIC_STAT_TYPES];
static uint32_t epic_stat_reject[EPIC_STAT_TYPES];
static uint32_t epic_stat_ms[EPIC_STAT_TYPES];
static uint32_t epic_stat_flush_ms;
static uint32_t epic_stat_inval_ms;
static uint32_t epic_stat_gpuwait_ms;

/* Cycle-level breakdown of the label path (the biggest EPIC cost). */
uint32_t lv_sifli_epic_dbg_font_cyc;
uint32_t lv_sifli_epic_dbg_blend_cyc;
uint32_t lv_sifli_epic_dbg_label_cyc;
uint32_t lv_sifli_epic_dbg_letter_cyc;   /* whole lv_draw_unit_draw_letter */
uint32_t lv_sifli_epic_dbg_dsc_cyc;      /* lv_font_get_glyph_dsc calls */
uint32_t lv_sifli_epic_dbg_release_cyc;  /* lv_font_glyph_release_draw_data */
uint32_t lv_sifli_epic_dbg_glyphs;

/* Cycle-level breakdown of the SW text rasterization path (label.c loop + sw letter cb). */
uint32_t lv_sifli_epic_dbg_sw_cb_cyc;     /* lv_draw_sw_letter.c draw_letter_cb 全程 */
uint32_t lv_sifli_epic_dbg_sw_fetch_cyc;  /* cb 内 lv_font_get_glyph_bitmap（A4->A8 展开） */
uint32_t lv_sifli_epic_dbg_sw_blend_cyc;  /* cb 内 lv_draw_sw_blend 调用 */
uint32_t lv_sifli_epic_dbg_loop_dsc_cyc;  /* label.c 循环内 lv_font_get_glyph_dsc */
uint32_t lv_sifli_epic_dbg_loop_body_cyc; /* label.c 循环体（含 draw_unit_draw_letter）减去已计项目 */

uint32_t lv_sifli_epic_dbg_cyc(void)
{
    /* Cortex-M33 DWT cycle counter. Lazy-enable TRCENA + CYCCNTENA. */
    static int inited;
    volatile uint32_t * demcr = (volatile uint32_t *)0xE000EDFC;
    volatile uint32_t * dwt_ctrl = (volatile uint32_t *)0xE0001000;
    volatile uint32_t * dwt_cyc = (volatile uint32_t *)0xE0001004;

    if(!inited) {
        inited = 1;
        *demcr |= (1u << 24);
        *dwt_ctrl |= 1u;
    }
    return *dwt_cyc;
}

void lv_sifli_epic_dump_stats(void)
{
    static const char * type_names[EPIC_STAT_TYPES] = {
        "NONE", "FILL", "BORDER", "BOX_SHADOW", "LETTER", "LABEL", "IMAGE", "LAYER",
        "LINE", "ARC", "TRIANGLE", "MASK_RECT", "MASK_BITMAP", "BLUR", "VECTOR", "3D"
    };
    uint32_t ta = 0, tr = 0, tm = 0;
    int i;

    for(i = 0; i < EPIC_STAT_TYPES; i++) {
        ta += epic_stat_accept[i];
        tr += epic_stat_reject[i];
        tm += epic_stat_ms[i];
    }
    printf("[epic] tasks accepted=%u rejected=%u total=%ums\n", (unsigned)ta, (unsigned)tr, (unsigned)tm);
    printf("[epic] overhead: cache_flush=%ums cache_inval=%ums gpu_wait=%ums\n",
           (unsigned)epic_stat_flush_ms, (unsigned)epic_stat_inval_ms, (unsigned)epic_stat_gpuwait_ms);
    /* cyc/240 = us @240MHz */
    printf("[epic] label path: glyphs=%u font=%uus blend=%uus label_total=%uus\n",
           (unsigned)lv_sifli_epic_dbg_glyphs,
           (unsigned)(lv_sifli_epic_dbg_font_cyc / 240u),
           (unsigned)(lv_sifli_epic_dbg_blend_cyc / 240u),
           (unsigned)(lv_sifli_epic_dbg_label_cyc / 240u));
    printf("[epic] label detail: letter_total=%uus dsc=%uus release=%uus\n",
           (unsigned)(lv_sifli_epic_dbg_letter_cyc / 240u),
           (unsigned)(lv_sifli_epic_dbg_dsc_cyc / 240u),
           (unsigned)(lv_sifli_epic_dbg_release_cyc / 240u));
    for(i = 0; i < EPIC_STAT_TYPES; i++) {
        if(epic_stat_accept[i] || epic_stat_reject[i]) {
            printf("[epic]   %-11s accept=%u reject=%u time=%ums\n",
                   type_names[i],
                   (unsigned)epic_stat_accept[i],
                   (unsigned)epic_stat_reject[i],
                   (unsigned)epic_stat_ms[i]);
        }
    }
    printf("[epic] sw text: cb=%uus fetch=%uus blend=%uus loopdsc=%uus loopbody=%uus\n",
           (unsigned)(lv_sifli_epic_dbg_sw_cb_cyc / 240u),
           (unsigned)(lv_sifli_epic_dbg_sw_fetch_cyc / 240u),
           (unsigned)(lv_sifli_epic_dbg_sw_blend_cyc / 240u),
           (unsigned)(lv_sifli_epic_dbg_loop_dsc_cyc / 240u),
           (unsigned)(lv_sifli_epic_dbg_loop_body_cyc / 240u));
    {
        extern uint32_t lv_font_expand_cache_hit_cnt, lv_font_expand_cache_miss_cnt;
        extern uint32_t lv_font_expand_cache_fail_cnt, lv_font_expand_cache_used_bytes;
        extern uint32_t lv_font_dsc_cache_hit_cnt, lv_font_dsc_cache_miss_cnt;
        printf("[epic] font cache: expand hit=%u miss=%u fail=%u used=%uB dsc hit=%u miss=%u\n",
               (unsigned)lv_font_expand_cache_hit_cnt, (unsigned)lv_font_expand_cache_miss_cnt,
               (unsigned)lv_font_expand_cache_fail_cnt, (unsigned)lv_font_expand_cache_used_bytes,
               (unsigned)lv_font_dsc_cache_hit_cnt, (unsigned)lv_font_dsc_cache_miss_cnt);
    }

    /* Ported SDK v8 op engine (draw/sifli/epic_mw): per-op count / us. */
    {
        static const char * op_names[DRV_EPIC_DRAW_MAX] = {
            "IMAGE", "FILL", "LETTERS", "ARC", "RECT", "LINE", "BORDER"
        };
        uint32_t op_total = 0, us_total = 0;
        for(i = 0; i < DRV_EPIC_DRAW_MAX; i++) {
            op_total += epic_mw_dbg_ops[i];
            us_total += epic_mw_dbg_us[i];
        }
        printf("[epic_mw] ops=%u total=%uus\n", (unsigned)op_total, (unsigned)us_total);
        for(i = DRV_EPIC_DRAW_MIN; i < DRV_EPIC_DRAW_MAX; i++) {
            if(epic_mw_dbg_ops[i]) {
                printf("[epic_mw]   %-8s ops=%u time=%uus\n",
                       op_names[i],
                       (unsigned)epic_mw_dbg_ops[i],
                       (unsigned)epic_mw_dbg_us[i]);
            }
        }
    }
}

/**********************
 *      MACROS
 **********************/


/* Vector glyphs are not handled by the EPIC label path below.
 * Keep the check lightweight and fall back only when the first glyph is vector. */
static bool _epic_label_has_vector_glyph(const lv_draw_label_dsc_t * draw_dsc)
{
#if LV_USE_FREETYPE && LV_USE_VECTOR_GRAPHIC && LV_USE_THORVG
    if(draw_dsc == NULL || draw_dsc->font == NULL || draw_dsc->text == NULL || draw_dsc->text[0] == '\0') {
        return false;
    }

    lv_font_glyph_dsc_t glyph_dsc;
    uint32_t txt_ofs = 0;
    uint32_t letter = lv_text_encoded_next(draw_dsc->text, &txt_ofs);

    if(letter == '\0') {
        return false;
    }

    if(lv_font_get_glyph_dsc(draw_dsc->font, &glyph_dsc, letter, '\0') &&
       glyph_dsc.format == LV_FONT_GLYPH_FORMAT_VECTOR) {
        return true;
    }
#else
    LV_UNUSED(draw_dsc);
#endif

    return false;
}

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_draw_sifli_epic_init(void)
{
    if(epic_draw_unit_initialized) {
        return;
    }

    /* Initialize EPIC hardware */
    lv_epic_init();
    if(!lv_epic_is_initialized()) {
        return;
    }

    /* Initialize the SDK v8 op engine middle layer (render lists, mask pools) */
    epic_mw_init();

    /* Initialize draw buffer handlers */
    lv_draw_buf_sifli_epic_init_handlers();

    /* Create EPIC draw unit */
    lv_draw_sifli_epic_unit_t * draw_epic_unit = lv_draw_create_unit(sizeof(lv_draw_sifli_epic_unit_t));
    draw_epic_unit->base_unit.evaluate_cb = _epic_evaluate;
    draw_epic_unit->base_unit.dispatch_cb = _epic_dispatch;
    draw_epic_unit->base_unit.delete_cb = _epic_delete;
    draw_epic_unit->base_unit.name = "SiFli_EPIC";
#if LV_USE_OS
    draw_epic_unit->base_unit.wait_for_finish_cb = _epic_wait_for_finish;
#endif

#if LV_USE_SIFLI_EPIC_DRAW_THREAD
    /* Initialized before the thread starts so _epic_wait_for_finish() can
     * never wait on an uninitialized sync object. */
    draw_epic_unit->q_w = 0;
    draw_epic_unit->q_r = 0;
    draw_epic_unit->waiter = false;
    lv_thread_sync_init(&draw_epic_unit->sync_done);
    if(lv_thread_init(&draw_epic_unit->thread, "epic_draw", LV_DRAW_THREAD_PRIO,
                      _epic_render_thread_cb, LV_DRAW_THREAD_STACK_SIZE, draw_epic_unit) != LV_RESULT_OK) {
        LV_LOG_WARN("EPIC render thread create failed");
    }
#endif

    epic_draw_unit_initialized = true;
}

void lv_draw_sifli_epic_deinit(void)
{
    if(!epic_draw_unit_initialized) {
        return;
    }

    epic_mw_deinit();
    lv_epic_deinit();
    epic_draw_unit_initialized = false;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static int32_t _epic_evaluate(lv_draw_unit_t * draw_unit, lv_draw_task_t * task)
{
    int32_t r = _epic_evaluate_inner(draw_unit, task);
    uint32_t t = (uint32_t)task->type;

    if(t < EPIC_STAT_TYPES) {
        if(r) {
            epic_stat_accept[t]++;
        }
        else {
            epic_stat_reject[t]++;
        }
    }
    return r;
}

static int32_t _epic_evaluate_inner(lv_draw_unit_t * draw_unit, lv_draw_task_t * task)
{
    LV_UNUSED(draw_unit);

    if(task->target_layer == NULL) {
        return 0;
    }

    if(!lv_epic_cf_supported(task->target_layer->color_format, 0)) {
        return 0;
    }

    switch(task->type) {
        case LV_DRAW_TASK_TYPE_FILL: {
                const lv_draw_fill_dsc_t * draw_dsc = (const lv_draw_fill_dsc_t *)task->draw_dsc;

                if(task->target_layer == NULL) {
                    return 0;
                }

                /* Rounded corners are handled by the epic_mw op layer
                 * (DRV_EPIC_DRAW_RECT, draw/sifli/epic_mw).  The op layer
                 * paints solid color only, so gradient + radius stays on SW. */
                if(draw_dsc->radius != 0) {
                    if(!LV_SIFLI_EPIC_USE_RADIUS) {
                        return 0;
                    }

                    /* DRV_EPIC_DRAW_RECT is solid-color only. */
                    if(draw_dsc->grad.dir != LV_GRAD_DIR_NONE) {
                        return 0;
                    }
                }

                /* Check gradient support */
                if(draw_dsc->grad.dir != LV_GRAD_DIR_NONE) {
                    if(draw_dsc->grad.dir != LV_GRAD_DIR_HOR && draw_dsc->grad.dir != LV_GRAD_DIR_VER) {
                        return 0;
                    }

                    /* Only support 2-stop gradients */
                    if(draw_dsc->grad.stops_count != 2) {
                        return 0;
                    }

                    /* Check if output format supports gradient */
                    lv_layer_t * target_layer = task->target_layer;
                    if(target_layer) {
                        lv_color_format_t dest_cf = target_layer->color_format;
                        uint32_t epic_cf = lv_img_cf_to_epic_cf(dest_cf);
                        if(!EPIC_SUPPROT_OUT_FORMAT(epic_cf)) {
                            return 0;
                        }
                    }
                }

                if(task->preference_score > 70) {
                    task->preference_score = 70;
                    task->preferred_draw_unit_id = DRAW_UNIT_ID_SIFLI_EPIC;
                }
                return 1;
            }

        case LV_DRAW_TASK_TYPE_IMAGE: {
                const lv_draw_image_dsc_t * draw_dsc = (const lv_draw_image_dsc_t *)task->draw_dsc;

                if(draw_dsc->src == NULL) {
                    return 0;
                }

                /* Check if recolor is used */
                bool has_recolor = (draw_dsc->recolor_opa != LV_OPA_TRANSP);
                if(has_recolor) {
                    return 0;
                }

                /* Check if transform is used */
                bool has_transform = (draw_dsc->rotation != 0 ||
                                      draw_dsc->scale_x != LV_SCALE_NONE ||
                                      draw_dsc->scale_y != LV_SCALE_NONE);

                if(draw_dsc->clip_radius != 0) {
                    if(!LV_SIFLI_EPIC_USE_ROUND_CLIP) {
                        return 0;
                    }

                    /* epic_mw's IMAGE renderer has no HAL_API_TRANSFORM path. */
                    if(has_transform) {
                        return 0;
                    }

                    /* Only one mask layer can be fed to EPIC; keep the images
                     * that carry their own mask on the SW path. */
                    if((LV_COLOR_FORMAT_RGB565A8 == draw_dsc->header.cf) ||
                       (draw_dsc->bitmap_mask_src != NULL)) {
                        return 0;
                    }
                }

                if(draw_dsc->blend_mode != LV_BLEND_MODE_NORMAL) {
                    return 0;
                }

                /* Check color format support */
                if(!lv_epic_cf_supported(draw_dsc->header.cf, draw_dsc->header.flags)) {
                    return 0;
                }

                /* RGB565A8 with transform is not supported */
                if((LV_COLOR_FORMAT_RGB565A8 == draw_dsc->header.cf) && has_transform) {
                    return 0;
                }

                if(task->preference_score > 80) {
                    task->preference_score = 80;
                    task->preferred_draw_unit_id = DRAW_UNIT_ID_SIFLI_EPIC;
                }
                return 1;
            }

        case LV_DRAW_TASK_TYPE_BORDER: {
                const lv_draw_border_dsc_t * draw_dsc = (const lv_draw_border_dsc_t *)task->draw_dsc;

                if(draw_dsc->side == LV_BORDER_SIDE_NONE || draw_dsc->width == 0) {
                    return 0;
                }

                /* LV_BORDER_SIDE_INTERNAL (button matrix) is not a rectangle
                 * border; epic_mw's render_border() would draw nothing. */
                if(draw_dsc->side & LV_BORDER_SIDE_INTERNAL) {
                    return 0;
                }

                if(draw_dsc->side != LV_BORDER_SIDE_FULL || draw_dsc->radius != 0) {
                    /* Partial sides (radius 0) reuse the fill-strip path in
                     * lv_draw_sifli_epic_border(); rounded borders (and
                     * rounded + partial) go through epic_mw render_border(). */
                    if(!LV_SIFLI_EPIC_USE_ROUND_BORDER) {
                        return 0;
                    }
                }

                if(task->preference_score > 90) {
                    task->preference_score = 90;
                    task->preferred_draw_unit_id = DRAW_UNIT_ID_SIFLI_EPIC;
                }
                return 1;
            }

        case LV_DRAW_TASK_TYPE_LABEL: {
                /* A/B 实测（2026-10-17）：EPIC 每字形一次 ContBlendRepeat 且
                 * HAL 在 Repeat 里 EPIC_WaitDone，GPU 串行化；SW 光栅化
                 * 反而更快（Screen sized text 264ms→194ms/帧）。默认固定
                 * 走 SW；LV_SIFLI_EPIC_USE_LABEL=1 恢复 EPIC 标签路径。 */
                if(!LV_SIFLI_EPIC_USE_LABEL) {
                    return 0;
                }

                const lv_draw_label_dsc_t * draw_dsc = (const lv_draw_label_dsc_t *)task->draw_dsc;

                if(draw_dsc->font == NULL || draw_dsc->text == NULL) {
                    return 0;
                }

                if(_epic_label_has_vector_glyph(draw_dsc)) {
                    return 0;
                }

                if(task->preference_score > 95) {
                    task->preference_score = 95;
                    task->preferred_draw_unit_id = DRAW_UNIT_ID_SIFLI_EPIC;
                }
                return 1;
            }

        case LV_DRAW_TASK_TYPE_LINE: {
                /* 2026-10-17 A/B：EPIC line≈122us/task（与 SW 持平）、arc≈1.1ms/task
                 * （比 SW 贵），且整圆/AA 有视觉风险 → 默认固定走 SW。
                 * 2026-10-18：改为选择性加速 —— 形态检查通过且估算像素量
                 * ≥ LV_SIFLI_EPIC_LINE_MIN_PX 才走 EPIC，小线回 SW。 */
                if(!LV_SIFLI_EPIC_USE_LINE) {
                    return 0;
                }

                const lv_draw_line_dsc_t * draw_dsc = (const lv_draw_line_dsc_t *)task->draw_dsc;

                /* drv_epic has no dash support and no polyline path. */
                if(draw_dsc->dash_width > 0 || draw_dsc->dash_gap > 0) {
                    return 0;
                }

                if(draw_dsc->points != NULL) {
                    return 0;
                }

                if(draw_dsc->width <= 0 || draw_dsc->width > EPIC_COORDINATES_MAX) {
                    return 0;
                }

                if(draw_dsc->opa <= LV_OPA_MIN) {
                    return 0;
                }

                /* dsc->p1/p2 are lv_point_precise_t (float with LV_USE_FLOAT);
                 * bound-check before the int cast in the draw path. */
                if(!(draw_dsc->p1.x >= -EPIC_COORDINATES_MAX && draw_dsc->p1.x <= EPIC_COORDINATES_MAX &&
                     draw_dsc->p1.y >= -EPIC_COORDINATES_MAX && draw_dsc->p1.y <= EPIC_COORDINATES_MAX &&
                     draw_dsc->p2.x >= -EPIC_COORDINATES_MAX && draw_dsc->p2.x <= EPIC_COORDINATES_MAX &&
                     draw_dsc->p2.y >= -EPIC_COORDINATES_MAX && draw_dsc->p2.y <= EPIC_COORDINATES_MAX)) {
                    return 0;
                }

#if LV_SIFLI_EPIC_LINE_MIN_PX > 0
                {
                    /* area ≈ ceil(line length * width). EPIC pays a fixed
                     * ~120us per op, so thin/short lines stay on SW.
                     * Coordinates are bounded by EPIC_COORDINATES_MAX (1010),
                     * so dx*dx + dy*dy fits in uint32_t. */
                    int32_t dx = (int32_t)draw_dsc->p2.x - (int32_t)draw_dsc->p1.x;
                    int32_t dy = (int32_t)draw_dsc->p2.y - (int32_t)draw_dsc->p1.y;
                    uint32_t len;
                    uint32_t area;

                    dx = dx < 0 ? -dx : dx;
                    dy = dy < 0 ? -dy : dy;
                    len = (uint32_t)lv_sqrt32((uint32_t)dx * (uint32_t)dx +
                                              (uint32_t)dy * (uint32_t)dy);
                    area = len * (uint32_t)draw_dsc->width;
                    if(area < LV_SIFLI_EPIC_LINE_MIN_PX) {
                        return 0;
                    }
                }
#endif

                if(task->preference_score > 75) {
                    task->preference_score = 75;
                    task->preferred_draw_unit_id = DRAW_UNIT_ID_SIFLI_EPIC;
                }
                return 1;
            }

        case LV_DRAW_TASK_TYPE_ARC: {
                if(!LV_SIFLI_EPIC_USE_ARC) {
                    return 0;
                }

                const lv_draw_arc_dsc_t * draw_dsc = (const lv_draw_arc_dsc_t *)task->draw_dsc;

                /* Image arcs need the image blend path. */
                if(draw_dsc->img_src != NULL) {
                    return 0;
                }

                if(draw_dsc->width <= 0 || draw_dsc->radius == 0) {
                    return 0;
                }

                if(draw_dsc->start_angle == draw_dsc->end_angle) {
                    return 0;
                }

                if(draw_dsc->opa <= LV_OPA_MIN) {
                    return 0;
                }

                /* render_arc() builds the rounded-cap mask in mask_buf2_pool:
                 * LV_ASSERT(width * width <= 1600) (epic_mw.c:1029). */
                if(draw_dsc->rounded && draw_dsc->width > 40) {
                    return 0;
                }

                if(draw_dsc->radius > EPIC_COORDINATES_MAX) {
                    return 0;
                }

                if(draw_dsc->center.x < -EPIC_COORDINATES_MAX || draw_dsc->center.x > EPIC_COORDINATES_MAX ||
                   draw_dsc->center.y < -EPIC_COORDINATES_MAX || draw_dsc->center.y > EPIC_COORDINATES_MAX) {
                    return 0;
                }

                /* Reject NaN / out-of-range angles before the int cast in the
                 * draw path (lv_value_precise_t may be float). */
                if(!(draw_dsc->start_angle >= -3600 && draw_dsc->start_angle <= 3600 &&
                     draw_dsc->end_angle >= -3600 && draw_dsc->end_angle <= 3600)) {
                    return 0;
                }

#if LV_SIFLI_EPIC_ARC_MIN_PX > 0
                {
                    /* area ≈ arc length * width
                     *      = radius * |end-start| * π/180 * width.
                     * π/180 ≈ 1745/100000; 64-bit to avoid overflow. */
                    int32_t da = (int32_t)draw_dsc->start_angle - (int32_t)draw_dsc->end_angle;
                    uint32_t adiff;
                    uint64_t area;

                    if(da < 0) {
                        da = -da;
                    }
                    adiff = (uint32_t)da;
                    area = (uint64_t)draw_dsc->radius * adiff *
                           (uint32_t)draw_dsc->width * 1745u / 100000u;
                    if(area < (uint64_t)LV_SIFLI_EPIC_ARC_MIN_PX) {
                        return 0;
                    }
                }
#endif

                if(task->preference_score > 75) {
                    task->preference_score = 75;
                    task->preferred_draw_unit_id = DRAW_UNIT_ID_SIFLI_EPIC;
                }
                return 1;
            }

        case LV_DRAW_TASK_TYPE_LAYER: {
                const lv_draw_image_dsc_t * draw_dsc = (const lv_draw_image_dsc_t *)task->draw_dsc;
                lv_layer_t * layer_to_draw = (lv_layer_t *)draw_dsc->src;

                if(layer_to_draw == NULL) {
                    return 0;
                }

                if(layer_to_draw->draw_buf == NULL) {
                    return 0;
                }

                /* Check if recolor is used */
                bool has_recolor = (draw_dsc->recolor_opa != LV_OPA_TRANSP);
                if(has_recolor) {
                    return 0;
                }

                /* Check if transform is used */
                bool has_transform = (draw_dsc->rotation != 0 ||
                                      draw_dsc->scale_x != LV_SCALE_NONE ||
                                      draw_dsc->scale_y != LV_SCALE_NONE);

                /* Check color format support */
                if(!lv_epic_cf_supported(layer_to_draw->color_format, 0)) {
                    return 0;
                }

                /* RGB565A8 with transform is not supported */
                if((LV_COLOR_FORMAT_RGB565A8 == layer_to_draw->color_format) && has_transform) {
                    return 0;
                }

                if(task->preference_score > 80) {
                    task->preference_score = 80;
                    task->preferred_draw_unit_id = DRAW_UNIT_ID_SIFLI_EPIC;
                }
                return 1;
            }

        default:
            return 0;
    }

    return 0;
}

#if LV_USE_SIFLI_EPIC_DRAW_THREAD
static inline uint32_t _epic_queue_count(lv_draw_sifli_epic_unit_t * u)
{
    uint32_t w = __atomic_load_n(&u->q_w, __ATOMIC_ACQUIRE);
    uint32_t r = __atomic_load_n(&u->q_r, __ATOMIC_ACQUIRE);
    return w - r;
}

static inline bool _epic_queue_push(lv_draw_sifli_epic_unit_t * u, lv_draw_task_t * task)
{
    uint32_t w = __atomic_load_n(&u->q_w, __ATOMIC_RELAXED);
    uint32_t r = __atomic_load_n(&u->q_r, __ATOMIC_ACQUIRE);

    if((w - r) >= LV_SIFLI_EPIC_QUEUE_LEN) {
        return false;
    }

    u->task_q[w % LV_SIFLI_EPIC_QUEUE_LEN] = task;
    /* Publish the slot before the consumer can observe the new index */
    __atomic_store_n(&u->q_w, w + 1, __ATOMIC_RELEASE);
    return true;
}

static inline lv_draw_task_t * _epic_queue_pop(lv_draw_sifli_epic_unit_t * u)
{
    uint32_t r = __atomic_load_n(&u->q_r, __ATOMIC_RELAXED);
    uint32_t w = __atomic_load_n(&u->q_w, __ATOMIC_ACQUIRE);

    if(r == w) {
        return NULL;
    }

    lv_draw_task_t * task = u->task_q[r % LV_SIFLI_EPIC_QUEUE_LEN];
    __atomic_store_n(&u->q_r, r + 1, __ATOMIC_RELEASE);
    return task;
}
#endif

static int32_t _epic_dispatch(lv_draw_unit_t * draw_unit, lv_layer_t * layer)
{
    lv_draw_sifli_epic_unit_t * draw_epic_unit = (lv_draw_sifli_epic_unit_t *)draw_unit;

    /* Try to get a ready to draw task */
    lv_draw_task_t * task = lv_draw_get_next_available_task(layer, NULL, DRAW_UNIT_ID_SIFLI_EPIC);

    if(task == NULL) {
        return LV_DRAW_UNIT_IDLE;
    }

#if LV_USE_SIFLI_EPIC_DRAW_THREAD
    /* Hand-off queue full: report "busy" so lv_draw_dispatch() waits for the
     * completion request instead of spinning on this unit. */
    if(_epic_queue_count(draw_epic_unit) >= LV_SIFLI_EPIC_QUEUE_LEN) {
        return 0;
    }
#else
    /* Return immediately if EPIC is busy */
    if(draw_epic_unit->task_act) {
        return 0;
    }
#endif

    /* Let the SW unit draw this task if not preferred for EPIC */
    if(task->preferred_draw_unit_id != DRAW_UNIT_ID_SIFLI_EPIC) {
        return LV_DRAW_UNIT_IDLE;
    }

    /* Allocate buffer for layer */
    if(lv_draw_layer_alloc_buf(layer) == NULL) {
        EPIC_ASSERT_MSG(false, "EPIC: Failed to allocate layer draw buffer");
        return LV_DRAW_UNIT_IDLE;
    }

#if LV_USE_SIFLI_EPIC_DRAW_THREAD
    /* Batch: hand every currently independent EPIC task of this layer to the
     * render thread in one go.  lv_draw_get_next_available_task() only returns
     * non-overlapping WAITING tasks, so this is the same task set repeated
     * lv_draw_dispatch() calls would have produced, minus the wakeup per task. */
    int32_t taken = 0;
    while(task != NULL &&
          _epic_queue_count(draw_epic_unit) < LV_SIFLI_EPIC_QUEUE_LEN) {
        if(task->preferred_draw_unit_id != DRAW_UNIT_ID_SIFLI_EPIC) {
            break;
        }

        task->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
        task->draw_unit = &draw_epic_unit->base_unit;

        if(!_epic_queue_push(draw_epic_unit, task)) {
            /* Should not happen (count checked above), but never lose a task:
             * put it back to WAITING so a later dispatch can pick it up. */
            task->state = LV_DRAW_TASK_STATE_WAITING;
            break;
        }

        taken++;
        task = lv_draw_get_next_available_task(layer, task, DRAW_UNIT_ID_SIFLI_EPIC);
    }

    if(epic_dispatch_log_cnt < 8) {
        LV_LOG_WARN("EPIC dispatch[%u]: n=%d inited=%d",
                    (unsigned int)epic_dispatch_log_cnt,
                    (int)taken,
                    draw_epic_unit->inited ? 1 : 0);
        epic_dispatch_log_cnt++;
    }

    /* Let the render thread work (sync is sticky, a queued task cannot be lost) */
    if(taken > 0 && draw_epic_unit->inited) {
        lv_thread_sync_signal(&draw_epic_unit->sync);
    }

    return taken > 0 ? taken : LV_DRAW_UNIT_IDLE;
#else
    /* Mark task as in progress */
    task->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
    task->draw_unit = &draw_epic_unit->base_unit;

    draw_epic_unit->task_act = task;
    /* Execute drawing immediately */
    _epic_execute_drawing(draw_epic_unit);

    draw_epic_unit->task_act->state = LV_DRAW_TASK_STATE_FINISHED;
    draw_epic_unit->task_act = NULL;

    /* Request new dispatching */
    lv_draw_dispatch_request();
    return 1;
#endif
}

static int32_t _epic_delete(lv_draw_unit_t * draw_unit)
{
#if LV_USE_SIFLI_EPIC_DRAW_THREAD
    lv_draw_sifli_epic_unit_t * draw_epic_unit = (lv_draw_sifli_epic_unit_t *)draw_unit;

    draw_epic_unit->exit_status = true;

    if(draw_epic_unit->inited) {
        lv_thread_sync_signal(&draw_epic_unit->sync);
    }

    int32_t ret = lv_thread_delete(&draw_epic_unit->thread);
    /* Release a possible waiter (shutdown path) and free the completion sync */
    lv_thread_sync_signal(&draw_epic_unit->sync_done);
    lv_thread_sync_delete(&draw_epic_unit->sync_done);
    return ret;
#else
    LV_UNUSED(draw_unit);
    return 0;
#endif
}

static void _epic_execute_drawing(lv_draw_sifli_epic_unit_t * u)
{
    lv_draw_task_t * task = u->task_act;
    lv_layer_t * layer = task->target_layer;
    uint32_t t_start = lv_tick_get();
    uint32_t t0;

    t0 = lv_tick_get();
    lv_epic_gpu_check_done();
    epic_stat_gpuwait_ms += lv_tick_elaps(t0);

    if(layer && layer->draw_buf) {
        lv_area_t draw_area;

        if(lv_area_intersect(&draw_area, &task->_real_area, &task->clip_area)) {
            lv_area_move(&draw_area, -layer->buf_area.x1, -layer->buf_area.y1);
            t0 = lv_tick_get();
            lv_draw_buf_flush_cache(layer->draw_buf, &draw_area);
            epic_stat_flush_ms += lv_tick_elaps(t0);
        }
    }

    /* Execute based on task type */
    switch(task->type) {
        case LV_DRAW_TASK_TYPE_BORDER:
            lv_draw_sifli_epic_border(task);
            break;

        case LV_DRAW_TASK_TYPE_LABEL:
            lv_draw_sifli_epic_label(task);
            break;

        case LV_DRAW_TASK_TYPE_FILL:
            lv_draw_sifli_epic_fill(task);
            break;

        case LV_DRAW_TASK_TYPE_IMAGE:
            lv_draw_sifli_epic_img(task);
            break;

        case LV_DRAW_TASK_TYPE_LAYER:
            lv_draw_sifli_epic_layer(task);
            break;

        case LV_DRAW_TASK_TYPE_LINE:
            lv_draw_sifli_epic_line(task);
            break;

        case LV_DRAW_TASK_TYPE_ARC:
            lv_draw_sifli_epic_arc(task);
            break;

        default:
            break;
    }

    t0 = lv_tick_get();
    lv_epic_gpu_check_done();
    epic_stat_gpuwait_ms += lv_tick_elaps(t0);

    if(layer && layer->draw_buf) {
        lv_area_t draw_area;

        if(lv_area_intersect(&draw_area, &task->_real_area, &task->clip_area)) {
            lv_area_move(&draw_area, -layer->buf_area.x1, -layer->buf_area.y1);
            t0 = lv_tick_get();
            lv_draw_buf_invalidate_cache(layer->draw_buf, &draw_area);
            epic_stat_inval_ms += lv_tick_elaps(t0);
        }
    }

    if((uint32_t)task->type < EPIC_STAT_TYPES) {
        epic_stat_ms[task->type] += lv_tick_elaps(t_start);
    }
}

#if LV_USE_OS
static int32_t _epic_wait_for_finish(lv_draw_unit_t * draw_unit)
{
#if LV_USE_SIFLI_EPIC_DRAW_THREAD
    lv_draw_sifli_epic_unit_t * u = (lv_draw_sifli_epic_unit_t *)draw_unit;
    uint32_t start = lv_tick_get();

    /* Block (instead of returning immediately) until the render thread has
     * finished everything queued for it.  The previous no-op implementation
     * made lv_draw_dispatch() re-arm the dispatch request while the EPIC thread
     * was still busy, so the main thread busy-spun in lv_draw_dispatch for a
     * whole 100 ms RR timeslice and the draw thread never got the CPU in time
     * (Empty 270 ms, Multi rect 825 ms).
     *
     * The render thread signals sync_done after clearing task_act and draining
     * the queue.  The signal is sticky, so a completion that happens between
     * the check and the wait cannot be lost. */
    while(_epic_queue_count(u) > 0 || u->task_act != NULL) {
        if(u->exit_status) {
            break;
        }

        if(u->inited) {
            /* Publish the waiter flag before the re-check: the render thread
             * clears task_act/queue first and only then consumes the flag, so
             * either it signals us or the re-check below sees the idle unit. */
            __atomic_store_n(&u->waiter, true, __ATOMIC_SEQ_CST);
            if(_epic_queue_count(u) == 0 && u->task_act == NULL) {
                __atomic_store_n(&u->waiter, false, __ATOMIC_SEQ_CST);
                break;
            }
            lv_thread_sync_wait(&u->sync_done);
        }
        else {
            /* Thread not up yet (or creation failed): never spin, yield a tick */
            lv_delay_ms(1);
        }

        if(lv_tick_elaps(start) > 5000) {
            LV_LOG_ERROR("EPIC wait_for_finish timeout");
            break;
        }
    }
#else
    LV_UNUSED(draw_unit);
    lv_epic_gpu_check_done();
#endif
    return 1;
}
#endif

#if LV_USE_SIFLI_EPIC_DRAW_THREAD
static void _epic_render_thread_cb(void * ptr)
{
    lv_draw_sifli_epic_unit_t * u = ptr;

    lv_thread_sync_init(&u->sync);
    u->inited = true;

    if(!epic_thread_start_logged) {
#if LV_USE_NUTTX
        int policy = 0;
        struct sched_param param;
        if(pthread_getschedparam(pthread_self(), &policy, &param) == 0) {
            LV_LOG_WARN("EPIC render thread started (prio=%d policy=%d)", param.sched_priority, policy);
        }
        else {
            LV_LOG_WARN("EPIC render thread started (prio query failed)");
        }
#else
        LV_LOG_WARN("EPIC render thread started");
#endif
        epic_thread_start_logged = true;
    }

    while(1) {
        if(u->exit_status) {
            break;
        }

        /* Drain the hand-off queue: every task of a batch runs back-to-back,
         * so the dispatcher wakeup cost is paid once per batch, not per task. */
        lv_draw_task_t * task = _epic_queue_pop(u);
        if(task == NULL) {
            lv_thread_sync_wait(&u->sync);
            continue;
        }

        u->task_act = task;

        if(epic_render_start_log_cnt < 8) {
            LV_LOG_WARN("EPIC render start[%u]: type=%d",
                        (unsigned int)epic_render_start_log_cnt,
                        (int)task->type);
            epic_render_start_log_cnt++;
        }

        _epic_execute_drawing(u);

        if(epic_render_done_log_cnt < 8) {
            LV_LOG_WARN("EPIC render done[%u]: type=%d",
                        (unsigned int)epic_render_done_log_cnt,
                        (int)task->type);
            epic_render_done_log_cnt++;
        }

        /* Signal the ready state to dispatcher */
        task->state = LV_DRAW_TASK_STATE_FINISHED;

        /* Cleanup */
        u->task_act = NULL;

        /* Request new dispatching (lets the UI thread refill the queue) */
        lv_draw_dispatch_request();

        /* Wake the main thread only if it is blocked in
         * _epic_wait_for_finish() and the whole batch is done.  Skipping the
         * signal when nobody waits saves one condvar signal per batch. */
        if(_epic_queue_count(u) == 0) {
            if(__atomic_exchange_n(&u->waiter, false, __ATOMIC_SEQ_CST)) {
                lv_thread_sync_signal(&u->sync_done);
            }
        }
    }

    u->inited = false;
    lv_thread_sync_delete(&u->sync);
}
#endif

#endif /*LV_USE_SIFLI_EPIC*/
