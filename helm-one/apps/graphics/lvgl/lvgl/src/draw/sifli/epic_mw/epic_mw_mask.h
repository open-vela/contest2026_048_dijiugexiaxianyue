/**
 * @file epic_mw_mask.h
 *
 * Mask engine of the EPIC middle layer.
 * Verbatim port of SiFli SDK 2.4 drv_epic_mask.h (LVGL v8 lv_draw_mask renamed),
 * cropped per /tmp/lvgl_bench/port_plan.md §2.2.  Original copyright:
 * Copyright (c) 2019 - 2022, Sifli Technology (see SDK header).
 *
 * Ported by script: drv_epic_mask_* -> epic_mw_mask_*.
 */

/**
 * @file drv_epic_mask.h
 *
 */

/* Cropped for the EPIC middle-layer port (port_plan.md §2.2):
 * fade/map masks, mask_init/deinit (empty mutex) removed. */
#ifndef EPIC_MW_MASK_H
#define EPIC_MW_MASK_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "epic_mw_rtcompat.h"
#include <stdint.h>

/*********************
 *      DEFINES
 *********************/
#define LV_MASK_ID_INV  (-1)
#define LV_MASK_MAX_NUM     16

/**********************
 *      TYPEDEFS
 **********************/

typedef enum
{
    DRAW_MASK_RES_TRANSP,
    DRAW_MASK_RES_FULL_COVER,
    DRAW_MASK_RES_CHANGED,
    DRAW_MASK_RES_UNKNOWN
} epic_mw_mask_res_t;


typedef enum
{
    DRAW_MASK_TYPE_LINE,
    DRAW_MASK_TYPE_ANGLE,
    DRAW_MASK_TYPE_RADIUS,
    DRAW_MASK_TYPE_FADE,
    DRAW_MASK_TYPE_MAP,
} epic_mw_mask_type_t;

typedef enum
{
    DRAW_MASK_LINE_SIDE_LEFT = 0,
    DRAW_MASK_LINE_SIDE_RIGHT,
    DRAW_MASK_LINE_SIDE_TOP,
    DRAW_MASK_LINE_SIDE_BOTTOM,
} epic_mw_mask_line_side_t;


typedef uint8_t epic_mw_mask_opa_t;


typedef struct
{
    int32_t x;
    int32_t y;
} epic_mw_mask_point_t;


/** Represents an area of the screen.*/
typedef struct
{
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
} epic_mw_mask_area_t;


/**
 * A common callback type for every mask type.
 * Used internally by the library.
 */
typedef epic_mw_mask_res_t (*epic_mw_mask_xcb_t)(epic_mw_mask_opa_t *mask_buf, int32_t abs_x, int32_t abs_y,
        int32_t len,
        void *p);
typedef struct
{
    uint8_t *buf;
    epic_mw_mask_opa_t *cir_opa;          /**< Opacity of values on the circumference of an 1/4 circle */
    uint16_t *x_start_on_y;     /**< The x coordinate of the circle for each y value */
    uint16_t *opa_start_on_y;   /**< The index of `cir_opa` for each y value */
    int32_t life;               /**< How many times the entry way used */
    uint32_t used_cnt;          /**< Like a semaphore to count the referencing masks */
    int32_t radius;             /**< The radius of the entry */
} epic_mw_mask_radius_circle_dsc_t;

typedef struct
{
    epic_mw_mask_xcb_t cb;
    epic_mw_mask_type_t type;
} epic_mw_mask_common_dsc_t;

typedef struct
{
    /** The first element must be the common descriptor */
    epic_mw_mask_common_dsc_t dsc;

    struct
    {
        /*First point*/
        epic_mw_mask_point_t p1;

        /*Second point*/
        epic_mw_mask_point_t p2;

        /*Which side to keep?*/
        epic_mw_mask_line_side_t side : 2;
    } cfg;

    /** A point of the line */
    epic_mw_mask_point_t origo;

    /** X / (1024*Y) steepness (X is 0..1023 range). What is the change of X in 1024 Y? */
    int32_t xy_steep;

    /** Y / (1024*X) steepness (Y is 0..1023 range). What is the change of Y in 1024 X? */
    int32_t yx_steep;

    /** Helper which stores yx_steep for flat lines and xy_steep for steep (non flat) lines */
    int32_t steep;

    /** Steepness in 1 px in 0..255 range. Used only by flat lines. */
    int32_t spx;

    /** 1: It's a flat line? (Near to horizontal) */
    uint8_t flat : 1;

    /** Invert the mask. The default is: Keep the left part.
     *It is used to select left/right/top/bottom */
    uint8_t inv: 1;
} epic_mw_mask_line_param_t;

typedef struct
{
    /** The first element must be the common descriptor */
    epic_mw_mask_common_dsc_t dsc;

    struct
    {
        epic_mw_mask_point_t vertex_p;
        int32_t start_angle;
        int32_t end_angle;
    } cfg;

    epic_mw_mask_line_param_t start_line;
    epic_mw_mask_line_param_t end_line;
    uint16_t delta_deg;
} epic_mw_mask_angle_param_t;

