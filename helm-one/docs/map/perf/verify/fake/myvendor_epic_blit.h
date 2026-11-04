/*
 * 宿主差分测试用的假头：只留原型，不拉 nuttx/config.h（宿主机上没有）。
 * 行为由 verify/stubs_host.c 里的桩决定。
 */
#ifndef MYVENDOR_EPIC_BLIT_H
#define MYVENDOR_EPIC_BLIT_H

#include <stdbool.h>
#include <stdint.h>

bool myvendor_epic_ready(void);
bool myvendor_epic_busy(void);
bool myvendor_epic_copy(uint16_t * dst_buf, uint32_t dst_stride, int32_t dx,
    int32_t dy, const uint16_t * src_buf, uint32_t src_stride, int32_t sx,
    int32_t sy, int32_t w, int32_t h);
bool myvendor_epic_fill(uint16_t * dst_buf, uint32_t dst_stride, int32_t x,
    int32_t y, int32_t w, int32_t h, uint16_t color565);
bool myvendor_epic_rotate(const uint16_t * src_buf, uint32_t src_stride,
    int32_t src_w, int32_t src_h, uint16_t * dst_buf, uint32_t dst_stride,
    int32_t dst_w, int32_t dst_h, int32_t angle10, int32_t pivot_x,
    int32_t pivot_y, int32_t dst_cx, int32_t dst_cy);
bool myvendor_epic_rotate_selftest(void);
void myvendor_epic_wait(void);

#endif /* MYVENDOR_EPIC_BLIT_H */
