/**
 * @file lv_draw_sifli_epic_fill.c
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

/**
 * Rounded solid fill through the SDK op engine (epic_mw).
 * Straight (radius == 0) fills keep using lv_epic_fill (port_plan.md §5.1).
 */
static void _epic_fill_rounded(lv_draw_task_t * task, const lv_draw_fill_dsc_t * dsc,
                               const lv_area_t * blend_area);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_draw_sifli_epic_fill(lv_draw_task_t * task)
{
    const lv_draw_fill_dsc_t * dsc = (const lv_draw_fill_dsc_t *)task->draw_dsc;
    const lv_area_t * coords = &task->area;

    if(dsc->opa <= LV_OPA_MIN) {
        return;
    }

    lv_layer_t * layer = task->target_layer;

    lv_area_t blend_area;
    if(!lv_area_intersect(&blend_area, coords, &task->clip_area)) {
        return; /* Fully clipped */
    }

    if(!lv_area_intersect(&blend_area, &blend_area, &layer->buf_area)) {
        return; /* Fully clipped */
    }

    lv_grad_dir_t grad_dir = dsc->grad.dir;
    if(grad_dir == LV_GRAD_DIR_NONE) {
        if(dsc->radius != 0) {
            /* Rounded solid fill: epic_mw op layer (DRV_EPIC_DRAW_RECT + mask engine) */
            _epic_fill_rounded(task, dsc, &blend_area);
            return;
        }

        /* Solid color fill */
        EPIC_LayerConfigTypeDef input_layers[1];
        EPIC_LayerConfigTypeDef output_canvas;

        if(lv_epic_setup_layers(&input_layers[0], &output_canvas, task, coords)) {
            return; /* Fully clipped */
        }

        /* Set fill color */
        output_canvas.color_r = dsc->color.red;
        output_canvas.color_g = dsc->color.green;
        output_canvas.color_b = dsc->color.blue;
        output_canvas.color_en = true;
        if(dsc->opa < LV_OPA_MAX) {
            /* Alpha blending with background */
            input_layers[0].alpha = (dsc->opa == 0) ? 255 : (256 - dsc->opa);
            (void)lv_epic_fill(input_layers, 1, &output_canvas);
        }
        else {
            /* Opaque fill */
            (void)lv_epic_fill(NULL, 0, &output_canvas);
        }
    }
    else {
        /* Gradient fill */
        lv_color_format_t dest_cf = layer->color_format;
        uint32_t epic_cf = lv_img_cf_to_epic_cf(dest_cf);

        LV_ASSERT_MSG(EPIC_SUPPROT_OUT_FORMAT(epic_cf),
                      "EPIC: Color format not supported for gradient fill");

        uint8_t * dest_buf = lv_draw_layer_go_to_xy(layer,
                                                    blend_area.x1 - layer->buf_area.x1,
                                                    blend_area.y1 - layer->buf_area.y1);

        EPIC_GradCfgTypeDef param;
        HAL_EPIC_FillGradDataInit(&param);

        param.start = dest_buf;
        param.color_mode = epic_cf;
        param.width = lv_area_get_width(&blend_area);
        param.height = lv_area_get_height(&blend_area);
        param.total_width = layer->draw_buf ? lv_epic_stride_to_width(layer->draw_buf->header.stride,
                                                                      dest_cf) : lv_area_get_width(&layer->buf_area);
        /* Setup gradient colors */
        if(grad_dir == LV_GRAD_DIR_VER) {
            /* Vertical gradient */
            LV_ASSERT(dsc->grad.stops_count == 2);
            EPIC_ColorDef color1 = lv_color_to_epic_color(
                                       dsc->grad.stops[0].color,
                                       LV_OPA_MIX2(dsc->grad.stops[0].opa, dsc->opa));
            EPIC_ColorDef color2 = lv_color_to_epic_color(
                                       dsc->grad.stops[1].color,
                                       LV_OPA_MIX2(dsc->grad.stops[1].opa, dsc->opa));

            param.color[0][0] = color1;
            param.color[0][1] = color1;
            param.color[1][0] = color2;
            param.color[1][1] = color2;
        }
        else if(grad_dir == LV_GRAD_DIR_HOR) {
            /* Horizontal gradient */
            LV_ASSERT(dsc->grad.stops_count == 2);
            EPIC_ColorDef color1 = lv_color_to_epic_color(
                                       dsc->grad.stops[0].color,
                                       LV_OPA_MIX2(dsc->grad.stops[0].opa, dsc->opa));
            EPIC_ColorDef color2 = lv_color_to_epic_color(
                                       dsc->grad.stops[1].color,
                                       LV_OPA_MIX2(dsc->grad.stops[1].opa, dsc->opa));

            param.color[0][0] = color1;
            param.color[0][1] = color2;
            param.color[1][0] = color1;
            param.color[1][1] = color2;
        }

        (void)lv_epic_fill_grad(&param);
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Rounded solid fill via the ported SDK v8 op engine (epic_mw).
 *
 * Mapping follows lv_gpu_new_api.c:1089-1126 (v8 draw_bg) as described in
 * port_plan.md §5.2:
 *   op = DRV_EPIC_DRAW_RECT, area = coords, clip_area = coords ∩ clip ∩ buf,
 *   argb8888 = opa<<24 | r<<16 | g<<8 | b (positive opa, no 256-opa inversion),
 *   LV_RADIUS_CIRCLE -> 0x7FFF.
 */
static void _epic_fill_rounded(lv_draw_task_t * task, const lv_draw_fill_dsc_t * dsc,
                               const lv_area_t * blend_area)
{
    lv_layer_t * layer = task->target_layer;
    lv_color_format_t dest_cf = layer->color_format;
    uint8_t * dst = (uint8_t *)lv_draw_layer_go_to_xy(layer, 0, 0);
    uint32_t alpha;

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
    /* ⚠ The SDK op layer wrote total_width = width (drv_epic.c:4928/5031);
     * v9 draw buffers may carry a padded stride -> pass it explicitly
     * (port_plan.md §6.6). */
    buf.total_width = layer->draw_buf ?
                      lv_epic_stride_to_width(layer->draw_buf->header.stride, dest_cf) :
                      (uint16_t)lv_area_get_width(&layer->buf_area);

    epic_mw_rl_t rl = epic_mw_begin(&buf, NULL);
    if(rl == NULL) {
        LV_LOG_WARN("epic_mw: no free render list for rounded fill");
        return;
    }

    epic_mw_op_t * o = epic_mw_alloc_op(&buf);
    if(o == NULL) {
        epic_mw_discard(rl);
        return;
    }

    o->op = DRV_EPIC_DRAW_RECT;

    /* Only the coords ∩ clip ∩ buf_area region is drawn. */
    o->clip_area.x0 = blend_area->x1;
    o->clip_area.y0 = blend_area->y1;
    o->clip_area.x1 = blend_area->x2;
    o->clip_area.y1 = blend_area->y2;

    o->desc.rectangle.area.x0 = task->area.x1;
    o->desc.rectangle.area.y0 = task->area.y1;
    o->desc.rectangle.area.x1 = task->area.x2;
    o->desc.rectangle.area.y1 = task->area.y2;

    o->desc.rectangle.radius = (dsc->radius >= (int32_t)LV_RADIUS_CIRCLE) ?
                               (uint16_t)0x7FFF : (uint16_t)dsc->radius;
    /* v9 has one radius for all corners -> always split top/bottom
     * (drv_epic.c:3396-3489 handles both set as the general case). */
    o->desc.rectangle.top_fillet = 1;
    o->desc.rectangle.bot_fillet = 1;

    /* Positive opacity (the op layer's draw_fill/draw_rect use
     * input_layers[].alpha = opa; NOT the 256-opa inversion of the legacy
     * lv_epic_fill path — port_plan.md §5.2). */
    alpha = (dsc->opa == 0) ? 0 : dsc->opa;
    o->desc.rectangle.argb8888 = (alpha << 24) |
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
