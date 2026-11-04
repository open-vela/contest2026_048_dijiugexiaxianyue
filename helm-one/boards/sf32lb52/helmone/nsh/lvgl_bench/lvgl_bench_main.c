/**
 * @file lvgl_bench_main.c
 * @brief NSH 命令 lvgl_bench：LVGL 官方 benchmark 压测。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "demos/benchmark/lv_demo_benchmark.h"
#include "myvendor_watchdog.h"
#include "src/draw/sifli/epic/lv_draw_sifli_epic.h"
#if LV_USE_SIFLI_EPIC
#include "src/draw/sifli/epic/lv_sifli_epic_cache.h"
#endif

#if defined(CONFIG_MYVENDOR_LCD_DISP) && CONFIG_MYVENDOR_LCD_DISP
#include "myvendor_lcd_disp.h"
#endif

#if LV_USE_PERF_MONITOR
#include "src/display/lv_display_private.h"
#include "src/misc/lv_timer_private.h"
#include "src/debugging/sysmon/lv_sysmon.h"
#endif

#if LV_USE_MYVENDOR_LCD_DISP || LV_USE_NUTTX_LCD
#ifndef LVGL_BENCH_LCD_PATH
#define LVGL_BENCH_LCD_PATH "/dev/lcd0"
#endif
#ifndef LVGL_BENCH_LCD_WAIT_MS
#define LVGL_BENCH_LCD_WAIT_MS 30000
#endif
#else
#ifndef LVGL_BENCH_FBDEV_PATH
#define LVGL_BENCH_FBDEV_PATH "/dev/fb0"
#endif
#ifndef LVGL_BENCH_FBDEV_WAIT_MS
#define LVGL_BENCH_FBDEV_WAIT_MS 30000
#endif
#endif

#if defined(CONFIG_MYVENDOR_LCD_DISP) && CONFIG_MYVENDOR_LCD_DISP
/** @brief LCD flush 完成回调，通知 LVGL。 */
static void bench_lcd_flush_done(void * user_data)
{
    /* Do not lv_async_call from worker: wait_for_flushing() busy-spins. */
    lv_display_flush_ready((lv_display_t *)user_data);
}
#endif

/** @brief benchmark 结束回调，打印 CSV 摘要。 */
static volatile bool s_bench_done;

static void bench_on_end(const lv_demo_benchmark_summary_t * summary)
{
    s_bench_done = true;
#if LV_USE_SIFLI_EPIC
    lv_epic_cache_stats_t cache_stats;
    lv_epic_cache_bench_get(&cache_stats);
    printf("EPIC_CACHE clean=%lu flush=%lu invalidate=%lu skipped=%lu requested_bytes=%llu skipped_bytes=%llu\n",
           (unsigned long)cache_stats.clean_calls,
           (unsigned long)cache_stats.flush_calls,
           (unsigned long)cache_stats.invalidate_calls,
           (unsigned long)cache_stats.skipped_calls,
           (unsigned long long)cache_stats.requested_bytes,
           (unsigned long long)cache_stats.skipped_bytes);
#endif
    /* Telemetry via printf: stdout goes straight to the console and cannot be
     * swallowed by the LVGL log level (CONFIG_LV_LOG_LEVEL=WARN). */
    printf("[lvgl_bench] end_cb reached\n");

    /* Direct CSV dump straight from the summary: reliable on serial regardless
     * of LVGL log level / perf-monitor plumbing. Note summary_create() already
     * decremented measurement_cnt for scenes with cnt > 1, and the *_avg fields
     * are raw sums to be divided by the (decremented) cnt. */
    printf("[lvgl_bench] valid_scene_cnt=%d\n", (int)summary->valid_scene_cnt);
    printf("Name, Avg. CPU, Avg. FPS, Avg. time (render + flush)\n");

    for (size_t i = 0; summary->scenes[i].create_cb != NULL; i++) {
        const lv_demo_benchmark_scene_dsc_t * s = &summary->scenes[i];
        const uint32_t cnt = s->measurement_cnt > 1 ? s->measurement_cnt : 0;

        if (cnt == 0) {
            printf("%s, N/A (cnt=%u)\n", s->name, (unsigned)s->measurement_cnt);
        } else {
            const uint32_t render = s->render_avg_time / cnt;
            const uint32_t flush = s->flush_avg_time / cnt;
            printf("%s, %u%%, %u FPS, %u ms (%u + %u)\n",
                   s->name,
                   (unsigned)(s->cpu_avg_usage / cnt),
                   (unsigned)(s->fps_avg / cnt),
                   (unsigned)(render + flush),
                   (unsigned)render,
                   (unsigned)flush);
        }
    }
    fflush(stdout);

#if LV_USE_SIFLI_EPIC
    lv_sifli_epic_dump_stats();
    fflush(stdout);
#endif

    lv_demo_benchmark_summary_display(summary);

    printf("[lvgl_bench] summary displayed\n");
    fflush(stdout);

    LV_LOG_INFO("========== lvgl_bench finished ==========");
    LV_LOG_INFO("Search serial log for 'Benchmark Summary' CSV (Name, Avg. CPU, Avg. FPS, ...)");
    syslog(LOG_INFO, "LVGL_BENCH_DONE\n");

#if LV_USE_PERF_MONITOR
    /* Summary screen is static; stop 300ms perf timer (no more sysmon spam). */
    lv_sysmon_performance_pause(lv_display_get_default());
#endif
}

