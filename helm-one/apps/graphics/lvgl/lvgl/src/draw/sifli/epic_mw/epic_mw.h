/**
 * @file epic_mw.h
 *
 * EPIC middle layer — SDK v8 drv_epic.c NEW_API ported to openvela/NuttX.
 *
 * The types below are copied verbatim from SiFli SDK 2.4
 * rtos/rtthread/bsp/sifli/drivers/drv_epic.h (NEW_API, lines 116-246) so that
 * the v9 draw-task -> op mapping code reads like the SDK's lv_gpu_new_api.c.
 * Only the OS return type (rt_err_t -> int) and the lifecycle entry points
 * change.  See /tmp/lvgl_bench/port_plan.md §4.2.
 *
 * Session ownership (hard rule, port_plan.md §4.4):
 *   HAL_EPIC_ContBlendStart/Repeat may only be issued by the op layer and MUST
 *   be closed by the same epic_mw_flush() call (CONT_BLEND_STOP).  No session
 *   survives a flush, so the op layer can alternate safely with the existing
 *   lv_epic_fill/lv_epic_blend paths.
 */

#ifndef EPIC_MW_H
#define EPIC_MW_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include "../../../lv_conf_internal.h"

#if LV_USE_SIFLI_EPIC
#include "../epic/lv_sifli_epic_cfg.h"

/*********************
 *      DEFINES
 *********************/

#define EPIC_MW_TIMEOUT_MS 500

/**********************
 *      TYPEDEFS
 **********************/

/* --- drv_epic.h:116-128 --- */
typedef enum {
    DRV_EPIC_DRAW_MIN = 0,
    DRV_EPIC_DRAW_IMAGE = DRV_EPIC_DRAW_MIN,
    DRV_EPIC_DRAW_FILL,
    DRV_EPIC_DRAW_LETTERS,
    DRV_EPIC_DRAW_ARC,
    DRV_EPIC_DRAW_RECT,
    DRV_EPIC_DRAW_LINE,
    DRV_EPIC_DRAW_BORDER,
    DRV_EPIC_DRAW_MAX,
    DRV_EPIC_INVALID = 0xFFFF,      /*Invalid*/
} drv_epic_op_type_t;

/* --- drv_epic.h:131-135 --- */
typedef struct {
    const uint8_t * data;
    EPIC_AreaTypeDef area;
} drv_epic_letter_type_t;

/* --- drv_epic.h:137-142 --- */
typedef enum {
    EPIC_BLEND_MODE_FIXED_BG = 0,       /*Fixed background color, no blending with destination buffer*/
    EPIC_BLEND_MODE_NORMAL  = 1,        /*Normal mode, blending with destination buffer*/
    EPIC_BLEND_MODE_OVERWRITE  = 2,     /*Overwrite mode, no background*/
} drv_epic_blend_mode_type_t;

/* --- drv_epic.h:144-238 --- */
typedef struct {
    drv_epic_op_type_t  op;
    EPIC_AreaTypeDef clip_area;
    EPIC_LayerConfigTypeDef mask;

    union {
        struct {
            EPIC_LayerConfigTypeDef layer;
            uint8_t use_dest_as_bg; /* See drv_epic_blend_mode_type_t */
            uint8_t r;
            uint8_t g;
            uint8_t b;
        } blend;
        struct {
            uint8_t r;
            uint8_t g;
            uint8_t b;
            uint8_t opa;
        } fill;
        struct {
            uint32_t color_mode;
            uint8_t r;
            uint8_t g;
            uint8_t b;
            uint8_t opa;

            uint32_t letter_num;
            drv_epic_letter_type_t * p_letters;
        } label;
        struct {
            int16_t center_x;
            int16_t center_y;

            uint16_t start_angle;
            uint16_t end_angle;

            uint16_t width;
            uint16_t radius;
            uint8_t round_start;
            uint8_t round_end;

            uint32_t argb8888;
        } arc;
        struct {
            EPIC_AreaTypeDef area;

            uint16_t radius;
            uint8_t top_fillet;
            uint8_t bot_fillet;

            uint32_t argb8888;
        } rectangle;
        struct {
            EPIC_PointTypeDef p1;
            EPIC_PointTypeDef p2;

            uint16_t width;
            int32_t dash_width;
            int32_t dash_gap;
            uint32_t argb8888;
            uint8_t round_start;
            uint8_t round_end;
            uint8_t raw_end;
        } line;
        struct {
            EPIC_AreaTypeDef area;

            uint16_t radius;
            uint16_t width;

            uint8_t top_side : 1;
            uint8_t bot_side : 1;
            uint8_t left_side : 1;
            uint8_t right_side : 1;
            uint8_t reserved : 4;

            uint32_t argb8888;
        } border;

    } desc;

    /*Offset to specified dst buf, internal use only*/
    int16_t offset_x;
    int16_t offset_y;
} drv_epic_operation;

