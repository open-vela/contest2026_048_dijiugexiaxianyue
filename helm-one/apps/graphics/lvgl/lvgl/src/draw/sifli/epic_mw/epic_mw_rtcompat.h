/**
 * @file epic_mw_rtcompat.h
 *
 * RT-Thread -> openvela (NuttX + LVGL) compatibility shim for the EPIC
 * middle-layer port.
 *
 * Ported from SiFli SDK 2.4 rtos/rtthread/bsp/sifli/drivers/drv_epic.c
 * (NEW_API region).  See /tmp/lvgl_bench/port_plan.md §1 / §4.3.
 *
 * Design rules (construction plan §6.1):
 *  - The RT-Thread device/thread/message-queue/semaphore model is NOT ported.
 *    openvela runs EPIC synchronously (CONFIG_LV_USE_SIFLI_EPIC_DRAW_THREAD=n,
 *    HAL_EPIC_BlendStartEx / HAL_EPIC_ContBlendStop wait for completion
 *    internally), so no blocking primitive is required here.
 *  - Cache maintenance is delegated to the existing openvela helpers
 *    (lv_epic_flush_cache_range / lv_epic_invalidate_cache_range).
 *  - rt_malloc/rt_free -> lv_malloc/lv_free (PSRAM pool, plan §4.3).
 */

#ifndef EPIC_MW_RTCOMPAT_H
#define EPIC_MW_RTCOMPAT_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include "../../../lv_conf_internal.h"

#if LV_USE_SIFLI_EPIC

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "../../../misc/lv_assert.h"
#include "../../../misc/lv_log.h"
#include "../../../stdlib/lv_mem.h"
#include "../../../misc/lv_area.h"
#include "../../../tick/lv_tick.h"

#include "bf0_hal.h"

#if LV_USE_NUTTX
    #include <nuttx/irq.h>
    #include <nuttx/cache.h>
    #include <nuttx/clock.h>
#endif

/*********************
 *      DEFINES
 *********************/

/* --- return types / constants (drv_epic.c uses rt_err_t everywhere) --- */
typedef int32_t rt_err_t;
typedef uint32_t rt_uint32_t;
typedef int32_t rt_int32_t;

#define RT_EOK       0
#define RT_ERROR     1
#define RT_EEMPTY    7
#define RT_ETIMEOUT  3

#define RT_UNUSED(x) ((void)(x))

/* --- assertion / logging --- */
#define RT_ASSERT(x)            LV_ASSERT(x)
#define RT_WEAK                 __attribute__((weak))
#define LOG_E(...)              LV_LOG_ERROR(__VA_ARGS__)
#define LOG_I(...)              LV_LOG_INFO(__VA_ARGS__)
#define LOG_D(...)              LV_LOG_TRACE(__VA_ARGS__)

/* --- linker section / attribute macros (no-ops on NuttX) --- */
#define L1_RET_BSS_SECT_BEGIN(n)
#define L1_RET_BSS_SECT_END
#define L1_NON_RET_BSS_SECT_BEGIN(n)
#define L1_NON_RET_BSS_SECT_END
#define L1_NON_RET_BSS_SECT(n, x)   x
#define __ROM_USED
#define RT_ALIGN_SIZE       4
#define ALIGN(n)            __attribute__((aligned(n)))

#define RT_ALIGN(s, a)          (((s) + (a) - 1) & ~((a) - 1))
#define RT_ALIGN_DOWN(s, a)     ((s) & ~((a) - 1))

/* --- memory: the mask engine (epic_mw_mask.c) uses these --- */
#define rt_malloc   lv_malloc
#define rt_free     lv_free

/* --- critical section: single-threaded draw path (plan §1.1 / §6.1) --- */
static rt_uint32_t epic_mw_irq_state;
#define rt_enter_critical()     do { epic_mw_irq_state = 0; } while (0)
#define rt_exit_critical()      do { } while (0)

/* --- time --- */
#define rt_tick_get()               ((rt_uint32_t)lv_tick_get())
static inline uint32_t rt_tick_get_millisecond(void)
{
    return (uint32_t)lv_tick_get();
}
static inline rt_uint32_t rt_tick_from_millisecond(rt_int32_t ms)
{
    return (rt_uint32_t)ms;
}

/* --- cache (forward to the openvela EPIC helpers, plan §6.2) --- */
/* lv_epic_flush_cache_range / lv_epic_invalidate_cache_range are declared in
 * ../epic/lv_sifli_epic_cfg.h and implemented in lv_sifli_epic_cfg.c. */

/* --- GPIO debug probes used by the SDK build (drv_epic.c:2202-2228) --- */
#define OUTPUT_TO_GPIO_PIN 0

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_USE_SIFLI_EPIC*/

#endif /*EPIC_MW_RTCOMPAT_H*/
