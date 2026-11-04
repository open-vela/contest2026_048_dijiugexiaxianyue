/**
 * @file vmap_route_log.h
 * @brief vmap route_log 模块。
 *
 * 经 vsyslog/vprintf 转发，避免本平台 uint32_t=unsigned long 与 %u 的 -Wformat。
 */

#ifndef VMAP_ROUTE_LOG_H
#define VMAP_ROUTE_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>

static inline void vmap_nav_vsyslog(int prio, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsyslog(prio, fmt, ap);
    va_end(ap);
}

static inline void vmap_nav_vprintf_ln(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/**
 * @brief 导航规划详细日志开关（VMAP_NAV_LOG / VMAP_NAV_WARN / VMAP_NAV_OUT）。
 *
 * @note 默认 **0 = 关闭**。规划一条路线会刷几十到几百行 `[nav] snap/compute/
 *       Dijkstra/paint...`，`VMAP_NAV_OUT` 还会同时 printf 到控制台（与 syslog
 *       重复一遍）；规划期的 WARN（already at dest / corridor segment fail）
 *       也是过程噪声。`VMAP_NAV_ERR` 始终保留。
 *       调试时：编译加 `-DVMAP_NAV_VERBOSE_LOG=1`，或临时把这里改成 1。
 */
#ifndef VMAP_NAV_VERBOSE_LOG
#define VMAP_NAV_VERBOSE_LOG 0
#endif

#if VMAP_NAV_VERBOSE_LOG
#define VMAP_NAV_LOG(fmt, ...)  vmap_nav_vsyslog(LOG_NOTICE,  "[nav] " fmt, ##__VA_ARGS__)
#define VMAP_NAV_WARN(fmt, ...) vmap_nav_vsyslog(LOG_WARNING, "[nav] " fmt, ##__VA_ARGS__)
/** @brief 只走 syslog。
 * @note 不要在这里加 `printf`：2026-11-03 实测，路由线程里 `vprintf` 会在 stdio
 *       互斥量上 `NXSEM_IS_MUTEX` 断言（`route_worker_thread` → `vmap_nav_vprintf_ln`
 *       → `vprintf` → assert），一开详细日志就会把板子重启。 */
#define VMAP_NAV_OUT(fmt, ...) vmap_nav_vsyslog(LOG_NOTICE, "[nav] " fmt, ##__VA_ARGS__)
#else
/* 参数不求值：这些调用只用于日志，没有副作用。 */
#define VMAP_NAV_LOG(fmt, ...)  do { } while (0)
#define VMAP_NAV_WARN(fmt, ...) do { } while (0)
#define VMAP_NAV_OUT(fmt, ...)  do { } while (0)
#endif

#define VMAP_NAV_ERR(fmt, ...)  vmap_nav_vsyslog(LOG_ERR,     "[nav] " fmt, ##__VA_ARGS__)

#endif /* VMAP_ROUTE_LOG_H */
