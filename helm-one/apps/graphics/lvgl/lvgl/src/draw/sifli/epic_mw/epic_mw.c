/**
 * @file epic_mw.c
 *
 * EPIC middle layer: the SiFli SDK v8 op engine (drv_epic.c NEW_API) ported to
 * openvela (NuttX + LVGL v9 draw unit).
 *
 * Source: /home/jinsc/SDK/SiFli/SDK/2.4/rtos/rtthread/bsp/sifli/drivers/drv_epic.c
 * Plan:   /tmp/lvgl_bench/port_plan.md (§2 minimal function subset, §4.2 API,
 *         §6 risk items).  Original copyright: Sifli Technology.
 *
 * Port deltas vs the SDK file:
 *  - OS glue: no rt_thread / rt_mq / rt_semaphore / rt_flash lock / PM / EZIP
 *    (plan §1.1, §6.1).  EPIC runs synchronously: HAL_API_BLEND_EX uses
 *    HAL_EPIC_BlendStartEx (waits inside HAL) instead of ..._IT.
 *  - HAL_API_TRANSFORM / HAL_API_COPY dropped: HAL_EPIC_Adv only exists as an
 *    RVDS-only prebuilt lib on openvela (plan §3.3).
 *  - DRV_EPIC_DRAW_LETTERS renderer dropped (stage 0/1); DRV_EPIC_DRAW_IMAGE
 *    renderer ported (drv_epic.c:4107) for clip_radius image masks; the
 *    non-rounded IMAGE path stays on lv_epic_blend.
 *  - Statistics reduced to epic_mw_dbg_ops/epic_mw_dbg_us per op type.
 *  - dst.total_width honours an explicit stride (plan §6.6).
 *  - mask ping-pong buffer is explicitly cache-cleaned before the GPU reads it
 *    (plan §6.2).
 */

#include "epic_mw.h"

#if LV_USE_SIFLI_EPIC

#include "epic_mw_rtcompat.h"
#include "epic_mw_mask.h"
#include "string.h"
#include <stdio.h>   /* snprintf in the ported debug printers */

/*********************
 *      DEFINES
 *********************/

/* --- drv_epic.c:71-89 --- */
#ifndef MIN
    #define MIN(x,y) (((x)<(y))?(x):(y))
#endif

#ifndef MAX
    #define MAX(x,y) (((x)>(y))?(x):(y))
#endif

#ifndef ABS
    #define ABS(x)          ((x) < (0) ? -(x) : (x))
#endif

#define EPIC_OPA_MIN 2
#define EPIC_OPA_MAX 255

#undef OPA_MAX
#define OPA_MAX 254

/* --- drv_epic.c:91-102 --- */
#define GPU_DEVICE_START()      /* no PM on the v9 path (plan §6.2) */
#define GPU_DEVICE_STOP()
#define GPU_BLEND_EXP_MS        1000
#define mono_layer_addr         HPSYS_RAM1_BASE  /* Any accessable address for mono layer, SRAM is better */

/* --- drv_epic.c:105-129 --- */
#define render_list_pool_max    2
#define src_list_max            80
#define mask_buf_max_bytes      (16*1024)
#define mask_buf2_max_bytes     1600

#define rl_flag_commit          0x01
#define rl_flag_rendering       0x02
#define rl_flag_overwritting    0x04

#define radius_circle           0x7FFF /**< A very big radius to always draw as circle*/

/* Cycle counter -> us divisor (panel HCLK 240 MHz, same convention as
 * lv_sifli_epic_dbg_cyc() in the v9 draw unit). */
#define EPIC_MW_CYC_PER_US      240u

typedef enum {
    HAL_API_BLEND_EX,
    HAL_API_CONT_BLEND,
    HAL_API_CONT_BLEND_STOP,
    HAL_API_CONT_BLEND_ASYNC_STOP,
} HAL_API_TypeDef;

/**********************
 *      TYPEDEFS
 **********************/

/* --- drv_epic.c:130-143 (letter_pool dropped: LABEL stays on the SW path) --- */
typedef struct {
    uint32_t  used;
    uint32_t  flag;
    EPIC_LayerConfigTypeDef dst;

    uint16_t src_list_alloc_len; /* Allocated src list len */
    uint16_t src_list_len; /* Committed src len*/
    drv_epic_operation src_list[src_list_max];
    EPIC_AreaTypeDef  commit_area; /*Committed invalid area*/

} priv_render_list_t;

typedef struct {
    priv_render_list_t * render_list_pool;
    priv_render_list_t * using_rl;  /* Committing rl */

    uint8_t * mask_buf_pool;   /* Pool for draw mask */
    uint8_t * mask_buf2_pool;  /* Pool2 for draw mask (arc round caps) */

    uint32_t dbg_flag_print_rl : 1;         /* Show render list before start */
    uint32_t dbg_flag_print_exe_detail : 1; /* Show render operation execute detail */
    uint32_t dbg_flag_dis_merge_operations; /* != 0xFFFFFFFF => op merging enabled */
    uint32_t dbg_flag_dis_operations;
    uint32_t dbg_mask_buf_pool_max;

    /* render_lock bookkeeping (flash lock is a no-op here, plan §6.4) */
    uint32_t gpu_last_op;
    uint32_t gpu_fg_addr;
    uint32_t gpu_bg_addr;
    uint32_t gpu_mask_addr;
    uint32_t gpu_output_addr;
    uint32_t gpu_output_size;
} epic_mw_dev_t;

/**********************
 *  STATIC VARIABLES
 **********************/

static epic_mw_dev_t epic_mw_dev;
static priv_render_list_t epic_mw_render_list_pool[render_list_pool_max];
static uint8_t epic_mw_mask_buf_pool[mask_buf_max_bytes];
static uint8_t epic_mw_mask_buf2_pool[mask_buf2_max_bytes];
static uint8_t epic_mw_inited;

uint32_t epic_mw_dbg_ops[DRV_EPIC_DRAW_MAX];
uint32_t epic_mw_dbg_us[DRV_EPIC_DRAW_MAX];

/*********************
 *      MACROS
 *********************/

/* --- drv_epic.c:2046-2049 ---
 * plan §6.5: the SDK asserts (89 RT_ASSERT sites) on these paths; on a UI path
 * an assertion means a white screen / reset -> log and skip the operation. */
