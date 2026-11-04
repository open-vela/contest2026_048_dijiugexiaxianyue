/**
 * @file lv_draw_sifli_epic_img.c
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
#if LV_SIFLI_EPIC_USE_ROUND_CLIP
#include "../epic_mw/epic_mw.h"
#include "../epic_mw/epic_mw_mask.h"
#endif
#include "../../../draw/lv_draw_image_private.h"
#include "../../../draw/lv_image_decoder_private.h"
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

static void epic_img_draw_core(lv_draw_task_t * task, const lv_draw_image_dsc_t * draw_dsc,
                               const lv_image_decoder_dsc_t * decoder_dsc, lv_draw_image_sup_t * sup,
                               const lv_area_t * img_coords, const lv_area_t * clipped_img_area);

#if LV_SIFLI_EPIC_USE_ROUND_CLIP
/**
 * Image blend with clip_radius: the corners are cut by an A8 mask generated
 * by the epic_mw radius mask engine and fed to the EPIC IMAGE renderer
 * (epic_mw.c: render_image) as its third input layer.
 */
static void epic_img_blend_rounded(lv_draw_task_t * task, const lv_draw_image_dsc_t * draw_dsc,
                                   const EPIC_LayerConfigTypeDef * fg_layer,
                                   const lv_area_t * img_coords, const lv_area_t * clipped_img_area);
