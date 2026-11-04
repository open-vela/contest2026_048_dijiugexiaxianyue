/**
 * @file lv_draw_sifli_epic.h
 *
 */

/**
 * Copyright 2024 SiFli Technologies
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef LV_DRAW_SIFLI_EPIC_H
#define LV_DRAW_SIFLI_EPIC_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include "../../../lv_conf_internal.h"

#if LV_USE_SIFLI_EPIC
#include "../../lv_draw_private.h"
#include "../../../display/lv_display_private.h"
#include "../../../misc/lv_area_private.h"

/*********************
 *      DEFINES
 *********************/

/* EPIC/SW capability switches.
 * 0 = let the SW rasterizer draw this task type (A/B showed SW is faster for
 *     the primitive sizes seen in current UI/tests).
 * 1 = accept the task type in the EPIC unit (implemented via the ported
 *     epic_mw middle layer / native v9 epic paths).
 * LV_SIFLI_EPIC_ACCEPT_ALL=1 flips every switch to EPIC in one shot.
 * LINE/ARC are additionally gated by an area threshold below (small shapes
 * are cheaper on the SW rasterizer even when the switch is 1). */
#ifndef LV_SIFLI_EPIC_ACCEPT_ALL
    #define LV_SIFLI_EPIC_ACCEPT_ALL 0
#endif
#ifndef LV_SIFLI_EPIC_USE_LABEL
    #define LV_SIFLI_EPIC_USE_LABEL LV_SIFLI_EPIC_ACCEPT_ALL
#endif
#ifndef LV_SIFLI_EPIC_USE_LINE
    #define LV_SIFLI_EPIC_USE_LINE 1   /* selective: area gate below */
#endif
#ifndef LV_SIFLI_EPIC_USE_ARC
    #define LV_SIFLI_EPIC_USE_ARC 1    /* selective: area gate below */
#endif
#ifndef LV_SIFLI_EPIC_USE_RADIUS
    #define LV_SIFLI_EPIC_USE_RADIUS LV_SIFLI_EPIC_ACCEPT_ALL   /* rounded rect fill */
#endif
#ifndef LV_SIFLI_EPIC_USE_ROUND_BORDER
    #define LV_SIFLI_EPIC_USE_ROUND_BORDER LV_SIFLI_EPIC_ACCEPT_ALL /* radius!=0 / partial-side border */
#endif
#ifndef LV_SIFLI_EPIC_USE_ROUND_CLIP
    #define LV_SIFLI_EPIC_USE_ROUND_CLIP LV_SIFLI_EPIC_ACCEPT_ALL /* image clip_radius (A8 mask) */
#endif

/* Area thresholds for LINE/ARC (in pixels). An EPIC op has a large fixed
 * cost (~120us for a line, ~1.2ms for an arc) while SW costs ~20ns/pixel,
 * so below this pixel count the SW rasterizer is cheaper and the task is
 * rejected by the EPIC evaluate callback. 0 disables the gate. */
#ifndef LV_SIFLI_EPIC_LINE_MIN_PX
    #define LV_SIFLI_EPIC_LINE_MIN_PX 6000
#endif
#ifndef LV_SIFLI_EPIC_ARC_MIN_PX
    #define LV_SIFLI_EPIC_ARC_MIN_PX 60000
#endif

/**********************
 *      TYPEDEFS
 **********************/

/**
 * SiFli EPIC draw unit structure.
 * Similar to NXP PXP's lv_draw_pxp_unit_t.
 */
/* Batch depth of the draw-thread hand-off queue.  The dispatcher (UI thread)
 * pushes every independent task the EPIC unit accepts in one dispatch round;
 * the render thread drains the queue back-to-back, so one wakeup covers a
 * whole batch instead of one sync + context switch per task. */
#ifndef LV_SIFLI_EPIC_QUEUE_LEN
    #define LV_SIFLI_EPIC_QUEUE_LEN 8
#endif

typedef struct _lv_draw_sifli_epic_unit_t {
    lv_draw_unit_t base_unit;       /**< Base draw unit */
    lv_draw_task_t * volatile task_act; /**< Currently active task (cross-thread) */

#if LV_USE_SIFLI_EPIC_DRAW_THREAD
    lv_thread_sync_t sync;          /**< Wakes the render thread when a task is ready */
    lv_thread_sync_t sync_done;     /**< Signals task completion back to the dispatcher */
    lv_thread_t thread;             /**< Render thread */
    lv_draw_task_t * volatile task_q[LV_SIFLI_EPIC_QUEUE_LEN]; /**< Pending tasks */
    volatile uint32_t q_w;          /**< Queue write index (dispatcher only) */
    volatile uint32_t q_r;          /**< Queue read index (render thread only) */
    volatile bool waiter;           /**< Dispatcher is blocked in wait_for_finish */
    volatile bool inited;           /**< Initialization status */
    volatile bool exit_status;      /**< Exit status for thread */
#endif
} lv_draw_sifli_epic_unit_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Initialize SiFli EPIC draw unit.
 * This function should be called during display initialization.
 */
void lv_draw_sifli_epic_init(void);

/**
 * Deinitialize SiFli EPIC draw unit.
 * This function should be called during display deinitialization.
 */
void lv_draw_sifli_epic_deinit(void);

/**
 * Initialize draw buffer handlers for EPIC.
 */
void lv_draw_buf_sifli_epic_init_handlers(void);

/**
 * EPIC fill operation.
 * @param t Draw task
 */
void lv_draw_sifli_epic_fill(lv_draw_task_t * t);

/**
 * EPIC border draw operation.
 * @param t Draw task
 */
void lv_draw_sifli_epic_border(lv_draw_task_t * t);

/**
 * EPIC label draw operation.
 * @param t Draw task
 */
void lv_draw_sifli_epic_label(lv_draw_task_t * t);

/**
 * EPIC image draw operation.
 * @param t Draw task
 */
void lv_draw_sifli_epic_img(lv_draw_task_t * t);

/**
 * EPIC layer blend operation.
 * @param t Draw task
 */
void lv_draw_sifli_epic_layer(lv_draw_task_t * t);

/**
 * EPIC line draw operation (single segment, epic_mw op engine).
 * @param t Draw task
 */
void lv_draw_sifli_epic_line(lv_draw_task_t * t);

/**
 * EPIC arc draw operation (epic_mw op engine).
 * @param t Draw task
 */
void lv_draw_sifli_epic_arc(lv_draw_task_t * t);

/**
 * Dump accept/reject counters per draw-task type (benchmark diagnostics).
 */
void lv_sifli_epic_dump_stats(void);

/**********************
 *      MACROS
 **********************/

#endif /*LV_USE_SIFLI_EPIC*/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_DRAW_SIFLI_EPIC_H*/