typedef drv_epic_operation epic_mw_op_t;

/* --- drv_epic.h:241-246 + explicit stride (port_plan.md §6.6) ---
 * total_width is the destination stride in pixels.  The SDK writes
 * `dst.total_width = dst.width` (drv_epic.c:4928/5031), which silently
 * mis-lays-out v9 draw buffers whose header.stride > width * bpp.  The v9
 * adapter must pass lv_epic_stride_to_width(draw_buf->header.stride, cf);
 * 0 means "derive from the area width" (SDK behaviour). */
typedef struct {
    uint32_t cf;       /**< color mode, refer to EPIC_COLOR_XXX, like #EPIC_COLOR_RGB565 */
    uint8_t * data;
    EPIC_AreaTypeDef area;
    uint16_t total_width; /**< stride in pixels, 0 = area width */
} epic_mw_buf_t;

typedef void * epic_mw_rl_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Allocate the render-list / mask pools and clean the circle cache.
 * Called from lv_draw_sifli_epic_init(); must run before any other call.
 * @return 0 on success
 */
int epic_mw_init(void);

/**
 * Release the middle layer (drops the circle cache).
 */
void epic_mw_deinit(void);

/**
 * Start a render list for one draw task.
 * @param dst      destination buffer (stride aware)
 * @param ow_area  overwrite area (may be NULL)
 * @return render list handle, or NULL if the pool is exhausted
 */
epic_mw_rl_t epic_mw_begin(const epic_mw_buf_t * dst, const EPIC_AreaTypeDef * ow_area);

/**
 * Allocate one operation inside the current render list.
 * The caller must fill `op->op`, `op->clip_area` and the matching
 * `op->desc.*` member, then call epic_mw_commit_op().
 * @return operation pointer or NULL if the list is full
 */
epic_mw_op_t * epic_mw_alloc_op(const epic_mw_buf_t * dst);

/**
 * Commit a previously allocated operation.
 * @return 0 on success
 */
int epic_mw_commit_op(epic_mw_op_t * op);

/**
 * Synchronously execute + tear down the render list.
 * render_start() -> render(rl) -> CONT_BLEND_STOP -> gpu check done ->
 * destroy_render_list()  (port_plan.md §4.2/§6.3: no session survives a flush)
 * @return 0 on success, -1 on error
 */
int epic_mw_flush(epic_mw_rl_t rl);

/**
 * Drop the render list without rendering it (error path).
 * @return 0 on success
 */
int epic_mw_discard(epic_mw_rl_t rl);

/**
 * Enable/disable debug printing (mirrors drv_epic_cfg printRl/printDetail).
 */
void epic_mw_set_print_rl(uint32_t en);
void epic_mw_set_print_detail(uint32_t en);

/**********************
 *   DEBUG STATISTICS
 **********************/

/* Per-op counters / cycle-derived microseconds, indexed by drv_epic_op_type_t.
 * Printed by lv_sifli_epic_dump_stats(). */
extern uint32_t epic_mw_dbg_ops[DRV_EPIC_DRAW_MAX];
extern uint32_t epic_mw_dbg_us[DRV_EPIC_DRAW_MAX];

/**********************
 *      MACROS
 **********************/

#endif /*LV_USE_SIFLI_EPIC*/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*EPIC_MW_H*/