#define DRV_EPIC_ASSERT(expr) do {\
    if(!(expr)){ LV_LOG_ERROR("epic_mw: assert(" #expr ") failed at " __FILE__ ":%d", __LINE__); }\
} while(0)

#define AreaString "x0y0x1y1=[%d,%d,%d,%d]"

/* --- drv_epic.c:290-311 --- */
#define FORMATED_LAYER_INFO(layer, layer_name) \
                        "%s,cf=0x%x,data=%p,total_w=%d,"AreaString" frac[%x,%x]", \
                                    (layer_name), (layer)->color_mode, (void *)(layer)->data, (layer)->total_width,\
                                    (layer)->x_offset, \
                                    (layer)->y_offset, \
                                    (layer)->x_offset + (layer)->width - 1, \
                                    (layer)->y_offset + (layer)->height - 1,\
                                    (layer)->x_offset_frac, (layer)->y_offset_frac

/* --- drv_epic.c:299-311 --- */
#define FORMATED_LAYER_EXTRA_INFO(layer) \
                        "ax=%d[0:NONE,1:MASK], alpha=%d, angle=%d, scale_xy=%d,%d, pivot_xy=%d,%d  mirror_hv=%d,%d", \
                                    (layer)->ax_mode, (layer)->alpha, \
                                    (layer)->transform_cfg.angle, \
                                    (layer)->transform_cfg.scale_x, (layer)->transform_cfg.scale_y, \
                                    (layer)->transform_cfg.pivot_x, (layer)->transform_cfg.pivot_y, \
                                    (layer)->transform_cfg.h_mirror, (layer)->transform_cfg.v_mirror

#define AreaParams(area) (area)->x0,(area)->y0,(area)->x1,(area)->y1

/**********************
 *  STATIC PROTOTYPES
 **********************/

static bool _layer_IntersectArea(const EPIC_LayerConfigTypeDef * layer0,
                                 const EPIC_AreaTypeDef * p0_area, EPIC_AreaTypeDef * res_area);
static void _clip_layer(EPIC_LayerConfigTypeDef * p_layer, const EPIC_AreaTypeDef * clip_area);
static void overwrite_with_circle(const epic_mw_mask_opa_t * circle_mask, const EPIC_AreaTypeDef * blend_area,
                                  const EPIC_AreaTypeDef * circle_area,
                                  epic_mw_mask_opa_t * mask_buf, int32_t width);
static void add_circle(const epic_mw_mask_opa_t * circle_mask, const EPIC_AreaTypeDef * blend_area,
                       const EPIC_AreaTypeDef * circle_area,
                       epic_mw_mask_opa_t * mask_buf, int32_t width);
static HAL_StatusTypeDef Call_Hal_Api(HAL_API_TypeDef api, void * p1, void * p2, void * p3);
static void draw_fill(EPIC_LayerConfigTypeDef * dst, EPIC_LayerConfigTypeDef * p_mask_layer,
                      const EPIC_AreaTypeDef * p_fill_area, uint32_t argb8888);
static void draw_rect(EPIC_LayerConfigTypeDef * dst, EPIC_LayerConfigTypeDef * p_mask_layer,
                      const EPIC_AreaTypeDef * p_fill_area, uint32_t argb8888);
static void draw_rect2(EPIC_LayerConfigTypeDef * dst, const EPIC_AreaTypeDef * p_clip_area,
                       EPIC_LayerConfigTypeDef * p_mask_layer,
                       const EPIC_AreaTypeDef * p_fill_area, uint32_t argb8888);
static rt_err_t draw_masked_rect(EPIC_LayerConfigTypeDef * dst, EPIC_LayerConfigTypeDef * p_mask_layer,
                                 void * masks[], const EPIC_AreaTypeDef * p_draw_area, uint32_t argb8888,
                                 const EPIC_AreaTypeDef * p_circle_mask_area1,
                                 const EPIC_AreaTypeDef * p_circle_mask_area2,
                                 epic_mw_mask_opa_t * p_circle_mask_buf,
                                 uint32_t circle_mask_width);
static rt_err_t draw_masked_rect2(EPIC_LayerConfigTypeDef * dst, const EPIC_AreaTypeDef * p_clip_area,
                                  EPIC_LayerConfigTypeDef * p_mask_layer,
                                  void * masks[], const EPIC_AreaTypeDef * p_draw_area, uint32_t argb8888,
                                  const EPIC_AreaTypeDef * p_circle_mask_area1,
                                  const EPIC_AreaTypeDef * p_circle_mask_area2,
                                  epic_mw_mask_opa_t * p_circle_mask_buf,
                                  uint32_t circle_mask_width);
static void get_rounded_area(int16_t angle, int32_t radius, uint8_t thickness, EPIC_AreaTypeDef * res_area);
static rt_err_t render_arc(drv_epic_operation * p_operation, EPIC_LayerConfigTypeDef * dst,
                           const EPIC_AreaTypeDef * p_clip_area);
static rt_err_t render_rectangle(drv_epic_operation * p_operation, EPIC_LayerConfigTypeDef * dst,
                                 const EPIC_AreaTypeDef * p_clip_area);
static rt_err_t render_border_complex(drv_epic_operation * p_operation, EPIC_LayerConfigTypeDef * dst,
                                      const EPIC_AreaTypeDef * p_clip_area,
                                      const EPIC_AreaTypeDef * outer_area, const EPIC_AreaTypeDef * inner_area,
                                      int32_t rout, int32_t rin, uint32_t argb8888);
static rt_err_t render_border_simple(drv_epic_operation * p_operation, EPIC_LayerConfigTypeDef * dst,
                                     const EPIC_AreaTypeDef * p_clip_area,
                                     const EPIC_AreaTypeDef * outer_area, const EPIC_AreaTypeDef * inner_area,
                                     uint32_t argb8888);
static rt_err_t render_border(drv_epic_operation * p_operation, EPIC_LayerConfigTypeDef * dst,
                              const EPIC_AreaTypeDef * p_clip_area);
static rt_err_t render_line_hor(drv_epic_operation * p_operation, EPIC_LayerConfigTypeDef * dst,
                                const EPIC_AreaTypeDef * p_clip_area);
static rt_err_t render_line_ver(drv_epic_operation * p_operation, EPIC_LayerConfigTypeDef * dst,
                                const EPIC_AreaTypeDef * p_clip_area);
static rt_err_t render_line_skew(drv_epic_operation * p_operation, EPIC_LayerConfigTypeDef * dst,
                                 const EPIC_AreaTypeDef * p_clip_area);
static rt_err_t render_line(drv_epic_operation * p_operation, EPIC_LayerConfigTypeDef * dst,
                            const EPIC_AreaTypeDef * p_clip_area);
static void render_fill(drv_epic_operation * p_operation, EPIC_LayerConfigTypeDef * dst,
                        const EPIC_AreaTypeDef * p_clip_area);
static void render_image(drv_epic_operation * p_operation, EPIC_LayerConfigTypeDef * dst,
                         const EPIC_AreaTypeDef * p_clip_area);
static void pixel_offset(uint32_t offset_bytes, uint32_t line_width, uint32_t color_bytes, int16_t * x_off,
                         int16_t * y_off);
static int16_t merge_a2b(const epic_mw_buf_t * p_buf_a, const EPIC_LayerConfigTypeDef * p_buf_b,
                         int16_t * x_off, int16_t * y_off, uint16_t * new_height);
static bool AreaExcept(EPIC_AreaTypeDef * res_p, const EPIC_AreaTypeDef * aex_p, const EPIC_AreaTypeDef * aholder_p);
static char * operation_name(drv_epic_op_type_t op);
static void print_operation(const char * name, const drv_epic_operation * op);
static void print_rl(priv_render_list_t * rl);

static rt_err_t render(epic_mw_rl_t list);
static rt_err_t render_list(priv_render_list_t * rl);
static void lock_render_list(epic_mw_rl_t list);
static int epic_mw_destroy_rl(epic_mw_rl_t list);
static int epic_mw_render_list_init(void);

/**********************
 *  SMALL HELPERS
 **********************/

static uint32_t epic_mw_cyc(void)
{
    /* Cortex-M33 DWT cycle counter, lazily enabled (same probe as the v9
     * draw unit's lv_sifli_epic_dbg_cyc()). */
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

/* drv_epic.c:396-404 */
/* plan §6.2: the mask ping-pong buffer is CPU-written / GPU-read and is NOT
 * covered by the v9 draw unit's per-layer cache maintenance. */
static void epic_mw_clean_range(const void * data, uint32_t size)
{
    if(data && size) {
        lv_epic_flush_cache_range(data, size);
    }
}

/* plan §6.1: replace drv_epic's epic_sema; the sync HAL waits internally, so
 * only a freshly launched ContBlendStart can still be in flight. */
static void wait_hw_done(void)
{
    EPIC_HandleTypeDef * hw_epic_handle = lv_epic_get_handle();
    uint32_t start;

    if(NULL == hw_epic_handle) {
        return;
    }

    start = lv_tick_get();
    while(HAL_EPIC_IsHWBusy(hw_epic_handle)) {
        if(lv_tick_elaps(start) > EPIC_MW_TIMEOUT_MS) {
            LV_LOG_WARN("epic_mw: wait_hw_done timeout");
            lv_epic_gpu_reset();
            break;
        }
    }
}

/* ContBlend session bookkeeping (drv_epic.c:2550 kept as a file-scope static so
 * epic_mw_prepare_sync() can clear a half-open session). */
static uint8_t epic_mw_cont_blend_states;

/* Mirror of lv_sifli_epic_cfg.c's epic_prepare_sync_nuttx(): the openvela
 * route is synchronous, so before each op-layer task reconcile the HW/State
 * pair.  A leftover cont session, a stuck EOF IRQ or another unit's failed
 * start would otherwise make the next HAL start return HAL_BUSY (plan §6.1). */
static void epic_mw_prepare_sync(void)
{
    EPIC_HandleTypeDef * hw_epic_handle = lv_epic_get_handle();
    uint32_t start;

    if(NULL == hw_epic_handle) {
        return;
    }

    if(HAL_EPIC_IsHWBusy(hw_epic_handle)) {
        /* Close a half-open cont session (HAL_EPIC_ContBlendStop waits and
         * disables the layers when api_type == CONTINUE). */
        (void)HAL_EPIC_ContBlendStop(hw_epic_handle);

        start = lv_tick_get();
        while(HAL_EPIC_IsHWBusy(hw_epic_handle)) {
            if(lv_tick_elaps(start) > 200) {
                LV_LOG_WARN("epic_mw: GPU still busy at flush entry, resetting");
                lv_epic_gpu_reset();
                break;
            }
        }
    }

    epic_mw_cont_blend_states = 0;
    hw_epic_handle->State = HAL_EPIC_STATE_READY;
}

/* plan §6.2: drv_epic.c:2286-2313 (SCB_CleanDCache / SCB_InvalidateDCache /
 * PM request) becomes a no-op; the v9 draw unit owns cache maintenance
 * (_epic_execute_drawing -> lv_draw_buf_flush_cache/invalidate_cache). */
static void render_start(void)
{
}

static void render_end(void)
{
}

/* plan §6.4: drv_epic.c:2230-2284 locks the XIP flash ranges of the GPU
 * layers.  Only reachable when op->mask.data != NULL, which the v9 adapter
 * never sets; the existing v9 paths never took this lock either. */
static void render_lock(drv_epic_op_type_t ops,
                        uint32_t fg,
                        uint32_t bg,
                        uint32_t mask)
{
    epic_mw_dev.gpu_last_op = ops;
    epic_mw_dev.gpu_fg_addr = fg;
    epic_mw_dev.gpu_bg_addr = bg;
    epic_mw_dev.gpu_mask_addr = mask;
}

static void render_unlock(void)
{
    epic_mw_dev.gpu_fg_addr  = 0;
    epic_mw_dev.gpu_bg_addr  = 0;
    epic_mw_dev.gpu_mask_addr = 0;
    epic_mw_dev.gpu_output_addr = 0;
    epic_mw_dev.gpu_output_size = 0;
}

/* ======== _layer_IntersectArea / _clip_layer / circle helpers (epic_mw_dev.c:2457-2544) ======== */

static bool _layer_IntersectArea(const EPIC_LayerConfigTypeDef *layer0,
                                 const EPIC_AreaTypeDef *p0_area, EPIC_AreaTypeDef *res_area)
{
    int16_t x0;
    int16_t y0;
    int16_t x1;
    int16_t y1;

    /* calculate the intersect region */
    x0 = HAL_MAX(layer0->x_offset, p0_area->x0);
    y0 = HAL_MAX(layer0->y_offset, p0_area->y0);
    x1 = HAL_MIN(layer0->x_offset + layer0->width - 1, p0_area->x1);
    y1 = HAL_MIN(layer0->y_offset + layer0->height - 1, p0_area->y1);

    if ((x0 <= x1) && (y0 <= y1))
    {
        if (res_area != NULL)
        {
            res_area->x0 = x0;
            res_area->y0 = y0;
            res_area->x1 = x1;
            res_area->y1 = y1;
        }
        return true;
    }
    else
    {
        return false;
    }
}

static void _clip_layer(EPIC_LayerConfigTypeDef *p_layer, const EPIC_AreaTypeDef *clip_area)
{
    //Clip output_layer
    if ((clip_area->x0 >= p_layer->x_offset + p_layer->width)
            || (clip_area->y0 >= p_layer->y_offset + p_layer->height))
        return; //No intersection.

    int16_t x_off = clip_area->x0 - p_layer->x_offset;
    int16_t y_off = clip_area->y0 - p_layer->y_offset;
    uint32_t dst_color_bytes = HAL_EPIC_GetColorDepth(p_layer->color_mode) >> 3;

    p_layer->data     += (y_off * p_layer->total_width + x_off) * dst_color_bytes;
    p_layer->x_offset += x_off;
    p_layer->y_offset += y_off;
    p_layer->width    = clip_area->x1 - clip_area->x0 + 1;
    p_layer->height   = clip_area->y1 - clip_area->y0 + 1;
}

static void overwrite_with_circle(const epic_mw_mask_opa_t *circle_mask, const EPIC_AreaTypeDef *blend_area, const EPIC_AreaTypeDef *circle_area,
                                  epic_mw_mask_opa_t *mask_buf,  int32_t width)
{
    EPIC_AreaTypeDef circle_common_area;
    if (HAL_EPIC_AreaIntersect(&circle_common_area, circle_area, blend_area))
    {
        const epic_mw_mask_opa_t *circle_mask_tmp = circle_mask + width * (circle_common_area.y0 - circle_area->y0);
        circle_mask_tmp += circle_common_area.x0 - circle_area->x0;

        epic_mw_mask_opa_t *mask_buf_tmp = mask_buf + circle_common_area.x0 - blend_area->x0;

        uint32_t x;
        uint32_t w = HAL_EPIC_AreaWidth(&circle_common_area);
        memcpy(mask_buf_tmp, circle_mask_tmp, w);
    }

}

static void add_circle(const epic_mw_mask_opa_t *circle_mask, const EPIC_AreaTypeDef *blend_area, const EPIC_AreaTypeDef *circle_area,
                       epic_mw_mask_opa_t *mask_buf,  int32_t width)
{
    EPIC_AreaTypeDef circle_common_area;
    if (HAL_EPIC_AreaIntersect(&circle_common_area, circle_area, blend_area))
    {
        const epic_mw_mask_opa_t *circle_mask_tmp = circle_mask + width * (circle_common_area.y0 - circle_area->y0);
        circle_mask_tmp += circle_common_area.x0 - circle_area->x0;

        epic_mw_mask_opa_t *mask_buf_tmp = mask_buf + circle_common_area.x0 - blend_area->x0;

        uint32_t x;
        uint32_t w = HAL_EPIC_AreaWidth(&circle_common_area);
        for (x = 0; x < w; x++)
        {
            uint32_t res = mask_buf_tmp[x] + circle_mask_tmp[x];
            mask_buf_tmp[x] = res > 255 ? 255 : res;
        }
    }

}
/* ======== pixel_offset / merge_a2b (epic_mw_dev.c:2347-2437) ======== */

static void pixel_offset(uint32_t offset_bytes, uint32_t line_width, uint32_t color_bytes, int16_t *x_off, int16_t *y_off)
{
    LV_ASSERT(0 == (offset_bytes % color_bytes));
    uint32_t offset_pixels = offset_bytes / color_bytes;

    *y_off = offset_pixels / line_width;
    *x_off = offset_pixels - (line_width * (*y_off));
}

static int16_t merge_a2b(
    const epic_mw_buf_t *p_buf_a,
    const EPIC_LayerConfigTypeDef *p_buf_b,
    int16_t *x_off, //Vector of a->b
    int16_t *y_off,
    uint16_t *new_height)
{
    if (p_buf_a->cf != p_buf_b->color_mode) return 0;
    if (p_buf_a->area.x1 - p_buf_a->area.x0 + 1 != p_buf_b->total_width) return 0;

    uint32_t color_bytes = HAL_EPIC_GetColorDepth(p_buf_b->color_mode) >> 3;
    LV_ASSERT(color_bytes > 0);

    uint32_t p_buf_a_bytes = HAL_EPIC_AreaWidth(&p_buf_a->area) * HAL_EPIC_AreaHeight(&p_buf_a->area) * color_bytes;
    uint32_t p_buf_b_bytes = p_buf_b->total_width * p_buf_b->height * color_bytes;

    if ((p_buf_a->data == p_buf_b->data) && (p_buf_a_bytes == p_buf_b_bytes))
    {
        *x_off = 0;
        *y_off = 0;
        return 1;//Same buffer
    }
    else if (p_buf_a->data <= p_buf_b->data)
    {
        if ((p_buf_a->data + p_buf_a_bytes) >= (p_buf_b->data + p_buf_b_bytes))
        {
            pixel_offset(p_buf_b->data - p_buf_a->data, p_buf_b->total_width, color_bytes, x_off, y_off);

            return 2;//A include B
        }
        else if ((p_buf_a->data + p_buf_a_bytes) >= p_buf_b->data)
        {
            pixel_offset(p_buf_b->data - p_buf_a->data, p_buf_b->total_width, color_bytes, x_off, y_off);

            if (*x_off)
            {
                return 0;//Can't merge to 1 buffer
            }
            else
            {
                *new_height = p_buf_b->height + *y_off;
                return 3;//A&B have intersection
            }
        }
        else
        {
            return 0;//No intersection
        }
    }
    else
    {
        if ((p_buf_b->data + p_buf_b_bytes) >= (p_buf_a->data + p_buf_a_bytes))
        {
            pixel_offset(p_buf_a->data - p_buf_b->data, p_buf_b->total_width, color_bytes, x_off, y_off);
            *y_off = - *y_off;
            *x_off = - *x_off;
            return 4;//B include A
        }
        else if ((p_buf_b->data + p_buf_b_bytes) >= p_buf_a->data)
        {
            pixel_offset(p_buf_a->data - p_buf_b->data, p_buf_b->total_width, color_bytes, x_off, y_off);

            if (*x_off)
            {
                return 0;//Can't merge to 1 buffer
            }
            else
            {
                *new_height = HAL_EPIC_AreaHeight(&p_buf_a->area) + *y_off;
                *y_off = - *y_off;
                *x_off = - *x_off;
                return 5;//B&A have intersection
            }
        }
        else
        {
            return 0; //No intersection
        }
    }


}
/* ======== draw_fill / draw_rect / draw_rect2 / draw_masked_rect(2) (epic_mw_dev.c:2807-3176) ======== */

static void draw_fill(EPIC_LayerConfigTypeDef *dst, EPIC_LayerConfigTypeDef *p_mask_layer,
                      const EPIC_AreaTypeDef *p_fill_area, uint32_t argb8888)
{
    HAL_StatusTypeDef ret;
    EPIC_LayerConfigTypeDef output_layer;
    EPIC_AreaTypeDef com_area;
    uint8_t output_has_alpha = (EPIC_OUTPUT_ARGB8565 == dst->color_mode)
                               || (EPIC_OUTPUT_ARGB8888 == dst->color_mode);

    uint8_t opa = (uint8_t)((argb8888 >> 24) & 0xFF);


    if (!_layer_IntersectArea(dst, p_fill_area, &com_area))
        return;

    if (NULL == p_mask_layer->data) p_mask_layer = NULL; //Disable mask layer if no data

    //Clip output_layer
    memcpy(&output_layer, dst, sizeof(EPIC_LayerConfigTypeDef));
    _clip_layer(&output_layer, &com_area);

    {
        output_layer.color_r = (uint8_t)((argb8888 >> 16) & 0xFF);
        output_layer.color_g = (uint8_t)((argb8888 >> 8) & 0xFF);
        output_layer.color_b = (uint8_t)((argb8888 >> 0) & 0xFF);

        if (p_mask_layer)
        {
            EPIC_LayerConfigTypeDef input_layers[2];

            memcpy(&input_layers[1], p_mask_layer, sizeof(EPIC_LayerConfigTypeDef));

            input_layers[0] = output_layer;
            input_layers[0].data = (uint8_t *) mono_layer_addr;
            input_layers[0].color_mode = EPIC_INPUT_MONO;
            input_layers[0].alpha = opa;
            input_layers[0].ax_mode = ALPHA_BLEND_RGBCOLOR;
            input_layers[0].color_en = true;


            ret =  Call_Hal_Api(HAL_API_BLEND_EX, input_layers, (void *)((uint32_t)2), &output_layer);
        }
#ifndef SF32LB55X
        else if (opa != 255)
        {
            EPIC_LayerConfigTypeDef input_layers[1];

            input_layers[0] = output_layer;
            input_layers[0].data = (uint8_t *) mono_layer_addr;
            input_layers[0].color_mode = EPIC_INPUT_MONO;
            input_layers[0].alpha = opa;
            input_layers[0].ax_mode = ALPHA_BLEND_RGBCOLOR;
            input_layers[0].color_en = true;

            ret =  Call_Hal_Api(HAL_API_BLEND_EX, input_layers, (void *)((uint32_t)1), &output_layer);
        }
#else /* !SF32LB55X */
        else if (opa != 255)
        {
            if (output_has_alpha)
            {
                LV_ASSERT(0 == argb8888);//Only support fill with 0x00000000 on SF32LB55X

                uint32_t dst_color_bytes = HAL_EPIC_GetColorDepth(dst->color_mode) >> 3;
                uint32_t dst_layer_stride = dst->total_width * dst_color_bytes;

                uint16_t output_new_width = output_layer.width * dst_color_bytes / 2;

                //Software fills the bytes that are not 2-bytes aligned
                if (0 != output_layer.width - output_new_width)
                {
                    for (uint32_t y = 0; y < output_layer.height; y++)
                    {
                        uint8_t *p_dst = output_layer.data + y * dst_layer_stride + output_new_width * 2;
                        *p_dst = 0x00;
                    }
                }


                //EPIC fills the bytes that are 2 bytes aligned
                output_layer.color_mode = EPIC_OUTPUT_RGB565;
                output_layer.total_width = dst_layer_stride / 2;
                output_layer.width = output_new_width;

                output_layer.color_en = true;
                ret =  Call_Hal_Api(HAL_API_BLEND_EX, NULL, (void *)0, &output_layer);
            }
            else
            {
                EPIC_LayerConfigTypeDef input_layer = output_layer;

                input_layer.alpha = (0 == opa) ? 255 : (256 - opa);

                output_layer.color_en = true;
                ret =  Call_Hal_Api(HAL_API_BLEND_EX, &input_layer, (void *)1, &output_layer);
            }
        }
#endif /*SF32LB55X */
        else
        {
            output_layer.color_en = true;
            ret =  Call_Hal_Api(HAL_API_BLEND_EX, NULL, (void *)0, &output_layer);
        }
        DRV_EPIC_ASSERT(HAL_OK == ret);

    }
}

static void draw_rect(EPIC_LayerConfigTypeDef *dst, EPIC_LayerConfigTypeDef *p_mask_layer,
                      const EPIC_AreaTypeDef *p_fill_area, uint32_t argb8888)
{
    HAL_StatusTypeDef ret;
    EPIC_LayerConfigTypeDef output_layer;
    EPIC_AreaTypeDef com_area;

    uint8_t opa = (uint8_t)((argb8888 >> 24) & 0xFF);
    if (opa <= EPIC_OPA_MIN) return;

    if (!_layer_IntersectArea(dst, p_fill_area, &com_area))
        return;

    if (NULL == p_mask_layer->data) p_mask_layer = NULL; //Disable mask layer if no data

    //Clip output_layer
    memcpy(&output_layer, dst, sizeof(EPIC_LayerConfigTypeDef));
    _clip_layer(&output_layer, &com_area);

    {


        output_layer.color_r = (uint8_t)((argb8888 >> 16) & 0xFF);
        output_layer.color_g = (uint8_t)((argb8888 >> 8) & 0xFF);
        output_layer.color_b = (uint8_t)((argb8888 >> 0) & 0xFF);

        if (p_mask_layer)
        {
            EPIC_LayerConfigTypeDef input_layers[3];

            memcpy(&input_layers[2], p_mask_layer, sizeof(EPIC_LayerConfigTypeDef));

            input_layers[1] = output_layer;
            input_layers[1].data = (uint8_t *) mono_layer_addr;
            input_layers[1].color_mode = EPIC_INPUT_MONO;
            input_layers[1].alpha = opa;
            input_layers[1].ax_mode = ALPHA_BLEND_RGBCOLOR;
            input_layers[1].color_en = true;


            memcpy(&input_layers[0], dst, sizeof(EPIC_LayerConfigTypeDef));
            ret =  Call_Hal_Api(HAL_API_BLEND_EX, input_layers, (void *)((uint32_t)3), &output_layer);
        }
#ifndef SF32LB55X
        else if (opa != 255)
        {
            EPIC_LayerConfigTypeDef input_layers[2];

            input_layers[1] = output_layer;
            input_layers[1].data = (uint8_t *) mono_layer_addr;
            input_layers[1].color_mode = EPIC_INPUT_MONO;
            input_layers[1].alpha = opa;
            input_layers[1].ax_mode = ALPHA_BLEND_RGBCOLOR;
            input_layers[1].color_en = true;

            memcpy(&input_layers[0], dst, sizeof(EPIC_LayerConfigTypeDef));
            ret =  Call_Hal_Api(HAL_API_BLEND_EX, input_layers, (void *)((uint32_t)2), &output_layer);
        }
#else /* !SF32LB55X */
        else if (opa != 255)
        {

            EPIC_LayerConfigTypeDef input_layer = output_layer;

            input_layer.alpha = (0 == opa) ? 255 : (256 - opa);

            output_layer.color_en = true;
            ret =  Call_Hal_Api(HAL_API_BLEND_EX, &input_layer, (void *)1, &output_layer);
        }
#endif /*SF32LB55X */
        else
        {
            output_layer.color_en = true;
            ret =  Call_Hal_Api(HAL_API_BLEND_EX, NULL, (void *)0, &output_layer);
        }
        DRV_EPIC_ASSERT(HAL_OK == ret);

    }
}

static void draw_rect2(EPIC_LayerConfigTypeDef *dst, const EPIC_AreaTypeDef *p_clip_area,
                              EPIC_LayerConfigTypeDef *p_mask_layer,
                              const EPIC_AreaTypeDef *p_fill_area, uint32_t argb8888)
{
    EPIC_AreaTypeDef com_area;
    if (HAL_EPIC_AreaIntersect(&com_area, p_fill_area, p_clip_area))
        draw_rect(dst, p_mask_layer, &com_area, argb8888);
}

static rt_err_t draw_masked_rect(EPIC_LayerConfigTypeDef *dst, EPIC_LayerConfigTypeDef *p_mask_layer,
                                 void *masks[], const EPIC_AreaTypeDef *p_draw_area, uint32_t argb8888,
                                 const EPIC_AreaTypeDef *p_circle_mask_area1,
                                 const EPIC_AreaTypeDef *p_circle_mask_area2,
                                 epic_mw_mask_opa_t *p_circle_mask_buf,
                                 uint32_t circle_mask_width
                                )
{
    HAL_StatusTypeDef ret;
    EPIC_LayerConfigTypeDef output_layer;
    EPIC_LayerConfigTypeDef fg_layer;
    EPIC_AreaTypeDef com_area;

    if (!_layer_IntersectArea(dst, p_draw_area, &com_area))
        return RT_EOK;

    int32_t blend_h = HAL_EPIC_AreaHeight(&com_area);
    int32_t blend_w = HAL_EPIC_AreaWidth(&com_area);

    if (blend_h <= 0 || blend_w <= 0) return RT_EEMPTY;
    if (NULL == p_mask_layer->data) p_mask_layer = NULL; //Disable mask layer if no data

    //ping-pong buffer for CPU and GPU paralell
    int32_t  mask_buf_h = MIN((epic_mw_dev.dbg_mask_buf_pool_max / 2) / blend_w, blend_h);
    LV_ASSERT(mask_buf_h > 0);
    epic_mw_mask_opa_t *mask_buf = (epic_mw_mask_opa_t *)epic_mw_dev.mask_buf_pool;
    epic_mw_mask_opa_t *mask_buf_ping_pong = mask_buf;

    EPIC_AreaTypeDef mask_area = com_area;
    EPIC_AreaTypeDef fill_area = com_area;



    /*Inital fg layer*/
    HAL_EPIC_LayerConfigInit(&fg_layer);
    fg_layer.alpha = (uint8_t)((argb8888 >> 24) & 0xFF);
    fg_layer.color_en = true;
    fg_layer.color_r = (uint8_t)((argb8888 >> 16) & 0xFF);
    fg_layer.color_g = (uint8_t)((argb8888 >> 8) & 0xFF);
    fg_layer.color_b = (uint8_t)((argb8888 >> 0) & 0xFF);
    fg_layer.color_mode = EPIC_INPUT_A8;
    fg_layer.ax_mode = ALPHA_BLEND_RGBCOLOR;
    fg_layer.width  = blend_w;
    fg_layer.total_width = fg_layer.width;
    fg_layer.x_offset = fill_area.x0;




    /*Inital output layer*/
    memcpy(&output_layer, dst, sizeof(EPIC_LayerConfigTypeDef));
    uint32_t dst_color_bytes = HAL_EPIC_GetColorDepth(dst->color_mode) >> 3;
    output_layer.width    = blend_w;


    /*
        Avoid 'mask_buf_ping_pong' is still using by previous cont_blend,
        befre we overwrite it.
    */
    ret =  Call_Hal_Api(HAL_API_CONT_BLEND_STOP, NULL, NULL, NULL);
    DRV_EPIC_ASSERT(HAL_OK == ret);

    for (int32_t f = 0; f < blend_h;)
    {
        int32_t fill_h = MIN(mask_buf_h, blend_h - f);
        epic_mw_mask_opa_t *mask_buf_sub;
        epic_mw_mask_res_t mask_res = DRAW_MASK_RES_TRANSP;


        mask_area.y0 = fill_area.y0;
        mask_area.y1 = fill_area.y0;

        mask_buf_sub = mask_buf_ping_pong;
        memset(mask_buf_sub, 0xff, blend_w * fill_h);
        for (int32_t h = 0; h < fill_h; h++)
        {
            epic_mw_mask_res_t mask_res_sub = epic_mw_mask_apply(masks, mask_buf_sub, mask_area.x0, mask_area.y0, blend_w);


            if (p_circle_mask_area1 && p_circle_mask_buf)
                if (mask_area.y0 >= p_circle_mask_area1->y0 && mask_area.y0 <= p_circle_mask_area1->y1)
                {
                    if (mask_res_sub == DRAW_MASK_RES_TRANSP)
                    {
                        memset(mask_buf_sub, 0x00, blend_w);
                        overwrite_with_circle(p_circle_mask_buf, &mask_area, p_circle_mask_area1, mask_buf_sub, circle_mask_width);
                        mask_res_sub = DRAW_MASK_RES_CHANGED;
                    }
                    else
                        add_circle(p_circle_mask_buf, &mask_area, p_circle_mask_area1, mask_buf_sub, circle_mask_width);
                }



            if (p_circle_mask_area2 && p_circle_mask_buf)
                if (mask_area.y0 >= p_circle_mask_area2->y0 && mask_area.y0 <= p_circle_mask_area2->y1)
                {
                    if (mask_res_sub == DRAW_MASK_RES_TRANSP)
                    {
                        memset(mask_buf_sub, 0x00, blend_w);
                        overwrite_with_circle(p_circle_mask_buf, &mask_area, p_circle_mask_area2, mask_buf_sub, circle_mask_width);
                        mask_res_sub = DRAW_MASK_RES_CHANGED;
                    }
                    else
                        add_circle(p_circle_mask_buf, &mask_area, p_circle_mask_area2, mask_buf_sub, circle_mask_width);
                }


            if (mask_res_sub != DRAW_MASK_RES_TRANSP)
                mask_res = mask_res_sub;
            else
                memset(mask_buf_sub, 0x00, blend_w);

            mask_area.y0 ++;
            mask_area.y1 ++;
            mask_buf_sub += blend_w;
        }



        fill_area.y1 =  fill_area.y0 + fill_h - 1;
        if (mask_res != DRAW_MASK_RES_TRANSP)
        {
            //setup fg
            fg_layer.data = (uint8_t *)mask_buf_ping_pong;
            fg_layer.height = fill_h;
            fg_layer.y_offset = fill_area.y0;


            //setup output_layer
            {
                int16_t x_off = fill_area.x0 - dst->x_offset;
                int16_t y_off = fill_area.y0 - dst->y_offset;

                output_layer.x_offset = dst->x_offset + x_off;
                output_layer.y_offset = dst->y_offset + y_off;
                output_layer.data     = dst->data + (y_off * dst->total_width + x_off) * dst_color_bytes;
                output_layer.height   = fill_h;
            }

            /* port_plan.md §6.2: mask_buf_ping_pong is CPU-written then
             * GPU-read; the v9 per-layer cache maintenance does not cover it. */
            epic_mw_clean_range(mask_buf_ping_pong, (uint32_t)blend_w * (uint32_t)fill_h);

            ret =  Call_Hal_Api(HAL_API_CONT_BLEND, &fg_layer, p_mask_layer, &output_layer);
            DRV_EPIC_ASSERT(HAL_OK == ret);


            if (mask_buf_ping_pong == mask_buf)
                mask_buf_ping_pong = mask_buf + (blend_w * mask_buf_h);
            else
                mask_buf_ping_pong = mask_buf;
        }
        fill_area.y0 += fill_h;
        fill_area.y1 += fill_h;
        f += fill_h;
    }

    return RT_EOK;
}

static rt_err_t draw_masked_rect2(EPIC_LayerConfigTypeDef *dst, const EPIC_AreaTypeDef *p_clip_area,
        EPIC_LayerConfigTypeDef *p_mask_layer,
        void *masks[], const EPIC_AreaTypeDef *p_draw_area, uint32_t argb8888,
        const EPIC_AreaTypeDef *p_circle_mask_area1,
        const EPIC_AreaTypeDef *p_circle_mask_area2,
        epic_mw_mask_opa_t *p_circle_mask_buf,
        uint32_t circle_mask_width
                                        )
{
    EPIC_AreaTypeDef com_area;
    if (HAL_EPIC_AreaIntersect(&com_area, p_draw_area, p_clip_area))
        return draw_masked_rect(dst, p_mask_layer, masks, &com_area, argb8888,
                                p_circle_mask_area1, p_circle_mask_area2, p_circle_mask_buf, circle_mask_width);

    return RT_EOK;
}
/* ======== arc / rectangle / border / line renderers (epic_mw_dev.c:3178-4030) ======== */

static void get_rounded_area(int16_t angle, int32_t radius, uint8_t thickness, EPIC_AreaTypeDef *res_area)
{
    int32_t thick_half = thickness / 2;
    uint8_t thick_corr = (thickness & 0x01) ? 0 : 1;

    int32_t cir_x;
    int32_t cir_y;


    cir_x = ((radius - thick_half) * EPIC_TrigoCos(angle)) >> (EPIC_TRIGO_SHIFT - 8);
    cir_y = ((radius - thick_half) * EPIC_TrigoSin(angle)) >> (EPIC_TRIGO_SHIFT - 8);

    /*The center of the pixel need to be calculated so apply 1/2 px offset*/
    if (cir_x > 0)
    {
        cir_x = (cir_x - 128) >> 8;
        res_area->x0 = cir_x - thick_half + thick_corr;
        res_area->x1 = cir_x + thick_half;
    }
    else
    {
        cir_x = (cir_x + 128) >> 8;
        res_area->x0 = cir_x - thick_half;
        res_area->x1 = cir_x + thick_half - thick_corr;
    }

    if (cir_y > 0)
    {
        cir_y = (cir_y - 128) >> 8;
        res_area->y0 = cir_y - thick_half + thick_corr;
        res_area->y1 = cir_y + thick_half;
    }
    else
    {
        cir_y = (cir_y + 128) >> 8;
        res_area->y0 = cir_y - thick_half;
        res_area->y1 = cir_y + thick_half - thick_corr;
    }
}


static rt_err_t render_arc(drv_epic_operation *p_operation, EPIC_LayerConfigTypeDef *dst, const EPIC_AreaTypeDef *p_clip_area)
{
    int16_t center_x = p_operation->desc.arc.center_x;
    int16_t center_y = p_operation->desc.arc.center_y;
    uint16_t radius = p_operation->desc.arc.radius;
    uint16_t start_angle = p_operation->desc.arc.start_angle;
    uint16_t end_angle = p_operation->desc.arc.end_angle;
    uint8_t opa = (uint8_t)((p_operation->desc.arc.argb8888 >> 24) & 0xFF);

    if (opa <= EPIC_OPA_MIN) return RT_EOK;
    if (p_operation->desc.arc.width == 0) return RT_EOK;
    if (start_angle == end_angle) return RT_EOK;

    int32_t width = p_operation->desc.arc.width;
    if (width > radius) width = radius;

    EPIC_AreaTypeDef area_out;
    area_out.x0 = center_x - radius;
    area_out.y0 = center_y - radius;
    area_out.x1 = center_x + radius - 1;  /*-1 because the center already belongs to the left/bottom part*/
    area_out.y1 = center_y + radius - 1;



    EPIC_AreaTypeDef clipped_area;
    if (!HAL_EPIC_AreaIntersect(&clipped_area, &area_out, p_clip_area)) return RT_EOK;


    EPIC_AreaTypeDef area_in;
    HAL_EPIC_AreaCopy(&area_in, &area_out);
    area_in.x0 += p_operation->desc.arc.width;
    area_in.y0 += p_operation->desc.arc.width;
    area_in.x1 -= p_operation->desc.arc.width;
    area_in.y1 -= p_operation->desc.arc.width;

    while (start_angle >= 360) start_angle -= 360;
    while (end_angle >= 360) end_angle -= 360;




    //Prepare round mask
    epic_mw_mask_opa_t *circle_mask = NULL;
    EPIC_AreaTypeDef round_area_1;
    EPIC_AreaTypeDef round_area_2;
    if ((p_operation->desc.arc.round_start || p_operation->desc.arc.round_end)
            && (start_angle != end_angle) //Not draw round end points if it is an circle
       )
    {
        LV_ASSERT(width * width <= mask_buf2_max_bytes);
        circle_mask = (epic_mw_mask_opa_t *)epic_mw_dev.mask_buf2_pool;

        /*
            Avoid 'mask_buf2_pool' is still using by previous cont_blend,
            befre we overwrite it.
        */
        HAL_StatusTypeDef ret =  Call_Hal_Api(HAL_API_CONT_BLEND_STOP, NULL, NULL, NULL);
        DRV_EPIC_ASSERT(HAL_OK == ret);

        memset(circle_mask, 0xff, width * width);

        epic_mw_mask_area_t circle_area = {0, 0, width - 1, width - 1};
        epic_mw_mask_radius_param_t circle_mask_param;
        epic_mw_mask_radius_init(&circle_mask_param, &circle_area, width / 2, false);
        void *circle_mask_list[2] = {&circle_mask_param, NULL};


        epic_mw_mask_opa_t *circle_mask_tmp = circle_mask;
        for (int32_t h = 0; h < width; h++)
        {
            epic_mw_mask_res_t res = epic_mw_mask_apply(circle_mask_list, circle_mask_tmp, 0, h, width);
            if (res == DRAW_MASK_RES_TRANSP)
            {
                memset(circle_mask_tmp, 0x00, width);
            }

            circle_mask_tmp += width;
        }

        epic_mw_mask_free_param(&circle_mask_param);

        get_rounded_area(start_angle, radius, width, &round_area_1);
        HAL_EPIC_AreaMove(&round_area_1, center_x, center_y);
        get_rounded_area(end_angle, radius, width, &round_area_2);
        HAL_EPIC_AreaMove(&round_area_2, center_x, center_y);

    }


    void *mask_list[4] = {0, 0, 0, 0};

    int32_t  mask_counts = 0;
    epic_mw_mask_angle_param_t mask_angle_param;
    /*Create an angle mask*/
    if (start_angle != end_angle)
    {
        epic_mw_mask_angle_init(&mask_angle_param, center_x, center_y, start_angle, end_angle);
        mask_list[mask_counts] = &mask_angle_param;
        mask_counts++;
    }

    /*Create an outer mask*/
    epic_mw_mask_radius_param_t mask_out_param;
    epic_mw_mask_radius_init2(&mask_out_param, &area_out, radius_circle, false);
    mask_list[mask_counts] = &mask_out_param;
    mask_counts++;

    /*Create inner the mask*/
    epic_mw_mask_radius_param_t mask_in_param;
    if (HAL_EPIC_AreaWidth(&area_in) > 0 && HAL_EPIC_AreaHeight(&area_in) > 0)
    {
        epic_mw_mask_radius_init2(&mask_in_param, &area_in, radius_circle, true);
        mask_list[mask_counts] = &mask_in_param;
        mask_counts++;
    }


    if (p_operation->mask.data) render_lock(p_operation->op, 0, 0, (uint32_t)(p_operation->mask.data));


    draw_masked_rect(dst, &p_operation->mask,
                     mask_list, &clipped_area, p_operation->desc.arc.argb8888,
                     &round_area_1, &round_area_2,
                     circle_mask,
                     width
                    );


    if (p_operation->mask.data) render_unlock();

    for (int32_t i = 0; i < mask_counts; i++) epic_mw_mask_free_param(mask_list[i]);

    return RT_EOK;
}

static rt_err_t render_rectangle(drv_epic_operation *p_operation, EPIC_LayerConfigTypeDef *dst, const EPIC_AreaTypeDef *p_clip_area)
{
    const int32_t SPLIT_LIMIT = 100;
    EPIC_AreaTypeDef *p_rect_area = &p_operation->desc.rectangle.area;
    uint32_t color = p_operation->desc.rectangle.argb8888;
    int32_t coords_w = HAL_EPIC_AreaWidth(p_rect_area);
    int32_t coords_h = HAL_EPIC_AreaHeight(p_rect_area);
    EPIC_AreaTypeDef draw_area;

    if (!HAL_EPIC_AreaIntersect(&draw_area, p_rect_area, p_clip_area))  return RT_EOK;

    /*Get the real radius*/
    int32_t rout = p_operation->desc.rectangle.radius;
    int32_t short_side = MIN(coords_w, coords_h);
    if (rout > short_side >> 1) rout = short_side >> 1;

    if (rout <= 0)
    {
        draw_rect(dst, &p_operation->mask, &draw_area, color);
    }
    else
    {
        void *mask_list[2] = {0, 0};
        int top_fillet = p_operation->desc.rectangle.top_fillet;
        int bot_fillet = p_operation->desc.rectangle.bot_fillet;

        epic_mw_mask_radius_param_t mask_out_param;

        epic_mw_mask_radius_init2(&mask_out_param, p_rect_area, rout, false);
        mask_list[0] = &mask_out_param;

        uint8_t split = 0;

        if ((coords_w > coords_h) && (coords_w - 2 * rout > SPLIT_LIMIT))
            split = 1;
        else if ((coords_w <= coords_h) && (coords_h - 2 * rout > SPLIT_LIMIT))
            split = 2;

        EPIC_AreaTypeDef fill_area;
        fill_area.x0 = p_rect_area->x0;
        fill_area.x1 = p_rect_area->x1;

        if (top_fillet && !bot_fillet)
        {
            //Draw round rect area
            fill_area.y0 = p_rect_area->y0;
            fill_area.y1 = p_rect_area->y0 + rout;
            draw_masked_rect2(dst, p_clip_area, &p_operation->mask,
                              mask_list, &fill_area, color,
                              NULL, NULL, NULL, 0);


            //Draw NONE round rect area
            fill_area.y0 = p_rect_area->y0 + rout + 1;
            fill_area.y1 = p_rect_area->y1;
            draw_rect2(dst, p_clip_area, &p_operation->mask, &fill_area, color);

        }
        else if (!top_fillet && bot_fillet)
        {
            //Draw round rect area
            fill_area.y0 = p_rect_area->y1 - rout;
            fill_area.y1 = p_rect_area->y1;
            draw_masked_rect2(dst, p_clip_area, &p_operation->mask,
                              mask_list, &fill_area, color,
                              NULL, NULL, NULL, 0);



            //Draw NONE round rect area
            fill_area.y0 = p_rect_area->y0;
            fill_area.y1 = p_rect_area->y1 - rout - 1;
            draw_rect2(dst, p_clip_area, &p_operation->mask, &fill_area, color);
        }
        else if (2 == split)
        {
            //Draw round rect area
            fill_area.y0 = p_rect_area->y0;
            fill_area.y1 = p_rect_area->y0 + rout;
            //Top
            draw_masked_rect2(dst, p_clip_area, &p_operation->mask,
                              mask_list, &fill_area, color,
                              NULL, NULL, NULL, 0);

            fill_area.y0 = p_rect_area->y1 - rout;
            fill_area.y1 = p_rect_area->y1;
            //Bottom
            draw_masked_rect2(dst, p_clip_area, &p_operation->mask,
                              mask_list, &fill_area, color,
                              NULL, NULL, NULL, 0);




            //Draw NONE round rect area
            fill_area.y0 = p_rect_area->y0 + rout + 1;
            fill_area.y1 = p_rect_area->y1 - rout - 1;
            draw_rect2(dst, p_clip_area, &p_operation->mask, &fill_area, color);
        }
        else if (1 == split)
        {
            fill_area.y0 = p_rect_area->y0;
            fill_area.y1 = p_rect_area->y1;

            //Draw round rect area
            fill_area.x0 = p_rect_area->x0;
            fill_area.x1 = p_rect_area->x0 + rout - 1;
            //Left
            draw_masked_rect2(dst, p_clip_area, &p_operation->mask,
                              mask_list, &fill_area, color,
                              NULL, NULL, NULL, 0);


            fill_area.x0 = p_rect_area->x1 - rout + 1;
            fill_area.x1 = p_rect_area->x1;
            //Right
            draw_masked_rect2(dst, p_clip_area, &p_operation->mask,
                              mask_list, &fill_area, color,
                              NULL, NULL, NULL, 0);




            //Draw NONE round rect area
            fill_area.x0 = p_rect_area->x0 + rout;
            fill_area.x1 = p_rect_area->x1 - rout;
            draw_rect2(dst, p_clip_area, &p_operation->mask, &fill_area, color);
        }
        else
        {
            fill_area.y0 = p_rect_area->y0;
            fill_area.y1 = p_rect_area->y1;
            draw_masked_rect2(dst, p_clip_area, &p_operation->mask,
                              mask_list, &fill_area, color,
                              NULL, NULL, NULL, 0);
        }

        epic_mw_mask_free_param(&mask_out_param);
    }
    return RT_EOK;
}

static rt_err_t render_border_complex(drv_epic_operation *p_operation, EPIC_LayerConfigTypeDef *dst,  const EPIC_AreaTypeDef *p_clip_area,
                                      const EPIC_AreaTypeDef *outer_area, const EPIC_AreaTypeDef *inner_area,
                                      int32_t rout, int32_t rin, uint32_t argb8888)
{
    const int32_t SPLIT_LIMIT = 100;
    /*Get clipped draw area which is the real draw area.
     *It is always the same or inside `coords`*/
    EPIC_AreaTypeDef draw_area;
    if (!HAL_EPIC_AreaIntersect(&draw_area, outer_area, p_clip_area)) return RT_EOK;
    int32_t draw_area_w = HAL_EPIC_AreaWidth(&draw_area);



    EPIC_AreaTypeDef blend_area;


    /*Calculate the x and y coordinates where the straight parts area is*/
    EPIC_AreaTypeDef core_area;
    core_area.x0 = MAX(outer_area->x0 + rout, inner_area->x0);
    core_area.x1 = MIN(outer_area->x1 - rout, inner_area->x1);
    core_area.y0 = MAX(outer_area->y0 + rout, inner_area->y0);
    core_area.y1 = MIN(outer_area->y1 - rout, inner_area->y1);
    int32_t core_w = HAL_EPIC_AreaWidth(&core_area);

    bool top_side = outer_area->y0 <= inner_area->y0;
    bool bottom_side = outer_area->y1 >= inner_area->y1;

    /*No masks*/
    bool left_side = outer_area->x0 <= inner_area->x0;
    bool right_side = outer_area->x1 >= inner_area->x1;

    bool split_hor = true;
    if (left_side && right_side && top_side && bottom_side &&
            core_w < SPLIT_LIMIT)
    {
        split_hor = false;
    }

    epic_mw_mask_res_t blend_dsc_mask_res = DRAW_MASK_RES_FULL_COVER;
    /*Draw the straight lines first if they are long enough*/
    if (top_side && split_hor)
    {
        blend_area.x0 = core_area.x0;
        blend_area.x1 = core_area.x1;
        blend_area.y0 = outer_area->y0;
        blend_area.y1 = inner_area->y0 - 1;
        draw_rect2(dst, p_clip_area, &p_operation->mask, &blend_area, argb8888);
    }

    if (bottom_side && split_hor)
    {
        blend_area.x0 = core_area.x0;
        blend_area.x1 = core_area.x1;
        blend_area.y0 = inner_area->y1 + 1;
        blend_area.y1 = outer_area->y1;
        draw_rect2(dst, p_clip_area, &p_operation->mask, &blend_area, argb8888);
    }

    /*If the border is very thick and the vertical sides overlap horizontally draw a single rectangle*/
    if (inner_area->x0 >= inner_area->x1 && left_side && right_side)
    {
        blend_area.x0 = outer_area->x0;
        blend_area.x1 = outer_area->x1;
        blend_area.y0 = core_area.y0;
        blend_area.y1 = core_area.y1;
        draw_rect2(dst, p_clip_area, &p_operation->mask, &blend_area, argb8888);
    }
    else
    {
        if (left_side)
        {
            blend_area.x0 = outer_area->x0;
            blend_area.x1 = inner_area->x0 - 1;
            blend_area.y0 = core_area.y0;
            blend_area.y1 = core_area.y1;
            draw_rect2(dst, p_clip_area, &p_operation->mask, &blend_area, argb8888);
        }

        if (right_side)
        {
            blend_area.x0 = inner_area->x1 + 1;
            blend_area.x1 = outer_area->x1;
            blend_area.y0 = core_area.y0;
            blend_area.y1 = core_area.y1;
            draw_rect2(dst, p_clip_area, &p_operation->mask, &blend_area, argb8888);
        }
    }

    /*Draw the corners*/
    int32_t blend_w;
    void *mask_list[3] = {0, 0, 0};

    /*Create mask for the inner mask*/
    epic_mw_mask_radius_param_t mask_rin_param;
    epic_mw_mask_radius_init2(&mask_rin_param, inner_area, rin, true);
    mask_list[0] = &mask_rin_param;

    /*Create mask for the outer area*/
    epic_mw_mask_radius_param_t mask_rout_param;
    if (rout > 0)
    {
        epic_mw_mask_radius_init2(&mask_rout_param, outer_area, rout, false);
        mask_list[1] = &mask_rout_param;
    }

    /*Left and right corner together if they are close to each other*/
    if (!split_hor)
    {
        /*Calculate the top corner and mirror it to the bottom*/
        blend_area.x0 = draw_area.x0;
        blend_area.x1 = draw_area.x1;

        /*Top Left&Right corner*/
        blend_area.y0 = draw_area.y0;
        blend_area.y1 = core_area.y0 - 1;
        draw_masked_rect2(dst, p_clip_area, &p_operation->mask,
                          mask_list, &blend_area, argb8888,
                          NULL, NULL, NULL, 0);

        /*Bottom left&right corner*/
        blend_area.y0 = core_area.y1 + 1;
        blend_area.y1 = draw_area.y1;
        draw_masked_rect2(dst, p_clip_area, &p_operation->mask,
                          mask_list, &blend_area, argb8888,
                          NULL, NULL, NULL, 0);
    }
    else
    {
        /*Left corners*/
        blend_area.x0 = draw_area.x0;
        blend_area.x1 = MIN(draw_area.x1, core_area.x0 - 1);
        blend_w = HAL_EPIC_AreaWidth(&blend_area);
        if (blend_w > 0)
        {
            if (left_side || top_side)
            {
                blend_area.y0 = draw_area.y0;
                blend_area.y1 = core_area.y0 - 1;
                draw_masked_rect2(dst, p_clip_area, &p_operation->mask,
                                  mask_list, &blend_area, argb8888,
                                  NULL, NULL, NULL, 0);
            }

            if (left_side || bottom_side)
            {
                blend_area.y0 = core_area.y1 + 1;
                blend_area.y1 = draw_area.y1;
                draw_masked_rect2(dst, p_clip_area, &p_operation->mask,
                                  mask_list, &blend_area, argb8888,
                                  NULL, NULL, NULL, 0);
            }
        }

        /*Right corners*/
        blend_area.x0 = MAX(draw_area.x0, blend_area.x1 + 1);    /*To not overlap with the left side*/
        blend_area.x0 = MAX(draw_area.x0, core_area.x1 + 1);

        blend_area.x1 = draw_area.x1;
        blend_w = HAL_EPIC_AreaWidth(&blend_area);

        if (blend_w > 0)
        {
            if (right_side || top_side)
            {
                blend_area.y0 = draw_area.y0;
                blend_area.y1 = core_area.y0 - 1;
                draw_masked_rect2(dst, p_clip_area, &p_operation->mask,
                                  mask_list, &blend_area, argb8888,
                                  NULL, NULL, NULL, 0);
            }

            if (right_side || bottom_side)
            {
                blend_area.y0 = core_area.y1 + 1;
                blend_area.y1 = draw_area.y1;
                draw_masked_rect2(dst, p_clip_area, &p_operation->mask,
                                  mask_list, &blend_area, argb8888,
                                  NULL, NULL, NULL, 0);
            }
        }
    }

    epic_mw_mask_free_param(&mask_rin_param);
    if (rout > 0) epic_mw_mask_free_param(&mask_rout_param);

    return RT_EOK;
}

static rt_err_t render_border_simple(drv_epic_operation *p_operation, EPIC_LayerConfigTypeDef *dst,  const EPIC_AreaTypeDef *p_clip_area,
                                     const EPIC_AreaTypeDef *outer_area, const EPIC_AreaTypeDef *inner_area,
                                     uint32_t argb8888)
{
    EPIC_AreaTypeDef a;

    bool top_side = outer_area->y0 <= inner_area->y0;
    bool bottom_side = outer_area->y1 >= inner_area->y1;
    bool left_side = outer_area->x0 <= inner_area->x0;
    bool right_side = outer_area->x1 >= inner_area->x1;

    /*Top*/
    a.x0 = outer_area->x0;
    a.x1 = outer_area->x1;
    a.y0 = outer_area->y0;
    a.y1 = inner_area->y0 - 1;
    if (top_side)
    {
        draw_rect2(dst, p_clip_area, &p_operation->mask, &a, argb8888);
    }

    /*Bottom*/
    a.y0 = inner_area->y1 + 1;
    a.y1 = outer_area->y1;
    if (bottom_side)
    {
        draw_rect2(dst, p_clip_area, &p_operation->mask, &a, argb8888);
    }

    /*Left*/
    a.x0 = outer_area->x0;
    a.x1 = inner_area->x0 - 1;
    a.y0 = (top_side) ? inner_area->y0 : outer_area->y0;
    a.y1 = (bottom_side) ? inner_area->y1 : outer_area->y1;
    if (left_side)
    {
        draw_rect2(dst, p_clip_area, &p_operation->mask, &a, argb8888);
    }

    /*Right*/
    a.x0 = inner_area->x1 + 1;
    a.x1 = outer_area->x1;
    if (right_side)
    {
        draw_rect2(dst, p_clip_area, &p_operation->mask, &a, argb8888);
    }

    return RT_EOK;
}

static rt_err_t render_border(drv_epic_operation *p_operation, EPIC_LayerConfigTypeDef *dst, const EPIC_AreaTypeDef *p_clip_area)
{
    uint8_t opa = (p_operation->desc.border.argb8888 >> 24) & 0xFF;
    if (p_operation->desc.border.width == 0) return RT_EOK;
    if (opa <= EPIC_OPA_MIN) return RT_EOK;
    if ((0 == p_operation->desc.border.top_side)
            && (0 == p_operation->desc.border.bot_side)
            && (0 == p_operation->desc.border.left_side)
            && (0 == p_operation->desc.border.right_side))
        return RT_EOK;

    int32_t border_w = p_operation->desc.border.width;
    int32_t coords_w = HAL_EPIC_AreaWidth(&p_operation->desc.border.area);
    int32_t coords_h = HAL_EPIC_AreaHeight(&p_operation->desc.border.area);
    int32_t rout = p_operation->desc.border.radius;
    int32_t short_side = MIN(coords_w, coords_h);
    if (rout > short_side >> 1) rout = short_side >> 1;

    /*Get the inner area*/
    EPIC_AreaTypeDef area_inner = p_operation->desc.border.area;
    area_inner.x0 += ((p_operation->desc.border.left_side) ? border_w : - (border_w + rout));
    area_inner.x1 -= ((p_operation->desc.border.right_side) ? border_w : - (border_w + rout));
    area_inner.y0 += ((p_operation->desc.border.top_side) ? border_w : - (border_w + rout));
    area_inner.y1 -= ((p_operation->desc.border.bot_side) ? border_w : - (border_w + rout));

    int32_t rin = rout - border_w;
    if (rin < 0) rin = 0;

    if (rout == 0 && rin == 0)
    {
        render_border_simple(p_operation, dst, p_clip_area, &p_operation->desc.border.area, &area_inner, p_operation->desc.border.argb8888);
    }
    else
    {
        render_border_complex(p_operation, dst, p_clip_area, &p_operation->desc.border.area, &area_inner, rout, rin, p_operation->desc.border.argb8888);
    }

    return RT_EOK;
}



static rt_err_t render_line_hor(drv_epic_operation *p_operation, EPIC_LayerConfigTypeDef *dst, const EPIC_AreaTypeDef *p_clip_area)
{
    int32_t w = p_operation->desc.line.width - 1;
    int32_t w_half0 = w >> 1;
    int32_t w_half1 = w_half0 + (w & 0x1); /*Compensate rounding error*/

    EPIC_AreaTypeDef blend_area;
    blend_area.x0 = (int32_t)MIN(p_operation->desc.line.p1.x, p_operation->desc.line.p2.x);
    blend_area.x1 = (int32_t)MAX(p_operation->desc.line.p1.x, p_operation->desc.line.p2.x)  - 1;
    blend_area.y0 = (int32_t)p_operation->desc.line.p1.y - w_half1;
    blend_area.y1 = (int32_t)p_operation->desc.line.p1.y + w_half0;

    bool is_common;
    is_common = HAL_EPIC_AreaIntersect(&blend_area, &blend_area, p_clip_area);
    if (!is_common) return RT_EOK;

    bool dashed = p_operation->desc.line.dash_gap && p_operation->desc.line.dash_width;

    /*If there is no mask then simply draw a rectangle*/
    if (!dashed)
    {
        draw_rect(dst, &p_operation->mask, &blend_area, p_operation->desc.line.argb8888);
    }
    else
    {
        return RT_ERROR;//Dash line not supported now
    }
    return RT_EOK;
}

static rt_err_t render_line_ver(drv_epic_operation *p_operation, EPIC_LayerConfigTypeDef *dst, const EPIC_AreaTypeDef *p_clip_area)
{
    int32_t w = p_operation->desc.line.width - 1;
    int32_t w_half0 = w >> 1;
    int32_t w_half1 = w_half0 + (w & 0x1); /*Compensate rounding error*/

    EPIC_AreaTypeDef blend_area;
    blend_area.x0 = (int32_t)p_operation->desc.line.p1.x - w_half1;
    blend_area.x1 = (int32_t)p_operation->desc.line.p1.x + w_half0;
    blend_area.y0 = (int32_t)MIN(p_operation->desc.line.p1.y, p_operation->desc.line.p2.y);
    blend_area.y1 = (int32_t)MAX(p_operation->desc.line.p1.y, p_operation->desc.line.p2.y) - 1;

    bool is_common;
    is_common = HAL_EPIC_AreaIntersect(&blend_area, &blend_area, p_clip_area);
    if (!is_common) return RT_EOK;

    bool dashed = p_operation->desc.line.dash_gap && p_operation->desc.line.dash_width;

    /*If there is no mask then simply draw a rectangle*/
    if (!dashed)
    {
        draw_rect(dst, &p_operation->mask, &blend_area, p_operation->desc.line.argb8888);
    }
    else
    {
        return RT_ERROR;//Dash line not supported now
    }

    return RT_EOK;
}

static rt_err_t render_line_skew(drv_epic_operation *p_operation, EPIC_LayerConfigTypeDef *dst, const EPIC_AreaTypeDef *p_clip_area)
{
    /*Keep the great y in p1*/
    EPIC_PointTypeDef p1;
    EPIC_PointTypeDef p2;

    if (p_operation->desc.line.p1.y < p_operation->desc.line.p2.y)
    {
        p1 = p_operation->desc.line.p1;
        p2 = p_operation->desc.line.p2;
    }
    else
    {
        p1 = p_operation->desc.line.p2;
        p2 = p_operation->desc.line.p1;
    }

    int32_t xdiff = p2.x - p1.x;
    int32_t ydiff = p2.y - p1.y;
    bool flat = ABS(xdiff) > ABS(ydiff);

    static const uint8_t wcorr[] =
    {
        128, 128, 128, 129, 129, 130, 130, 131,
        132, 133, 134, 135, 137, 138, 140, 141,
        143, 145, 147, 149, 151, 153, 155, 158,
        160, 162, 165, 167, 170, 173, 175, 178,
        181,
    };

    int32_t w = p_operation->desc.line.width;
    int32_t wcorr_i = 0;
    if (flat) wcorr_i = (ABS(ydiff) << 5) / ABS(xdiff);
    else wcorr_i = (ABS(xdiff) << 5) / ABS(ydiff);

    w = (w * wcorr[wcorr_i] + 63) >> 7;     /*+ 63 for rounding*/
    int32_t w_half0 = w >> 1;
    int32_t w_half1 = w_half0 + (w & 0x1); /*Compensate rounding error*/

    EPIC_AreaTypeDef blend_area;
    blend_area.x0 = MIN(p1.x, p2.x) - w;
    blend_area.x1 = MAX(p1.x, p2.x) + w;
    blend_area.y0 = MIN(p1.y, p2.y) - w;
    blend_area.y1 = MAX(p1.y, p2.y) + w;

    /*Get the union of `coords` and `clip`*/
    /*`clip` is already truncated to the `draw_buf` size
     *in 'lv_refr_area' function*/
    bool is_common = HAL_EPIC_AreaIntersect(&blend_area, &blend_area, p_clip_area);
    if (is_common == false) return RT_EOK;

    epic_mw_mask_line_param_t mask_left_param;
    epic_mw_mask_line_param_t mask_right_param;
    epic_mw_mask_line_param_t mask_top_param;
    epic_mw_mask_line_param_t mask_bottom_param;

    void *masks[5] = {&mask_left_param, & mask_right_param, NULL, NULL, NULL};

    if (flat)
    {
        if (xdiff > 0)
        {
            epic_mw_mask_line_points_init(&mask_left_param, p1.x, p1.y - w_half0, p2.x, p2.y - w_half0,
                                           DRAW_MASK_LINE_SIDE_LEFT);
            epic_mw_mask_line_points_init(&mask_right_param, p1.x, p1.y + w_half1, p2.x, p2.y + w_half1,
                                           DRAW_MASK_LINE_SIDE_RIGHT);
        }
        else
        {
            epic_mw_mask_line_points_init(&mask_left_param, p1.x, p1.y + w_half1, p2.x, p2.y + w_half1,
                                           DRAW_MASK_LINE_SIDE_LEFT);
            epic_mw_mask_line_points_init(&mask_right_param, p1.x, p1.y - w_half0, p2.x, p2.y - w_half0,
                                           DRAW_MASK_LINE_SIDE_RIGHT);
        }
    }
    else
    {
        epic_mw_mask_line_points_init(&mask_left_param, p1.x + w_half1, p1.y, p2.x + w_half1, p2.y,
                                       DRAW_MASK_LINE_SIDE_LEFT);
        epic_mw_mask_line_points_init(&mask_right_param, p1.x - w_half0, p1.y, p2.x - w_half0, p2.y,
                                       DRAW_MASK_LINE_SIDE_RIGHT);

    }

    /*Use the normal vector for the endings*/

    if (!p_operation->desc.line.raw_end)
    {
        epic_mw_mask_line_points_init(&mask_top_param, p1.x, p1.y, p1.x - ydiff, p1.y + xdiff,
                                       DRAW_MASK_LINE_SIDE_BOTTOM);
        epic_mw_mask_line_points_init(&mask_bottom_param, p2.x, p2.y, p2.x - ydiff, p2.y + xdiff,
                                       DRAW_MASK_LINE_SIDE_TOP);
        masks[2] = &mask_top_param;
        masks[3] = &mask_bottom_param;
    }


    if (p_operation->mask.data) render_lock(p_operation->op, 0, 0, (uint32_t)(p_operation->mask.data));


    //draw_masked_rect
    draw_masked_rect(dst, &p_operation->mask,
                     masks, &blend_area, p_operation->desc.line.argb8888,
                     NULL, NULL, NULL, 0);


    if (p_operation->mask.data) render_unlock();


    epic_mw_mask_free_param(&mask_left_param);
    epic_mw_mask_free_param(&mask_right_param);
    if (!p_operation->desc.line.raw_end)
    {
        epic_mw_mask_free_param(&mask_top_param);
        epic_mw_mask_free_param(&mask_bottom_param);
    }

    return RT_EOK;
}

static rt_err_t render_line(drv_epic_operation *p_operation, EPIC_LayerConfigTypeDef *dst, const EPIC_AreaTypeDef *p_clip_area)
{
    uint8_t opa = (p_operation->desc.line.argb8888 >> 24) & 0xFF;
    if (p_operation->desc.line.width == 0) return RT_EOK;
    if (opa <= EPIC_OPA_MIN) return RT_EOK;

    if (p_operation->desc.line.p1.x == p_operation->desc.line.p2.x && p_operation->desc.line.p1.y == p_operation->desc.line.p2.y) return RT_EOK;

    EPIC_AreaTypeDef clip_line;
    clip_line.x0 = (int32_t)MIN(p_operation->desc.line.p1.x, p_operation->desc.line.p2.x) - p_operation->desc.line.width / 2;
    clip_line.x1 = (int32_t)MAX(p_operation->desc.line.p1.x, p_operation->desc.line.p2.x) + p_operation->desc.line.width / 2;
    clip_line.y0 = (int32_t)MIN(p_operation->desc.line.p1.y, p_operation->desc.line.p2.y) - p_operation->desc.line.width / 2;
    clip_line.y1 = (int32_t)MAX(p_operation->desc.line.p1.y, p_operation->desc.line.p2.y) + p_operation->desc.line.width / 2;

    bool is_common;
    is_common = HAL_EPIC_AreaIntersect(&clip_line, &clip_line, p_clip_area);
    if (!is_common) return RT_EOK;

    if (p_operation->desc.line.p1.y == p_operation->desc.line.p2.y) render_line_hor(p_operation, dst, p_clip_area);
    else if (p_operation->desc.line.p1.x == p_operation->desc.line.p2.x) render_line_ver(p_operation, dst, p_clip_area);
    else render_line_skew(p_operation, dst, p_clip_area);

    if (p_operation->desc.line.round_end || p_operation->desc.line.round_start)
    {
        int32_t r = (p_operation->desc.line.width >> 1);
        int32_t r_corr = (p_operation->desc.line.width & 1) ? 0 : 1;
        EPIC_AreaTypeDef cir_area;

        if (p_operation->desc.line.round_start)
        {
            cir_area.x0 = (int32_t)p_operation->desc.line.p1.x - r;
            cir_area.y0 = (int32_t)p_operation->desc.line.p1.y - r;
            cir_area.x1 = (int32_t)p_operation->desc.line.p1.x + r - r_corr;
            cir_area.y1 = (int32_t)p_operation->desc.line.p1.y + r - r_corr ;


            void *mask_list[2] = {0};
            /*Create an outer mask*/
            epic_mw_mask_radius_param_t mask_out_param;
            epic_mw_mask_radius_init2(&mask_out_param, &cir_area, r, false);
            mask_list[0] = &mask_out_param;
            //draw_masked_rect
            draw_masked_rect(dst, NULL,
                             mask_list, &clip_line, p_operation->desc.line.argb8888,
                             NULL, NULL, NULL, 0);
            epic_mw_mask_free_param(&mask_out_param);

        }

        if (p_operation->desc.line.round_end)
        {
            cir_area.x0 = (int32_t)p_operation->desc.line.p2.x - r;
            cir_area.y0 = (int32_t)p_operation->desc.line.p2.y - r;
            cir_area.x1 = (int32_t)p_operation->desc.line.p2.x + r - r_corr;
            cir_area.y1 = (int32_t)p_operation->desc.line.p2.y + r - r_corr ;



            void *mask_list[2] = {0};
            /*Create an outer mask*/
            epic_mw_mask_radius_param_t mask_out_param;
            epic_mw_mask_radius_init2(&mask_out_param, &cir_area, r, false);
            mask_list[0] = &mask_out_param;
            //draw_masked_rect
            draw_masked_rect(dst, NULL,
                             mask_list, &clip_line, p_operation->desc.line.argb8888,
                             NULL, NULL, NULL, 0);
            epic_mw_mask_free_param(&mask_out_param);

        }
    }

    return RT_EOK;
}
/* ======== render_fill (epic_mw_dev.c:4161-4168) ======== */

/* drv_epic.c:4107-4155 */
static void render_image(drv_epic_operation *p_operation, EPIC_LayerConfigTypeDef *dst,
                         const EPIC_AreaTypeDef *p_clip_area)
{
    HAL_StatusTypeDef ret;
    EPIC_LayerConfigTypeDef output_layer;

    /*Clip output_layer*/
    memcpy(&output_layer, dst, sizeof(EPIC_LayerConfigTypeDef));
    _clip_layer(&output_layer, p_clip_area);

    {
        EPIC_LayerConfigTypeDef input_layers[3];
        HAL_API_TypeDef api;

        uint32_t input_layer_cnt = 2;

        memcpy(&input_layers[0], dst, sizeof(EPIC_LayerConfigTypeDef));
        memcpy(&input_layers[1], &p_operation->desc.blend.layer, sizeof(EPIC_LayerConfigTypeDef));
        if(p_operation->mask.data) {
            memcpy(&input_layers[2], &p_operation->mask, sizeof(EPIC_LayerConfigTypeDef));
            input_layer_cnt++;
        }

        if(p_operation->desc.blend.layer.transform_cfg.type != 0) {
            /* HAL_API_TRANSFORM (HAL_EPIC_Adv) is not linkable on openvela
             * (epic_mw.c header, plan §3.3).  The v9 adapter only submits
             * non-transformed images here. */
            LV_ASSERT(EPIC_BLEND_MODE_NORMAL == p_operation->desc.blend.use_dest_as_bg);
            LV_ASSERT(0);
            return;
        }
        else {
            api = HAL_API_BLEND_EX;
        }

        if(EPIC_BLEND_MODE_NORMAL != p_operation->desc.blend.use_dest_as_bg) {
            output_layer.color_en = (EPIC_BLEND_MODE_FIXED_BG == p_operation->desc.blend.use_dest_as_bg);
            output_layer.color_r  = p_operation->desc.blend.r;
            output_layer.color_g  = p_operation->desc.blend.g;
            output_layer.color_b  = p_operation->desc.blend.b;
            ret = Call_Hal_Api(api, &input_layers[1], (void *)(input_layer_cnt - 1), &output_layer);
        }
        else {
            ret = Call_Hal_Api(api, input_layers, (void *)input_layer_cnt, &output_layer);
        }
        DRV_EPIC_ASSERT(HAL_OK == ret);
    }
}

static void render_fill(drv_epic_operation *p_operation, EPIC_LayerConfigTypeDef *dst, const EPIC_AreaTypeDef *p_clip_area)
{
    uint32_t argb8888 = (((uint32_t)p_operation->desc.fill.opa) << 24)
                        | (((uint32_t)p_operation->desc.fill.r) << 16)
                        | (((uint32_t)p_operation->desc.fill.g) << 8)
                        | (((uint32_t)p_operation->desc.fill.b) << 0);
    draw_fill(dst, &p_operation->mask, p_clip_area, argb8888);
}
/* ======== AreaExcept (epic_mw_dev.c:4825-4867) ======== */

static bool AreaExcept(EPIC_AreaTypeDef *res_p, const EPIC_AreaTypeDef *aex_p, const EPIC_AreaTypeDef *aholder_p)
{
    if ((aholder_p->x0 == aex_p->x0) && (aholder_p->x1 == aex_p->x1))
    {
        if ((aholder_p->y0 == aex_p->y0) && (aholder_p->y1 > aex_p->y1))
        {
            res_p->x0 = aholder_p->x0;
            res_p->x1 = aholder_p->x1;
            res_p->y0 = aex_p->y1 + 1;
            res_p->y1 = aholder_p->y1;
            return true;
        }
        else if ((aholder_p->y0 < aex_p->y0) && (aholder_p->y1 == aex_p->y1))
        {
            res_p->x0 = aholder_p->x0;
            res_p->x1 = aholder_p->x1;
            res_p->y0 = aholder_p->y0;
            res_p->y1 = aex_p->y0 - 1;
            return true;
        }
    }
    else if ((aholder_p->y0 == aex_p->y0) && (aholder_p->y1 == aex_p->y1))
    {
        if ((aholder_p->x0 == aex_p->x0) && (aholder_p->x1 > aex_p->x1))
        {
            res_p->x0 = aex_p->x1 + 1;
            res_p->x1 = aholder_p->x1;
            res_p->y0 = aholder_p->y0;
            res_p->y1 = aholder_p->y1;
            return true;
        }
        else if ((aholder_p->x0 < aex_p->x0) && (aholder_p->x1 == aex_p->x1))
        {
            res_p->x0 = aholder_p->x0;
            res_p->x1 = aex_p->x0 - 1;
            res_p->y0 = aholder_p->y0;
            res_p->y1 = aholder_p->y1;
            return true;
        }
    }

    return false;
}
/* ======== operation_name / print_operation / print_rl (epic_mw_dev.c:2061-2200) ======== */

static char *operation_name(drv_epic_op_type_t op)
{
#define OP_TO_NAME_CASE(op) case op: return #op
    switch (op)
    {
        OP_TO_NAME_CASE(DRV_EPIC_DRAW_IMAGE);
        OP_TO_NAME_CASE(DRV_EPIC_DRAW_FILL);
        OP_TO_NAME_CASE(DRV_EPIC_DRAW_LETTERS);
        OP_TO_NAME_CASE(DRV_EPIC_DRAW_ARC);
        OP_TO_NAME_CASE(DRV_EPIC_DRAW_RECT);
        OP_TO_NAME_CASE(DRV_EPIC_DRAW_LINE);
        OP_TO_NAME_CASE(DRV_EPIC_DRAW_BORDER);

    default:
        return "UNKNOW";
    }
}


static void print_operation(const char *name, const drv_epic_operation *op)
{


    LOG_E("<<%s, %s>>", name, operation_name(op->op));
    LOG_E("Clip area "AreaString, AreaParams(&op->clip_area));
    if (op->mask.data)
    {
        LOG_E(FORMATED_LAYER_INFO(&op->mask, "Mask"));
    }

    switch (op->op)
    {
    case DRV_EPIC_DRAW_IMAGE:
    {
        LOG_E(FORMATED_LAYER_INFO(&op->desc.blend.layer, "FG"));
        LOG_E(FORMATED_LAYER_EXTRA_INFO(&op->desc.blend.layer));
        if (EPIC_BLEND_MODE_FIXED_BG == op->desc.blend.use_dest_as_bg)
        {
            LOG_E("BG rgb:%d,%d,%d",
                  op->desc.blend.r,
                  op->desc.blend.g,
                  op->desc.blend.b);
        }
    }
    break;

    case DRV_EPIC_DRAW_FILL:
    {
        LOG_E("Fill argb:%d,%d,%d,%d",
              op->desc.fill.opa,
              op->desc.fill.r,
              op->desc.fill.g,
              op->desc.fill.b);
    }
    break;


    case DRV_EPIC_DRAW_LETTERS:
    {
        LOG_E("Label argb:%d,%d,%d,%d, nums=%d, cf=%d",
              op->desc.label.opa,
              op->desc.label.r,
              op->desc.label.g,
              op->desc.label.b,
              op->desc.label.letter_num,
              op->desc.label.color_mode);

    }
    break;

    case DRV_EPIC_DRAW_ARC:
    {
        LOG_E("center[%d,%d], angle[%d~%d], round=%d,%d, w=%d, r=%d, argb:0x%08x",
              op->desc.arc.center_x, op->desc.arc.center_y,
              op->desc.arc.start_angle, op->desc.arc.end_angle,
              op->desc.arc.round_start, op->desc.arc.round_end,
              op->desc.arc.width, op->desc.arc.radius,
              op->desc.arc.argb8888
             );
    }
    break;

    case DRV_EPIC_DRAW_RECT:
    {
        LOG_E(AreaString" r=%d, top/bot_fillet=%d,%d argb:0x%08x",
              AreaParams(&op->desc.rectangle.area), op->desc.rectangle.radius,

              op->desc.rectangle.top_fillet, op->desc.rectangle.bot_fillet,

              op->desc.rectangle.argb8888
             );
    }
    break;

    case DRV_EPIC_DRAW_LINE:
    {
        LOG_E("p1=%d,%d, p2=%d,%d, w=%d, round=%d,%d argb:0x%08x",
              op->desc.line.p1.x, op->desc.line.p1.y,
              op->desc.line.p2.x, op->desc.line.p2.y,
              op->desc.line.width,
              op->desc.line.round_start, op->desc.line.round_end,
              op->desc.line.argb8888
             );
    }
    break;

    case DRV_EPIC_DRAW_BORDER:
    {
        LOG_E(AreaString" r=%d, w=%d, argb:0x%08x, tblr=%d%d%d%d",
              AreaParams(&op->desc.border.area), op->desc.border.radius,
              op->desc.border.width, op->desc.border.argb8888,
              op->desc.border.top_side, op->desc.border.bot_side,
              op->desc.border.left_side, op->desc.border.right_side
             );
    }
    break;


    default:
    {
        LOG_E("Unknow %d", op->op);
    }
    break;
    }
}

static void print_rl(priv_render_list_t *rl)
{
    LOG_E("\n\n\nPrint Render list");
    LOG_E(FORMATED_LAYER_INFO(&rl->dst, "DST"));
    for (uint16_t i = 0; i < rl->src_list_len; i++)
    {
        drv_epic_operation *p_operation = &rl->src_list[i];
        char idex_str[6];
        snprintf(idex_str, sizeof(idex_str), "%u", (unsigned)i);
        print_operation(&idex_str[0], p_operation);
    }
    LOG_E("\n\nPrint Render list DONE.\n");

}
/**********************
 *   SCHEDULER / HAL SUBMIT
 **********************/

/**
 * drv_epic.c:2546-2803, adapted for the openvela sync model (plan §6.1):
 *  - HAL_API_TRANSFORM removed (HAL_EPIC_Adv not linkable, plan §3.3),
 *    HAL_API_COPY removed (no caller in the 4 target capabilities).
 *  - HAL_API_BLEND_EX now uses the *synchronous* HAL_EPIC_BlendStartEx
 *    (waits inside HAL, like the existing lv_epic_fill path) instead of the
 *    interrupt-driven ..._IT + semaphore "shadow handle" dance.
 *  - No semaphore: wait_hw_done() polls the HW busy bit.
 */
static HAL_StatusTypeDef Call_Hal_Api(HAL_API_TypeDef api, void * p1, void * p2, void * p3)
{
    HAL_StatusTypeDef ret;
    /*0:Not started, 1:continue blend mode, 2:Stopping continue blend mode*/

    EPIC_HandleTypeDef * hw_epic_handle = lv_epic_get_handle();

    if(NULL == hw_epic_handle) {
        return HAL_ERROR;
    }

    if(epic_mw_dev.dbg_flag_print_exe_detail) {
#define PRINT_API_NAME(api) case api: LOG_I("Call_Hal_Api "#api); break;
        switch(api) {
            PRINT_API_NAME(HAL_API_CONT_BLEND)
            PRINT_API_NAME(HAL_API_CONT_BLEND_STOP)
            PRINT_API_NAME(HAL_API_CONT_BLEND_ASYNC_STOP)
            PRINT_API_NAME(HAL_API_BLEND_EX)
            default:
                LOG_I("Call_Hal_Api= %d", api);
                break;
        }
        LOG_I("epic_mw_cont_blend_states=%d", epic_mw_cont_blend_states);
    }

    if(HAL_API_CONT_BLEND == api) {
        if(epic_mw_cont_blend_states != 1) {
            if(2 == epic_mw_cont_blend_states) {
                /* Restart cont_blend as required (drv_epic.c:2576-2580). */
                HAL_EPIC_ContBlendStop(hw_epic_handle);
            }
            else {
                /* Take new semaphore & Wait previous async things done. */
                wait_hw_done();
            }

            ret = HAL_EPIC_ContBlendStart(hw_epic_handle,
                                          (EPIC_LayerConfigTypeDef *)p1,
                                          (EPIC_LayerConfigTypeDef *)p2,
                                          (EPIC_LayerConfigTypeDef *)p3);
            if(HAL_OK == ret) {
                epic_mw_cont_blend_states = 1;
            }
            else {
                /* HAL_EPIC_ContBlendStart sets State=BUSY before configuring;
                 * a failed start would leak that state (bf0_hal_epic.c:4641).
                 * ContBlendStop() waits + disables the layers + State=READY. */
                (void)HAL_EPIC_ContBlendStop(hw_epic_handle);
                epic_mw_cont_blend_states = 0;
            }
            DRV_EPIC_ASSERT(HAL_OK == ret);
        }
        else {
            ret = HAL_EPIC_ContBlendRepeat(hw_epic_handle,
                                           (EPIC_LayerConfigTypeDef *)p1,
                                           (EPIC_LayerConfigTypeDef *)p2,
                                           (EPIC_LayerConfigTypeDef *)p3);
            DRV_EPIC_ASSERT(HAL_OK == ret);
        }
        return ret;
    }
    else if(HAL_API_CONT_BLEND_STOP == api) {
        if(epic_mw_cont_blend_states != 0) {
            /* HAL_EPIC_ContBlendStop() waits for completion internally
             * (hal/bf0_hal_epic.c:4781) -> the op layer needs no semaphore. */
            ret = HAL_EPIC_ContBlendStop(hw_epic_handle);
            DRV_EPIC_ASSERT(HAL_OK == ret);
            epic_mw_cont_blend_states = 0;
        }
        return HAL_OK;
    }
    else if(HAL_API_CONT_BLEND_ASYNC_STOP == api) {
        /*
            the continue blend operations should be restarted next time
        */
        if(1 == epic_mw_cont_blend_states) epic_mw_cont_blend_states = 2;
        return HAL_OK;
    }
    else if(HAL_API_BLEND_EX == api) {
        /* A live ContBlend session owns the GPU: stop it first (keeps holding
         * the "semaphore" conceptually), exactly like the SDK's
         * dbg_flag_dis_ram_instance==1 path (drv_epic.c:2667-2683). */
        if(epic_mw_cont_blend_states != 0) {
            ret = HAL_EPIC_ContBlendStop(hw_epic_handle);
            DRV_EPIC_ASSERT(HAL_OK == ret);
            epic_mw_cont_blend_states = 0;
        }
        else {
            wait_hw_done();
        }

        /* Reconcile a stale BUSY state instead of failing (mirrors
         * epic_prepare_sync_nuttx in lv_sifli_epic_cfg.c). */
        if(HAL_EPIC_STATE_READY != hw_epic_handle->State) {
            LV_LOG_WARN("epic_mw: State=%d before blend, reconciling", (int)hw_epic_handle->State);
            hw_epic_handle->State = HAL_EPIC_STATE_READY;
        }

        if((NULL == p1) || (0 == (uint32_t)p2)) {
            /* drv_epic.c:2905-2909 / 2985-2989 submit the fully opaque case
             * with input_layer_cnt == 0.  The SDK's interrupt-driven
             * BlendStartEx_IT accepted that; the synchronous one rejects it
             * (bf0_hal_epic.c:3907).  Route it through HAL_EPIC_FillStart,
             * exactly like lv_epic_fill() does on NuttX (cfg.c:400-414). */
            EPIC_LayerConfigTypeDef * p_out = (EPIC_LayerConfigTypeDef *)p3;
            EPIC_FillingCfgTypeDef param;

            HAL_EPIC_FillDataInit(&param);
            param.start = p_out->data;
            param.color_mode = p_out->color_mode;
            param.width = p_out->width;
            param.height = p_out->height;
            param.total_width = p_out->total_width;
            param.color_r = p_out->color_r;
            param.color_g = p_out->color_g;
            param.color_b = p_out->color_b;
            param.alpha = EPIC_LAYER_OPAQUE;

            lv_epic_flush_cache_range(p_out->data,
                                      (uint32_t)p_out->total_width * p_out->height *
                                      (HAL_EPIC_GetColorDepth(p_out->color_mode) / 8U));
            ret = HAL_EPIC_FillStart(hw_epic_handle, &param);
            DRV_EPIC_ASSERT(HAL_OK == ret);
            return HAL_OK;
        }

        ret = HAL_EPIC_BlendStartEx(hw_epic_handle,
                                    (EPIC_LayerConfigTypeDef *)p1,
                                    (uint8_t)(uint32_t)p2,
                                    (EPIC_LayerConfigTypeDef *)p3);
        DRV_EPIC_ASSERT(HAL_OK == ret);
        return HAL_OK;
    }

    DRV_EPIC_ASSERT(0);
    return HAL_ERROR;
}

/**
 * drv_epic.c:4182-4292, statistics/history stripped, IMAGE/LETTERS executors
 * dropped (port_plan.md §2.1: only FILL/RECT/BORDER/LINE/ARC are ported).
 */
static rt_err_t render(epic_mw_rl_t list)
{
    EPIC_AreaTypeDef intersect_area;
    priv_render_list_t * rl = (priv_render_list_t *) list;
    EPIC_LayerConfigTypeDef * dst = &rl->dst;
    rt_err_t ret_v = RT_EEMPTY;

    if(epic_mw_dev.dbg_flag_print_exe_detail) {
        LOG_E("\n");
        LOG_E(FORMATED_LAYER_INFO(dst, "--Render DST--"));
    }

    for(uint16_t i = 0; i < rl->src_list_len; i++) {
        drv_epic_operation * p_operation = &rl->src_list[i];

        if(_layer_IntersectArea(dst, &p_operation->clip_area, &intersect_area)) {
            if(0 == (epic_mw_dev.dbg_flag_dis_operations & (1 << p_operation->op))) {
                /* The v9 adapter keeps op->mask.data == NULL except for
                 * DRV_EPIC_DRAW_IMAGE, where it carries the A8 rounded-clip
                 * mask (LV_SIFLI_EPIC_USE_ROUND_CLIP). */
                LV_ASSERT(NULL == p_operation->mask.data || DRV_EPIC_DRAW_IMAGE == p_operation->op);

                if(epic_mw_dev.dbg_flag_print_exe_detail) {
                    char idex_str[32];
                    snprintf(idex_str, sizeof(idex_str), "EXECUTE>> << %d", i);
                    print_operation(idex_str, p_operation);
                }

                uint32_t start_cyc = epic_mw_cyc();

                switch(p_operation->op) {
                    case DRV_EPIC_DRAW_IMAGE:
                        render_image(p_operation, dst, &intersect_area);
                        break;

                    case DRV_EPIC_DRAW_ARC:
                        render_arc(p_operation, dst, &intersect_area);
                        break;

                    case DRV_EPIC_DRAW_RECT:
                        render_rectangle(p_operation, dst, &intersect_area);
                        break;

                    case DRV_EPIC_DRAW_FILL:
                        render_fill(p_operation, dst, &intersect_area);
                        break;

                    case DRV_EPIC_DRAW_LINE:
                        render_line(p_operation, dst, &intersect_area);
                        break;

                    case DRV_EPIC_DRAW_BORDER:
                        render_border(p_operation, dst, &intersect_area);
                        break;

                    default:
                        LV_ASSERT(0);
                        break;
                }

                /* Notify the continue blend operations should be restarted next time
                 * (drv_epic.c:4268). */
                HAL_StatusTypeDef ret = Call_Hal_Api(HAL_API_CONT_BLEND_ASYNC_STOP, NULL, NULL, NULL);
                DRV_EPIC_ASSERT(HAL_OK == ret);

                epic_mw_dbg_ops[p_operation->op]++;
                epic_mw_dbg_us[p_operation->op] += (epic_mw_cyc() - start_cyc) / EPIC_MW_CYC_PER_US;
            }

            ret_v = RT_EOK;
        }
    }

    return ret_v;
}

/* drv_epic.c:4294-4305 */
static rt_err_t render_list(priv_render_list_t * rl)
{
    return render((epic_mw_rl_t)rl);
}

/* drv_epic.c:4307-4318 (rt_enter/exit_critical are no-ops here, plan §6.1) */
static void lock_render_list(epic_mw_rl_t list)
{
    priv_render_list_t * rl = (priv_render_list_t *) list;

    rt_enter_critical();
    if(0 == (rl->flag & rl_flag_overwritting))
        rl->flag |= rl_flag_rendering;
    else
        rl->flag &= ~rl_flag_commit;
    rt_exit_critical();
}

/* drv_epic.c:4439-4456 (rl_sem_release dropped: single-threaded, plan §6.1) */
static int epic_mw_destroy_rl(epic_mw_rl_t list)
{
    priv_render_list_t * rl = (priv_render_list_t *) list;

    rt_enter_critical();
    rl->flag = 0;
    rl->src_list_len = 0;
    rl->src_list_alloc_len = 0;
    rl->used = 0;
    rl->commit_area.x0 = 0;
    rl->commit_area.x1 = -1;
    rl->commit_area.y0 = 0;
    rl->commit_area.y1 = -1;
    rt_exit_critical();
    return 0;
}

/* drv_epic.c:5277-5298 */
static int epic_mw_render_list_init(void)
{
    if(epic_mw_dev.render_list_pool) {
        for(uint32_t i = 0; i < render_list_pool_max; i++) {
            epic_mw_dev.render_list_pool[i].used = 0;
            epic_mw_dev.render_list_pool[i].flag = 0;
            epic_mw_dev.render_list_pool[i].src_list_alloc_len = 0;
            epic_mw_dev.render_list_pool[i].src_list_len = 0;
            epic_mw_dev.render_list_pool[i].commit_area.x0 = 0;
            epic_mw_dev.render_list_pool[i].commit_area.x1 = -1;
            epic_mw_dev.render_list_pool[i].commit_area.y0 = 0;
            epic_mw_dev.render_list_pool[i].commit_area.y1 = -1;
        }
    }

    epic_mw_mask_cleanup();

    return 0;
}

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/* drv_epic.c:4869-4881 + 5320-5364, trimmed: no mq/thread/semaphore, the
 * pools are static (plan §4.3), the EPIC handle stays owned by
 * lv_sifli_epic_cfg.c. */
int epic_mw_init(void)
{
    if(epic_mw_inited) {
        return 0;
    }

    memset(&epic_mw_dev, 0, sizeof(epic_mw_dev));

    epic_mw_dev.render_list_pool = &epic_mw_render_list_pool[0];
    epic_mw_dev.mask_buf_pool = &epic_mw_mask_buf_pool[0];
    epic_mw_dev.mask_buf2_pool = &epic_mw_mask_buf2_pool[0];

    epic_mw_dev.dbg_flag_print_rl = 0;
    epic_mw_dev.dbg_flag_print_exe_detail = 0;
    epic_mw_dev.dbg_flag_dis_operations = 0;

    /* Stage 1: op merging disabled (port_plan.md §2.1 -> enable in stage 4). */
    epic_mw_dev.dbg_flag_dis_merge_operations = 0xFFFFFFFFu;

    epic_mw_dev.dbg_mask_buf_pool_max = mask_buf_max_bytes;

    epic_mw_render_list_init();

    epic_mw_inited = 1;
    return 0;
}

void epic_mw_deinit(void)
{
    if(!epic_mw_inited) {
        return;
    }

    /* Close any session we might have left behind (defensive). */
    (void)Call_Hal_Api(HAL_API_CONT_BLEND_STOP, NULL, NULL, NULL);

    epic_mw_mask_cleanup();
    epic_mw_dev.using_rl = NULL;
    epic_mw_inited = 0;
}

void epic_mw_set_print_rl(uint32_t en)
{
    epic_mw_dev.dbg_flag_print_rl = en ? 1 : 0;
}

void epic_mw_set_print_detail(uint32_t en)
{
    epic_mw_dev.dbg_flag_print_exe_detail = en ? 1 : 0;
}

/* drv_epic.c:4883-4937; rl_sem_take dropped (single-threaded) and the
 * destination stride is taken from the caller (port_plan.md §6.6). */
epic_mw_rl_t epic_mw_begin(const epic_mw_buf_t * p_buf, const EPIC_AreaTypeDef * p_ow_area)
{
    priv_render_list_t * rl_overwrite = NULL;
    priv_render_list_t * rl_ret = NULL;

    if(!epic_mw_inited) {
        return NULL;
    }

    rt_enter_critical();

    for(uint32_t idx = 0; idx < render_list_pool_max; idx++) {
        priv_render_list_t * rl = &epic_mw_dev.render_list_pool[idx];
        if(0 == rl->used) {
            LV_ASSERT(0 == rl->flag);
            LV_ASSERT(0 == rl->src_list_len);

            rl->used = 1;
            rl_ret = rl;
            break;
        }
        else if(0 == (rl_flag_rendering & rl->flag)) {
            /*Make sure all operations were commited.*/
            LV_ASSERT(rl->src_list_len == rl->src_list_alloc_len);
            rl->src_list_alloc_len = 0;
            rl->src_list_len = 0;
            rl->flag |= rl_flag_overwritting;
            rl_overwrite = rl;
        }
    }
    rt_exit_critical();
    LV_ASSERT(rl_ret);

    if(rl_overwrite && !rl_ret) rl_ret = rl_overwrite;

    if(rl_ret) {
        /*Make sure all operations were commited.*/
        LV_ASSERT(rl_ret->src_list_len == rl_ret->src_list_alloc_len);
        HAL_EPIC_LayerConfigInit(&rl_ret->dst);
        rl_ret->dst.data        = p_buf->data;
        rl_ret->dst.color_mode  = p_buf->cf;
        rl_ret->dst.width       = HAL_EPIC_AreaWidth(&p_buf->area);
        rl_ret->dst.height      = HAL_EPIC_AreaHeight(&p_buf->area);
        /* ⚠ SDK wrote dst.total_width = dst.width here (drv_epic.c:4928),
         * which mis-lays-out v9 buffers with stride > width (plan §6.6). */
        rl_ret->dst.total_width = (0 != p_buf->total_width) ? p_buf->total_width : rl_ret->dst.width;
        rl_ret->dst.x_offset    = p_buf->area.x0;
        rl_ret->dst.y_offset    = p_buf->area.y0;

        if(p_ow_area) HAL_EPIC_AreaCopy(&rl_ret->commit_area, p_ow_area);
    }

    epic_mw_dev.using_rl = rl_ret;
    return (epic_mw_rl_t)rl_ret;
}

/* drv_epic.c:4939-5071; `#if 0` merge search dropped, stride aware (plan §6.6). */
epic_mw_op_t * epic_mw_alloc_op(const epic_mw_buf_t * p_buf)
{
    priv_render_list_t * found_list = NULL;
    priv_render_list_t * free_list = NULL;
    int16_t merge_ret = 0;
    int16_t x_off = 0, y_off = 0; /*Vector from 'p_buf(A)' to 'rl->dst(B)'*/
    uint16_t new_height = 0;
    priv_render_list_t * rl;

    rl = epic_mw_dev.using_rl;
    LV_ASSERT(rl);
    if(NULL == rl) {
        return NULL;
    }
    LV_ASSERT(0 == (rl->flag & rl_flag_rendering));

    if(rl->src_list_len > 0) {
        found_list = rl;
        if((p_buf->data == rl->dst.data) && (p_buf->cf == rl->dst.color_mode)) {
            /* Fast path: same destination buffer as the render list.
             * merge_a2b() cannot be used when total_width != area width
             * (its own stride assumption, plan §6.6). */
            merge_ret = 1; /*A == B*/
        }
        else {
            merge_ret = merge_a2b(p_buf, &rl->dst, &x_off, &y_off, &new_height);
        }
    }
    else {
        free_list = rl;
    }

    //Update the dst layer of rendering list
    if(found_list) {
        rl = found_list;

        switch(merge_ret) {
            case 2://A include B
                rl->dst.data        = p_buf->data;
                rl->dst.height      = HAL_EPIC_AreaHeight(&p_buf->area);
                rl->dst.x_offset    = rl->dst.x_offset - x_off;
                rl->dst.y_offset    = rl->dst.y_offset - y_off;
                break;

            case 3://A&B have intersection
                rl->dst.data        = p_buf->data;
                rl->dst.height      = new_height;
                rl->dst.x_offset    = rl->dst.x_offset - x_off;
                rl->dst.y_offset    = rl->dst.y_offset - y_off;
                break;

            case 5://B&A have intersection
                rl->dst.height      = new_height;
                break;

            case 1: //A == B
            case 4: //B include A
                break;

            default:
                LV_ASSERT(0);
                break;
        }
    }
    else if(free_list) {
        rl = free_list;

        HAL_EPIC_LayerConfigInit(&rl->dst);
        rl->dst.data        = p_buf->data;
        rl->dst.color_mode  = p_buf->cf;
        rl->dst.width       = HAL_EPIC_AreaWidth(&p_buf->area);
        rl->dst.height      = HAL_EPIC_AreaHeight(&p_buf->area);
        rl->dst.total_width = (0 != p_buf->total_width) ? p_buf->total_width : rl->dst.width;
        rl->dst.x_offset    = p_buf->area.x0;
        rl->dst.y_offset    = p_buf->area.y0;
    }
    else {
        return NULL;
    }

    if(rl->src_list_alloc_len < src_list_max) {
        drv_epic_operation * ret_op;
        LV_ASSERT(rl->src_list_len == rl->src_list_alloc_len);
        ret_op = &rl->src_list[rl->src_list_alloc_len];

        memset(ret_op, 0, sizeof(drv_epic_operation));
        HAL_EPIC_LayerConfigInit(&ret_op->mask);

        if((rl == found_list) && ((4 == merge_ret) || (5 == merge_ret))) {
            ret_op->offset_x = x_off;
            ret_op->offset_y = y_off;
        }
        else {
            ret_op->offset_x = 0;
            ret_op->offset_y = 0;
        }

        rl->src_list_alloc_len++;
        return ret_op;
    }
    else {
        LOG_E("Render list is full!");
        print_rl(rl);
        return NULL;
    }
}

/* drv_epic.c:5074-5206; the IMAGE source-address assert is dropped (no IMAGE
 * renderer in this stage).  Op merging stays compiled but is disabled by
 * dbg_flag_dis_merge_operations = 0xFFFFFFFF (plan §2.1). */
int epic_mw_commit_op(epic_mw_op_t * op)
{
    priv_render_list_t * rl = epic_mw_dev.using_rl;

    LV_ASSERT(rl);
    LV_ASSERT(0 == (rl->flag & (rl_flag_rendering)));
    LV_ASSERT(rl->used);
    LV_ASSERT(rl->src_list_len + 1 == rl->src_list_alloc_len);

    HAL_EPIC_AreaMove(&op->clip_area, op->offset_x, op->offset_y);

    if(0xFFFFFFFF != epic_mw_dev.dbg_flag_dis_merge_operations) {
        if(rl->src_list_len > 0) {
            drv_epic_operation * prev_op = &rl->src_list[rl->src_list_len - 1];
            drv_epic_operation * curr_op = op;

            if((DRV_EPIC_DRAW_FILL == prev_op->op) && (prev_op->desc.fill.opa >= OPA_MAX) && (NULL == prev_op->mask.data)
               && (DRV_EPIC_DRAW_IMAGE == op->op) && (EPIC_BLEND_MODE_FIXED_BG == op->desc.blend.use_dest_as_bg)
               && (0 == op->desc.blend.layer.transform_cfg.angle) && (0 == op->desc.blend.layer.transform_cfg.type)
               && (EPIC_INPUT_SCALE_NONE == op->desc.blend.layer.transform_cfg.scale_x)
               && (EPIC_INPUT_SCALE_NONE == op->desc.blend.layer.transform_cfg.scale_y)
              ) {
                if(epic_mw_dev.dbg_flag_dis_merge_operations & 1) goto __COMMIT_OPERATION;

                EPIC_AreaTypeDef prev_area = prev_op->clip_area;
                if(HAL_EPIC_AreaIsIn(&op->clip_area, &prev_area)) {
                    //Merge filling&blending operations to one
                    op->desc.blend.use_dest_as_bg = EPIC_BLEND_MODE_FIXED_BG;
                    op->desc.blend.r = prev_op->desc.fill.r;
                    op->desc.blend.g = prev_op->desc.fill.g;
                    op->desc.blend.b = prev_op->desc.fill.b;

                    op->clip_area = prev_area;

                    memcpy(prev_op, op, sizeof(drv_epic_operation));
                    rl->src_list_len--;
                }
            }
            else if((DRV_EPIC_DRAW_FILL == prev_op->op) && (DRV_EPIC_DRAW_FILL == curr_op->op)) {
                if(epic_mw_dev.dbg_flag_dis_merge_operations & 2) goto __COMMIT_OPERATION;

                if((NULL == prev_op->mask.data) && (NULL == curr_op->mask.data)) {
                    EPIC_AreaTypeDef res_area;

                    if(AreaExcept(&res_area, &prev_op->clip_area, &curr_op->clip_area)) {
                        LV_ASSERT(HAL_EPIC_AreaIsValid(&res_area));
                        if(epic_mw_dev.dbg_flag_dis_merge_operations & 4) goto __COMMIT_OPERATION;

                        if(curr_op->desc.fill.opa >= OPA_MAX) {
                            memcpy(prev_op, curr_op, sizeof(drv_epic_operation)); //Overwrite previous
                            rl->src_list_len--;
                        }
                        else if(prev_op->desc.fill.opa >= OPA_MAX) {
                            //TODO  Reduce current area, and mix current color to previous color
                        }
                    }
                    else if(AreaExcept(&res_area, &curr_op->clip_area, &prev_op->clip_area)) {
                        LV_ASSERT(HAL_EPIC_AreaIsValid(&res_area));
                        if(epic_mw_dev.dbg_flag_dis_merge_operations & 8) goto __COMMIT_OPERATION;

                        if(curr_op->desc.fill.opa >= OPA_MAX) {
                            prev_op->clip_area = res_area; //Reduce previous area
                        }
                        else if(prev_op->desc.fill.opa >= OPA_MAX) {
                            //TODO  Reduce current area, and mix current color to previous color
                        }
                    }
                    else if(HAL_EPIC_AreaIsIn(&prev_op->clip_area, &curr_op->clip_area)) {
                        if(epic_mw_dev.dbg_flag_dis_merge_operations & 0x10) goto __COMMIT_OPERATION;

                        if(curr_op->desc.fill.opa >= OPA_MAX) {
                            prev_op->desc.fill.r = curr_op->desc.fill.r;
                            prev_op->desc.fill.g = curr_op->desc.fill.g;
                            prev_op->desc.fill.b = curr_op->desc.fill.b;
                            prev_op->desc.fill.opa = curr_op->desc.fill.opa;

                            prev_op->clip_area = curr_op->clip_area;//Overwrite

                            rl->src_list_len--;
                        }
                        else if(HAL_EPIC_AreaIsIn(&curr_op->clip_area, &prev_op->clip_area)) { //Same area
                            //Todo Mix current color to previous color
                        }
                    }
                }
            }
        }
    }

__COMMIT_OPERATION:
    /*Commit operation*/
    LV_ASSERT(rl->src_list_len < src_list_max);
    rl->src_list_len++;
    return 0;
}

/* drv_epic.c:4693-4720 (EPIC_MSG_RENDER_TO_BUF, no-scaling branch) turned into
 * a synchronous flush; session ownership rules in port_plan.md §4.2/§6.3. */
int epic_mw_flush(epic_mw_rl_t rl_)
{
    priv_render_list_t * rl = (priv_render_list_t *) rl_;
    rt_err_t ret;

    if(NULL == rl) {
        return -1;
    }

    lock_render_list(rl_);
    if(rl->flag & rl_flag_rendering) {
        render_start();

        /* plan §6.1: reconcile HW/State before touching the GPU. */
        epic_mw_prepare_sync();

        /* Defensive: never inherit a ContBlend session (plan §6.3). */
        (void)Call_Hal_Api(HAL_API_CONT_BLEND_STOP, NULL, NULL, NULL);

        ret = render_list(rl);

        /* Always close the session our ops may have opened: no session may
         * survive a flush, otherwise flush_cb / lv_epic_* resets fight it. */
        (void)Call_Hal_Api(HAL_API_CONT_BLEND_STOP, NULL, NULL, NULL);
        if(rl->src_list_len) {
            lv_epic_gpu_check_done(); /* no-op on NuttX (sync HAL) */
        }

        epic_mw_destroy_rl(rl_);
        render_end();

        return (RT_EOK == ret) ? 0 : 0; /* RT_EEMPTY: nothing to draw, not an error */
    }

    epic_mw_destroy_rl(rl_);
    return -1;
}

int epic_mw_discard(epic_mw_rl_t rl_)
{
    if(NULL == rl_) {
        return -1;
    }

    priv_render_list_t * rl = (priv_render_list_t *) rl_;
    if(rl->flag & rl_flag_rendering) {
        /* Already rendering (should not happen in the sync model). */
        return -1;
    }

    epic_mw_dev.using_rl = NULL;
    return epic_mw_destroy_rl(rl_);
}

#endif /*LV_USE_SIFLI_EPIC*/
