/** SPDX-License-Identifier: MIT
 * SF32LB52 cache policy and benchmark-only counters. No HAL dependency.
 */
#ifndef LV_SIFLI_EPIC_CACHE_H
#define LV_SIFLI_EPIC_CACHE_H

#include <stdbool.h>
#include <stdint.h>

/* Match system_bf0_ap.c:mpu_config(). Only skip ranges wholly contained in
 * a known non-cacheable RAM region; unknown/overflowing ranges stay conservative.
 * Keep this list in sync if the board MPU layout changes.
 */
static inline bool lv_epic_sf32lb52_uncached_range(uint32_t start, uint32_t size)
{
    if(size == 0 || size - 1 > UINT32_MAX - start) return false;
    uint32_t last = start + size - 1;
    return (start >= 0x00010000U && last <= 0x0002ffffU) ||
           (start >= 0x20000000U && last <= 0x2027ffffU) ||
           (start >= 0x203fc000U && last <= 0x204fffffU) ||
           (start >= 0x20bfc000U && last <= 0x20cbffffU);
}

/* Cache primitives shared by the EPIC adapter and the LVGL NuttX driver.
 * The NuttX driver installs its callbacks after the EPIC adapter does. */
void lv_epic_flush_cache_range(const void * data, uint32_t size);
void lv_epic_invalidate_cache_range(const void * data, uint32_t size);
void lv_epic_clean_invalidate_cache_range(const void * data, uint32_t size);

#if defined(CONFIG_MYVENDOR_LVGL_BENCH)
typedef struct {
    uint32_t clean_calls;
    uint32_t flush_calls;
    uint32_t invalidate_calls;
    uint32_t skipped_calls;
    uint64_t requested_bytes;
    uint64_t skipped_bytes;
} lv_epic_cache_stats_t;

/* Call only from the LVGL thread, with no draw operation in flight. */
void lv_epic_cache_bench_reset(bool filter_enabled);
void lv_epic_cache_bench_get(lv_epic_cache_stats_t * stats);
#endif

#endif
