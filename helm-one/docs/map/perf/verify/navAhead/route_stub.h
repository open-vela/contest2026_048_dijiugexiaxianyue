#ifndef ROUTE_STUB_H
#define ROUTE_STUB_H

#include "vmap_route.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 宿主桩：见 route_stub.c（替代 vmap_route.c，只提供导航匹配需要的字段）。 */
vmap_route_t * route_stub_create(void);
void route_stub_destroy(vmap_route_t * r);
bool route_stub_set(vmap_route_t * r, const vmap_route_pt_t * pts,
    uint32_t n, double total_m);
uint32_t route_stub_progress_seg(const vmap_route_t * r);

#ifdef __cplusplus
}
#endif

#endif /* ROUTE_STUB_H */