#endif

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_draw_sifli_epic_img(lv_draw_task_t * task)
{    const lv_draw_image_dsc_t * dsc = (const lv_draw_image_dsc_t *)task->draw_dsc;
    const lv_area_t * coords = &task->area;

    if(dsc->opa <= LV_OPA_MIN) {
        return;
    }

    if(dsc->tile) {
        lv_draw_image_tiled_helper(task, dsc, coords, epic_img_draw_core, NULL);
    }
    else {
        lv_draw_image_normal_helper(task, dsc, coords, epic_img_draw_core, NULL);
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void epic_img_draw_core(lv_draw_task_t * task, const lv_draw_image_dsc_t * draw_dsc,
                               const lv_image_decoder_dsc_t * decoder_dsc, lv_draw_image_sup_t * sup,
                               const lv_area_t * img_coords, const lv_area_t * clipped_img_area)
{
    const lv_draw_buf_t * decoded = decoder_dsc->decoded;
    const uint8_t * src_buf;
    lv_color_format_t cf;
    uint32_t img_total_width;
    const bool is_ezip = (decoded != NULL) && ((decoded->header.flags & LV_IMAGE_FLAGS_EZIP) != 0U);
#if defined(EPIC_INPUT_JPEG)
    const bool is_jpeg = (decoded != NULL) && ((decoded->header.flags & LV_IMAGE_FLAGS_JPEG) != 0U);
#else
    const bool is_jpeg = false;
#endif

    if(decoded == NULL || decoded->data == NULL) {
        return;
    }

    if(draw_dsc->clip_radius != 0 && !LV_SIFLI_EPIC_USE_ROUND_CLIP) {
        return;
    }

    if(draw_dsc->blend_mode != LV_BLEND_MODE_NORMAL) {
        return;
    }

    lv_draw_buf_flush_cache(decoded, NULL);

    src_buf = decoded->data;
    cf = decoded->header.cf;

    if(is_ezip || is_jpeg) {
        img_total_width = decoded->header.w;
    }
    else {
        img_total_width = lv_epic_stride_to_width(decoded->header.stride, cf);
    }

    EPIC_LayerConfigTypeDef input_layers[4];
    EPIC_LayerConfigTypeDef output_layer;
    uint8_t input_layer_cnt = 2;

    if(lv_epic_setup_layers(&input_layers[0], &output_layer, task, clipped_img_area)) {
        return;
    }

    HAL_EPIC_LayerConfigInit(&input_layers[1]);
    input_layers[1].transform_cfg.angle = (draw_dsc->rotation + 3600) % 3600;
    input_layers[1].transform_cfg.pivot_x = draw_dsc->pivot.x;
    input_layers[1].transform_cfg.pivot_y = draw_dsc->pivot.y;
    input_layers[1].transform_cfg.scale_x = LV_SCALE_NONE * EPIC_INPUT_SCALE_NONE / (uint32_t)draw_dsc->scale_x;
    input_layers[1].transform_cfg.scale_y = LV_SCALE_NONE * EPIC_INPUT_SCALE_NONE / (uint32_t)draw_dsc->scale_y;
    input_layers[1].alpha = draw_dsc->opa;
    input_layers[1].x_offset = img_coords->x1;
    input_layers[1].y_offset = img_coords->y1;

    if(is_ezip) {
        input_layers[1].color_mode = EPIC_INPUT_EZIP;
    }
#if defined(EPIC_INPUT_JPEG)
    else if(is_jpeg) {
        input_layers[1].color_mode = EPIC_INPUT_JPEG;
    }
#endif
    else {
        input_layers[1].color_mode = lv_img_cf_to_epic_cf(cf);
    }

    input_layers[1].data = (uint8_t *)src_buf;
    input_layers[1].data_size = decoded->data_size;
    input_layers[1].width = lv_area_get_width(img_coords);
    input_layers[1].height = lv_area_get_height(img_coords);
    input_layers[1].total_width = img_total_width;

#ifdef EPIC_SUPPORT_A8
    if(input_layers[1].color_mode == EPIC_INPUT_A8) {
        input_layers[1].color_en = true;
        input_layers[1].color_r = sup->alpha_color.red;
        input_layers[1].color_g = sup->alpha_color.green;
        input_layers[1].color_b = sup->alpha_color.blue;
        input_layers[1].ax_mode = ALPHA_BLEND_RGBCOLOR;
    }
    else
#endif
#ifdef EPIC_SUPPORT_L8
        if(input_layers[1].color_mode == EPIC_INPUT_L8) {
            input_layers[1].lookup_table = (uint8_t *)decoder_dsc->palette;
            input_layers[1].color_en = false;
        }
        else
#endif
        {
            input_layers[1].color_en = false;
        }

#ifdef EPIC_SUPPORT_MASK
    if(cf == LV_COLOR_FORMAT_RGB565A8) {
        HAL_EPIC_LayerConfigInit(&input_layers[input_layer_cnt]);
        input_layers[input_layer_cnt].alpha = draw_dsc->opa;
        input_layers[input_layer_cnt].x_offset = img_coords->x1;
        input_layers[input_layer_cnt].y_offset = img_coords->y1;
        input_layers[input_layer_cnt].color_mode = EPIC_INPUT_A8;
        input_layers[input_layer_cnt].data = ((uint8_t *)src_buf) + decoded->header.h * decoded->header.stride;
        input_layers[input_layer_cnt].ax_mode = ALPHA_BLEND_OVERWRITE;
        input_layers[input_layer_cnt].width = lv_area_get_width(img_coords);
        input_layers[input_layer_cnt].height = lv_area_get_height(img_coords);
        input_layers[input_layer_cnt].total_width = decoded->header.w;
        input_layer_cnt++;
    }
#endif

    lv_image_decoder_dsc_t mask_decoder_dsc;
    bool mask_decoder_opened = false;

    if(draw_dsc->bitmap_mask_src != NULL) {
        lv_area_t mask_area;
        const lv_image_dsc_t * mask_dsc = draw_dsc->bitmap_mask_src;
        const lv_area_t * image_area = lv_area_get_width(&draw_dsc->image_area) < 0 ? img_coords : &draw_dsc->image_area;

        if(lv_image_src_get_type(mask_dsc) == LV_IMAGE_SRC_VARIABLE &&
           (mask_dsc->header.cf == LV_COLOR_FORMAT_A8 || mask_dsc->header.cf == LV_COLOR_FORMAT_L8)) {
            const lv_image_header_t * header = &mask_dsc->header;

            lv_area_set(&mask_area, 0, 0, header->w - 1, header->h - 1);
            lv_area_align(image_area, &mask_area, LV_ALIGN_CENTER, 0, 0);

            HAL_EPIC_LayerConfigInit(&input_layers[input_layer_cnt]);
            input_layers[input_layer_cnt].alpha = LV_OPA_COVER;
            input_layers[input_layer_cnt].x_offset = mask_area.x1;
            input_layers[input_layer_cnt].y_offset = mask_area.y1;
            input_layers[input_layer_cnt].color_mode = EPIC_INPUT_A8;
            input_layers[input_layer_cnt].data = (uint8_t *)mask_dsc->data;
            input_layers[input_layer_cnt].ax_mode = ALPHA_BLEND_OVERWRITE;
            input_layers[input_layer_cnt].width = lv_area_get_width(&mask_area);
            input_layers[input_layer_cnt].height = lv_area_get_height(&mask_area);
            input_layers[input_layer_cnt].total_width = header->stride;
            input_layer_cnt++;
        }
        else if(lv_image_decoder_open(&mask_decoder_dsc, draw_dsc->bitmap_mask_src, NULL) == LV_RESULT_OK &&
                mask_decoder_dsc.decoded != NULL) {
            const lv_draw_buf_t * mask_img = mask_decoder_dsc.decoded;
            mask_decoder_opened = true;

            if(mask_img->header.cf == LV_COLOR_FORMAT_A8 || mask_img->header.cf == LV_COLOR_FORMAT_L8) {
                lv_area_set(&mask_area, 0, 0, mask_img->header.w - 1, mask_img->header.h - 1);
                lv_area_align(image_area, &mask_area, LV_ALIGN_CENTER, 0, 0);

                HAL_EPIC_LayerConfigInit(&input_layers[input_layer_cnt]);
                input_layers[input_layer_cnt].alpha = LV_OPA_COVER;
                input_layers[input_layer_cnt].x_offset = mask_area.x1;
                input_layers[input_layer_cnt].y_offset = mask_area.y1;
                input_layers[input_layer_cnt].color_mode = EPIC_INPUT_A8;
                input_layers[input_layer_cnt].data = (uint8_t *)mask_img->data;
                input_layers[input_layer_cnt].ax_mode = ALPHA_BLEND_OVERWRITE;
                input_layers[input_layer_cnt].width = lv_area_get_width(&mask_area);
                input_layers[input_layer_cnt].height = lv_area_get_height(&mask_area);
                input_layers[input_layer_cnt].total_width = mask_img->header.stride;
                input_layer_cnt++;
            }
            else {
            }
        }
        else {
        }
    }

#if LV_SIFLI_EPIC_USE_ROUND_CLIP
    if(draw_dsc->clip_radius != 0 && input_layer_cnt == 2 && draw_dsc->rotation == 0 &&
       draw_dsc->scale_x == LV_SCALE_NONE && draw_dsc->scale_y == LV_SCALE_NONE) {
        epic_img_blend_rounded(task, draw_dsc, &input_layers[1], img_coords, clipped_img_area);
    }
    else
#endif
    {
        lv_epic_blend(input_layers, input_layer_cnt, &output_layer);
    }

    if(mask_decoder_opened) {
        lv_image_decoder_close(&mask_decoder_dsc);
    }
}

#if LV_SIFLI_EPIC_USE_ROUND_CLIP
static void epic_img_blend_rounded(lv_draw_task_t * task, const lv_draw_image_dsc_t * draw_dsc,
                                   const EPIC_LayerConfigTypeDef * fg_layer,
                                   const lv_area_t * img_coords, const lv_area_t * clipped_img_area)
{
    lv_layer_t * layer = task->target_layer;

    if(layer == NULL || layer->draw_buf == NULL) {
        return;
    }

    int32_t w = lv_area_get_width(clipped_img_area);
    int32_t h = lv_area_get_height(clipped_img_area);
    if(w <= 0 || h <= 0) {
        return;
    }

    uint8_t * mask_buf = lv_malloc((size_t)w * (size_t)h);
    if(mask_buf == NULL) {
        LV_LOG_WARN("epic: no memory for rounded image mask (%dx%d)", (int)w, (int)h);
        return;
    }

    /* The radius is relative to the whole image, like the SW renderer
     * (lv_draw_sw_img.c uses draw_dsc->image_area). */
    const lv_area_t * radius_area = (lv_area_get_width(&draw_dsc->image_area) >= 0) ?
                                    &draw_dsc->image_area : img_coords;

    epic_mw_mask_area_t radius_rect;
    radius_rect.x1 = radius_area->x1;
    radius_rect.y1 = radius_area->y1;
    radius_rect.x2 = radius_area->x2;
    radius_rect.y2 = radius_area->y2;

    epic_mw_mask_radius_param_t radius_param;
    epic_mw_mask_radius_init(&radius_param, &radius_rect, draw_dsc->clip_radius, false);

    void * masks[2] = {&radius_param, NULL};
    uint8_t * row = mask_buf;
    for(int32_t y = clipped_img_area->y1; y <= clipped_img_area->y2; y++, row += w) {
        memset(row, 0xFF, (size_t)w);
        (void)epic_mw_mask_apply(masks, row, clipped_img_area->x1, y, w);
    }
    epic_mw_mask_free_param(&radius_param);

    /* CPU-written mask read by the GPU: not covered by the layer cache
     * maintenance of lv_epic_blend. */
    lv_epic_flush_cache_range(mask_buf, (uint32_t)w * (uint32_t)h);

    EPIC_LayerConfigTypeDef mask_layer;
    HAL_EPIC_LayerConfigInit(&mask_layer);
    mask_layer.alpha = LV_OPA_COVER;
    mask_layer.x_offset = clipped_img_area->x1;
    mask_layer.y_offset = clipped_img_area->y1;
    mask_layer.color_mode = EPIC_INPUT_A8;
    mask_layer.data = mask_buf;
    mask_layer.data_size = (uint32_t)w * (uint32_t)h;
    mask_layer.ax_mode = ALPHA_BLEND_OVERWRITE;
    mask_layer.width = w;
    mask_layer.height = h;
    mask_layer.total_width = w;

    lv_color_format_t dest_cf = layer->color_format;
    epic_mw_buf_t buf;
    buf.cf = lv_img_cf_to_epic_cf(dest_cf);
    buf.data = (uint8_t *)lv_draw_layer_go_to_xy(layer, 0, 0);
    buf.area.x0 = layer->buf_area.x1;
    buf.area.y0 = layer->buf_area.y1;
    buf.area.x1 = layer->buf_area.x2;
    buf.area.y1 = layer->buf_area.y2;
    buf.total_width = lv_epic_stride_to_width(layer->draw_buf->header.stride, dest_cf);

    epic_mw_rl_t rl = epic_mw_begin(&buf, NULL);
    if(rl == NULL) {
        LV_LOG_WARN("epic_mw: no free render list for rounded image");
        lv_free(mask_buf);
        return;
    }

    epic_mw_op_t * o = epic_mw_alloc_op(&buf);
    if(o == NULL) {
        epic_mw_discard(rl);
        lv_free(mask_buf);
        return;
    }

    o->op = DRV_EPIC_DRAW_IMAGE;
    o->clip_area.x0 = clipped_img_area->x1;
    o->clip_area.y0 = clipped_img_area->y1;
    o->clip_area.x1 = clipped_img_area->x2;
    o->clip_area.y1 = clipped_img_area->y2;
    o->desc.blend.use_dest_as_bg = EPIC_BLEND_MODE_NORMAL;
    o->desc.blend.layer = *fg_layer;
    o->mask = mask_layer;

    if(epic_mw_commit_op(o) != 0) {
        epic_mw_discard(rl);
        lv_free(mask_buf);
        return;
    }

    (void)epic_mw_flush(rl);

    /* epic_mw_flush() is synchronous (HAL_EPIC_BlendStartEx waits), so the
     * mask buffer can be released here. */
    lv_free(mask_buf);
}
#endif /*LV_SIFLI_EPIC_USE_ROUND_CLIP*/

#endif /*LV_USE_SIFLI_EPIC*/
