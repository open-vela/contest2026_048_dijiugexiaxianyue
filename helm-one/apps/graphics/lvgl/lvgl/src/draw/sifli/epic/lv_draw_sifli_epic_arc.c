/**
 * @file lv_draw_sifli_epic_arc.c
 *
 * ARC draw task -> SDK v8 op engine (epic_mw) mapping.
 * Mirrors lv_gpu_new_api.c:1180-1210 (v8 draw_arc), see
 * /tmp/lvgl_bench/port_plan.md §5.2/§5.3.
 *
 * Angle units: v8 and v9 LVGL both use degrees (0..360, 0 deg = 3 o'clock);
 * the EPIC mask engine (epic_mw_mask_angle_init / EPIC_TrigoSin) expects the
 * same units, so no scaling is applied.  v9's `lv_value_precise_t` is
 * truncated to int.
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
#include "../../lv_draw_arc.h"
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

/**
 * Convert v9 start/end angles (degrees, may be negative or wrap) to the
 * [0,359] range used by epic_mw_mask_angle_init().
 */
static void _normalize_angles(const lv_draw_arc_dsc_t * dsc, uint16_t * start, uint16_t * end);

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
 * Draw an arc with the EPIC op engine.
 *
 * Mapping (port_plan.md §5.2):
 *   op = DRV_EPIC_DRAW_ARC,
 *   center/radius/width from the dsc,
 *   start/end angle in degrees (normalized to 0..359 for the mask engine),
 *   round_start = round_end = dsc->rounded,
 *   argb8888 = opa<<24 | r<<16 | g<<8 | b.
 */
void lv_draw_sifli_epic_arc(lv_draw_task_t * task)
{
    const lv_draw_arc_dsc_t * dsc = (const lv_draw_arc_dsc_t *)task->draw_dsc;
    lv_layer_t * layer = task->target_layer;

    if(layer == NULL || layer->draw_buf == NULL) {
        return;
    }

    if(dsc->img_src != NULL) {
        return; /* image arcs are rejected in evaluate */
    }

    if(dsc->width <= 0 || dsc->radius == 0) {
        return;
    }

    if(dsc->opa <= LV_OPA_MIN) {
        return;
    }

    if(dsc->start_angle == dsc->end_angle) {
        return;
    }

    /* render_arc() builds the rounded-cap mask in mask_buf2_pool:
     * LV_ASSERT(width * width <= 1600) (epic_mw.c:1029). */
    if(dsc->rounded && dsc->width > 40) {
        return;
    }

    if(dsc->radius > EPIC_COORDINATES_MAX) {
        return;
    }

    int32_t cx = dsc->center.x;
    int32_t cy = dsc->center.y;
    if(cx < -EPIC_COORDINATES_MAX || cx > EPIC_COORDINATES_MAX ||
       cy < -EPIC_COORDINATES_MAX || cy > EPIC_COORDINATES_MAX) {
        return;
    }

    uint16_t start_angle;
    uint16_t end_angle;
    _normalize_angles(dsc, &start_angle, &end_angle);

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
        LV_LOG_WARN("epic_mw: no free render list for arc");
        return;
    }

    epic_mw_op_t * o = epic_mw_alloc_op(&buf);
    if(o == NULL) {
        epic_mw_discard(rl);
        return;
    }

    o->op = DRV_EPIC_DRAW_ARC;

    o->clip_area.x0 = blend_area.x1;
    o->clip_area.y0 = blend_area.y1;
    o->clip_area.x1 = blend_area.x2;
    o->clip_area.y1 = blend_area.y2;

    o->desc.arc.center_x = (int16_t)cx;
    o->desc.arc.center_y = (int16_t)cy;
    o->desc.arc.radius = dsc->radius;
    o->desc.arc.width = (uint16_t)dsc->width;
    o->desc.arc.start_angle = start_angle;
    o->desc.arc.end_angle = end_angle;
    o->desc.arc.round_start = dsc->rounded ? 1 : 0;
    o->desc.arc.round_end = dsc->rounded ? 1 : 0;

    o->desc.arc.argb8888 = ((uint32_t)dsc->opa << 24) |
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

static void _normalize_angles(const lv_draw_arc_dsc_t * dsc, uint16_t * start, uint16_t * end)
{
    int32_t sa_i = (int32_t)dsc->start_angle;
    int32_t ea_i = (int32_t)dsc->end_angle;
    int32_t sweep = ea_i - sa_i;
    int32_t sa;
    int32_t ea;

    /* Clockwise sweep: v9 expresses "wrap" as end < start and a full circle
     * as end == start + 360. */
    while(sweep < 0) sweep += 360;
    while(sweep > 360) sweep -= 360;

    sa = sa_i % 360;
    if(sa < 0) sa += 360;

    if(sweep >= 360) {
        /* Full circle.  epic_mw_mask_angle_init() clamps angles to 359, and
         * render_arc() skips start==end, so a 359-degree sweep is the closest
         * representable full ring (same approximation as the SDK). */
        ea = (sa + 359) % 360;
    }
    else {
        ea = sa + sweep;
        if(ea > 359) ea -= 360; /* wrap: end < start is handled by the mask engine */
    }

    *start = (uint16_t)sa;
    *end = (uint16_t)ea;
}

#endif /*LV_USE_SIFLI_EPIC*/