#if LV_USE_PERF_MONITOR
/* Drop the display-layer sysmon label observer (LOG_MODE prints to serial).
 * Keep perf_sysmon_backend timer + subject for lv_demo_benchmark stats. */
/** @brief 隐藏屏上 sysmon 标签，保留后台计时。 */
static void bench_silence_display_sysmon(lv_display_t * disp)
{
    if (disp == NULL) {
        return;
    }

    lv_sysmon_hide_performance(disp);
    if (disp->perf_label != NULL) {
        lv_obj_delete(disp->perf_label);
        disp->perf_label = NULL;
    }
}
#endif

/** @brief 轮询等待显示设备就绪。 */
static bool wait_for_display(const char * path, int timeout_ms)
{
    const int step_ms = 100;
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        int fd = open(path, O_RDWR);
        if (fd >= 0) {
            close(fd);
            LV_LOG_INFO("display ready: %s (%d ms)", path, elapsed);
            return true;
        }

        usleep(step_ms * 1000);
        elapsed += step_ms;
    }

    LV_LOG_ERROR("display not ready: %s (waited %d ms, errno=%d)",
                 path, timeout_ms, errno);
    return false;
}

/**
 * @brief NSH 命令 lvgl_bench：LVGL 官方 benchmark 压测。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int main(int argc, char * argv[])
{
    lv_nuttx_dsc_t info;
    lv_nuttx_result_t result;
    bool fast = false;
    bool spin = false;
    bool cache_filter = true;

    for (int argi = 1; argi < argc; argi++) {
        if (strcmp(argv[argi], "fast") == 0) fast = true;
        else if (strcmp(argv[argi], "spin") == 0) spin = true;
        else if (strcmp(argv[argi], "--cache=legacy") == 0) cache_filter = false;
        else if (strcmp(argv[argi], "--cache=filtered") == 0) cache_filter = true;
        else {
            fprintf(stderr, "Usage: lvgl_bench [fast|spin] [--cache=legacy|--cache=filtered]\n");
            return 1;
        }
    }
    if (fast && spin) {
        fprintf(stderr, "lvgl_bench: choose fast or spin\n");
        return 1;
    }

    if (lv_is_initialized()) {
        LV_LOG_ERROR("LVGL already initialized");
        return 1;
    }

#if !LV_USE_DEMO_BENCHMARK
    LV_LOG_ERROR("Rebuild with CONFIG_LV_USE_DEMO_BENCHMARK=y");
    return 1;
#endif

    lv_init();

#if LV_USE_MYVENDOR_LCD_DISP || LV_USE_NUTTX_LCD
    if (!wait_for_display(LVGL_BENCH_LCD_PATH, LVGL_BENCH_LCD_WAIT_MS)) {
        return 1;
    }

    lv_nuttx_dsc_init(&info);
    info.fb_path = LVGL_BENCH_LCD_PATH;
#else
    if (!wait_for_display(LVGL_BENCH_FBDEV_PATH, LVGL_BENCH_FBDEV_WAIT_MS)) {
        return 1;
    }

    lv_nuttx_dsc_init(&info);
    info.fb_path = LVGL_BENCH_FBDEV_PATH;
#endif

    lv_nuttx_init(&info, &result);

    if (result.disp == NULL) {
        syslog(LOG_ERR, "lvgl_bench: lv_nuttx_init failed (disp=NULL)");
        LV_LOG_ERROR("lv_nuttx_init failed");
        return 1;
    }

#if defined(CONFIG_MYVENDOR_LCD_DISP) && CONFIG_MYVENDOR_LCD_DISP
    if (myvendor_lcd_disp_register_done_cb(bench_lcd_flush_done, result.disp) < 0) {
        syslog(LOG_ERR, "lvgl_bench: register_done_cb failed");
        LV_LOG_ERROR("myvendor_lcd_disp_register_done_cb failed");
        return 1;
    }

    if (fast) {
        extern void lv_myvendor_lcd_set_skip_flush(bool skip);

        lv_myvendor_lcd_set_skip_flush(true);
#if LV_USE_PERF_MONITOR
        /* Not bound by the 16ms refresh tick: render as fast as possible. */
        if (result.disp->refr_timer != NULL) {
            result.disp->refr_timer->period = 1;
        }
