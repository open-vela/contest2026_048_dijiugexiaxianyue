/*
 * 宿主差分测试用的假头：不拉板级 board_malloc.h（宿主机上没有）。
 * 行为由 verify/stubs_host.c 里的桩决定：直接转发到 malloc/free。
 */
#ifndef VMAP_ALLOC_H
#define VMAP_ALLOC_H

#include <stddef.h>

void * vmap_malloc(size_t size);
void   vmap_free(void * ptr);

#endif /* VMAP_ALLOC_H */