typedef struct
{
    /** The first element must be the common descriptor */
    epic_mw_mask_common_dsc_t dsc;

    struct
    {
        epic_mw_mask_area_t rect;
        int32_t radius;
        /** Invert the mask. 0: Keep the pixels inside. */
        uint8_t outer: 1;
    } cfg;

    epic_mw_mask_radius_circle_dsc_t *circle;
} epic_mw_mask_radius_param_t;


/**********************
 * GLOBAL PROTOTYPES
 **********************/

//! @cond Doxygen_Suppress

/**
 * Apply the added buffers on a line. Used internally by the library's drawing routines.
 * @param masks the masks list to apply, must be ended with NULL pointer in array.
 * @param mask_buf store the result mask here. Has to be `len` byte long. Should be initialized with `0xFF`.
 * @param abs_x absolute X coordinate where the line to calculate start
 * @param abs_y absolute Y coordinate where the line to calculate start
 * @param len length of the line to calculate (in pixel count)
 * @return One of these values:
 * - `LV_DRAW_MASK_RES_FULL_TRANSP`: the whole line is transparent. `mask_buf` is not set to zero
 * - `LV_DRAW_MASK_RES_FULL_COVER`: the whole line is fully visible. `mask_buf` is unchanged
 * - `LV_DRAW_MASK_RES_CHANGED`: `mask_buf` has changed, it shows the desired opacity of each pixel in the given line
 */
epic_mw_mask_res_t /* LV_ATTRIBUTE_FAST_MEM */ epic_mw_mask_apply(void *masks[], epic_mw_mask_opa_t *mask_buf,
        int32_t abs_x,
        int32_t abs_y,
        int32_t len);

//! @endcond

/**
 * Free the data from the parameter.
 * It's called inside `epic_mw_mask_remove_id` and `epic_mw_mask_remove_custom`
 * Needs to be called only in special cases when the mask is not added by `lv_draw_mask_add`
 * and not removed by `lv_draw_mask_remove_id` or `lv_draw_mask_remove_custom`
 * @param p pointer to a mask parameter
 */
void epic_mw_mask_free_param(void *p);

/**
 *Initialize a line mask from two points.
 * @param param pointer to a `epic_mw_mask_param_t` to initialize
 * @param p1x X coordinate of the first point of the line
 * @param p1y Y coordinate of the first point of the line
 * @param p2x X coordinate of the second point of the line
 * @param p2y y coordinate of the second point of the line
 * @param side and element of `lv_draw_mask_line_side_t` to describe which side to keep.
 * With `LV_DRAW_MASK_LINE_SIDE_LEFT/RIGHT` and horizontal line all pixels are kept
 * With `LV_DRAW_MASK_LINE_SIDE_TOP/BOTTOM` and vertical line all pixels are kept
 */
void epic_mw_mask_line_points_init(epic_mw_mask_line_param_t *param, int32_t p1x, int32_t p1y,
                                    int32_t p2x,
                                    int32_t p2y, epic_mw_mask_line_side_t side);

/**
 *Initialize a line mask from a point and an angle.
 * @param param  pointer to a `epic_mw_mask_param_t` to initialize
 * @param px     X coordinate of a point of the line
 * @param py     X coordinate of a point of the line
 * @param angle  right 0 deg, bottom: 90
 * @param side   an element of `lv_draw_mask_line_side_t` to describe which side to keep.
 * With `LV_DRAW_MASK_LINE_SIDE_LEFT/RIGHT` and horizontal line all pixels are kept
 * With `LV_DRAW_MASK_LINE_SIDE_TOP/BOTTOM` and vertical line all pixels are kept
 */
void epic_mw_mask_line_angle_init(epic_mw_mask_line_param_t *param, int32_t px, int32_t py, int16_t angle,
                                   epic_mw_mask_line_side_t side);

/**
 * Initialize an angle mask.
 * @param param pointer to a `epic_mw_mask_param_t` to initialize
 * @param vertex_x X coordinate of the angle vertex (absolute coordinates)
 * @param vertex_y Y coordinate of the angle vertex (absolute coordinates)
 * @param start_angle start angle in degrees. 0 deg on the right, 90 deg, on the bottom
 * @param end_angle end angle
 */
void epic_mw_mask_angle_init(epic_mw_mask_angle_param_t *param, int32_t vertex_x, int32_t vertex_y,
                              int32_t start_angle, int32_t end_angle);

/**
 * Initialize a fade mask.
 * @param param pointer to an `epic_mw_mask_radius_param_t` to initialize
 * @param rect coordinates of the rectangle to affect (absolute coordinates)
 * @param radius radius of the rectangle
 * @param inv true: keep the pixels inside the rectangle; keep the pixels outside of the rectangle
 */
void epic_mw_mask_radius_init(epic_mw_mask_radius_param_t *param, const epic_mw_mask_area_t *rect, int32_t radius,
                               bool inv);
static inline void epic_mw_mask_radius_init2(epic_mw_mask_radius_param_t *param, const EPIC_AreaTypeDef *rect, int32_t radius,
        bool inv)
{
    epic_mw_mask_area_t _mask_area;
    _mask_area.x1 = rect->x0;
    _mask_area.x2 = rect->x1;
    _mask_area.y1 = rect->y0;
    _mask_area.y2 = rect->y1;


    epic_mw_mask_radius_init(param, &_mask_area, radius, inv);
}

void epic_mw_mask_cleanup(void);
/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*EPIC_MW_MASK_H*/