#endif
        printf("[lvgl_bench] MODE=fast (skip LCD flush, refr period 1ms)\n");
    } else if (spin) {
        printf("[lvgl_bench] MODE=spin (LCD flush on, refr 16ms, busy-wait pacing)\n");
    } else {
        printf("[lvgl_bench] MODE=normal (with LCD flush, refr 16ms)\n");
    }
    fflush(stdout);
#endif

#if LV_USE_PERF_MONITOR
    bench_silence_display_sysmon(result.disp);
#endif

    syslog(LOG_INFO, "lvgl_bench: starting benchmark (EPIC=%d)",
           (int)LV_USE_SIFLI_EPIC);
    LV_LOG_INFO("Starting lv_demo_benchmark (EPIC=%d, PERF_MONITOR=%d)",
                (int)LV_USE_SIFLI_EPIC,
                (int)LV_USE_PERF_MONITOR);

#if LV_USE_SIFLI_EPIC
    lv_epic_cache_bench_reset(cache_filter);
#endif
    syslog(LOG_INFO, "LVGL_BENCH_BEGIN cache=%s opt=%s refresh_ms=%d\n",
           cache_filter ? "filtered" : "legacy", CONFIG_LV_OPTLEVEL,
           LV_DEF_REFR_PERIOD);
    lv_demo_benchmark_set_end_cb(bench_on_end);
    lv_demo_benchmark();

    if (fast) {
        /* LVGL's animation engine runs on its own LV_DEF_REFR_PERIOD (16ms)
         * timer; it -- not the display refr timer -- is what bounds scene
         * invalidation, so lift it as well for a true render-rate ceiling. */
        lv_timer_t * anim_timer = lv_anim_get_timer();

        if (anim_timer != NULL) {
            anim_timer->period = 1;
            printf("[lvgl_bench] fast: anim tick period 1ms (was %d)\n",
                   (int)LV_DEF_REFR_PERIOD);
            printf("[lvgl_bench] fast: periods refr=%u anim=%u\n",
                   (unsigned)result.disp->refr_timer->period,
                   (unsigned)anim_timer->period);
            fflush(stdout);
        }
    }

    LV_LOG_INFO("Benchmark running (~90s). No touch needed; CSV prints to serial when done.");

    while (1) {
        /* 本命令独占 UI 位置（bicycle 未起）：像 bicycle 主循环一样喂狗。
         * 否则 bench 满载 ~90 s 期间 idle/work beat 过期，看门狗 15 s 复位。 */
#if defined(CONFIG_MYVENDOR_WATCHDOG) && CONFIG_MYVENDOR_WATCHDOG
        myvendor_watchdog_ui_beat();
        myvendor_watchdog_busy_pump();
#endif
        uint32_t idle = lv_timer_handler();

        /* LV_NO_TIMER_READY(0xFFFFFFFF) * 1000 wraps to ~71 min in usleep:
         * clamp so the loop keeps polling after the demo's last timer dies. */
        if (idle == LV_NO_TIMER_READY) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                printf("[lvgl_bench] no LVGL timer ready (demo finished or stalled)\n");
                fflush(stdout);
            }
            idle = 1000;
        }
        if (idle > 1000) idle = 1000;

        if (fast) {
            /* NuttX tick is 10ms: usleep(1ms) really sleeps ~10-20ms and
             * throttles the loop (and every LVGL timer) to ~50Hz.  Spin so
             * the 1ms refr/anim timers are serviced at their real period.
             * Once the demo is over, stop spinning so lower-priority services
             * (BLE/diag) are not starved for the rest of the capture. */
            if (!s_bench_done) {
                continue;
            }
        }

        if (spin) {
            /* Busy-wait pacing: usleep(idle) rounds up by one 10ms NuttX
             * tick (8ms -> ~20ms), so light scenes cap at ~33 FPS.  Spin on
             * lv_tick_elaps() instead to service the 16ms refr timer without
             * oversleeping.  Keep the watchdog fed while spinning; stop once
             * the demo is done so lower-priority services are not starved. */
            if (!s_bench_done) {
                if (idle) {
                    uint32_t t0 = lv_tick_get();

                    while (lv_tick_elaps(t0) < idle) {
#if defined(CONFIG_MYVENDOR_WATCHDOG) && CONFIG_MYVENDOR_WATCHDOG
                        myvendor_watchdog_ui_beat();
                        myvendor_watchdog_busy_pump();
#endif
                    }
                }
                continue;
            }
        }

        usleep(idle ? idle * 1000 : 1000);
    }

    /* not reached */
}
