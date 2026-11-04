/* CPU-only build: keep the bicycle blit interface available without EPIC. */
#include "myvendor_epic_blit.h"

bool myvendor_epic_ready(void) { return false; }
bool myvendor_epic_busy(void) { return false; }

bool myvendor_epic_copy(uint16_t *dst_buf, uint32_t dst_stride, int32_t dx,
                        int32_t dy, const uint16_t *src_buf, uint32_t src_stride,
                        int32_t sx, int32_t sy, int32_t w, int32_t h)
{
    (void)dst_buf; (void)dst_stride; (void)dx; (void)dy;
    (void)src_buf; (void)src_stride; (void)sx; (void)sy; (void)w; (void)h;
    return false;
}

bool myvendor_epic_fill(uint16_t *dst_buf, uint32_t dst_stride, int32_t x,
                        int32_t y, int32_t w, int32_t h, uint16_t color565)
{
    (void)dst_buf; (void)dst_stride; (void)x; (void)y;
    (void)w; (void)h; (void)color565;
    return false;
}

void myvendor_epic_wait(void) {}
