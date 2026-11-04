/**
 * @file lv_draw_sw_blend_private.h
 *
 */

#ifndef LV_DRAW_SW_BLEND_PRIVATE_H
#define LV_DRAW_SW_BLEND_PRIVATE_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include "lv_draw_sw_blend.h"

#if LV_USE_DRAW_SW

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *      TYPEDEFS
 **********************/

struct _lv_draw_sw_blend_dsc_t {
    const lv_area_t * blend_area;   /**< The area with absolute coordinates to draw on `layer->buf`
                                     *   will be clipped to `layer->clip_area` */
    const void * src_buf;           /**< Pointer to an image to blend. If set `fill_color` is ignored */
    uint32_t src_stride;
    lv_color_format_t src_color_format;
    const lv_area_t * src_area;
    lv_opa_t opa;                   /**< The overall opacity*/
    lv_color_t color;               /**< Fill color*/
    const lv_opa_t * mask_buf;      /**< NULL if ignored, or an alpha mask to apply on `blend_area`*/
    lv_draw_sw_mask_res_t mask_res; /**< The result of the previous mask operation */
    const lv_area_t * mask_area;    /**< The area of `mask_buf` with absolute coordinates*/
    int32_t mask_stride;
    lv_blend_mode_t blend_mode;     /**< E.g. LV_BLEND_MODE_ADDITIVE*/
};

struct _lv_draw_sw_blend_fill_dsc_t {
    void * dest_buf;
    int32_t dest_w;
    int32_t dest_h;
    int32_t dest_stride;
    const lv_opa_t * mask_buf;
    int32_t mask_stride;
    lv_color_t color;
    lv_opa_t opa;
    lv_area_t relative_area;
};

struct _lv_draw_sw_blend_image_dsc_t {
    void * dest_buf;
    int32_t dest_w;
    int32_t dest_h;
    int32_t dest_stride;
    const lv_opa_t * mask_buf;
    int32_t mask_stride;
    const void * src_buf;
    int32_t src_stride;
    lv_color_format_t src_color_format;
    lv_opa_t opa;
    lv_blend_mode_t blend_mode;
    lv_area_t relative_area;    /**< The blend area relative to the layer's buffer area. */
    lv_area_t src_area;             /**< The original src area. */
};

/**
 * Read or write four bytes of a pixel or mask buffer as one word. The caller must align the
 * pointer to a word first.
 *
 * Through a union and not a cast: C99 6.5p7 lets an object be accessed through an aggregate
 * that has its type among the members, `*(uint32_t *)ptr` on a `uint8_t` or `uint16_t` buffer
 * is undefined and GCC 13 at -O2 really does keep a stale half word across it.
 */
typedef union {
    uint32_t u32;
    uint16_t u16[2];
    uint8_t u8[4];
    lv_color32_t c32;
} lv_draw_sw_word_t;


/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**********************
 *      MACROS
 **********************/

/**
 * Prepare an RGB565 color for mixing: spread it over 32 bits so each channel gets room to
 * grow and a whole pixel can be mixed with one multiplication.
 * See https://stackoverflow.com/a/50012418/1999969
 */
#define LV_COLOR_MIX_16_PREPARE(c) ((((uint32_t)(c)) | (((uint32_t)(c)) << 16)) & 0x07E0F81Fu)

/**
 * Mix a prepared foreground into an RGB565 background. `mix` must be 1..254, the
 * callers handle 0 and 255 themselves. `res` may be the same variable as `bg`.
 * A macro and not a function because -Os inlines neither, and this runs on every pixel.
 * See https://stackoverflow.com/a/50012418/1999969
 */
#define LV_COLOR_MIX_16_TO_16_PREPARED(res, fg_prep, bg, mix)                               \
    do {                                                                                    \
        uint32_t bg_prep_ = LV_COLOR_MIX_16_PREPARE(bg);                                    \
        uint32_t mix5_ = ((uint32_t)(mix) + 4) >> 3;                                        \
        uint32_t res_ = (((((fg_prep) - bg_prep_) * mix5_) >> 5) + bg_prep_) & 0x07E0F81Fu; \
        (res) = (uint16_t)((res_ >> 16) | res_);                                            \
    } while(0)

#endif /* LV_USE_DRAW_SW */

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_DRAW_SW_BLEND_PRIVATE_H*/
