/**
 * @file lv_draw_sifli_epic_line.c
 *
 * LINE draw task -> SDK v8 op engine (epic_mw) mapping.
 * Mirrors lv_gpu_new_api.c:1214-1243 (v8 draw_line), see
 * /tmp/lvgl_bench/port_plan.md §5.2/§5.3.
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
#include "../epic_mw/epic_mw.h"
#include "../../lv_draw_line.h"
#include "../../../misc/lv_area_private.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Draw a single line segment with the EPIC op engine.
 *
 * Mapping (port_plan.md §5.2):
 *   op = DRV_EPIC_DRAW_LINE,
 *   p1/p2 = lv_point_precise_t truncated to int (v9 endpoints may be float),
 *   clip_area = task->area ∩ task->clip_area ∩ layer->buf_area,
 *   argb8888 = opa<<24 | r<<16 | g<<8 | b (positive opa, no inversion),
 *   dash_* = 0 (dashed lines are rejected in evaluate: drv_epic cannot do them).
 */
void lv_draw_sifli_epic_line(lv_draw_task_t * task)
{
    const lv_draw_line_dsc_t * dsc = (const lv_draw_line_dsc_t *)task->draw_dsc;
    lv_layer_t * layer = task->target_layer;

    if(layer == NULL || layer->draw_buf == NULL) {
        return;
    }

    if(dsc->width <= 0 || dsc->width > EPIC_COORDINATES_MAX) {
        return;
    }

    if(dsc->opa <= LV_OPA_MIN) {
        return;
    }

    /* Dashed lines: drv_epic's render_line_hor/ver return RT_ERROR and the
     * skew path has no dash implementation (v8 asserts).  Must stay on SW. */
    if(dsc->dash_width > 0 || dsc->dash_gap > 0) {
        return;
    }

    /* Polylines are rejected in evaluate; p1/p2 are used here. */
    if(dsc->points != NULL) {
        return;
    }

    int32_t x1 = (int32_t)dsc->p1.x;
    int32_t y1 = (int32_t)dsc->p1.y;
    int32_t x2 = (int32_t)dsc->p2.x;
    int32_t y2 = (int32_t)dsc->p2.y;

    if(x1 < -EPIC_COORDINATES_MAX || x1 > EPIC_COORDINATES_MAX ||
       y1 < -EPIC_COORDINATES_MAX || y1 > EPIC_COORDINATES_MAX ||
       x2 < -EPIC_COORDINATES_MAX || x2 > EPIC_COORDINATES_MAX ||
       y2 < -EPIC_COORDINATES_MAX || y2 > EPIC_COORDINATES_MAX) {
        return;
    }

    lv_area_t blend_area;
    if(!lv_area_intersect(&blend_area, &task->area, &task->clip_area)) {
        return; /* Fully clipped */
    }

    if(!lv_area_intersect(&blend_area, &blend_area, &layer->buf_area)) {
        return; /* Fully clipped */
    }

    lv_color_format_t dest_cf = layer->color_format;
    uint8_t * dst = (uint8_t *)lv_draw_layer_go_to_xy(layer, 0, 0);
    if(dst == NULL) {
        return;
    }

    epic_mw_buf_t buf;
    buf.cf = lv_img_cf_to_epic_cf(dest_cf);
    buf.data = dst;
    buf.area.x0 = layer->buf_area.x1;
    buf.area.y0 = layer->buf_area.y1;
    buf.area.x1 = layer->buf_area.x2;
    buf.area.y1 = layer->buf_area.y2;
    /* v9 draw buffers may carry a padded stride -> pass it explicitly
     * (port_plan.md §6.6). */
    buf.total_width = layer->draw_buf ?
                      lv_epic_stride_to_width(layer->draw_buf->header.stride, dest_cf) :
                      (uint16_t)lv_area_get_width(&layer->buf_area);

    epic_mw_rl_t rl = epic_mw_begin(&buf, NULL);
    if(rl == NULL) {
        LV_LOG_WARN("epic_mw: no free render list for line");
        return;
    }

    epic_mw_op_t * o = epic_mw_alloc_op(&buf);
    if(o == NULL) {
        epic_mw_discard(rl);
        return;
    }

    o->op = DRV_EPIC_DRAW_LINE;

    o->clip_area.x0 = blend_area.x1;
    o->clip_area.y0 = blend_area.y1;
    o->clip_area.x1 = blend_area.x2;
    o->clip_area.y1 = blend_area.y2;

    o->desc.line.p1.x = (int16_t)x1;
    o->desc.line.p1.y = (int16_t)y1;
    o->desc.line.p2.x = (int16_t)x2;
    o->desc.line.p2.y = (int16_t)y2;
    o->desc.line.width = (uint16_t)dsc->width;

    /* Dashed lines are rejected above. */
    o->desc.line.dash_width = 0;
    o->desc.line.dash_gap = 0;

    o->desc.line.round_start = dsc->round_start ? 1 : 0;
    o->desc.line.round_end = dsc->round_end ? 1 : 0;
    o->desc.line.raw_end = dsc->raw_end ? 1 : 0;

    o->desc.line.argb8888 = ((uint32_t)dsc->opa << 24) |
                            ((uint32_t)dsc->color.red << 16) |
                            ((uint32_t)dsc->color.green << 8) |
                            ((uint32_t)dsc->color.blue);

    if(epic_mw_commit_op(o) != 0) {
        epic_mw_discard(rl);
        return;
    }

    (void)epic_mw_flush(rl);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

#endif /*LV_USE_SIFLI_EPIC*/
