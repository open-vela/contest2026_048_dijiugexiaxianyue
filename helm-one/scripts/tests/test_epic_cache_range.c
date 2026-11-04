/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include "../../apps/graphics/lvgl/lvgl/src/draw/sifli/epic/lv_sifli_epic_cache.h"

int main(void)
{
    /* Display strips reside in non-cacheable HCPU SRAM. */
    assert(lv_epic_sf32lb52_uncached_range(0x20010000, 240 * 160 * 2));
    assert(lv_epic_sf32lb52_uncached_range(0x2027ffff, 1));
    /* Never skip maintenance for PSRAM/code, unknown addresses, crossing
     * boundaries, empty ranges or overflowing arithmetic. */
    assert(!lv_epic_sf32lb52_uncached_range(0x60010000, 76800));
    assert(!lv_epic_sf32lb52_uncached_range(0x10010000, 76800));
    assert(!lv_epic_sf32lb52_uncached_range(0x2027ffff, 2));
    assert(!lv_epic_sf32lb52_uncached_range(0x1fffffff, 2));
    assert(!lv_epic_sf32lb52_uncached_range(0x20000000, 0));
    assert(!lv_epic_sf32lb52_uncached_range(0x20000000, UINT32_MAX));
    assert(!lv_epic_sf32lb52_uncached_range(UINT32_MAX, 2));
    assert(lv_epic_sf32lb52_uncached_range(0x00010000, 0x20000));
    assert(!lv_epic_sf32lb52_uncached_range(0x00010000, 0x20001));
    assert(lv_epic_sf32lb52_uncached_range(0x203fc000, 0x104000));
    assert(!lv_epic_sf32lb52_uncached_range(0x203fc000, 0x104001));
    assert(lv_epic_sf32lb52_uncached_range(0x20bfc000, 0xc4000));
    assert(!lv_epic_sf32lb52_uncached_range(0x20bfc000, 0xc4001));
    return 0;
}
