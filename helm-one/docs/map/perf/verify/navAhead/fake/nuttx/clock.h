#ifndef FAKE_NUTTX_CLOCK_H
#define FAKE_NUTTX_CLOCK_H
#include <stdint.h>
static inline uint32_t clock_systime_ticks(void) { static uint32_t t; return (t += 100); }
#define MSEC2TICK(ms) ((uint32_t)((ms) / 10u))
#endif
