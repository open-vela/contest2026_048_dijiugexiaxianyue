/**
 * @file lv_draw_sifli_epic_border.c
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
#include "../epic_mw/epic_mw.h"
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

static void draw_border_simple(lv_draw_task_t * task, const lv_area_t * outer_area, const lv_area_t * inner_area,
                               const lv_draw_border_dsc_t * dsc);
static void draw_fill_part(lv_draw_task_t * task, const lv_area_t * area, const lv_draw_border_dsc_t * dsc);
static void draw_border_rounded(lv_draw_task_t * task, const lv_draw_border_dsc_t * dsc);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_draw_sifli_epic_border(lv_draw_task_t * task)
{
    const lv_draw_border_dsc_t * dsc = (const lv_draw_border_dsc_t *)task->draw_dsc;
    const lv_area_t * coords = &task->area;

    if(dsc->opa <= LV_OPA_MIN) return;
    if(dsc->width == 0) return;
    if(dsc->side == LV_BORDER_SIDE_NONE) return;

    if(dsc->radius != 0) {
        /* Rounded border (and rounded + partial side): epic_mw DRV_EPIC_DRAW_BORDER
         * (render_border -> render_border_complex with the mask engine). */
        draw_border_rounded(task, dsc);
        return;
    }

    lv_area_t area_inner;
    lv_area_copy(&area_inner, coords);
    area_inner.x1 += ((dsc->side & LV_BORDER_SIDE_LEFT) ? dsc->width : -dsc->width);
    area_inner.x2 -= ((dsc->side & LV_BORDER_SIDE_RIGHT) ? dsc->width : -dsc->width);
    area_inner.y1 += ((dsc->side & LV_BORDER_SIDE_TOP) ? dsc->width : -dsc->width);
    area_inner.y2 -= ((dsc->side & LV_BORDER_SIDE_BOTTOM) ? dsc->width : -dsc->width);

    draw_border_simple(task, coords, &area_inner, dsc);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void draw_border_simple(lv_draw_task_t * task, const lv_area_t * outer_area, const lv_area_t * inner_area,
                               const lv_draw_border_dsc_t * dsc)
{
    lv_area_t area;

    bool top_side = outer_area->y1 <= inner_area->y1;
    bool bottom_side = outer_area->y2 >= inner_area->y2;
    bool left_side = outer_area->x1 <= inner_area->x1;
    bool right_side = outer_area->x2 >= inner_area->x2;

    /* Top */
    area.x1 = outer_area->x1;
    area.x2 = outer_area->x2;
    area.y1 = outer_area->y1;
    area.y2 = inner_area->y1 - 1;
    if(top_side && area.y1 <= area.y2) {
        draw_fill_part(task, &area, dsc);
    }

    /* Bottom */
    area.y1 = inner_area->y2 + 1;
    area.y2 = outer_area->y2;
    if(bottom_side && area.y1 <= area.y2) {
        draw_fill_part(task, &area, dsc);
    }

    /* Left */
    area.x1 = outer_area->x1;
    area.x2 = inner_area->x1 - 1;
    area.y1 = top_side ? inner_area->y1 : outer_area->y1;
    area.y2 = bottom_side ? inner_area->y2 : outer_area->y2;
    if(left_side && area.x1 <= area.x2 && area.y1 <= area.y2) {
        draw_fill_part(task, &area, dsc);
    }

    /* Right */
    area.x1 = inner_area->x2 + 1;
    area.x2 = outer_area->x2;
    if(right_side && area.x1 <= area.x2 && area.y1 <= area.y2) {
        draw_fill_part(task, &area, dsc);
    }
}

static void draw_fill_part(lv_draw_task_t * task, const lv_area_t * area, const lv_draw_border_dsc_t * dsc)
{
    lv_draw_fill_dsc_t fill_dsc;
    lv_draw_fill_dsc_init(&fill_dsc);
    fill_dsc.color = dsc->color;
    fill_dsc.opa = dsc->opa;

    lv_draw_task_t fill_task = *task;
    fill_task.draw_dsc = &fill_dsc;
    fill_task.area = *area;
    lv_draw_sifli_epic_fill(&fill_task);
}

/**
 * Rounded border via the ported SDK v8 op engine (epic_mw).
 *
 * Mapping follows lv_gpu_new_api.c (v8 draw_border) / port_plan.md §5.2:
 *   op = DRV_EPIC_DRAW_BORDER,
 *   area = task->area (outer coords), clip_area = area ∩ clip ∩ buf_area,
 *   radius = LV_RADIUS_CIRCLE -> 0x7FFF (render_border clamps to half the
 *   short side), side bits map 1:1 onto lv_border_side_t,
 *   argb8888 = opa<<24 | r<<16 | g<<8 | b (positive opa, no inversion).
 */
static void draw_border_rounded(lv_draw_task_t * task, const lv_draw_border_dsc_t * dsc)
{
    lv_layer_t * layer = task->target_layer;

    if(layer == NULL || layer->draw_buf == NULL) {
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
    buf.total_width = layer->draw_buf ?
                      lv_epic_stride_to_width(layer->draw_buf->header.stride, dest_cf) :
                      (uint16_t)lv_area_get_width(&layer->buf_area);

    epic_mw_rl_t rl = epic_mw_begin(&buf, NULL);
    if(rl == NULL) {
        LV_LOG_WARN("epic_mw: no free render list for rounded border");
        return;
    }

    epic_mw_op_t * o = epic_mw_alloc_op(&buf);
    if(o == NULL) {
        epic_mw_discard(rl);
        return;
    }

    o->op = DRV_EPIC_DRAW_BORDER;

    o->clip_area.x0 = blend_area.x1;
    o->clip_area.y0 = blend_area.y1;
    o->clip_area.x1 = blend_area.x2;
    o->clip_area.y1 = blend_area.y2;

    o->desc.border.area.x0 = task->area.x1;
    o->desc.border.area.y0 = task->area.y1;
    o->desc.border.area.x1 = task->area.x2;
    o->desc.border.area.y1 = task->area.y2;

    o->desc.border.radius = (dsc->radius >= (int32_t)LV_RADIUS_CIRCLE) ?
                            (uint16_t)0x7FFF : (uint16_t)dsc->radius;
    o->desc.border.width = (uint16_t)dsc->width;

    o->desc.border.top_side = (dsc->side & LV_BORDER_SIDE_TOP) ? 1 : 0;
    o->desc.border.bot_side = (dsc->side & LV_BORDER_SIDE_BOTTOM) ? 1 : 0;
    o->desc.border.left_side = (dsc->side & LV_BORDER_SIDE_LEFT) ? 1 : 0;
    o->desc.border.right_side = (dsc->side & LV_BORDER_SIDE_RIGHT) ? 1 : 0;

    o->desc.border.argb8888 = ((uint32_t)dsc->opa << 24) |
                              ((uint32_t)dsc->color.red << 16) |
                              ((uint32_t)dsc->color.green << 8) |
                              ((uint32_t)dsc->color.blue);

    if(epic_mw_commit_op(o) != 0) {
        epic_mw_discard(rl);
        return;
    }

    (void)epic_mw_flush(rl);
}

#endif /*LV_USE_SIFLI_EPIC*/
