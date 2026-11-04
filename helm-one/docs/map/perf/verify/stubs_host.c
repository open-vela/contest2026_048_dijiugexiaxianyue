/*
 * verify/harness.c 的宿主桩。
 *
 * 板级的 EPIC 搬运后端在宿主机上不存在，全部退化成"没搬成" —— 于是
 * `vmap_blit_*` 走 CPU 兜底那条路。差分只关心像素结果，所以这就够了。
 *
 * 与它一起被拼进同一个编译单元的还有两个假头（verify/fake/ 下），
 * 用来挡掉 `<nuttx/config.h>` 与板级 `board_malloc.h`。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

bool myvendor_epic_ready(void) { return false; }
bool myvendor_epic_busy(void) { return false; }

bool myvendor_epic_copy(uint16_t * d, uint32_t ds, int32_t dx, int32_t dy,
    const uint16_t * s, uint32_t ss, int32_t sx, int32_t sy, int32_t w, int32_t h)
{
    (void)d; (void)ds; (void)dx; (void)dy; (void)s; (void)ss;
    (void)sx; (void)sy; (void)w; (void)h;
    return false;
}

bool myvendor_epic_fill(uint16_t * d, uint32_t ds, int32_t x, int32_t y,
    int32_t w, int32_t h, uint16_t c)
{
    (void)d; (void)ds; (void)x; (void)y; (void)w; (void)h; (void)c;
    return false;
}

bool myvendor_epic_rotate(const uint16_t * s, uint32_t ss, int32_t sw,
    int32_t sh, uint16_t * d, uint32_t ds, int32_t dw, int32_t dh,
    int32_t angle10, int32_t px, int32_t py, int32_t cx, int32_t cy)
{
    (void)s; (void)ss; (void)sw; (void)sh; (void)d; (void)ds; (void)dw;
    (void)dh; (void)angle10; (void)px; (void)py; (void)cx; (void)cy;
    return false;
}

bool myvendor_epic_rotate_selftest(void) { return false; }

void myvendor_epic_wait(void) { }

void * vmap_malloc(size_t n) { return malloc(n); }
void   vmap_free(void * p)   { free(p); }
