/**
 * @file vmap_view.c
 * @brief 地图视图：瓦片泵 + 画布叠层（底图 → 导航线 → 标签 → REC 线 → REC 数字）。
 */

#include "vmap_view.h"

#include "board_psram_layout.h"
#include "board_malloc.h"   /* board_psram_canvas_arena() 的原型（原先靠隐式声明） */
#include "vmap_config.h"
#include "vmap_label.h"
#include "vmap_blit.h"
#include "vmap_render.h"
#include "vmap_tile_cache.h"
#include "vmap_tile_system.h"
#include "vmap_alloc.h"
#include "vmap_style.h"
#include "myvendor_epic_blit.h"
#include "myvendor_system_font.h"
#if VMAP_TRACK_ENABLE
#include "vmap_track.h"
#endif
#if VMAP_ROUTE_ENABLE
#include "vmap_route.h"
#endif
#if defined(CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE) && CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE
#include "bicycle_inval_probe.h"
#endif
#include "bicycle_c_debug.h"
#include <myvendor_mtp_lfs.h>
#include "myvendor_devctl.h"
#include "myvendor_watchdog.h"
#include "myvendor_schedmon.h"
#include "sf32lb_dvfs.h"
#include <limits.h>
#include <math.h>
#include <nuttx/clock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#define VMAP_PAN_MIN_STEP_PX  1

static void render_begin(vmap_view_t * view, bool reset_view, bool force_clear,
    bool full_restamp);
static void view_apply_center(vmap_view_t * view, double lon, double lat);
static void vmap_center_icon_apply(vmap_view_t * view);
static void vmap_view_apply_canvas_pos(vmap_view_t * view, double dx, double dy);

static void dvfs_map_busy(bool busy)
{
    if (busy) {
        sf32lb_dvfs_hold(SF32LB_DVFS_HOLD_MAP);
    } else {
        sf32lb_dvfs_release(SF32LB_DVFS_HOLD_MAP);
    }
}

/*
 * 渲染分段计时（只加观测，不改任何行为；置 0 即整体消失）。
 *
 * 时间基准用 DWT_CYCCNT，不用 tick：`CONFIG_USEC_PER_TICK=10000` 只有 10 ms
 * 分辨率，而单段常常是亚毫秒级。DWT 的解锁/自检写法照抄 services/myvendor_gnss.c
 * （Armv8-M 的 DWT 是锁访问的，两个 LAR 都写；CYCCNT 是全系统共享的，
 * **不要清零**，chips/sf32lb52/myvendor_idle_stat.c 正在用它统计 WFI 占比）。
 *
 * ⚠ **一律只报原始周期数，不在这里换算成微秒。** `SystemCoreClock` 是被
 * `sifli_systick_reclock()` 按 SysTick 基准赋的值（实测 24 MHz），**不等于 CPU
 * 核心频率**，拿它换算会差一个未知倍数。所以每帧额外打一个 `tickd`（本帧跨了
 * 几个 10 ms tick，由 `clock_systime_ticks()` 得到）：帧内 CPU 是忙的，
 * `cyc_tot / (tickd × 10 ms)` 就是 CYCCNT 的真实频率，可以据此把全部数字反解成
 * 微秒，且不依赖任何时钟树假设。`hz` 只作为 SystemCoreClock 的对照值保留。
 */
#define VMAP_PERF_PROBE 1

/** 多边形定点自测（'[vperf] polyself'）—— 默认关：一次 ~0.5 s。 */
#ifndef VMAP_RENDER_PRIM_SELF
#define VMAP_RENDER_PRIM_SELF 0
#endif

#if VMAP_PERF_PROBE
#define VMAP_PERF_DWT_DEMCR   (*(volatile uint32_t *)0xe000edfcu)
#define VMAP_PERF_DWT_CTRL    (*(volatile uint32_t *)0xe0001000u)
#define VMAP_PERF_DWT_CYCCNT  (*(volatile uint32_t *)0xe0001004u)
#define VMAP_PERF_DWT_LAR     (*(volatile uint32_t *)0xe0001fb0u)
#define VMAP_PERF_ITM_LAR     (*(volatile uint32_t *)0xe0000fb0u)
#define VMAP_PERF_LAR_KEY     0xc5acce55u
/** @brief 同一标签最快多久打一行。 */
#define VMAP_PERF_LOG_GAP_MS  2000u
/** @brief 单拍泵调用间隔超过它 ⇒ **无条件** WARNING（见 SLOW gap 的注释）。 */
#define VMAP_PERF_SLOW_GAP_MS   500u
/** @brief 单帧超过它 ⇒ **无条件** WARNING。 */
#define VMAP_PERF_SLOW_FRAME_MS 3000u

/** SoC 缓存控制器（见 chips/drivers/cmsis/sf32lb52x/cache.h）。 */
#define VMAP_PERF_CACHE_CCR    (*(volatile uint32_t *)0xe0080000u)
#define VMAP_PERF_CACHE_IMCR   (*(volatile uint32_t *)0xe0080004u)
#define VMAP_PERF_CACHE_IACR   (*(volatile uint32_t *)0xe0080008u)
#define VMAP_PERF_CACHE_DMCR   (*(volatile uint32_t *)0xe0080010u)
#define VMAP_PERF_CACHE_DACR   (*(volatile uint32_t *)0xe0080014u)
#define VMAP_PERF_CACHE_CNTEN  (1u << 0)
#define VMAP_PERF_CACHE_CNTCLR (1u << 1)

extern uint32_t SystemCoreClock;

/*
 * 屏幕**实际重绘**帧率统计（诊断用，默认不打印）。
 *
 * 取 LVGL 的 LV_EVENT_RENDER_START / RENDER_READY：`refr_invalid_areas()` 在
 * `inv_p == 0` 时直接返回，所以这对事件只包住"真的画了东西"的那一段（含所有
 * 条带与 flush），不会把空转的 16 ms 刷新节拍算进来。
 *
 * 为什么不用 LV_EVENT_REFR_START/READY：那一对在无脏区时也会发，fps 恒等于
 * 刷新定时器频率，没有意义。
 *
 * 打印级别是 LOG_DEBUG ⇒ 默认静默；要看时 `ctl log all`（看完 `ctl log notice`）。
 */
static struct {
    uint32_t t0;
    uint32_t n;
    uint32_t sum;
    uint32_t max;
    uint32_t win;
    /* 整次刷新（REFR_START..REFR_READY）与其中"画"的部分（RENDER_*）分开记：
     * 两者之差 = layout/失效合并/任务派发/flush 等待，是刷新管线的固定开销。 */
    uint32_t refr_t0;
    uint32_t refr_sum;
    uint32_t refr_n;
    /* 只统计**真的画了**的刷新：REFR_START/REFR_READY 在无脏区时也会发
     * （见 lv_refr.c），这里用 RENDER_START 置位、REFR_READY 消费并清零，
     * 把空转的刷新节拍剔掉，refr 平均值才不被稀释。 */
    bool rendered;
} s_fps;
static bool s_fps_hooked;

static void vmap_fps_probe_cb(lv_event_t * e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    uint32_t now = lv_tick_get();
    uint32_t dt;
    uint32_t win;

    if (code == LV_EVENT_REFR_START) {
        s_fps.refr_t0 = now;
        return;
    }
    if (code == LV_EVENT_REFR_READY) {
        /* 只统计真的画了的刷新（rendered 由 RENDER_START 置位）。 */
        if (s_fps.rendered) {
            s_fps.refr_sum += lv_tick_elaps(s_fps.refr_t0);
            s_fps.refr_n++;
            s_fps.rendered = false;
        }
        return;
    }
    if (code == LV_EVENT_RENDER_START) {
        s_fps.rendered = true;
        s_fps.t0 = now;
        return;
    }
    if (code != LV_EVENT_RENDER_READY) {
        return;
    }

    dt = lv_tick_elaps(s_fps.t0);
    if (s_fps.win == 0u) {
        s_fps.win = now;
    }
    s_fps.n++;
    s_fps.sum += dt;
    if (dt > s_fps.max) {
        s_fps.max = dt;
    }

    win = lv_tick_elaps(s_fps.win);
    if (win < 2000u) {
        return;
    }
    if (s_fps.n > 0u) {
        /* fps 用 x10 定点：n * 10000 / win。refr=整次刷新，ovh=刷新管线开销。 */
        const uint32_t refr = (s_fps.refr_n > 0u) ? (s_fps.refr_sum / s_fps.refr_n) : 0u;
        const uint32_t draw = s_fps.sum / s_fps.n;

        syslog(LOG_DEBUG,
            "[mapfps] n=%u fps=%u.%u avg=%ums max=%ums refr=%ums ovh=%ums win=%ums",
            (unsigned)s_fps.n,
            (unsigned)(s_fps.n * 10000u / win / 10u),
            (unsigned)(s_fps.n * 10000u / win % 10u),
            (unsigned)draw,
            (unsigned)s_fps.max,
            (unsigned)refr,
            (unsigned)((refr > draw) ? (refr - draw) : 0u),
            (unsigned)win);
    }
    s_fps.n = 0u;
    s_fps.sum = 0u;
    s_fps.max = 0u;
    s_fps.refr_sum = 0u;
    s_fps.refr_n = 0u;
    /* 不清 `rendered`：本次刷新的 REFR_READY 就在本回调返回之后才到，
     * 提前清掉会把每个窗口的最后一个样本丢掉（样本数与 draw 对不上）。 */
    s_fps.win = now;
}

static bool s_pf_ready;
static bool s_pf_usable;
static bool s_pf_in_frame;
static uint32_t s_pf_t0;
static clock_t s_pf_tick0;
static uint32_t s_pf_begin;
static uint32_t s_pf_warm;
static uint32_t s_pf_tile;
static uint32_t s_pf_nav;
static uint32_t s_pf_label;
static uint32_t s_pf_track;
/* stamp_overlays 也可能在没有帧的情况下被调（导航进度刷新），单独记一份。 */
static uint32_t s_pf_s_nav;
static uint32_t s_pf_s_track;
static uint32_t s_pf_n_tiles;
static uint32_t s_pf_n_warms;
static uint32_t s_pf_n_labels;
static uint32_t s_pf_last_log;
static uint32_t s_pf_last_stamp;
/** @brief 上一次泵调用的时间戳（判"UI 线程被占住多久"）。 */
static uint32_t s_pf_last_pump_cyc;
/* 0b：SoC 缓存控制器的命中/miss 计数（按帧取增量）。 */
static uint32_t s_pf_iacc0;
static uint32_t s_pf_imiss0;
static uint32_t s_pf_dacc0;
static uint32_t s_pf_dmiss0;
/* 0a：本帧瓦片光栅化发出的 store 次数与字节数。 */
static uint32_t s_pf_stores;
static uint32_t s_pf_bytes;
/* 使用过程里最大的两块：render_begin 内部（scroll/label-scroll/invalidate）
 * 与泵里"非脏瓦片"那一支（cache 查找 + 解析 + 标签候选收集）。 */
static uint32_t s_pf_scroll;
static uint32_t s_pf_lscroll;
static uint32_t s_pf_inval;
static uint32_t s_pf_meta;
/* 搬运路径计数（单跳 / 两跳 / CPU）的本帧增量 —— 把 scr 归到具体路径上。 */
static uint32_t s_pf_hop1;
static uint32_t s_pf_hop2;
static uint32_t s_pf_cpuc;
static uint32_t s_pf_hop0[3];   /**< 帧起点的三个计数。 */
/* 【临时取证】图元级拆分：帧起点快照 + 本帧增量（见 vmap_render.c 的 PRIM 块）。 */
static vmap_render_prim_t s_prim0;
static vmap_render_prim_t s_prim;

static void vmap_perf_psram_probe(void);
static void vmap_perf_dump_runnable(void);
static void vmap_perf_cache_probe(void);

static void vmap_perf_init_once(void)
{
    uint32_t a;
    uint32_t b;
    int i;

    if (s_pf_ready) {
        return;
    }
    s_pf_ready = true;

    VMAP_PERF_ITM_LAR = VMAP_PERF_LAR_KEY;
    VMAP_PERF_DWT_LAR = VMAP_PERF_LAR_KEY;
    VMAP_PERF_DWT_DEMCR |= (1u << 24);   /* TRCENA */
    VMAP_PERF_DWT_CTRL |= 1u;            /* CYCCNTENA */

    a = VMAP_PERF_DWT_CYCCNT;
    for (i = 0; i < 64; i++) {
        __asm__ volatile("nop");
    }
    b = VMAP_PERF_DWT_CYCCNT;
    s_pf_usable = (b != a);

    if (!s_pf_usable) {
        syslog(LOG_WARNING, "[vperf] DWT CYCCNT 不可用，分段计时停用\n");
    } else {
        vmap_perf_psram_probe();
        vmap_perf_cache_probe();
#if VMAP_RENDER_PRIM_SELF
        {
            /* 多边形定点自测：确定性输入，两个固件的数字可直接对减 */
            vmap_render_polyself_t st;

            vmap_render_polyselftest(&st);
            syslog(LOG_INFO,
                "[vperf] polyself pass=%u fpcyc=%u fpcross=%u intcyc=%u intcross=%u",
                (unsigned)st.pass, (unsigned)st.fp_cyc, (unsigned)st.fp_cross,
                (unsigned)st.in_cyc, (unsigned)st.in_cross);
        }
#endif
    }
}

static inline uint32_t vmap_perf_cyc(void)
{
    return VMAP_PERF_DWT_CYCCNT;
}

static void vmap_perf_frame_begin(void)
{
    if (!s_pf_usable) {
        return;
    }
    s_pf_t0 = vmap_perf_cyc();
    s_pf_tick0 = clock_systime_ticks();
    /* 帧起点作废泵间隔基准，避免把帧间空闲算成帧内停顿。 */
    s_pf_last_pump_cyc = 0u;
    s_pf_begin = 0u;
    s_pf_warm = 0u;
    s_pf_tile = 0u;
    s_pf_nav = 0u;
    s_pf_label = 0u;
    s_pf_track = 0u;
    s_pf_n_tiles = 0u;
    s_pf_n_warms = 0u;
    s_pf_n_labels = 0u;
    s_pf_stores = 0u;
    s_pf_bytes = 0u;
    s_pf_scroll = 0u;
    s_pf_lscroll = 0u;
    s_pf_inval = 0u;
    s_pf_meta = 0u;
    s_pf_hop1 = 0u;
    s_pf_hop2 = 0u;
    s_pf_cpuc = 0u;
    vmap_blit_stats(vmap_blit_get(), &s_pf_hop0[0], &s_pf_hop0[1],
        &s_pf_hop0[2]);
    s_pf_iacc0 = VMAP_PERF_CACHE_IACR;
    s_pf_imiss0 = VMAP_PERF_CACHE_IMCR;
    s_pf_dacc0 = VMAP_PERF_CACHE_DACR;
    s_pf_dmiss0 = VMAP_PERF_CACHE_DMCR;
    vmap_render_prim_get(&s_prim0);
    s_pf_in_frame = true;
}

static void vmap_perf_frame_end(uint32_t zoom, uint32_t dirty)
{
    uint32_t total;
    uint32_t now;
    uint32_t tickd;
    uint32_t iacc;
    uint32_t imiss;
    uint32_t dacc;
    uint32_t dmiss;
    uint32_t cps;

    if (!s_pf_usable || !s_pf_in_frame) {
        return;
    }
    s_pf_in_frame = false;

    total = vmap_perf_cyc() - s_pf_t0;
    tickd = (uint32_t)(clock_systime_ticks() - s_pf_tick0);
    iacc = VMAP_PERF_CACHE_IACR - s_pf_iacc0;
    imiss = VMAP_PERF_CACHE_IMCR - s_pf_imiss0;
    dacc = VMAP_PERF_CACHE_DACR - s_pf_dacc0;
    dmiss = VMAP_PERF_CACHE_DMCR - s_pf_dmiss0;
    cps = (s_pf_stores != 0u) ? (s_pf_tile / s_pf_stores) : 0u;
    {
        uint32_t h1 = 0u;
        uint32_t h2 = 0u;
        uint32_t hc = 0u;

        vmap_blit_stats(vmap_blit_get(), &h1, &h2, &hc);
        s_pf_hop1 = h1 - s_pf_hop0[0];
        s_pf_hop2 = h2 - s_pf_hop0[1];
        s_pf_cpuc = hc - s_pf_hop0[2];
    }
    now = lv_tick_get();
    if (s_pf_last_log != 0u && lv_tick_elaps(s_pf_last_log) < VMAP_PERF_LOG_GAP_MS) {
        return;
    }
    s_pf_last_log = now;

    /* 全部是原始周期数。tickd 就是毫秒（CONFIG_USEC_PER_TICK=1000）；
     * cyc_tot/tickd ≈ HCLK/1000，可自校准（cycms 即 cyc_tot/tickd）。
     * dirty 是这一帧的脏区条数：0 = 全量重画，>0 = scroll 补边。
     * st/by = 瓦片光栅化发出的 store 次数与字节数；cps = cyc_tile/st —— 判成本模型：
     *   cps ≈ 88  ⇒ 每次 store 一次总线事务（成本 ∝ store 次数）
     *   cyc_tile/(by/4) ≈ 88 而 cps 不成 ... 见 docs/map/perf/MEASUREMENTS.md
     * i/d = SoC 缓存的 access/miss 增量（判取指命中率）。 */
    /* 超长帧无条件 WARNING；正常帧留 INFO（用户 2026-09-26）。 */
    syslog((total > (VMAP_PERF_SLOW_FRAME_MS * 240u * 1000u)) ? LOG_WARNING : LOG_INFO,
        "[vperf] frame z=%u dirty=%u tickd=%u cycms=%u cyc_begin=%u cyc_warm=%u cyc_tile=%u cyc_nav=%u cyc_label=%u cyc_track=%u cyc_tot=%u tiles=%u warms=%u labels=%u st=%u by=%u cps=%u i=%u/%u d=%u/%u scr=%u lscr=%u inv=%u meta=%u hop1=%u hop2=%u cpuc=%u hz=%u",
        (unsigned)zoom, (unsigned)dirty, (unsigned)tickd,
        (unsigned)((tickd != 0u) ? (total / tickd) : 0u),
        (unsigned)s_pf_begin, (unsigned)s_pf_warm,
        (unsigned)s_pf_tile, (unsigned)s_pf_nav,
        (unsigned)s_pf_label, (unsigned)s_pf_track,
        (unsigned)total,
        (unsigned)s_pf_n_tiles, (unsigned)s_pf_n_warms,
        (unsigned)s_pf_n_labels,
        (unsigned)s_pf_stores, (unsigned)s_pf_bytes, (unsigned)cps,
        (unsigned)iacc, (unsigned)imiss, (unsigned)dacc, (unsigned)dmiss,
        (unsigned)s_pf_scroll, (unsigned)s_pf_lscroll,
        (unsigned)s_pf_inval, (unsigned)s_pf_meta,
        (unsigned)s_pf_hop1, (unsigned)s_pf_hop2, (unsigned)s_pf_cpuc,
        (unsigned)SystemCoreClock);

    /* 【临时取证】cyc_tile 按图元拆开：多边形 / 面填充 / 圆盘，外加两个工作量
     * 计数（cross = 逐行软除次数，scan = 行×边 扫描次数）。测完即撤。 */
    vmap_render_prim_get(&s_prim);
    syslog(LOG_DEBUG,
        "[vperf] prim polyfp=%u npfp=%u poly=%u np=%u disc=%u nd=%u cross=%u scan=%u h=%u,%u,%u,%u,%u,%u cn=%u,%u,%u,%u,%u,%u",
        (unsigned)(s_prim.poly_fp - s_prim0.poly_fp),
        (unsigned)(s_prim.n_poly_fp - s_prim0.n_poly_fp),
        (unsigned)(s_prim.poly - s_prim0.poly),
        (unsigned)(s_prim.n_poly - s_prim0.n_poly),
        (unsigned)(s_prim.disc - s_prim0.disc),
        (unsigned)(s_prim.n_disc - s_prim0.n_disc),
        (unsigned)(s_prim.cross - s_prim0.cross),
        (unsigned)(s_prim.scan - s_prim0.scan),
        (unsigned)s_prim.hist[0], (unsigned)s_prim.hist[1],
        (unsigned)s_prim.hist[2], (unsigned)s_prim.hist[3],
        (unsigned)s_prim.hist[4], (unsigned)s_prim.hist[5],
        (unsigned)s_prim.cyc_n[0], (unsigned)s_prim.cyc_n[1],
        (unsigned)s_prim.cyc_n[2], (unsigned)s_prim.cyc_n[3],
        (unsigned)s_prim.cyc_n[4], (unsigned)s_prim.cyc_n[5]);
}

static void vmap_perf_stamp_end(uint32_t zoom)
{
    uint32_t now;

    if (!s_pf_usable || s_pf_in_frame) {
        return;
    }
    if (s_pf_s_nav == 0u && s_pf_s_track == 0u) {
        return;
    }
    now = lv_tick_get();
    if (s_pf_last_stamp != 0u
        && lv_tick_elaps(s_pf_last_stamp) < VMAP_PERF_LOG_GAP_MS) {
        s_pf_s_nav = 0u;
        s_pf_s_track = 0u;
        return;
    }
    s_pf_last_stamp = now;

    syslog(LOG_DEBUG, "[vperf] stamp z=%u cyc_nav=%u cyc_track=%u hz=%u",
        (unsigned)zoom, (unsigned)s_pf_s_nav,
        (unsigned)s_pf_s_track, (unsigned)SystemCoreClock);
    s_pf_s_nav = 0u;
    s_pf_s_track = 0u;
}
/*
 * PSRAM 通路自检（开机一次，打一行）。
 *
 * 目的：把"渲染为什么慢"拆成**每次访问的固定开销**与**字节速率**两件事。
 * 手法：同样大小，分别用 4 字节逐字写、32 字节块写（stmia）、4 字节逐字读各跑
 * 一遍，比周期数。判据（blk_w / word_w 的百分比）：
 *
 *   ≈ 12%  ⇒ 写通路会把块写合并成突发：`vmap_render.c` 的 32 字节块写
 *            （fill_run_u16 / vmap_render_clear）已经赚到；
 *   ≈ 100% ⇒ 写通路不合并、时间花在**字节**上：块写白做（该回滚），真正的
 *            杠杆是减少要写的字节数，而不是换存储方式。
 *
 * 读一遍只作参照；注意它可能部分命中 D-cache，所以读数字是下界。
 */
#define VMAP_PERF_PSRAM_BYTES (128 * 1024)

typedef struct {
    uint32_t w[8];
} vmap_perf_blk32_t;

static void vmap_perf_psram_probe(void)
{
    uint8_t * buf = (uint8_t *)vmap_malloc(VMAP_PERF_PSRAM_BYTES);
    vmap_perf_blk32_t blk;
    uint32_t c0;
    uint32_t c_word;
    uint32_t c_blk;
    uint32_t c_read;
    uint32_t sum = 0;
    int i;

    if (buf == NULL) {
        syslog(LOG_WARNING, "[vperf] psram probe: 分配失败，跳过\n");
        return;
    }
    for (i = 0; i < 8; i++) {
        blk.w[i] = 0xa5a5a5a5u;
    }

    memset(buf, 0, VMAP_PERF_PSRAM_BYTES);   /* 先热一遍，别把首次触碰算进去 */

    c0 = vmap_perf_cyc();
    for (i = 0; i < VMAP_PERF_PSRAM_BYTES; i += 4) {
        *(uint32_t *)(void *)(buf + i) = 0xa5a5a5a5u;
    }
    __asm__ volatile("" ::: "memory");
    c_word = vmap_perf_cyc() - c0;

    c0 = vmap_perf_cyc();
    for (i = 0; i < VMAP_PERF_PSRAM_BYTES; i += (int)sizeof(blk)) {
        *(vmap_perf_blk32_t *)(void *)(buf + i) = blk;
    }
    __asm__ volatile("" ::: "memory");
    c_blk = vmap_perf_cyc() - c0;

    c0 = vmap_perf_cyc();
    for (i = 0; i < VMAP_PERF_PSRAM_BYTES; i += 4) {
        sum += *(const uint32_t *)(const void *)(buf + i);
    }
    __asm__ volatile("" ::: "memory");
    c_read = vmap_perf_cyc() - c0;

    syslog(LOG_INFO,
        "[vperf] psram %uKB word_w=%u blk_w=%u word_r=%u cyc blk/w=%u%% sum=%u",
        (unsigned)(VMAP_PERF_PSRAM_BYTES / 1024), (unsigned)c_word,
        (unsigned)c_blk, (unsigned)c_read,
        (unsigned)(c_word != 0u ? (c_blk * 100u / c_word) : 0u), (unsigned)sum);

    vmap_free(buf);
}

static void vmap_perf_cache_probe(void)
{
    const uint32_t ccr = VMAP_PERF_CACHE_CCR;

    /* 实测原值：全树没有任何代码碰过这个外设（grep hwp_cache-> 零命中）。
     * 首次上板读到的是 **ccr=0x00000000（irange=0 drange=0）**，而且整帧
     * I/D 计数器都是 0 ⇒ 缓存既不覆盖代码也不覆盖数据，**代码是从 PSRAM 裸取指**。
     * 详见 docs/map/perf/MEASUREMENTS.md §2.6。 */
    syslog(LOG_INFO,
        "[vperf] cache ccr=0x%08x irange=%u drange=%u cnten=%u",
        (unsigned)ccr,
        (unsigned)((ccr >> 16) & 0x1fu), (unsigned)((ccr >> 24) & 0x1fu),
        (unsigned)(ccr & 1u));

    /* **只打开计数器，不改缓存行为。**
     *
     * 实验做过了（2026-09-25）：把 IRANGE 设成 MPI1（只缓存代码区、DRANGE 保持 0，
     * 因为代码只读、缓存只读数据没有一致性问题，而这个外设又没有 clean/invalidate
     * 接口 ⇒ 不该缓存数据）——结果是 **cyc_tile 90.56M vs 不缓存 89.69M，没有变化**，
     * 而计数器同时显示取指 miss 率只有 **0.068%**（391,198,596 次访问 / 266,570 次 miss）。
     * ⇒ **未开缓存时顺序预取已经把紧凑代码喂饱了，取指不是瓶颈**，所以不设 IRANGE。
     * 详见 docs/map/perf/MEASUREMENTS.md §2.6。 */
    VMAP_PERF_CACHE_CCR = (ccr & ~VMAP_PERF_CACHE_CNTCLR) | VMAP_PERF_CACHE_CNTEN
        | VMAP_PERF_CACHE_CNTCLR;
}

/**
 * @brief SLOW gap 时点名"谁在运行 / 谁就绪"。
 *
 * 形态 C（用户 2026-09-26 现场）缺的就是这一条：探针能说"停了 1.61 秒"，但说不出
 * 那 1.61 秒是谁占着 CPU。判据来自另两条探针的组合 —— 喂狗线程（prio 180）没报
 * late ⇒ 不是全局冻结/长时间关中断；而 idle 心跳陈旧 ⇒ CPU 从没空过 ⇒ 是某个
 * prio ≥ 100 的任务在持续空转。所以这里把 Ready/Running 的任务列出来点名
 * （`state`: 2=Ready 3=Running，与 coredump 快照的 `snap … st=` 同一口径）。
 */
static void vmap_perf_dump_runnable(void)
{
    struct myvendor_schedmon_snap_s snap;
    char buf[176];
    size_t n = 0u;
    uint16_t i;

    if (!myvendor_schedmon_snap_copy(&snap)) {
        syslog(LOG_WARNING, "[vperf] SLOW: schedmon 快照不可用\n");
        return;
    }

    for (i = 0u; i < snap.ntask && n + 24u < sizeof(buf); i++) {
        const struct myvendor_schedmon_task_s * t = &snap.task[i];
        const int w = snprintf(buf + n, sizeof(buf) - n, "%s%s(pid%d,pri%d)",
                               n ? "," : "", t->name, (int)t->pid, (int)t->pri);

        if (t->state != 2u && t->state != 3u) {
            continue;
        }
        if (w <= 0) {
            break;
        }
        n += (size_t)w;
    }

    syslog(LOG_WARNING, "[vperf] SLOW: 运行/就绪 = %s\n", n ? buf : "(无)");
}

/** 段首取时间戳；必须与 VMAP_PERF_T1 成对，且在同一层花括号里。 */
#define VMAP_PERF_T0()      uint32_t _pf_t0 = vmap_perf_cyc()
/** 嵌套用（避免与 T0 的 _pf_t0 互相遮蔽）。 */
#define VMAP_PERF_T0S()     uint32_t _pf_ts = vmap_perf_cyc()
#define VMAP_PERF_T1S(f)    do { s_pf_##f += (vmap_perf_cyc() - _pf_ts); } while (0)
/** 段尾累加这一段的周期数到 s_pf_<field>。 */
#define VMAP_PERF_T1(f)     do { s_pf_##f += (vmap_perf_cyc() - _pf_t0); } while (0)
/** 计数 +1 / +n。 */
#define VMAP_PERF_N(f)      do { s_pf_##f++; } while (0)
#define VMAP_PERF_NB(f, n)  do { s_pf_##f += (uint32_t)(n); } while (0)
/** nav/track 段在"帧内"与"无帧的 stamp"两种情形下写入不同的桶。 */
#define VMAP_PERF_NAV_SLOT()   (s_pf_in_frame ? &s_pf_nav : &s_pf_s_nav)
#define VMAP_PERF_TRACK_SLOT() (s_pf_in_frame ? &s_pf_track : &s_pf_s_track)
/** 段尾按指针累加（用于上面两个运行期才能定的桶）。 */
#define VMAP_PERF_T1P(p)    do { *(p) += (vmap_perf_cyc() - _pf_t0); } while (0)
#else
#define VMAP_PERF_T0()      uint32_t _pf_t0 = 0u
#define VMAP_PERF_T1(f)     do { (void)_pf_t0; } while (0)
#define VMAP_PERF_T1P(p)    do { (void)_pf_t0; (void)(p); } while (0)
#define VMAP_PERF_N(f)      do { } while (0)
#define VMAP_PERF_NB(f, n)  do { } while (0)
#define VMAP_PERF_NAV_SLOT()   ((uint32_t *)0)
#define VMAP_PERF_TRACK_SLOT() ((uint32_t *)0)
#endif

#if !VMAP_PERF_PROBE
#define vmap_perf_init_once()   do { } while (0)
#define vmap_perf_frame_begin() do { } while (0)
#define vmap_perf_frame_end()   do { } while (0)
#define vmap_perf_stamp_end()   do { } while (0)
#endif

bool vmap_boot_ll_ok(double lon, double lat)
{
    if (!isfinite(lon) || !isfinite(lat)) {
        return false;
    }

    if (fabs(lon) < 0.001 && fabs(lat) < 0.001) {
        return false;
    }

    if (lon < VMAP_POS_LON_MIN || lon > VMAP_POS_LON_MAX ||
        lat < VMAP_POS_LAT_MIN || lat > VMAP_POS_LAT_MAX) {
        return false;
    }

    return true;
}

void vmap_boot_lonlat(double *lon, double *lat)
{
    int32_t lon_e7 = 0;
    int32_t lat_e7 = 0;
    double last_lon;
    double last_lat;

    if (lon == NULL || lat == NULL) {
        return;
    }

    if (myvendor_devctl_last_pos_get(&lon_e7, &lat_e7)) {
        last_lon = (double)lon_e7 / 10000000.0;
        last_lat = (double)lat_e7 / 10000000.0;
        if (vmap_boot_ll_ok(last_lon, last_lat)) {
            *lon = last_lon;
            *lat = last_lat;
            return;
        }
    }

    *lon = VMAP_DEFAULT_LON;
    *lat = VMAP_DEFAULT_LAT;
}

struct vmap_view {
    lv_obj_t * canvas;
    /** @brief create 时的父容器（`set_viewport()` 的自动改尺寸对象）。 */
    lv_obj_t * parent;
    /** @brief 父容器是否跟着 `set_viewport()` 改尺寸（见同名 API）。 */
    bool parent_autosize;
    lv_obj_t * arrow_layer;
    lv_obj_t * arrow;
    /** @brief 中心图标图源；NULL = 不画（组件化 API 的"隐藏"语义）。 */
    const void * center_icon;
    void * cbuf;
    bool cbuf_arena; /**< cbuf 来自板级专属 arena（**不能 free**，见 board_psram_layout.h）。 */
    int32_t w;
    int32_t h;
    int32_t view_w;
    int32_t view_h;
    int32_t margin;
    uint32_t stride;
    double lon;
    double lat;
    /** @brief 最近一次"跟车点"（GNSS/回放）。自由视图里地图中心由用户拖动决定，
     *         跟车点只用于箭头定位；恢复跟车时它也是回中目标。 */
    double veh_lon;
    double veh_lat;
    double render_lon;
    double render_lat;
    /** @brief 已上屏的整数平移（画布相对视口的位移）。**显示值**：geo→屏幕、
     *         箭头、旋转 pivot 全用它，保证覆盖层与画布像素对齐。 */
    double pan_dx;
    double pan_dy;
    /** @brief 请求的平移（**浮点**，像素）。亚像素余量记在这里：只有上屏那一步
     *         取整；重锚时按 `lround(pan_rx)` 滚动已有像素、余下的小数留在新原点，
     *         于是"拖 200 px"就是 200 px，连拖 100 次也不累积误差。 */
    double pan_rx;
    double pan_ry;
    int zoom;
    double scale;
    bool follow_mode;
    int32_t last_arrow_angle10;
    /** @brief 箭头当前**显示**角度（0.1°，补间中间值可能未归一化）；INT32_MIN=未初始化。 */
    int32_t disp_arrow_angle10;
    /** @brief 显示偏置（y）：只影响画布与箭头的屏幕位置，不进跟车/rebase 数学。 */
    int32_t y_bias;
    int32_t last_pan_px;
    int32_t last_pan_py;
    /** @brief 航向朝上：画布按航向旋转显示（画布本身仍正北向上）。 */
    bool heading_up;
    /** @brief 航向是否已初始化（首个有效 course 之前不做低通）。 */
    bool heading_valid;
    /** @brief 低通后的航向（度，正北 0，顺时针）。 */
    double heading_deg;
    /** @brief 已下发给画布的旋转角（0.1°，顺时针，0..3599）。 */
    int32_t rot10;
    /** @brief 待应用的目标角（限频窗口内先记账，由 vmap_rot_service 合并重烘）。 */
    int32_t want_rot10;
    /** @brief 上一次整幅重烘的时刻（用于重烘限频）。 */
    uint32_t last_bake_ms;
    /** @brief 上一次整幅重烘**失败**的时刻；仅当 `rot_fail_valid` 为真时有效。 */
    uint32_t last_fail_ms;
    /** @brief 有失败记录（用独立标志，避免 `lv_tick_get()==0` 与"无记录"混淆）。 */
    bool rot_fail_valid;
    /** @brief R 可能被写坏：失败重烘期间**冻结跟车平移**，等一次成功重烘再恢复。 */
    bool rot_broken;
    /** @brief C 有帧外增量、还没烘进 R（限频窗口内先记账，由航向拍兜底服务）。 */
    bool rot_pending;
    /** @brief 航向朝上时也烘路名（A/B 用；默认关，文字会转成斜字）。 */
    bool rot_labels;
    /** @brief 航向朝上时的正立路名对象（不烘进画布）；位置见 rlx/rly（bake 时算）。 */
    lv_obj_t * rot_label[VMAP_ROT_LABEL_MAX];
    int32_t rlx[VMAP_ROT_LABEL_MAX];
    int32_t rly[VMAP_ROT_LABEL_MAX];
    uint8_t rot_label_n;
    /* ===== 航向朝上：屏幕对齐的旋转缓冲 R =====
     *
     * 画布 C 仍是**正北向上**的渲染目标；R 是"按 rot10 转好、屏幕对齐"的视图，
     * 由 EPIC 旋转烘焙（`myvendor_epic_rotate`）。跟车时只移动 R 的对象（便宜），
     * 不用每次平移都重转整幅 —— 这是 2026-10-27 那轮 3 fps 的根因。
     */
    uint16_t * rbuf;
    lv_obj_t * rcanvas;
    int32_t rw;
    int32_t rh;
    uint32_t rstride;
    int32_t r_margin;
    /** @brief R 烘焙时的跟车 pan（与 r_rot10 一起定义"R 里的内容是哪一帧"）。 */
    double r_pan_x;
    double r_pan_y;
    /** @brief R 烘焙时的旋转角。 */
    int32_t r_rot10;
    vmap_tile_cache_t * cache;
    vmap_point_t * scratch;
    /** @brief 道路要素索引缓冲（每张瓦片一次扫描用；创建时分配，全局绑定）。 */
    vmap_road_ref_t * road_refs;
    vmap_label_cand_t * labels;
    vmap_label_retained_t * label_retained;
    vmap_label_collector_t label_col;
    const lv_font_t * font;
    uint32_t map_rev;
    uint32_t render_rev;
    int32_t pump_tx;
    int32_t pump_ty;
    int32_t pump_tx_min;
    int32_t pump_ty_min;
    int32_t pump_tx_max;
    int32_t pump_ty_max;
    int pump_cx;
    int pump_cy;
    /** @brief 瓦片投影原点（**浮点** map 像素）。重锚保留亚像素余量，新补的边
     *         与滚动过来的旧像素在同一网格上，不会出现 1 px 接缝。 */
    double pump_cx_d;
    double pump_cy_d;
    uint8_t pump_phase;
    uint8_t restamp_pending;
    /** @brief 进入航向朝上后等整幅干净底图再切 R（防旧标签双影）。 */
    uint8_t heading_switch_pending;
    /** @brief 整幅干净底图已就绪，等一次成功的 R 烘焙再切屏。 */
    uint8_t heading_switch_ready;
    /** @brief 当前帧是否为 keep-pan 整幅重铺（帧末据此完成航向切换）。 */
    uint8_t restamp_full_active;
    uint8_t pump_need_region;
    uint8_t pump_dirty_n;
    int paint_zoom;
    double paint_scale;
    int32_t pump_dirty_x1[VMAP_RENDER_DIRTY_MAX];
    int32_t pump_dirty_y1[VMAP_RENDER_DIRTY_MAX];
    int32_t pump_dirty_x2[VMAP_RENDER_DIRTY_MAX];
    int32_t pump_dirty_y2[VMAP_RENDER_DIRTY_MAX];
    struct vmap_track * track;
#if VMAP_ROUTE_ENABLE
    struct vmap_route * route;
#endif
};

/**
 * @brief 航向补间 exec：只更新箭头角度与显示角记录。
 */
static void vmap_arrow_angle_exec(void * var, int32_t v)
{
    vmap_view_t * view = (vmap_view_t *)var;

    if (view == NULL || view->arrow == NULL) {
        return;
    }

    view->disp_arrow_angle10 = v;
    lv_img_set_angle(view->arrow, v);
}

/**
 * @brief 把箭头角度从当前显示值补间到目标角（0.1°）。
 * @param view       视图。
 * @param angle10    目标角，0..3599。
 * @param allow_anim false 时立即到位（首帧 / 隐藏后重新出现 / 正好掉头）。
 */
static void vmap_arrow_apply(vmap_view_t * view, int32_t angle10, bool allow_anim)
{
    int32_t cur;
    int32_t delta;
    lv_anim_t a;

    if (view == NULL || view->arrow == NULL) {
        return;
    }

    cur = view->disp_arrow_angle10;
    if (cur == INT32_MIN || !allow_anim) {
        lv_anim_delete(view, vmap_arrow_angle_exec);
        view->disp_arrow_angle10 = angle10;
        lv_img_set_angle(view->arrow, angle10);
        return;
    }

    /* 最短弧：结果 -1800..1799（同模空间，cur 未归一化也成立）。 */
    delta = ((angle10 - cur + 3600 + 1800) % 3600) - 1800;
    if (abs(delta) < 30 || abs(delta) >= 1800) {
        lv_anim_delete(view, vmap_arrow_angle_exec);
        view->disp_arrow_angle10 = angle10;
        lv_img_set_angle(view->arrow, angle10);
        return;
    }

    lv_anim_delete(view, vmap_arrow_angle_exec);
    lv_anim_init(&a);
    lv_anim_set_var(&a, view);
    lv_anim_set_values(&a, cur, cur + delta);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, vmap_arrow_angle_exec);
    lv_anim_start(&a);
}

static void tile_range(int32_t center_x, int32_t center_y, int32_t canvas_w,
    int32_t canvas_h, double scale, int32_t ring,
    int32_t * tx_min, int32_t * ty_min, int32_t * tx_max, int32_t * ty_max)
{
    const int32_t tile_size = 256;
    double half_w = (canvas_w / 2.0) / scale;
    double half_h = (canvas_h / 2.0) / scale;
    double pad = (double)ring * tile_size;
    double left = (double)center_x - half_w - pad;
    double top = (double)center_y - half_h - pad;
    double right = (double)center_x + half_w + pad;
    double bottom = (double)center_y + half_h + pad;

    *tx_min = (int32_t)(left / tile_size) - (left < 0 ? 1 : 0);
    *ty_min = (int32_t)(top / tile_size) - (top < 0 ? 1 : 0);
    *tx_max = (int32_t)(right / tile_size);
    *ty_max = (int32_t)(bottom / tile_size);
}

/**
 * @brief 把旋转缓冲 R 摆到基准位置再叠加跟车补偿 (sx, sy)。
 */
static void vmap_rot_place(vmap_view_t * view, int32_t sx, int32_t sy)
{
    int32_t i;

    if (view == NULL || view->rcanvas == NULL) {
        return;
    }

    lv_obj_set_pos(view->rcanvas,
        (lv_coord_t)(-view->r_margin + sx),
        (lv_coord_t)(-view->r_margin + sy + view->y_bias));

    /* 正立路名跟着 R 一起平移（基准位置是上一次 bake 时算的屏幕坐标）。
     * y_bias 统一在这里加，这样导航横幅出现/消失时它们和画布一起挪。 */
    for (i = 0; i < (int32_t)view->rot_label_n; i++) {
        lv_obj_set_pos(view->rot_label[i], (lv_coord_t)(view->rlx[i] + sx),
            (lv_coord_t)(view->rly[i] + sy + view->y_bias));
    }
}

/** @brief 标签颜色（与烘焙路径同一套 style）。 */
static uint32_t vmap_rot_label_color(uint8_t kind)
{
    const vmap_style_t * st = vmap_style_current();

    if (kind == VMAP_LABEL_PLACE) {
        return st->lbl_place;
    }
    if (kind == VMAP_LABEL_WATER) {
        return st->lbl_water;
    }
    if (kind == VMAP_LABEL_POI) {
        return st->lbl_poi;
    }
    return st->lbl_road;
}

/** 旋转模式参与排序的候选上限（正立标签只有 8 个槽，取 2 倍足够）。 */
#define VMAP_ROT_PICK_MAX (VMAP_ROT_LABEL_MAX * 2)
/** 正立标签之间的最小空隙（px）。 */
#define VMAP_ROT_LABEL_GAP 4

/**
 * @brief 正立标签的字号：与**正北（烘进底图）**同一标准。
 *
 * @details 正北路径（vmap_label.c）：地名 = 地图字体（16 px），水名/POI/路名 =
 *          `VMAP_LABEL_ROAD_PX`（12 px）。航向朝上原来一律用 `view->font`（16），
 *          于是切个朝向同一批路名就大一号，用户 2026-11-03 指出"两种字体大小不一样"。
 *          这里按同一口径取字：地名 16、其余 12（12 这个字号界面已在用，不占新槽）。
 */
#define VMAP_ROT_LABEL_ROAD_PX 12
static const lv_font_t * vmap_rot_label_font(const vmap_view_t * view,
    uint8_t kind)
{
    static const lv_font_t * s_road_font;
    static bool s_tried;

    if (kind == VMAP_LABEL_PLACE) {
        return view->font;
    }

    if (!s_tried) {
        s_road_font = myvendor_system_font_get(VMAP_ROT_LABEL_ROAD_PX);
        s_tried = true;
    }

    return s_road_font != NULL ? s_road_font : view->font;
}

typedef struct {
    int16_t idx;
    uint8_t tier;
    uint8_t prio;
    int32_t dist2;
    int32_t sx;
    int32_t sy;
} vmap_rot_pick_t;

/** @brief 旋转标签层级：地名/水名 → 路名 → POI（与烘焙路径一致）。 */
static uint8_t vmap_rot_label_tier(uint8_t kind)
{
    if (kind == VMAP_LABEL_ROAD) {
        return 1u;
    }
    if (kind == VMAP_LABEL_POI) {
        return 2u;
    }
    return 0u;
}

/** @brief 旋转标签优先级：层级 → prio 大 → 离屏幕中心近。 */
static bool vmap_rot_pick_better(const vmap_rot_pick_t * a,
    const vmap_rot_pick_t * b)
{
    if (a->tier != b->tier) {
        return a->tier < b->tier;
    }
    if (a->prio != b->prio) {
        return a->prio > b->prio;
    }
    return a->dist2 < b->dist2;
}

/**
 * @brief 把名字排成逐字竖列（UTF-8 字符之间插 '\n'，字形保持正立）。
 * @details 与正北烘图路径（vmap_label.c 的 UTF-8 换行）同一逻辑：按屏幕方向
 *          选择横排或竖排，**不旋转字形**，避免航向朝上出现斜字。
 * @return 竖排时返回 buf；横排或参数非法时返回 name。
 */
static const char * vmap_rot_label_text(const char * name, bool vertical,
    char * buf, size_t cap)
{
    const char * p;
    size_t o = 0u;

    if (!vertical || name == NULL || buf == NULL || cap < 8u) {
        return name;
    }

    for (p = name; *p != '\0';) {
        const unsigned char ch = (unsigned char)*p;
        size_t n = 1u;

        if (ch >= 0xF0u) {
            n = 4u;
        } else if (ch >= 0xE0u) {
            n = 3u;
        } else if (ch >= 0xC0u) {
            n = 2u;
        }

        if (o + n + 2u > cap) {
            break;
        }
        memcpy(buf + o, p, n);
        o += n;
        p += n;
        if (*p != '\0') {
            buf[o++] = '\n';
        }
    }
    buf[o] = '\0';
    return o > 0u ? buf : name;
}

static bool vmap_rot_boxes_overlap(const lv_area_t * a, const lv_area_t * b,    int32_t gap)
{
    return a->x1 - gap <= b->x2 && a->x2 + gap >= b->x1
        && a->y1 - gap <= b->y2 && a->y2 + gap >= b->y1;
}

/**
 * @brief 把本帧收集到的路名按"旋转后的屏幕位置"摆成正立文本对象。
 * @details 文本不烘进画布（否则会跟着地图转成斜字）；位置 = R 中心 +
 *          R(θ)·(画布点 - 骑手点)，再换算到 map_area 坐标。同名只留一条；
 *          按层级/prio/离中心近排序后逐个放置，整框出视口或与已放标签
 *          重叠的直接跳过，避免两个路名叠在一起。
 */
static void vmap_rot_labels_update(vmap_view_t * view, int32_t pivot_x,
    int32_t pivot_y)
{
    const double th = (double)view->rot10 * 0.1 * M_PI / 180.0;
    const double c = cos(th);
    const double s = sin(th);
    const int32_t cx = view->view_w / 2;
    const int32_t cy = view->view_h / 2;
    vmap_rot_pick_t pick[VMAP_ROT_PICK_MAX];
    lv_area_t placed[VMAP_ROT_LABEL_MAX];
    const char * taken[VMAP_ROT_LABEL_MAX];
    int32_t pick_n = 0;
    int32_t placed_n = 0;
    int32_t i;
    int32_t n = 0;

    if (!view->rot_labels || view->font == NULL || view->label_col.count <= 0) {
        for (i = 0; i < VMAP_ROT_LABEL_MAX; i++) {
            lv_obj_add_flag(view->rot_label[i], LV_OBJ_FLAG_HIDDEN);
        }
        view->rot_label_n = 0;
        return;
    }

    /* 1) 先算屏幕位置，按优先级挑出前 N 个候选（只排序，不做碰撞）。 */
    for (i = 0; i < view->label_col.count; i++) {
        const vmap_label_cand_t * it = &view->label_col.items[i];
        const double dx = (double)it->x - (double)pivot_x;
        const double dy = (double)it->y - (double)pivot_y;
        const int32_t sx = (int32_t)lround((double)view->rw / 2.0
            + (c * dx - s * dy)) - view->r_margin;
        const int32_t sy = (int32_t)lround((double)view->rh / 2.0
            + (s * dx + c * dy)) - view->r_margin;
        const int32_t sy_vis = sy + view->y_bias;
        const int32_t ddx = sx - cx;
        const int32_t ddy = sy_vis - cy;
        vmap_rot_pick_t e;
        int32_t pos;

        if (it->name[0] == '\0') {
            continue;
        }
        /* 中心离视口太远的先排除；真正裁剪在放置阶段按整框做。 */
        if (sx < -48 || sy_vis < -48 || sx > view->view_w + 48
            || sy_vis > view->view_h + 48) {
            continue;
        }

        e.idx = (int16_t)i;
        e.tier = vmap_rot_label_tier(it->kind);
        e.prio = it->prio;
        e.dist2 = ddx * ddx + ddy * ddy;
        e.sx = sx;
        e.sy = sy;

        /* 插入排序：pick[0] 最优，容量 VMAP_ROT_PICK_MAX。 */
        pos = pick_n;
        while (pos > 0 && vmap_rot_pick_better(&e, &pick[pos - 1])) {
            if (pos < VMAP_ROT_PICK_MAX) {
                pick[pos] = pick[pos - 1];
            }
            pos--;
        }
        if (pos < VMAP_ROT_PICK_MAX) {
            pick[pos] = e;
            if (pick_n < VMAP_ROT_PICK_MAX) {
                pick_n++;
            }
        }
    }

    /* 2) 按优先级放置：整框必须在视口内，且不得与已放标签重叠。 */
    for (i = 0; i < pick_n && n < VMAP_ROT_LABEL_MAX; i++) {
        const vmap_label_cand_t * it = &view->label_col.items[pick[i].idx];
        lv_obj_t * l = view->rot_label[n];
        lv_area_t box;
        lv_coord_t w;
        lv_coord_t h;
        int32_t k;
        bool dup = false;

        for (k = 0; k < n; k++) {
            if (strcmp(taken[k], it->name) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }

        /* 按**已经上屏**的旋转角把道路切向转到屏幕方向，再与正北同一规则选
         * 横排/逐字竖排（|dy| > |dx| ⇒ 竖排）。字形始终正立，不旋转。
         * 方向未知（0,0，bbox 兜底）按横排。 */
        {
            const float dirx = (float)(c * (double)it->dir_x
                - s * (double)it->dir_y);
            const float diry = (float)(s * (double)it->dir_x
                + c * (double)it->dir_y);
            const bool vertical = (it->dir_x != 0.0f || it->dir_y != 0.0f)
                && (fabsf(diry) > fabsf(dirx));
            char vbuf[sizeof(it->name) * 2u];
            const char * text = vmap_rot_label_text(it->name, vertical, vbuf,
                sizeof(vbuf));

            lv_label_set_text(l, text);
        }
        lv_obj_set_style_text_color(l,
            lv_color_hex(vmap_rot_label_color(it->kind)), 0);
        /* 字号与正北烘进底图的标准一致：地名 16，路名/水名/POI 12。 */
        lv_obj_set_style_text_font(l, vmap_rot_label_font(view, it->kind), 0);
        lv_obj_update_layout(l);
        w = lv_obj_get_width(l);
        h = lv_obj_get_height(l);

        box.x1 = pick[i].sx - w / 2;
        box.y1 = pick[i].sy + view->y_bias - h / 2;
        box.x2 = box.x1 + w - 1;
        box.y2 = box.y1 + h - 1;
        if (box.x1 < 0 || box.y1 < 0
            || box.x2 >= view->view_w || box.y2 >= view->view_h) {
            continue;
        }
        for (k = 0; k < placed_n; k++) {
            if (vmap_rot_boxes_overlap(&box, &placed[k],
                    VMAP_ROT_LABEL_GAP)) {
                break;
            }
        }
        if (k < placed_n) {
            continue;
        }

        placed[placed_n++] = box;
        view->rlx[n] = box.x1;
        view->rly[n] = box.y1 - view->y_bias;
        lv_obj_set_pos(l, view->rlx[n],
            (lv_coord_t)(view->rly[n] + view->y_bias));
        lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
        taken[n] = it->name;
        n++;
    }

    for (i = n; i < VMAP_ROT_LABEL_MAX; i++) {
        lv_obj_add_flag(view->rot_label[i], LV_OBJ_FLAG_HIDDEN);
    }
    view->rot_label_n = (uint8_t)n;
}

/* 【临时探针】整幅重烘的触发原因（`[vrot] why <c>`，LOG_DEBUG）。测完删。
 *   A=待应用角度  N=有 pending 没脏区  B=子矩形回退（本版没有）
 *   T=跟车余量用完/角度不一致  E=帧末  S=开航向朝上  L=路名开关 */
static void vmap_rot_why(char c)
{
    syslog(LOG_DEBUG, "[vrot] why %c\n", c);
}

/**
 * @brief 用 EPIC 把整幅画布按当前 rot10 重新烘进 R。
 *
 * @details 一次约 138 ms（R 288×264，EPIC 任意角 ~0.55 Mpx/s），所以只在**角度变了 /
 *          余量用完 / 整帧结束**时调用；平地跟车只移动 R 的对象。
 */
static bool vmap_rot_full(vmap_view_t * view)
{
    int32_t pivot_x;
    int32_t pivot_y;

    if (view == NULL || !view->heading_up || view->rbuf == NULL
        || view->cbuf == NULL || view->rcanvas == NULL) {
        return false;
    }

    /* 切换窗口里 C 还在重铺：此时烘 R 会把半成品/旧标签带上屏。 */
    if (view->heading_switch_pending) {
        return false;
    }

    /*
     * ⚠ 帧在飞时**绝不能烘**：`vmap_rot_full` 会被 GNSS 定时器经
     * `apply_canvas_pos → vmap_rot_track` 调到，而那时正北画布只画了一半瓦片、
     * 标签收集器也只填了一部分 ⇒ 烘出来是"半成品"，下一帧再纠正，观感就是
     * 地图卡住 + 文字乱跳（2026-10-28 现场）。等 `render_finish` 里再烘。
     */
    if (view->pump_phase != 0u) {
        return false;
    }

    /*
     * 失败后的重试限频放在**这里**，而不是只放在 service 里：跟车余量、开关切换、
     * 帧末都直接调本函数，否则一次失败会在同一拍里被下一个入口再次触发。
     * 只限"失败重试"，正常重烘（帧末/余量/角度）不受这条窗口影响。
     */
    if (view->rot_fail_valid
        && (uint32_t)(lv_tick_get() - view->last_fail_ms) < VMAP_ROT_BAKE_MIN_MS) {
        return false;
    }

    /* 骑手在画布里的位置 = 画布中心 + 跟车 pan。 */
    pivot_x = view->margin + view->view_w / 2 + (int32_t)lround(view->pan_dx);
    pivot_y = view->margin + view->view_h / 2 + (int32_t)lround(view->pan_dy);
    if (pivot_x < 0 || pivot_y < 0 || pivot_x >= view->w || pivot_y >= view->h) {
        pivot_x = view->w / 2;
        pivot_y = view->h / 2;
    }

    if (!myvendor_epic_rotate(view->cbuf, view->stride, view->w, view->h,
            view->rbuf, view->rstride, view->rw, view->rh, view->rot10,
            pivot_x, pivot_y, view->rw / 2, view->rh / 2)) {
        /*
         * 失败分两种：参数/硬件不可用（R 没动），或 **EPIC 超时**（R 可能已被写了
         * 一部分，上一帧不能再信）。两种情况都按"R 是脏的"处理 —— 这里的记账是
         * 全树唯一的收口点，调用方（service / 跟车 / 帧末 / 开关切换）都不必各自判断：
         *   ① `rot_pending`：service 每拍重试整幅，直到成功；
         *   ② `last_fail_ms`：失败重试限频（入口处），避免同一拍被别的入口再打一次。
         *
         * ⚠ 已知边界：R 是单缓冲、EPIC 直写，超时后**无法回滚**上一帧。
         *   失败到下一次成功重烘之间，屏上可能是新旧混合的一帧；这段时间里
         *   `rot_broken` 会**冻结跟车平移**（否则按旧 r_pan 继续挪会把新旧两种基准
         *   叠在一起，错位越滚越大）。重试间隔 ≥1200 ms，连续失败可更久。
         *   要彻底消除得双缓冲（多 152 KB PSRAM），未做。
         */
        view->rot_pending = true;
        view->rot_fail_valid = true;
        view->rot_broken = true;
        view->last_fail_ms = lv_tick_get();
        return false;
    }

    view->r_pan_x = view->pan_dx;
    view->r_pan_y = view->pan_dy;
    view->r_rot10 = view->rot10;
    view->rot_pending = false;
    view->rot_fail_valid = false;
    view->rot_broken = false;
    /* 余量重烘/帧末重烘/增量重烘都计入同一个限频窗口。 */
    view->last_bake_ms = lv_tick_get();
    vmap_rot_place(view, 0, 0);
    vmap_rot_labels_update(view, pivot_x, pivot_y);
    lv_obj_invalidate(view->rcanvas);
    return true;
}

/**
 * @brief 把"帧外增量还没进 R"的记账补烘掉（限频窗口内先留着）。
 *
 * @details 由 200 ms 的航向拍调用（`vmap_view_set_heading`），所以即使某次增量
 *          之后再没有任何失效，内容也会在窗口到期后的下一拍上屏；不会永远停在旧版本。
 */
static void vmap_rot_service(vmap_view_t * view)
{
    bool angle;

    if (view == NULL || !view->heading_up) {
        return;
    }

    if (view->rcanvas != NULL
        && lv_obj_has_flag(view->rcanvas, LV_OBJ_FLAG_HIDDEN)) {
        /* R 还没接替（切换窗口或烘焙失败回退）：屏上是 C，先保住 C 的刷新，
         * 再按 5 Hz 航向拍尝试完成切换。 */
        if (view->heading_switch_pending) {
            if (view->heading_switch_ready) {
                view->heading_switch_pending = 0;
                if (vmap_rot_full(view)) {
                    view->heading_switch_ready = 0;
                    if (view->canvas) {
                        lv_obj_add_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);
                    }
                    lv_obj_clear_flag(view->rcanvas, LV_OBJ_FLAG_HIDDEN);
                } else {
                    view->heading_switch_pending = 1;
                }
            } else if (!view->restamp_full_active && !view->restamp_pending) {
                vmap_view_redraw_keep_pan(view);
            }
        } else if (vmap_rot_full(view)) {
            if (view->canvas) {
                lv_obj_add_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_clear_flag(view->rcanvas, LV_OBJ_FLAG_HIDDEN);
        }
        if (view->canvas) {
            lv_obj_invalidate(view->canvas);
        }
        return;
    }

    angle = (view->want_rot10 != view->rot10);
    if (!angle && !view->rot_pending) {
        return;
    }
    if (view->pump_phase != 0u || view->rcanvas == NULL
        || view->render_rev == 0u
        || lv_obj_has_flag(view->rcanvas, LV_OBJ_FLAG_HIDDEN)) {
        return;                 /* 帧在飞（帧末会烘）或还没显示/还没有首帧 */
    }
    if (view->last_bake_ms != 0u
        && lv_tick_elaps(view->last_bake_ms) < VMAP_ROT_BAKE_MIN_MS) {
        return;                 /* 限频窗口未到：下一拍再来 */
    }
    /*
     * 待应用角度与"帧外增量"合并到**同一次**重烘：否则先用旧角度烘一次增量会刷新
     * `last_bake_ms`，把角度请求一直挡在窗口外（持续记录时地图永远保持旧朝向）。
     * 角度**成功烘完才提交**：EPIC 旋转失败时保留 want_rot10，下一拍自动重试，
     * 不会把请求悄悄丢掉（否则静止时再没有别的东西会触发重烘）。
     */
    {
        const int32_t prev_rot10 = view->rot10;

        /* 探针放在限频检查**之后**：这里才是真的要烘一次（之前的位置会把被窗口挡掉
         * 的重试也记成一次触发）。 */
        vmap_rot_why(angle ? 'A' : 'N');
        view->rot10 = view->want_rot10;
        if (!vmap_rot_full(view)) {
            /* 失败记账在 `vmap_rot_full()` 里统一做（见那里的注释）。 */
            view->rot10 = prev_rot10;
        }
    }
}

/**
 * @brief 跟车：R 里的内容该位移 (-R(θ)·Δpan)；余量够只挪对象，不够重烘。
 */
static void vmap_rot_track(vmap_view_t * view)
{
    const double th = (double)view->r_rot10 * 0.1 * M_PI / 180.0;
    const double c = cos(th);
    const double s = sin(th);
    const double dpx = view->pan_dx - view->r_pan_x;
    const double dpy = view->pan_dy - view->r_pan_y;
    const int32_t sx = (int32_t)lround(-(c * dpx - s * dpy));
    const int32_t sy = (int32_t)lround(-(s * dpx + c * dpy));
    const int32_t lim = view->r_margin - VMAP_ROT_TRACK_GUARD;

    /*
     * R 可能被写坏（上次整幅超时）：冻结平移，等一次成功重烘把 R 整体重写。
     * 否则这次位移是按"上一次成功的 r_pan"算的，而 R 里已有一段按新 pan 写入的像素。
     */
    if (view->rot_broken) {
        return;
    }

    if (view->rot10 != view->r_rot10
        || abs((int)sx) > lim || abs((int)sy) > lim) {
        vmap_rot_why('T');
        vmap_rot_full(view);    /* 角度变了、或余量用完了；失败记账在它内部 */
        return;
    }

    vmap_rot_place(view, sx, sy);
}

static void vmap_view_apply_canvas_pos(vmap_view_t * view, double dx, double dy)
{
    lv_coord_t px;
    lv_coord_t py;

    if (!view || !view->canvas) {
        return;
    }

    /* 请求量原样记账（浮点），只有上屏这一步取整 —— 亚像素余量的唯一去处。 */
    view->pan_rx = dx;
    view->pan_ry = dy;
    px = (lv_coord_t)lround(dx);
    py = (lv_coord_t)lround(dy);

    if (px == view->last_pan_px && py == view->last_pan_py) {
        return;
    }

    /*
     * Moving a canvas invalidates the complete visible map. Coalesce sub-pixel
     * GNSS movement; the centered arrow still updates normally.
     */
    if (view->last_pan_px != INT32_MIN && view->last_pan_py != INT32_MIN
        && abs((int)px - view->last_pan_px) < VMAP_PAN_MIN_STEP_PX
        && abs((int)py - view->last_pan_py) < VMAP_PAN_MIN_STEP_PX) {
        return;
    }

    view->pan_dx = (double)px;
    view->pan_dy = (double)py;
    view->last_pan_px = px;
    view->last_pan_py = py;
    view->map_rev++;
    vmap_label_collector_set_focus(&view->label_col,
        view->w / 2 + px, view->h / 2 + py);
#if defined(CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE) && CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE
    bicycle_inval_probe_tag("vmap_pan");
#endif
    /* lv_obj_set_pos already invalidates old + new areas; do not inval again. */
    /* y_bias 只加在屏幕位置上：pan_dy 与投影原点都不动，于是偏置与跟车/rebase
     * 的数学完全解耦（偏置变化既不触发重算，也不改变 rebase 时机）。 */
    if (!view->heading_up || view->heading_switch_pending
        || !lv_obj_has_flag(view->canvas, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_pos(view->canvas,
            (lv_coord_t)(-view->margin - px),
            (lv_coord_t)(-view->margin - py + view->y_bias));
        return;
    }

    /*
     * 航向朝上：不再让 LVGL 旋转画布（那样每次平移都要重跑一次任意角 transform，
     * 实测 60 ms/次）。改成维护一块**屏幕对齐的旋转缓冲 R**：只有角度变化或
     * 余量用完才重烘，平地跟车只移动 R 的对象（见 vmap_rot_track）。
     */
    vmap_rot_track(view);
}

/**
 * @brief 航向朝上：地图跟随骑行方向旋转（箭头恒指上）。
 */
void vmap_view_set_heading_up(vmap_view_t * view, bool enable)
{
    if (!view || view->heading_up == enable) {
        return;
    }

    if (enable && (view->rcanvas == NULL || view->rbuf == NULL)) {
        syslog(LOG_WARNING,
            "[map] heading-up refused: rotated buffer unavailable\n");
        return;
    }

    view->heading_up = enable;
    view->heading_valid = false;
    view->rot10 = 0;
    view->want_rot10 = 0;
    view->r_rot10 = -1;         /* 强制下一次 track 重烘 */
    view->rot_pending = false;  /* 切换时会立刻重烘，旧的记账作废 */

    if (enable) {
        if (view->render_rev != 0u) {
            /* 先保持正北画布 C 可见，等整幅重铺出干净底图再切 R：
             * 旧路名不会跟着转进 R，也不会和正立标签形成双影。MTP 期间
             * redraw_keep_pan 会推迟，等 MTP 结束后的帧再补。 */
            int32_t i;

            view->heading_switch_pending = 1;
            view->heading_switch_ready = 0;
            if (view->rcanvas) {
                lv_obj_add_flag(view->rcanvas, LV_OBJ_FLAG_HIDDEN);
            }
            if (view->canvas) {
                lv_obj_clear_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);
            }
            for (i = 0; i < VMAP_ROT_LABEL_MAX; i++) {
                if (view->rot_label[i] != NULL) {
                    lv_obj_add_flag(view->rot_label[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
            view->rot_label_n = 0;
            vmap_view_redraw_keep_pan(view);
        } else {
            /* 首帧：C 还没有内容，直接显示 R；首帧烘完自动补齐。 */
            if (view->rcanvas) {
                lv_obj_clear_flag(view->rcanvas, LV_OBJ_FLAG_HIDDEN);
            }
            if (view->canvas) {
                lv_obj_add_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        view->heading_switch_pending = 0;
        view->heading_switch_ready = 0;
        if (view->rcanvas) {
            lv_obj_add_flag(view->rcanvas, LV_OBJ_FLAG_HIDDEN);
        }
        if (view->canvas) {
            lv_obj_clear_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);
        }
        /* 正立路名只在航向朝上时用；退回正北后由烘焙路径接管。 */
        {
            int32_t i;

            for (i = 0; i < VMAP_ROT_LABEL_MAX; i++) {
                if (view->rot_label[i] != NULL) {
                    lv_obj_add_flag(view->rot_label[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
            view->rot_label_n = 0;
        }
    }

    /* 位置公式与标签开关都变了：按新公式重摆，再整幅重来一帧。 */
    view->last_pan_px = INT32_MIN;
    view->last_pan_py = INT32_MIN;
    vmap_view_apply_canvas_pos(view, view->pan_dx, view->pan_dy);
    syslog(LOG_NOTICE, "[map] heading-up %s\n", enable ? "on" : "off");

    /* 首帧之前不要提前开帧（开机还没进过地图时由页面层控制）。 */
    if (!enable && view->render_rev != 0u) {
        vmap_view_render(view);
    }
}

bool vmap_view_heading_up(const vmap_view_t * view)
{
    return view != NULL && view->heading_up;
}

/**
 * @brief 航向朝上时是否也烘路名标签（A/B 用）。
 */
void vmap_view_set_rot_labels(vmap_view_t * view, bool enable)
{
    if (view == NULL || view->rot_labels == enable) {
        return;
    }

    view->rot_labels = enable;
    syslog(LOG_NOTICE, "[map] heading-up labels %s\n", enable ? "on" : "off");

    if (view->heading_up && view->render_rev != 0u) {
        /* 正立标签是独立对象：下一次 bake 就会按开关重新摆好。 */
        vmap_rot_why('L');
        vmap_rot_full(view);
        if (!enable) {
            int32_t i;

            for (i = 0; i < VMAP_ROT_LABEL_MAX; i++) {
                lv_obj_add_flag(view->rot_label[i], LV_OBJ_FLAG_HIDDEN);
            }
            view->rot_label_n = 0;
        }
    }
}

/**
 * @brief 更新航向（0–360，正北 0，顺时针）与当前速度。
 */
void vmap_view_set_heading(vmap_view_t * view, float heading_deg, float speed_kph)
{
    double d = (double)heading_deg;
    int32_t target;
    int32_t delta;

    if (!view || !view->canvas) {
        return;
    }
    if (!view->heading_up) {
        return;
    }
    if (!(d >= 0.0) || !(d < 360.0)) {
        /* NaN / 越界：角度不动，但帧外增量的记账照样要服务。 */
        vmap_rot_service(view);
        return;
    }

    if (!view->heading_valid) {
        /* 首个航向无条件接受：否则停车时箭头指上、地图还是正北。 */
        view->heading_deg = d;
        view->heading_valid = true;
    } else if (speed_kph >= VMAP_ROT_MIN_KPH) {
        double diff = d - view->heading_deg;

        /* 圆周低通：先把差值折到 -180..180，避免 359°→1° 绕一整圈。 */
        while (diff > 180.0) {
            diff -= 360.0;
        }
        while (diff < -180.0) {
            diff += 360.0;
        }
        view->heading_deg += diff * (double)VMAP_ROT_SMOOTH;
        while (view->heading_deg >= 360.0) {
            view->heading_deg -= 360.0;
        }
        while (view->heading_deg < 0.0) {
            view->heading_deg += 360.0;
        }
    } else {
        /* 低速：course 是噪声，保持上次朝向；增量仍要走服务拍。 */
        vmap_rot_service(view);
        return;
    }

    /*
     * 航向朝上 ⇒ 地图转 -heading（顺时针为正）。
     * **只记账不立即烘**：`want_rot10` 是"待应用目标"，真正的应用与限频都在
     * `vmap_rot_service()` 里，并且和帧外增量合并到同一次重烘。
     * 每烘一次 ~138 ms（288×264，0.55 Mpx/s）；1° 步进时若不限频，3.6°/s 的弯道
     * 会有 3~4 次/秒（≈50% UI）。1200 ms ⇒ 最多 0.83 次/秒 —— 转弯时地图比真实
     * 航向慢半拍，单步会放大到 ~2°（用户 2026-10-28 接受"旋转慢一些"）。
     */
    target = ((int32_t)lround(-view->heading_deg * 10.0) % 3600 + 3600) % 3600;
    delta = ((target - view->want_rot10 + 3600 + 1800) % 3600) - 1800;
    if (abs((int)delta) >= VMAP_ROT_STEP_10) {
        view->want_rot10 = target;
    }

    vmap_rot_service(view);
}

static bool vmap_view_geo_to_viewport_impl(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y, bool check_visible)
{
    double base_x = 0.0;
    double base_y = 0.0;
    double geo_x = 0.0;
    double geo_y = 0.0;
    double cx;
    double cy;
    int32_t sx;
    int32_t sy;
    const int32_t pad = 24;

    if (!view) {
        return false;
    }

    /* 原点用浮点：重锚保留的亚像素余量在这里生效 ⇒ 整数屏幕位移严格平移，
     * 箭头/轨迹与瓦片内容不会差半个像素。 */
    vmap_latlong_to_pixel_d(view->render_lat, view->render_lon, view->zoom,
        &base_x, &base_y);
    vmap_latlong_to_pixel_d(lat, lon, view->zoom, &geo_x, &geo_y);

    cx = (geo_x - base_x) * view->scale + (double)view->w / 2.0;
    cy = (geo_y - base_y) * view->scale + (double)view->h / 2.0;
    sx = (int32_t)lround(cx - (double)view->margin - view->pan_dx);
    sy = (int32_t)lround(cy - (double)view->margin - view->pan_dy);

    if (out_x) {
        *out_x = sx;
    }
    if (out_y) {
        *out_y = sy;
    }

    if (!check_visible) {
        return true;
    }

    return sx >= -pad && sy >= -pad
        && sx <= view->view_w + pad && sy <= view->view_h + pad;
}

/**
 * @brief 经纬度 → 视口像素（含 follow pan）。
 */
bool vmap_view_geo_to_viewport(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y)
{
    return vmap_view_geo_to_viewport_impl(view, lon, lat, out_x, out_y, true);
}

/**
 * @brief 经纬度 → 视口像素，不做可见性裁剪。
 */
bool vmap_view_geo_to_viewport_raw(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y)
{
    return vmap_view_geo_to_viewport_impl(view, lon, lat, out_x, out_y, false);
}

/**
 * @brief 经纬度 → 画布整数像素。
 */
bool vmap_view_geo_to_canvas_raw(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y)
{
    double base_x = 0.0;
    double base_y = 0.0;
    double geo_x = 0.0;
    double geo_y = 0.0;
    double cx;
    double cy;

    if (!view) {
        return false;
    }

    vmap_latlong_to_pixel_d(view->render_lat, view->render_lon, view->zoom,
        &base_x, &base_y);
    vmap_latlong_to_pixel_d(lat, lon, view->zoom, &geo_x, &geo_y);

    cx = (geo_x - base_x) * view->scale + (double)view->w / 2.0;
    cy = (geo_y - base_y) * view->scale + (double)view->h / 2.0;

    if (out_x) {
        *out_x = (int32_t)lround(cx);
    }
    if (out_y) {
        *out_y = (int32_t)lround(cy);
    }
    return true;
}

bool vmap_view_geo_to_canvas_f(const vmap_view_t * view, double lon, double lat,
    double * out_x, double * out_y)
{
    vmap_view_proj_t proj;

    if (!view) {
        return false;
    }

    vmap_view_proj_begin(view, &proj);
    vmap_view_proj_point_f(&proj, lon, lat, out_x, out_y);
    return true;
}

void vmap_view_proj_begin(const vmap_view_t * view, vmap_view_proj_t * proj)
{
    if (!proj) {
        return;
    }
    if (!view) {
        memset(proj, 0, sizeof(*proj));
        return;
    }

    proj->zoom = view->zoom;
    proj->scale = view->scale;
    proj->half_w = (double)view->w / 2.0;
    proj->half_h = (double)view->h / 2.0;
    vmap_latlong_to_pixel_d(view->render_lat, view->render_lon, view->zoom,
        &proj->base_x, &proj->base_y);
}

void vmap_view_proj_point(const vmap_view_proj_t * proj, double lon, double lat,
    int32_t * out_x, int32_t * out_y)
{
    int geo_x = 0;
    int geo_y = 0;
    double cx;
    double cy;

    if (!proj) {
        return;
    }

    vmap_latlong_to_pixel(lat, lon, proj->zoom, &geo_x, &geo_y);
    cx = (geo_x - proj->base_x) * proj->scale + proj->half_w;
    cy = (geo_y - proj->base_y) * proj->scale + proj->half_h;
    if (out_x) {
        *out_x = (int32_t)lround(cx);
    }
    if (out_y) {
        *out_y = (int32_t)lround(cy);
    }
}

void vmap_view_proj_point_f(const vmap_view_proj_t * proj, double lon, double lat,
    double * out_x, double * out_y)
{
    double geo_x = 0.0;
    double geo_y = 0.0;

    if (!proj) {
        return;
    }

    vmap_latlong_to_pixel_d(lat, lon, proj->zoom, &geo_x, &geo_y);
    if (out_x) {
        *out_x = (geo_x - (double)proj->base_x) * proj->scale + proj->half_w;
    }
    if (out_y) {
        *out_y = (geo_y - (double)proj->base_y) * proj->scale + proj->half_h;
    }
}

/**
 * @brief retained 画布宽高（含 overscan）。
 * @return 请求的值。
 */
void vmap_view_get_buffer_size(const vmap_view_t * view, int32_t * w, int32_t * h)
{
    if (!view) {
        return;
    }
    if (w) {
        *w = view->w;
    }
    if (h) {
        *h = view->h;
    }
}

/**
 * @brief 画布坐标点是否可安全描边。
 */
bool vmap_view_canvas_point_sane(const vmap_view_t * view, int32_t x, int32_t y)
{
    int32_t cw = 0;
    int32_t ch = 0;

    if (!view) {
        return false;
    }

    vmap_view_get_buffer_size(view, &cw, &ch);
    if (cw < 1 || ch < 1) {
        return false;
    }

    return x >= -cw * 4 && x <= cw * 5 && y >= -ch * 4 && y <= ch * 5;
}

/**
 * @brief 画布点是否落在可视窗口外扩 pad 内。
 */
bool vmap_view_canvas_point_in_pad(const vmap_view_t * view, int32_t x, int32_t y,
    int32_t pad)
{
    int32_t cw = 0;
    int32_t ch = 0;

    if (!view || pad < 0 || !vmap_view_canvas_point_sane(view, x, y)) {
        return false;
    }

    vmap_view_get_buffer_size(view, &cw, &ch);
    return x >= -pad && y >= -pad && x <= cw + pad && y <= ch + pad;
}

/**
 * @brief 画布点是否落在可视窗口内。
 */
bool vmap_view_canvas_point_visible(const vmap_view_t * view, int32_t x, int32_t y)
{
    return vmap_view_canvas_point_in_pad(view, x, y, 48);
}

/**
 * @brief 取出当前 RGB565 画布描述。
 */
bool vmap_view_get_canvas(const vmap_view_t * view, vmap_canvas_t * canvas)
{
    if (!view || !canvas || !view->cbuf) {
        return false;
    }

    canvas->buf = (uint16_t *)view->cbuf;
    canvas->w = view->w;
    canvas->h = view->h;
    canvas->stride = view->stride;
    vmap_render_clip_reset(canvas);
    return true;
}

void vmap_view_invalidate_canvas_area(const vmap_view_t * view,
    int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    lv_area_t area;
    lv_area_t coords;

    if (!view || !view->canvas || x1 > x2 || y1 > y2) {
        return;
    }

    if (x2 < 0 || y2 < 0 || x1 >= view->w || y1 >= view->h) {
        return;
    }

    if (view->restamp_full_active) {
        /* 当前帧就是 keep-pan 整幅重铺：这些区域本帧内会被瓦片/底色覆盖，
         * 不要再当成一次新的重铺请求排到帧末。 */
        return;
    }

    if (view->heading_up
        && !lv_obj_has_flag(view->canvas, LV_OBJ_FLAG_HIDDEN)) {
        /* R 还没接替（切换窗口/烘焙失败回退）：C 才是屏上内容。先按普通画布
         * 失效，再让 service 按 5 Hz 拍尝试完成切换。 */
        vmap_view_t * v = (vmap_view_t *)view;

        v->rot_pending = true;
        lv_obj_invalidate(view->canvas);
        vmap_rot_service(v);
        return;
    }

    if (view->heading_up) {
        /* 帧外增量（轨迹尾/导航进度/路线重绘）只改了 C；R 是 C 的快照，
         * 不作废也不重烘就永远停在旧内容。这里先记账，再按**与角度重烘共用**的
         * 最小间隔补一次：既保证增量能上屏，又不会因为"每加一个轨迹点"就多烘一次。
         * 窗口内没补成的由 200 ms 航向拍（vmap_rot_service）兜底，不会永远不补。
         * 帧在飞时 `vmap_rot_full` 自己会拒绝（pump_phase != 0），帧末会兜底。
         * 语义上这是"失效"而不是改调用者可见状态 ⇒ 保留 const 形参，只在这一处
         * 去掉 const（重烘要更新 r_pan/last_bake_ms 这些内部缓存）。 */
        vmap_view_t * v = (vmap_view_t *)view;

        v->rot_pending = true;
        vmap_rot_service(v);
        return;
    }

    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 >= view->w) {
        x2 = view->w - 1;
    }
    if (y2 >= view->h) {
        y2 = view->h - 1;
    }

    lv_obj_get_coords(view->canvas, &coords);
    area.x1 = coords.x1 + x1;
    area.y1 = coords.y1 + y1;
    area.x2 = coords.x1 + x2;
    area.y2 = coords.y1 + y2;
    lv_obj_invalidate_area(view->canvas, &area);
}

#if VMAP_TRACK_ENABLE
/**
 * @brief 绑定 REC 轨迹叠层。
 */
void vmap_view_attach_track(vmap_view_t * view, vmap_track_t * track)
{
    if (view) {
        view->track = track;
    }
}

#if VMAP_ROUTE_ENABLE
/**
 * @brief 把规划路线绑到本视图。
 */
void vmap_view_attach_route(vmap_view_t * view, vmap_route_t * route)
{
    if (view) {
        view->route = route;
        if (route) {
            vmap_route_bind_view(route, view);
        }
    }
}
#endif
#endif

/**
 * @brief 创建带 overscan 的 RGB565 大画布；canvas 隐藏到首帧完成。
 * @return 视图；失败为 NULL。
 */
vmap_view_t * vmap_view_create(lv_obj_t * parent, int32_t view_w, int32_t view_h,
    int32_t margin)
{
    vmap_view_t * view = (vmap_view_t *)calloc(1, sizeof(*view));
    if (!view) {
        return NULL;
    }

    vmap_perf_init_once();

    view->view_w = view_w;
    view->view_h = view_h;
    view->margin = margin;
    view->w = view_w + margin * 2;
    view->h = view_h + margin * 2;
    /* 父容器 = 视口容器。create 时它若正好和视口一样大，就是"专用容器"，
     * set_viewport() 顺带改它的尺寸（组件自足）；否则说明页面还在这块区域里
     * 放了别的东西，几何归页面管，默认不动。可用 set_parent_autosize 覆盖。
     *
     * 尺寸读**样式值**优先：create 往往发生在第一次 layout 之前，此时
     * `lv_obj_get_width()` 还是 0（坐标未刷新），会把专用容器误判成"不是"。
     * 样式是百分比/内容自适应（负值）时退回实际尺寸。 */
    view->parent = parent;
    if (parent != NULL) {
        lv_coord_t pw = lv_obj_get_style_width(parent, LV_PART_MAIN);
        lv_coord_t ph = lv_obj_get_style_height(parent, LV_PART_MAIN);

        if (pw < 0) {
            pw = lv_obj_get_width(parent);
        }
        if (ph < 0) {
            ph = lv_obj_get_height(parent);
        }
        view->parent_autosize = (pw == (lv_coord_t)view_w && ph == (lv_coord_t)view_h);
    }
    view->zoom = VMAP_DEFAULT_ZOOM;
    view->scale = VMAP_DEFAULT_SCALE;
    vmap_boot_lonlat(&view->lon, &view->lat);
    view->render_lon = view->lon;
    view->render_lat = view->lat;
    view->veh_lon = view->lon;
    view->veh_lat = view->lat;
    view->follow_mode = true;
    view->last_arrow_angle10 = INT32_MIN;
    view->disp_arrow_angle10 = INT32_MIN;
    view->last_pan_px = INT32_MIN;
    view->last_pan_py = INT32_MIN;
    view->paint_zoom = -1;

    view->stride = lv_draw_buf_width_to_stride(view->w, LV_COLOR_FORMAT_RGB565);
    /* ⚠ 画布**优先用专属 arena**（板级 MPU 只给它开 write-back，见
     * `board_psram_layout.h` 的 `BOARD_PSRAM_CANVAS_ARENA_*`）：普通 PSRAM 池是
     * write-through ⇒ 每次像素 store 都打到 PSRAM（~90 周期/次 ≈ 10 MB/s），
     * 这是地图渲染慢的底层原因。arena 放不下就回退普通池（慢但能用）。 */
    {
        const size_t need = (size_t)view->stride * (size_t)view->h;
        size_t arena_bytes = 0;
        void * arena = board_psram_canvas_arena(&arena_bytes);

        if (arena != NULL && need > 0 && need <= arena_bytes) {
            view->cbuf = arena;
            view->cbuf_arena = true;
            /* 这里**不要**写 WB/WT：写入属性由板级 MPU 决定（见 board_psram_layout.h），
             * MPU 侧没接上时 arena 仍是普通的 WT —— 写死"WB"会让人误判。 */
            syslog(LOG_NOTICE,
                   "[vmap] canvas arena %p +%u KiB (need %u KiB)",
                   arena, (unsigned)(arena_bytes / 1024u),
                   (unsigned)(need / 1024u));
        } else {
            view->cbuf = vmap_malloc(need);
            syslog(LOG_WARNING,
                   "[vmap] canvas fallback pool (need %u KiB, arena %u KiB)",
                   (unsigned)(need / 1024u), (unsigned)(arena_bytes / 1024u));
        }
    }

    if (!view->cbuf) {
        free(view);
        return NULL;
    }

    memset(view->cbuf, 0, (size_t)view->stride * (size_t)view->h);

    /*
     * 搬运后端自检（一次性，`[vblit]` 行）：拿真机画布几何测"同一块缓冲内
     * 源/目的矩形重叠"的搬运能不能交给 EPIC。放在这里是因为画布尺寸刚定下、
     * 还没有任何一帧在跑，而且自检缓冲用完立即释放。详见 vmap_blit.h。
     */
    vmap_blit_probe(view->w, view->h);

    view->scratch = (vmap_point_t *)vmap_malloc(sizeof(vmap_point_t) * VMAP_SCRATCH_CAP);

    /* 航向朝上用的屏幕对齐旋转缓冲 R（视口 + 四周余量）。分配不到就退回
     * "LVGL 直接旋转画布"的老路径（慢但能用）。 */
    view->r_margin = VMAP_ROT_MARGIN;
    view->rw = view_w + 2 * view->r_margin;
    view->rh = view_h + 2 * view->r_margin;
    view->rstride = lv_draw_buf_width_to_stride(view->rw, LV_COLOR_FORMAT_RGB565);
    view->rbuf = (uint16_t *)vmap_malloc((size_t)view->rstride * (size_t)view->rh);
    if (view->rbuf != NULL) {
        memset(view->rbuf, 0, (size_t)view->rstride * (size_t)view->rh);
    }
    /* 道路索引：只分配一次；失败只是回退逐 class 扫描，不影响可用性。 */
    view->road_refs = (vmap_road_ref_t *)vmap_malloc(
        sizeof(vmap_road_ref_t) * VMAP_ROAD_INDEX_CAP);
    view->labels = (vmap_label_cand_t *)vmap_malloc(sizeof(vmap_label_cand_t) * VMAP_LABEL_CAP);
    view->label_retained = (vmap_label_retained_t *)vmap_malloc(
        sizeof(vmap_label_retained_t) * VMAP_LABEL_RETAIN_CAP);
    view->cache = vmap_tile_cache_create();
    if (!view->scratch || !view->labels || !view->label_retained || !view->cache) {
        vmap_view_destroy(view);
        return NULL;
    }

    if (view->road_refs) {
        vmap_render_bind_road_index(view->road_refs, VMAP_ROAD_INDEX_CAP);
    } else {
        syslog(LOG_WARNING,
            "[vmap] road index unavailable (%d), fallback to per-class scan",
            VMAP_ROAD_INDEX_CAP);
        vmap_render_bind_road_index(NULL, 0);
    }

    vmap_label_collector_init(&view->label_col, view->labels, VMAP_LABEL_CAP,
        view->label_retained, VMAP_LABEL_RETAIN_CAP,
        view->w, view->h, view->view_w, view->view_h, view->margin);

    view->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(view->canvas, view->cbuf, view->w, view->h,
        LV_COLOR_FORMAT_RGB565);
    lv_obj_remove_style_all(view->canvas);
    lv_obj_set_size(view->canvas, view->w, view->h);
    lv_obj_set_pos(view->canvas, -margin, -margin);
    /* 首帧 render_finish 前不要把 PSRAM 噪声送到 LCD。 */
    lv_obj_add_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);

    /* 屏幕对齐的旋转缓冲对象（航向朝上时才显示）。 */
    if (view->rbuf != NULL) {
        view->rcanvas = lv_canvas_create(parent);
        lv_canvas_set_buffer(view->rcanvas, view->rbuf, view->rw, view->rh,
            LV_COLOR_FORMAT_RGB565);
        lv_obj_remove_style_all(view->rcanvas);
        lv_obj_set_size(view->rcanvas, view->rw, view->rh);
        vmap_rot_place(view, 0, 0);
        lv_obj_add_flag(view->rcanvas, LV_OBJ_FLAG_HIDDEN);
    } else {
        syslog(LOG_WARNING,
            "[vmap] rotated buffer unavailable (%dx%d) → heading-up disabled",
            (int)view->rw, (int)view->rh);
    }

    /* 航向朝上时的正立路名：独立文本对象（不烘进画布，否则会转成斜字）。 */
    {
        int32_t i;

        view->rot_labels = true;
        for (i = 0; i < VMAP_ROT_LABEL_MAX; i++) {
            lv_obj_t * l = lv_label_create(parent);

            lv_obj_remove_style_all(l);
            lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(l, LV_OBJ_FLAG_SCROLLABLE);
            if (view->font != NULL) {
                lv_obj_set_style_text_font(l, view->font, 0);
            }
            lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
            view->rot_label[i] = l;
        }
        view->rot_label_n = 0;
    }

    /* 帧率探针：挂在显示对象上，只挂一次（见 vmap_fps_probe_cb 的说明）。 */
    if (!s_fps_hooked) {
        lv_display_t * disp = lv_obj_get_display(view->canvas);

        if (disp != NULL) {
            lv_display_add_event_cb(disp, vmap_fps_probe_cb, LV_EVENT_ALL, NULL);
            s_fps_hooked = true;
        }
    }

    vmap_view_set_tile_dir(view, VMAP_TILE_DIR);
    return view;
}

/**
 * @brief 销毁视图：释放画布与瓦片缓存。
 */
void vmap_view_destroy(vmap_view_t * view)
{
    int32_t i;

    if (!view) {
        return;
    }

    /* 动画回调以 view 为 var，必须先停，避免 view 释放后回调访问野指针。 */
    lv_anim_delete(view, vmap_arrow_angle_exec);

    view->track = NULL;

    if (view->canvas) {
        lv_obj_delete(view->canvas);
        view->canvas = NULL;
    }
    if (view->arrow) {
        lv_obj_delete(view->arrow);
    }
    if (view->rcanvas) {
        lv_obj_delete(view->rcanvas);
        view->rcanvas = NULL;
    }
    /* 标签是挂 parent 的独立对象，单独销毁视图不删会泄漏。 */
    for (i = 0; i < VMAP_ROT_LABEL_MAX; i++) {
        if (view->rot_label[i] != NULL) {
            lv_obj_delete(view->rot_label[i]);
            view->rot_label[i] = NULL;
        }
    }
    view->rot_label_n = 0;
    if (view->rbuf) {
        vmap_free(view->rbuf);
        view->rbuf = NULL;
    }
    if (view->cbuf && !view->cbuf_arena) {
        vmap_free(view->cbuf);
    }
    if (view->scratch) {
        vmap_free(view->scratch);
    }
    if (view->labels) {
        vmap_free(view->labels);
    }
    if (view->label_retained) {
        vmap_free(view->label_retained);
    }
    /* 先解绑再释放：渲染路径不能拿着即将释放的索引。只解自己的绑定——换主题
     * 重建 shell 时可能"先建新视图再毁旧视图"，无条件解绑会把新视图的索引也
     * 摘掉，之后静默退回逐 class 扫描（只是慢，不报错）。 */
    if (vmap_render_road_index() == view->road_refs) {
        vmap_render_bind_road_index(NULL, 0);
    }
    if (view->road_refs) {
        vmap_free(view->road_refs);
        view->road_refs = NULL;
    }
    vmap_tile_cache_destroy(view->cache);
    free(view);
}

/**
 * @brief 底层 LVGL canvas 对象。
 */
lv_obj_t * vmap_view_get_obj(const vmap_view_t * view)
{
    return view ? view->canvas : NULL;
}

/**
 * @brief 设置地图目录（网格 .vpk 根）。
 */
void vmap_view_set_tile_dir(vmap_view_t * view, const char * dir)
{
    if (view && view->cache) {
        vmap_tile_cache_set_dir(view->cache, dir);
    }
}

/**
 * @brief 设置中心与缩放；不打开 .vpk。
 */
void vmap_view_set_view(vmap_view_t * view, double lon, double lat, int zoom)
{
    if (!view) {
        return;
    }
    view->lon = lon;
    view->lat = lat;
    view->render_lon = lon;
    view->render_lat = lat;
    view->zoom = zoom;
    /* 跳转：跟车/拖动留下的平移与亚像素余量都不再有意义。 */
    view->pan_dx = 0.0;
    view->pan_dy = 0.0;
    view->pan_rx = 0.0;
    view->pan_ry = 0.0;
    view->last_pan_px = INT32_MIN;
    view->last_pan_py = INT32_MIN;
    /*
     * set_view is an explicit coordinate-system jump (route review/zoom/boot),
     * not a follow rebase.  Never scroll pixels from the previous projection.
     */
    view->paint_zoom = -1;
    if (view->cache) {
        vmap_tile_cache_set_zoom(view->cache, zoom);
    }
}

/**
 * @brief 打开当前经纬度所在 cell 的 region pack。
 */
bool vmap_view_ensure_region(vmap_view_t * view, double lon, double lat)
{
    if (!view || !view->cache) {
        return false;
    }
    return vmap_tile_cache_ensure_region(view->cache, lon, lat);
}

/**
 * @brief 解析瓦片 LRU / region slot 缓存。
 */
vmap_tile_cache_t * vmap_view_tile_cache(vmap_view_t * view)
{
    return view ? view->cache : NULL;
}

/**
 * @brief 当前瓦片 zoom。
 */
int vmap_view_get_zoom(const vmap_view_t * view)
{
    return view ? view->zoom : 0;
}

/**
 * @brief 当前显示倍率。
 */
double vmap_view_get_scale(const vmap_view_t * view)
{
    return view ? view->scale : 1.0;
}

/**
 * @brief 当前跟车中心（经纬度）。
 */
void vmap_view_get_center(const vmap_view_t * view, double * lon, double * lat)
{
    if (!view) {
        if (lon) {
            *lon = 0.0;
        }
        if (lat) {
            *lat = 0.0;
        }
        return;
    }
    if (lon) {
        *lon = view->lon;
    }
    if (lat) {
        *lat = view->lat;
    }
}

/**
 * @brief 改显示倍率。
 */
void vmap_view_set_scale(vmap_view_t * view, double scale)
{
    if (!view) {
        return;
    }
    if (scale < 0.25) {
        scale = 0.25;
    } else if (scale > 8.0) {
        scale = 8.0;
    }
    view->scale = scale;
}

/* ══════════════════════════════════════════════════════════════════════════
 * 组件化 API：viewport / center / center icon / pan / follow / zoom_at
 * ══════════════════════════════════════════════════════════════════════════ */

/** @brief Web Mercator 像素 → 经纬度（vmap_latlong_to_pixel_d 的逆）。 */
static void vmap_pixel_to_latlong_d(double px, double py, int zoom,
    double * lon, double * lat)
{
    const double map_size = (double)vmap_map_size(zoom);
    const double n = M_PI - 2.0 * M_PI * (py / map_size);

    *lon = px / map_size * 360.0 - 180.0;
    *lat = 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}

/**
 * @brief 直接定位：中心设到 (lon,lat)，清跟车偏移，已渲染过则立即整幅重画。
 */
void vmap_view_center_on(vmap_view_t * view, double lon, double lat)
{
    if (!view || !isfinite(lon) || !isfinite(lat)) {
        return;
    }

    view->lon = lon;
    view->lat = lat;
    view->veh_lon = lon;
    view->veh_lat = lat;
    view->render_lon = lon;
    view->render_lat = lat;
    view->pan_dx = 0.0;
    view->pan_dy = 0.0;
    view->last_pan_px = INT32_MIN;
    view->last_pan_py = INT32_MIN;
    /* 坐标系统跳转：不复用旧像素，下一帧整幅清屏重画。 */
    view->paint_zoom = -1;
    if (view->cache) {
        vmap_tile_cache_set_zoom(view->cache, view->zoom);
    }
    if (view->canvas) {
        vmap_view_apply_canvas_pos(view, 0.0, 0.0);
    }

    /* 首帧仍由页面层控制（见 vmap_view_render 的说明）。 */
    if (view->render_rev != 0u && view->canvas && view->cbuf) {
        if (view->pump_phase != 0u) {
            /* 中止半帧：新帧会整幅清屏，残留像素不需要回滚。 */
            view->pump_phase = 0;
            view->pump_need_region = 0;
            view->pump_dirty_n = 0;
            view->restamp_pending = 0;
            view->restamp_full_active = 0;
            dvfs_map_busy(false);
        }
        vmap_view_render(view);
    }
}

/**
 * @brief 视口像素 → 经纬度（`vmap_view_geo_to_viewport()` 的逆）。
 *
 * 用**浮点**投影原点与显示平移反算 map 像素，因此和 `geo_to_viewport()` 严格互为
 * 逆（两者都不含 `y_bias`）。屏幕像素到地理坐标本来就有一像素的量化，回投影
 * 误差 ≤1 px。
 */
bool vmap_view_viewport_to_geo(const vmap_view_t * view, int32_t x, int32_t y,
    double * lon, double * lat)
{
    double base_x = 0.0;
    double base_y = 0.0;
    double map_x;
    double map_y;
    double scale;

    if (!view || lon == NULL || lat == NULL || view->zoom <= 0) {
        return false;
    }
    scale = view->scale > 0.0 ? view->scale : 1.0;
    vmap_latlong_to_pixel_d(view->render_lat, view->render_lon, view->zoom,
        &base_x, &base_y);
    /* geo_to_viewport: sx = (geo-base)*scale + w/2 - margin - pan  ⇒  反解 geo。 */
    map_x = base_x + ((double)x + (double)view->margin + view->pan_dx
        - (double)view->w / 2.0) / scale;
    map_y = base_y + ((double)y + (double)view->margin + view->pan_dy
        - (double)view->h / 2.0) / scale;
    if (!isfinite(map_x) || !isfinite(map_y)) {
        return false;
    }
    vmap_pixel_to_latlong_d(map_x, map_y, view->zoom, lon, lat);
    return isfinite(*lon) && isfinite(*lat);
}

/**
 * @brief 锚点缩放：缩放后 (sx,sy) 下的地理坐标不变。
 *
 * 做法：先把锚点反投影成经纬度（`viewport_to_geo`），改倍率，再把中心挪到
 * 「锚点 map 像素 - 锚点到视口中心的屏幕距离 / 新倍率」，于是同一个地物仍然
 * 落在 (sx,sy)。瓦片层级（z14）不变，只改连续 `scale`。
 */
bool vmap_view_zoom_at(vmap_view_t * view, int32_t sx, int32_t sy, double factor)
{
    double lon0 = 0.0;
    double lat0 = 0.0;
    double scale_new;
    double mx = 0.0;
    double my = 0.0;
    double nlon = 0.0;
    double nlat = 0.0;
    bool centered;
    const double old_lon = view ? view->lon : 0.0;
    const double old_lat = view ? view->lat : 0.0;

    if (!view || !(factor > 0.0) || !isfinite(factor)) {
        return false;
    }
    if (view->scale <= 0.0) {
        return false;
    }
    scale_new = view->scale * factor;
    if (scale_new < 0.25) {
        scale_new = 0.25;
    } else if (scale_new > 8.0) {
        scale_new = 8.0;
    }
    if (fabs(scale_new - view->scale) < 1e-9) {
        return false;
    }

    if (!vmap_view_viewport_to_geo(view, sx, sy, &lon0, &lat0)) {
        return false;
    }

    /* 锚点就在正中（±1 px）= 普通中心缩放；否则按锚点移动中心。 */
    centered = abs((int)(sx - view->view_w / 2)) <= 1
        && abs((int)(sy - view->view_h / 2)) <= 1;

    vmap_latlong_to_pixel_d(lat0, lon0, view->zoom, &mx, &my);
    vmap_pixel_to_latlong_d(
        mx - ((double)sx - (double)view->view_w / 2.0) / scale_new,
        my - ((double)sy - (double)view->view_h / 2.0) / scale_new,
        view->zoom, &nlon, &nlat);
    if (!isfinite(nlon) || !isfinite(nlat)) {
        return false;
    }

    view->scale = scale_new;
    view->lon = nlon;
    view->lat = nlat;
    view->render_lon = nlon;
    view->render_lat = nlat;
    view->pan_dx = 0.0;
    view->pan_dy = 0.0;
    view->pan_rx = 0.0;
    view->pan_ry = 0.0;
    view->last_pan_px = INT32_MIN;
    view->last_pan_py = INT32_MIN;
    view->paint_zoom = -1;      /* 倍率变了：整幅清屏重画 */
    view->paint_scale = 0.0;
    if (view->canvas) {
        vmap_view_apply_canvas_pos(view, 0.0, 0.0);
    }

    if (!centered) {
        /* 锚点不在中心 ⇒ 视图中心离开车辆，跟随必须让位（与拖动同语义）。 */
        if (view->veh_lon == 0.0 && view->veh_lat == 0.0) {
            view->veh_lon = old_lon;    /* 从未设过跟车点：拿旧中心兜底 */
            view->veh_lat = old_lat;
        }
        view->follow_mode = false;
    } else {
        /* 中心缩放：视图中心就是锚点，跟车点跟着走（箭头仍钉在正中）。 */
        view->veh_lon = nlon;
        view->veh_lat = nlat;
    }

    if (view->render_rev != 0u && view->canvas && view->cbuf) {
        if (view->pump_phase != 0u) {
            view->pump_phase = 0;
            view->pump_need_region = 0;
            view->pump_dirty_n = 0;
            view->restamp_pending = 0;
            view->restamp_full_active = 0;
            dvfs_map_busy(false);
        }
        vmap_view_render(view);
    }
    return true;
}

/**
 * @brief 当前视口尺寸（不含 overscan）。
 */
void vmap_view_get_viewport(const vmap_view_t * view, int32_t * w, int32_t * h)
{
    if (w) {
        *w = view ? view->view_w : 0;
    }
    if (h) {
        *h = view ? view->view_h : 0;
    }
}

int32_t vmap_view_get_margin(const vmap_view_t * view)
{
    return view ? view->margin : 0;
}

void vmap_view_set_parent_autosize(vmap_view_t * view, bool enable)
{
    if (view) {
        view->parent_autosize = enable;
    }
}

bool vmap_view_get_parent_autosize(const vmap_view_t * view)
{
    return view != NULL && view->parent_autosize;
}

void vmap_view_get_pan(const vmap_view_t * view, int32_t * dx, int32_t * dy)
{
    if (dx) {
        *dx = view ? (int32_t)lround(view->pan_dx) : 0;
    }
    if (dy) {
        *dy = view ? (int32_t)lround(view->pan_dy) : 0;
    }
}

/**
 * @brief 运行时重建画布（视口 + overscan）。
 *
 * 先把新缓冲全部分配好再提交，任何一步失败都不改变现视图。画布优先用板级
 * arena（与 create 同一策略）；放不下就回退普通 PSRAM 池。
 */
bool vmap_view_set_viewport(vmap_view_t * view, int32_t w, int32_t h)
{
    int32_t new_w;
    int32_t new_h;
    int32_t new_rw;
    int32_t new_rh;
    uint32_t new_stride;
    uint32_t new_rstride;
    void * new_cbuf = NULL;
    uint16_t * new_rbuf = NULL;
    bool new_cbuf_arena = false;
    bool old_cbuf_arena;
    void * old_cbuf;
    uint16_t * old_rbuf;

    if (!view || !view->canvas || w <= 0 || h <= 0) {
        return false;
    }
    if (w == view->view_w && h == view->view_h) {
        /* 尺寸没变：仍把父容器兜一次（外部可能改过它），保证"组件自足"不破。 */
        if (view->parent != NULL && view->parent_autosize
            && lv_obj_is_valid(view->parent)
            && (lv_obj_get_width(view->parent) != w
                || lv_obj_get_height(view->parent) != h)) {
            lv_obj_set_size(view->parent, w, h);
            lv_obj_update_layout(view->parent);
            if (view->arrow_layer == view->parent) {
                vmap_center_icon_apply(view);
            }
        }
        return true;
    }

    new_w = w + view->margin * 2;
    new_h = h + view->margin * 2;
    new_stride = lv_draw_buf_width_to_stride(new_w, LV_COLOR_FORMAT_RGB565);
    new_rw = w + 2 * view->r_margin;
    new_rh = h + 2 * view->r_margin;
    new_rstride = lv_draw_buf_width_to_stride(new_rw, LV_COLOR_FORMAT_RGB565);

    /* 旋转缓冲（航向朝上）：原来就有才重建，分配失败即整体放弃。 */
    if (view->rbuf != NULL) {
        new_rbuf = (uint16_t *)vmap_malloc((size_t)new_rstride * (size_t)new_rh);
        if (new_rbuf == NULL) {
            return false;
        }
        memset(new_rbuf, 0, (size_t)new_rstride * (size_t)new_rh);
    }

    /* 主画布：arena 放得下就复用（同一基址，按新 stride 重新解释）。 */
    {
        const size_t need = (size_t)new_stride * (size_t)new_h;
        size_t arena_bytes = 0;
        void * arena = board_psram_canvas_arena(&arena_bytes);

        if (arena != NULL && need > 0 && need <= arena_bytes) {
            new_cbuf = arena;
            new_cbuf_arena = true;
        } else {
            new_cbuf = vmap_malloc(need);
        }
        if (new_cbuf == NULL) {
            if (new_rbuf) {
                vmap_free(new_rbuf);
            }
            return false;
        }
        memset(new_cbuf, 0, need);
    }

    /* ── 提交：中止半帧 → 换缓冲 → 同步所有几何/缓存状态 ─────────────── */
    if (view->pump_phase != 0u) {
        view->pump_phase = 0;
        view->pump_need_region = 0;
        view->pump_dirty_n = 0;
        view->restamp_pending = 0;
        view->restamp_full_active = 0;
        dvfs_map_busy(false);
    }

    old_cbuf = view->cbuf;
    old_cbuf_arena = view->cbuf_arena;
    old_rbuf = view->rbuf;

    view->cbuf = new_cbuf;
    view->cbuf_arena = new_cbuf_arena;
    view->w = new_w;
    view->h = new_h;
    view->view_w = w;
    view->view_h = h;
    view->stride = new_stride;

    /* C 被整块换掉：上一次整幅就绪快照与帧身份都作废。 */
    view->heading_switch_ready = 0;
    view->restamp_full_active = 0;

    lv_canvas_set_buffer(view->canvas, view->cbuf, view->w, view->h,
        LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(view->canvas, view->w, view->h);
    view->last_pan_px = INT32_MIN;
    view->last_pan_py = INT32_MIN;
    vmap_view_apply_canvas_pos(view, view->pan_dx, view->pan_dy);

    /*
     * 组件自足：父容器（视口容器）也一起改。它决定可见区域的裁切矩形，尺寸不同步
     * 的话调用方还得再写一遍 lv_obj_set_size —— WP-VMAPAPI/R1 §4.2 的缺口。
     * 只在 autosize 打开时做（create 时父容器恰好等于视口 ⇒ 默认打开）。
     */
    if (view->parent != NULL && view->parent_autosize
        && lv_obj_is_valid(view->parent)) {
        lv_obj_set_size(view->parent, w, h);
        lv_obj_update_layout(view->parent);
        if (view->arrow_layer == view->parent) {
            vmap_center_icon_apply(view);   /* 图标钉在容器中心 ⇒ 重新居中 */
        }
    }

    if (old_cbuf != NULL && !old_cbuf_arena) {
        vmap_free(old_cbuf);
    }

    if (view->rcanvas != NULL) {
        view->rbuf = new_rbuf;
        view->rw = new_rw;
        view->rh = new_rh;
        view->rstride = new_rstride;
        lv_canvas_set_buffer(view->rcanvas, view->rbuf, view->rw, view->rh,
            LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(view->rcanvas, view->rw, view->rh);
        view->r_rot10 = -1;     /* 强制下一次航向服务整幅重烘 */
        vmap_rot_place(view, 0, 0);
        if (old_rbuf != NULL) {
            vmap_free(old_rbuf);
        }
    }

    /* 标签收集器的视口几何跟着视口走（retained 占位随整幅重画作废）。 */
    vmap_label_collector_init(&view->label_col, view->labels, VMAP_LABEL_CAP,
        view->label_retained, VMAP_LABEL_RETAIN_CAP,
        view->w, view->h, view->view_w, view->view_h, view->margin);
    view->label_col.clip_n = 0;

    view->paint_zoom = -1;
    view->paint_scale = 0.0;
    view->map_rev++;
    lv_obj_invalidate(view->canvas);

    syslog(LOG_NOTICE, "[vmap] viewport %dx%d (canvas %dx%d%s)",
        (int)view->view_w, (int)view->view_h, (int)view->w, (int)view->h,
        view->cbuf_arena ? " arena" : " pool");

    if (view->render_rev != 0u) {
        vmap_view_render(view);
    }
    return true;
}

/**
 * @brief 中心图标：NULL = 不画。
 *
 * 换图/隐藏/换回都在运行时生效：新图按它自己的尺寸重设 pivot，**已下发的旋转角
 * 立刻重放一次**（否则换图后箭头会回到正北，直到下一次 update_arrow 才恢复 ——
 * 静止时就是永远不恢复）。
 */
static void vmap_center_icon_apply(vmap_view_t * view)
{
    int32_t keep_angle10;

    if (view == NULL || view->arrow_layer == NULL) {
        return;
    }

    if (view->center_icon == NULL) {
        if (view->arrow != NULL) {
            lv_anim_delete(view, vmap_arrow_angle_exec);
            view->disp_arrow_angle10 = INT32_MIN;
            lv_obj_add_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (view->arrow == NULL) {
        view->arrow = lv_img_create(view->arrow_layer);
        lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_SCROLLABLE);
    }

    keep_angle10 = view->last_arrow_angle10;

    lv_img_set_src(view->arrow, view->center_icon);
    lv_obj_update_layout(view->arrow_layer);
    lv_obj_update_layout(view->arrow);
    {
        const lv_coord_t aw = lv_obj_get_width(view->arrow);
        const lv_coord_t ah = lv_obj_get_height(view->arrow);

        lv_img_set_pivot(view->arrow, aw / 2, ah / 2);
    }
    lv_anim_delete(view, vmap_arrow_angle_exec);
    view->last_arrow_angle10 = INT32_MIN;
    view->disp_arrow_angle10 = INT32_MIN;

    if (view->follow_mode) {
        lv_obj_align(view->arrow, LV_ALIGN_CENTER, 0, view->y_bias);
        lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* 自由视图：位置由下一次 update_arrow 按经纬度摆；先藏起来避免闪到 (0,0)。 */
        lv_obj_add_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
    }

    /* 朝向与"画的是哪张图"无关：换图后按上一次的角度立刻重放（不补间）。 */
    if (keep_angle10 != INT32_MIN) {
        view->last_arrow_angle10 = keep_angle10;
        vmap_arrow_apply(view, keep_angle10, false);
    }
}

void vmap_view_set_center_icon(vmap_view_t * view, const void * src)
{
    if (!view) {
        return;
    }
    view->center_icon = src;
    vmap_center_icon_apply(view);
}

const void * vmap_view_get_center_icon(const vmap_view_t * view)
{
    return view ? view->center_icon : NULL;
}

/**
 * @brief 拖动开始：打断跟车。
 */
void vmap_view_pan_begin(vmap_view_t * view)
{
    if (!view) {
        return;
    }
    view->follow_mode = false;
    if (view->veh_lon == 0.0 && view->veh_lat == 0.0) {
        view->veh_lon = view->lon;
        view->veh_lat = view->lat;
    }
}

/**
 * @brief 像素级平移：(dx,dy) 为内容位移（右/下为正），走跟车同一条路径。
 */
void vmap_view_pan_px(vmap_view_t * view, int32_t dx, int32_t dy)
{
    double cx = 0.0;
    double cy = 0.0;
    double lon = 0.0;
    double lat = 0.0;
    double scale;

    if (!view || (dx == 0 && dy == 0)) {
        return;
    }
    scale = view->scale > 0.0 ? view->scale : 1.0;

    vmap_latlong_to_pixel_d(view->lat, view->lon, view->zoom, &cx, &cy);
    vmap_pixel_to_latlong_d(cx - (double)dx / scale, cy - (double)dy / scale,
        view->zoom, &lon, &lat);
    if (!isfinite(lon) || !isfinite(lat)) {
        return;
    }

    /* 拖动是用户意图：即使 follow_mode 关闭也必须移动地图。 */
    view_apply_center(view, lon, lat);
}

/**
 * @brief 拖动结束：保持当前中心，不回弹。
 */
void vmap_view_pan_end(vmap_view_t * view)
{
    if (!view) {
        return;
    }
    /* 这里刻意什么也不做：follow 保持打断状态，中心停在拖动位置。
     * 恢复居中必须显式 set_follow_mode(true) / center_on()。 */
    syslog(LOG_DEBUG, "[map] pan end lon=%.6f lat=%.6f follow=%d",
        view->lon, view->lat, (int)view->follow_mode);
}

bool vmap_view_is_following(const vmap_view_t * view)
{
    return view != NULL && view->follow_mode;
}

/**
 * @brief 路名标签字体。
 */
void vmap_view_set_font(vmap_view_t * view, const lv_font_t * font)
{
    if (view) {
        int32_t i;

        view->font = font;
        /* 正立路名对象也换字体（它们是独立对象，不走烘焙路径）。 */
        for (i = 0; i < VMAP_ROT_LABEL_MAX; i++) {
            if (view->rot_label[i] != NULL) {
                lv_obj_set_style_text_font(view->rot_label[i], font, 0);
            }
        }
    }
}

static double vmap_clamp_d(double v, double lo, double hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static void vmap_follow_log(const vmap_view_t * view, double lon, double lat)
{
    static uint32_t s_last;

    if (!view) {
        return;
    }
    if (s_last != 0u && lv_tick_elaps(s_last) < 2000u) {
        return;
    }
    s_last = lv_tick_get();
    /* 几何诊断（用户 2026-09-25 报"导航中箭头不居中"）：把视图对象的屏幕位置与尺寸
     * 一起打出来 —— 一眼看出是容器没上移，还是视图自己被别处摆了位置。 */
    {
        lv_area_t a;

        if (view->arrow != NULL) {
            lv_obj_get_coords(view->arrow, &a);
            syslog(LOG_DEBUG, "[map] arrow  y1=%d y2=%d cy=%d (地图可见窗口中心期望值见页面几何)",
                (int)a.y1, (int)a.y2, (int)((a.y1 + a.y2) / 2));
        }
    }
}

/**
 * @brief 跟车位移的核心：把地图中心移到 (lon,lat)。
 *
 * 未越 overscan 只平移画布；越界才 rebase（整幅重画）。拖动（pan_px）与
 * 跟车（set_center）共用这一条路径，避免两套几何。
 */
static void view_apply_center(vmap_view_t * view, double lon, double lat)
{
    double now_x = 0.0;
    double now_y = 0.0;
    double base_x = 0.0;
    double base_y = 0.0;
    double dx;
    double dy;
    double limit;

    /*
     * 用**浮点** map 像素算位移（不能用整数版）：z14 下 1 map px = 1.69 screen px，
     * 先把中心量化到整数 map px 会把"拖动 200 px"变成 199 px（WP-VMAPAPI/R1 §4.1）。
     * 浮点位移直接交给 apply_canvas_pos，它只在上屏那一步取整、并把小数留在
     * `pan_rx` 里；重锚时再按整数部分滚动像素（见 render_begin）。
     */
    vmap_latlong_to_pixel_d(lat, lon, view->zoom, &now_x, &now_y);
    vmap_latlong_to_pixel_d(view->render_lat, view->render_lon, view->zoom,
        &base_x, &base_y);

    dx = (now_x - base_x) * view->scale;
    dy = (now_y - base_y) * view->scale;
    view->lon = lon;
    view->lat = lat;

    /*
     * Follow must not decode tiles. vmap_tile_cache_warm() is a blocking
     * LFS parse (~150ms) on this thread; doing it every GNSS tick once pan
     * crossed the old prefetch threshold pinned bicycle_ui at ~80% idle~10%.
     * New tiles load in vmap_view_render_pump() when the canvas rebases.
     */
    limit = (double)view->margin - (double)VMAP_PAN_GUARD;

    /*
     * 航向朝上时，屏上看到的是**旋转后**的视口：它的四角离骑手是半对角线
     * r = hypot(view_w/2, view_h/2)（240x216 → 161 px），画布必须盖住它，否则
     * 45° 附近四角露底。骑手到画布边的余量 = margin + view_h/2 - |pan|，要求
     * 不小于 r ⇒ pan <= margin - (r - view_h/2) - guard。取较紧的竖向。
     */
    if (view->heading_up) {
        const double r = hypot((double)view->view_w / 2.0,
            (double)view->view_h / 2.0);

        limit -= r - (double)view->view_h / 2.0;
        if (limit < 8.0) {
            limit = 8.0;
        }
    }

    if (dx >= -limit && dx <= limit && dy >= -limit && dy <= limit) {
        bicycle_c_debug_map_soft_pan();
        vmap_view_apply_canvas_pos(view, dx, dy);
        vmap_follow_log(view, lon, lat);
        return;
    }

    /* Soft-pan even when tiles cannot be reloaded (BLE LFS hold / in-flight pump). */
    if (view->pump_phase != 0 || myvendor_mtp_lfs_quiesce()) {
        bicycle_c_debug_map_soft_pan();
        vmap_view_apply_canvas_pos(view,
            vmap_clamp_d(dx, -limit, limit),
            vmap_clamp_d(dy, -limit, limit));
        vmap_follow_log(view, lon, lat);
        return;
    }

    bicycle_c_debug_map_full_render();
    vmap_view_render(view);
}

/**
 * @brief 跟车：改中心经纬度。未越 overscan 只平移 canvas。
 * @note follow_mode 关闭（被拖动打断）后不再移动地图：中心由用户控制，
 *       本调用只更新跟车点（箭头/恢复跟车用），不会把视图拉回去。
 */
void vmap_view_set_center(vmap_view_t * view, double lon, double lat)
{
    if (!view) {
        return;
    }

    view->veh_lon = lon;
    view->veh_lat = lat;

    if (!view->follow_mode) {
        return;
    }

    view_apply_center(view, lon, lat);
}

/**
 * @brief 绑定定位箭头所在 LVGL 层。
 */
void vmap_view_set_y_bias(vmap_view_t * view, int32_t bias)
{
    if (!view || view->y_bias == bias) {
        return;
    }

    view->y_bias = bias;

    /* 清掉角度去重，让箭头下一次更新必定重新摆位；否则角度没变就早退，
     * 箭头会留在偏置之前的位置上。 */
    view->last_arrow_angle10 = INT32_MIN;

    /* 画布立刻按新偏置重摆一次：导航开始/结束时地图常常是静止的，
     * 不能等下一次 pan 才生效。还没摆过（last_pan_* 未初始化）就交给下一次。 */
    if (view->canvas && view->last_pan_px != INT32_MIN
        && view->last_pan_py != INT32_MIN) {
        /* 走同一套位置公式：航向朝上时它还要做旋转补偿，不能在这里重写一遍。 */
        view->last_pan_px = INT32_MIN;
        view->last_pan_py = INT32_MIN;
        vmap_view_apply_canvas_pos(view, view->pan_dx, view->pan_dy);
    }
}

void vmap_view_bind_arrow_layer(vmap_view_t * view, lv_obj_t * layer)
{
    if (view) {
        view->arrow_layer = layer;
        /* 图标可能先于图层设置（组件化 API 允许任意顺序）：绑定时补建。 */
        if (layer != NULL && view->center_icon != NULL) {
            vmap_center_icon_apply(view);
        }
    }
}

/**
 * @brief 设置箭头图源。
 * @note 兼容入口：等价于 `vmap_view_set_center_icon(view, src)`；src 为 NULL
 *       时隐藏中心图标（组件化 API 的"不画"）。
 */
void vmap_view_set_arrow_image(vmap_view_t * view, const void * src)
{
    vmap_view_set_center_icon(view, src);
}

/**
 * @brief 跟随模式：箭头钉在视口中心。
 *
 * 由关闭切到开启时，把地图拉回最近一次跟车点（`veh_lon/veh_lat`），实现
 * "拖动后恢复跟随"；首帧之前只记录状态，不提前开帧。
 */
void vmap_view_set_follow_mode(vmap_view_t * view, bool enable)
{
    if (!view) {
        return;
    }
    if (view->follow_mode == enable) {
        return;
    }
    view->follow_mode = enable;

    if (!enable) {
        /* 打断跟车：中心停在原地。**不要**覆盖 veh——GNSS 仍在用
         * `vmap_view_set_center()` 更新跟车点，恢复跟随时要回到真正的车位置。 */
        return;
    }

    /* 恢复跟车：回中到跟车点（软平移/rebase 与 GNSS 跟车同一路径）。 */
    view->lon = view->veh_lon;
    view->lat = view->veh_lat;
    view_apply_center(view, view->veh_lon, view->veh_lat);
    if (view->arrow != NULL) {
        lv_obj_align(view->arrow, LV_ALIGN_CENTER, 0, view->y_bias);
        lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief 按经纬度与航向摆箭头。
 */
void vmap_view_update_arrow(vmap_view_t * view, double lon, double lat, float course_deg)
{
    int32_t sx;
    int32_t sy;
    int32_t angle10;

    if (!view || !view->arrow) {
        return;
    }

    /* 航向朝上：地图已经转到"骑行方向朝上"，箭头就该固定指上；否则按航向转。 */
    angle10 = view->heading_up ? 0
        : ((int32_t)lroundf(course_deg * 10.0f) % 3600 + 3600) % 3600;
    vmap_label_collector_set_focus(&view->label_col,
        view->w / 2 + (int32_t)lround(view->pan_dx),
        view->h / 2 + (int32_t)lround(view->pan_dy));

    if (view->follow_mode) {
        const bool hidden = lv_obj_has_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);

        if (angle10 == view->last_arrow_angle10 && !hidden
            && view->disp_arrow_angle10 == angle10) {
            return;
        }
        view->last_arrow_angle10 = angle10;
        lv_obj_align(view->arrow, LV_ALIGN_CENTER, 0, view->y_bias);
        vmap_arrow_apply(view, angle10, !hidden);
        lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(view->arrow);
        return;
    }

    if (!vmap_view_geo_to_viewport(view, lon, lat, &sx, &sy)) {
        lv_anim_delete(view, vmap_arrow_angle_exec);
        view->disp_arrow_angle10 = INT32_MIN;
        lv_obj_add_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    {
        const bool hidden = lv_obj_has_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);

        if (!hidden && angle10 == view->last_arrow_angle10
            && sx == lv_obj_get_x(view->arrow) + lv_obj_get_width(view->arrow) / 2
            && sy == lv_obj_get_y(view->arrow) + lv_obj_get_height(view->arrow) / 2
            && view->disp_arrow_angle10 == angle10) {
            return;
        }

        view->last_arrow_angle10 = angle10;
        lv_obj_set_pos(view->arrow,
            sx - lv_obj_get_width(view->arrow) / 2,
            sy - lv_obj_get_height(view->arrow) / 2 + view->y_bias);
        vmap_arrow_apply(view, angle10, !hidden);
        lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(view->arrow);
    }
}

/**
 * @brief 显示或隐藏定位箭头。
 */
void vmap_view_set_arrow_visible(vmap_view_t * view, bool visible)
{
    if (!view || !view->arrow) {
        return;
    }

    if (visible) {
        /* 中心图标被显式设成"不画"时，不要被 visible=true 意外放出来。 */
        if (view->center_icon != NULL) {
            lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    /* 隐藏即中止补间；重新显示时由 update_arrow 直接 snap。 */
    lv_anim_delete(view, vmap_arrow_angle_exec);
    view->disp_arrow_angle10 = INT32_MIN;
    lv_obj_add_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 把箭头提到 map_area 最前。
 */
void vmap_view_bring_arrow_to_front(vmap_view_t * view)
{
    if (view && view->arrow) {
        lv_obj_move_foreground(view->arrow);
    }
}

/**
 * @brief 丢弃瓦片索引/arena 并重绘。
 * @return 0 成功，负 errno 失败。
 */
void vmap_view_reload_storage(vmap_view_t * view)
{
    if (!view || !view->cache || myvendor_mtp_lfs_quiesce()) {
        return;
    }

    vmap_view_set_tile_dir(view, VMAP_TILE_DIR);
    vmap_tile_cache_set_zoom(view->cache, view->zoom);
    render_begin(view, true, true, false);
    vmap_view_bring_arrow_to_front(view);
}

/**
 * @brief 关闭已打开的地图分片句柄。
 */
void vmap_view_release_storage(vmap_view_t * view)
{
    if (!view || !view->cache) {
        return;
    }
    vmap_tile_cache_release_lfs(view->cache);
    /* 句柄已关，半帧 rebase 不能再解算；MTP reload 会 force_clear 重画。 */
    if (view->pump_phase != 0) {
        view->pump_phase = 0;
        view->pump_need_region = 0;
        view->pump_dirty_n = 0;
        view->restamp_pending = 0;
        view->restamp_full_active = 0;
        dvfs_map_busy(false);
        syslog(LOG_NOTICE, "[map] abort pump (lfs released)");
    }
}

static void view_canvas(const vmap_view_t * view, vmap_canvas_t * canvas,
    bool clip_dirty)
{
    uint8_t i;

    canvas->buf = (uint16_t *)view->cbuf;
    canvas->w = view->w;
    canvas->h = view->h;
    canvas->stride = view->stride;
    vmap_render_clip_reset(canvas);
    if (!clip_dirty) {
        return;
    }
    for (i = 0; i < view->pump_dirty_n; i++) {
        vmap_render_clip_add(canvas,
            view->pump_dirty_x1[i], view->pump_dirty_y1[i],
            view->pump_dirty_x2[i], view->pump_dirty_y2[i]);
    }
}

static void pump_dirty_add(vmap_view_t * view, int32_t x1, int32_t y1,
    int32_t x2, int32_t y2)
{
    uint8_t i;

    if (!view || x1 >= x2 || y1 >= y2) {
        return;
    }

    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 > view->w) {
        x2 = view->w;
    }
    if (y2 > view->h) {
        y2 = view->h;
    }
    if (x1 >= x2 || y1 >= y2 || view->pump_dirty_n >= VMAP_RENDER_DIRTY_MAX) {
        return;
    }

    i = view->pump_dirty_n;
    view->pump_dirty_x1[i] = x1;
    view->pump_dirty_y1[i] = y1;
    view->pump_dirty_x2[i] = x2;
    view->pump_dirty_y2[i] = y2;
    view->pump_dirty_n++;
}

static void pump_dirty_from_scroll(vmap_view_t * view, int32_t sx, int32_t sy)
{
    const int32_t pad = (int32_t)VMAP_RENDER_DIRTY_PAD;

    view->pump_dirty_n = 0;
    if (sx > 0) {
        pump_dirty_add(view, view->w - sx - pad, 0, view->w, view->h);
    } else if (sx < 0) {
        pump_dirty_add(view, 0, 0, -sx + pad, view->h);
    }
    if (sy > 0) {
        pump_dirty_add(view, 0, view->h - sy - pad, view->w, view->h);
    } else if (sy < 0) {
        pump_dirty_add(view, 0, 0, view->w, -sy + pad);
    }
}

static bool pump_rects_overlap(int32_t ax1, int32_t ay1, int32_t ax2, int32_t ay2,
    int32_t bx1, int32_t by1, int32_t bx2, int32_t by2)
{
    return ax1 < bx2 && ax2 > bx1 && ay1 < by2 && ay2 > by1;
}

static bool pump_tile_dirty(const vmap_view_t * view, int32_t tx, int32_t ty)
{
    const int32_t tile_size = 256;
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
    uint8_t i;

    if (!view || view->pump_dirty_n == 0) {
        return true;
    }

    x1 = (int32_t)lround((tx * tile_size - view->pump_cx) * view->scale
        + view->w / 2.0);
    y1 = (int32_t)lround((ty * tile_size - view->pump_cy) * view->scale
        + view->h / 2.0);
    x2 = (int32_t)lround(x1 + 256.0 * view->scale);
    y2 = (int32_t)lround(y1 + 256.0 * view->scale);

    for (i = 0; i < view->pump_dirty_n; i++) {
        if (pump_rects_overlap(x1, y1, x2, y2,
                view->pump_dirty_x1[i], view->pump_dirty_y1[i],
                view->pump_dirty_x2[i], view->pump_dirty_y2[i])) {
            return true;
        }
    }
    return false;
}

static bool pump_advance(vmap_view_t * view)
{
    view->pump_ty--;
    if (view->pump_ty < view->pump_ty_min) {
        view->pump_tx--;
        view->pump_ty = view->pump_ty_max;
    }
    return view->pump_tx >= view->pump_tx_min;
}

static void render_begin(vmap_view_t * view, bool reset_view, bool force_clear,
    bool full_restamp)
{
    vmap_canvas_t canvas;
    const double origin_lon = reset_view ? view->lon : view->render_lon;
    const double origin_lat = reset_view ? view->lat : view->render_lat;
    int32_t sx;
    int32_t sy;
    bool scrolled = false;

    /* 先置"帧在飞"再做任何 C 修改：期间的位置更新不能烘半成品。
     * 两条路径（reset_view / keep-pan）都必须置位，否则泵不会推进。 */
    view->pump_phase = 1;
    view->pump_need_region = 1;
    /* 这一帧是不是 keep-pan 整幅重铺：瓦片失败填底色、航向切换都靠它。 */
    view->restamp_full_active = full_restamp ? 1u : 0u;
    /* 新帧会改 C：上一次整幅就绪的快照作废，等本帧（整幅）结束再置 1。 */
    view->heading_switch_ready = 0;

    /* fopen/.vpk index belongs on the first pump tick, not GNSS/REC/set_view. */

    vmap_perf_frame_begin();
    VMAP_PERF_T0();
    dvfs_map_busy(true);
    view->pump_dirty_n = 0;
    sx = (view->last_pan_px == INT32_MIN) ? (int32_t)lround(view->pan_dx)
                                         : view->last_pan_px;
    sy = (view->last_pan_py == INT32_MIN) ? (int32_t)lround(view->pan_dy)
                                         : view->last_pan_py;

    if (reset_view) {
        /*
         * A hidden page can jump much farther than the last on-screen pan.
         * Derive the pixel migration from the two geographic origins instead
         * of trusting last_pan_px.  Otherwise a 1 km jump with a 14 px old pan
         * scrolls only 14 px, then marks the whole interior as the new origin.
         */
        const int32_t disp_sx = sx;     /* 当前屏上整数位移 */
        const int32_t disp_sy = sy;
        double base_lon = origin_lon;
        double base_lat = origin_lat;
        double rx = 0.0;
        double ry = 0.0;

        if (!force_clear && view->render_rev > 0
            && view->paint_zoom == view->zoom
            && view->paint_scale == view->scale) {
            double old_x = 0.0;
            double old_y = 0.0;
            double new_x = 0.0;
            double new_y = 0.0;
            double dsx;
            double dsy;

            vmap_latlong_to_pixel_d(view->render_lat, view->render_lon,
                view->zoom, &old_x, &old_y);
            vmap_latlong_to_pixel_d(origin_lat, origin_lon,
                view->zoom, &new_x, &new_y);
            dsx = (new_x - old_x) * view->scale;
            dsy = (new_y - old_y) * view->scale;
            sx = (int32_t)lround(dsx);
            sy = (int32_t)lround(dsy);

            /*
             * 亚像素重锚：若这次位移正是"屏上已经显示的那个整数位移"（拖动/跟车
             * 的延续），新原点只推进**整数**屏幕量：滚动 sx 像素 + 对象从 -sx 归 0，
             * 内容一像素都不重采样；剩下的 |小数| < 0.5 记在 pan_rx/pan_ry 里，
             * 下一拍 `view_apply_center()` 从浮点中心重新算出来。
             * 不做这一步的话，取整误差会被写进新原点，200 px 的拖动能变成 199 px。
             */
            if (sx == disp_sx && sy == disp_sy) {
                vmap_pixel_to_latlong_d(old_x + (double)sx / view->scale,
                    old_y + (double)sy / view->scale, view->zoom,
                    &base_lon, &base_lat);
                rx = dsx - (double)sx;
                ry = dsy - (double)sy;
            }
        }

        bool can_scroll = !force_clear && view->render_rev > 0
            && view->paint_zoom == view->zoom
            && view->paint_scale == view->scale
            && (sx != 0 || sy != 0)
            && sx > -view->w && sx < view->w
            && sy > -view->h && sy < view->h;

        view->render_lon = base_lon;
        view->render_lat = base_lat;
        view_canvas(view, &canvas, false);
        if (can_scroll) {
            {
                VMAP_PERF_T0S();
                vmap_render_scroll(&canvas, sx, sy, vmap_style_bg565());
                pump_dirty_from_scroll(view, sx, sy);
                VMAP_PERF_T1S(scroll);
            }
            {
                VMAP_PERF_T0S();
                vmap_label_retained_scroll(&view->label_col, sx, sy,
                    VMAP_RENDER_DIRTY_MAX, &view->pump_dirty_n,
                    view->pump_dirty_x1, view->pump_dirty_y1,
                    view->pump_dirty_x2, view->pump_dirty_y2);
                VMAP_PERF_T1S(lscroll);
            }
            scrolled = true;
            {
                VMAP_PERF_T0S();
                lv_obj_invalidate(view->canvas);
                VMAP_PERF_T1S(inval);
            }
        } else {
            VMAP_PERF_T0S();
            vmap_render_clear(&canvas, vmap_style_bg565());
            vmap_label_retained_reset(&view->label_col);
            VMAP_PERF_T1S(scroll);  /* 清屏路径复用同一格 */
        }

        view->pan_dx = 0.0;
        view->pan_dy = 0.0;
        view->last_pan_px = INT32_MIN;
        view->last_pan_py = INT32_MIN;
        vmap_view_apply_canvas_pos(view, 0.0, 0.0);
        /* 新的亚像素余量：相对**新原点**的请求位移（下一次 apply 会重新算，
         * 这里存下来只为 get_pan/诊断与 y_bias 重摆时保持连续）。 */
        view->pan_rx = rx;
        view->pan_ry = ry;
    } else {
        /* keep-pan redraw repaints every base tile, erasing all baked labels. */
        vmap_label_retained_reset(&view->label_col);
    }

    vmap_label_collector_reset(&view->label_col);
    view->label_col.clip_n = 0;
    vmap_label_collector_set_focus(&view->label_col,
        view->w / 2 + (int32_t)lround(view->pan_dx),
        view->h / 2 + (int32_t)lround(view->pan_dy));

    /* 浮点投影原点：与滚动后的像素同一网格（整数部分已滚走，小数留在这里）。 */
    vmap_latlong_to_pixel_d(view->render_lat, view->render_lon, view->zoom,
        &view->pump_cx_d, &view->pump_cy_d);
    view->pump_cx = (int)lround(view->pump_cx_d);
    view->pump_cy = (int)lround(view->pump_cy_d);
    /*
     * ring=0（2026-10-27，对齐 X-TRACK 的 tile rect 语义）：
     * 画布本身已经带了 margin 的 overscan，再往外加一圈瓦片只会选到**完全落在
     * 画布之外**的瓦片 —— 近边不超过画布边，`vmap_render_draw_tile` 的像素写入
     * 与 `label_pan_canvas_bounds()` 的标签收集都会把它们裁掉，却仍然要付要素
     * 遍历（道路层还要按 class 扫 14 遍）和首帧的闪存读。
     * 代价还落在节拍上：泵每 20 ms 只画 2 张，且从 (tx_max,ty_max) 开始扫，
     * 多出来的一圈排在**可见区域之前**。实测几何（240x216 视口、margin 128、
     * scale 1.69）是 16~20 张里只有 4~6 张与画布相交。
     */
    tile_range(view->pump_cx, view->pump_cy, view->w, view->h, view->scale, 0,
        &view->pump_tx_min, &view->pump_ty_min,
        &view->pump_tx_max, &view->pump_ty_max);
    if (full_restamp) {
        /* 取整原点与绘制的浮点原点可能差 1 像素；整幅重铺多画一圈边瓦片，
         * 保证 C 的四边也被瓦片（或失败时的底色填充）覆盖。 */
        view->pump_tx_min--;
        view->pump_ty_min--;
        view->pump_tx_max++;
        view->pump_ty_max++;
    }
    view->pump_tx = view->pump_tx_max;
    view->pump_ty = view->pump_ty_max;

    if (scrolled) {
        syslog(LOG_INFO,
            "[map] rebase scroll=%d,%d dirty=%u labels=%d z=%d",
            (int)sx, (int)sy, (unsigned)view->pump_dirty_n,
            (int)view->label_col.retained_count, view->zoom);
    }
    VMAP_PERF_T1(begin);
}

/**
 * @brief 瓦片没能贴上来时，把该瓦片的屏幕矩形填回底色。
 * @details 只在 keep-pan 整幅重铺的帧里调用：正常帧的 warm 失败保留旧内容即可；
 *          重铺帧失败若不填，旧路名墨迹会留在原处并被转进 R。
 */
static void vmap_clear_tile_rect(vmap_view_t * view, int32_t tx, int32_t ty)
{
    vmap_blit_surface_t surf;
    const double scale = view->scale;
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;

    if (!view || !view->cbuf) {
        return;
    }

    x1 = (int32_t)floor(((double)tx * 256.0 - view->pump_cx_d) * scale
        + view->w / 2.0);
    y1 = (int32_t)floor(((double)ty * 256.0 - view->pump_cy_d) * scale
        + view->h / 2.0);
    /* 两端都 floor：像素按 [x1,x2) 归属，相邻瓦片不共享列/行，
     * 失败填充不会擦到已画好邻居的边缘。 */
    x2 = (int32_t)floor(((double)tx * 256.0 - view->pump_cx_d) * scale
        + view->w / 2.0 + 256.0 * scale);
    y2 = (int32_t)floor(((double)ty * 256.0 - view->pump_cy_d) * scale
        + view->h / 2.0 + 256.0 * scale);
    if (x2 <= x1) {
        x2 = x1 + 1;
    }
    if (y2 <= y1) {
        y2 = y1 + 1;
    }

    surf.buf = view->cbuf;
    surf.w = view->w;
    surf.h = view->h;
    surf.stride = view->stride;
    (void)vmap_blit_fill(vmap_blit_get(), &surf, x1, y1, x2 - x1, y2 - y1,
        vmap_style_bg565());
}

static bool render_next_tile(vmap_view_t * view)
{
    const int32_t tile_size = 256;
    const double scale = view->scale;
    vmap_canvas_t canvas;
    vmap_tile_t tile;
    float base_x;
    float base_y;
    int32_t tx;
    int32_t ty;
    int32_t ix1;
    int32_t iy1;
    int32_t ix2;
    int32_t iy2;

    if (view->pump_need_region) {
        VMAP_PERF_T0();
        view->pump_need_region = 0;
        if (view->cache) {
            myvendor_watchdog_ui_beat();
            myvendor_watchdog_busy_pump();
            vmap_tile_cache_ensure_region(view->cache,
                view->render_lon, view->render_lat);
            myvendor_watchdog_ui_beat();
        }
        VMAP_PERF_T1(warm);
        return true;
    }

    while (view->pump_tx >= view->pump_tx_min) {
        tx = view->pump_tx;
        ty = view->pump_ty;

        if (!pump_tile_dirty(view, tx, ty)) {
            VMAP_PERF_T0();
            if (view->font
                && vmap_tile_cache_cached(view->cache, view->zoom, (int)tx, (int)ty)
                && vmap_tile_cache_get(view->cache, view->zoom, (int)tx, (int)ty,
                    &tile)) {
                base_x = (float)(((double)tx * tile_size - view->pump_cx_d)
                    * scale + view->w / 2.0);
                base_y = (float)(((double)ty * tile_size - view->pump_cy_d)
                    * scale + view->h / 2.0);
                vmap_label_collector_add_tile(&view->label_col, &tile,
                    base_x, base_y, (float)scale);
            }
            VMAP_PERF_T1(meta);
            if (!pump_advance(view)) {
                return false;
            }
            continue;
        }

        if (!vmap_tile_cache_cached(view->cache, view->zoom, (int)tx, (int)ty)) {
            bool warmed;

            VMAP_PERF_T0();
            warmed = vmap_tile_cache_warm(view->cache, view->zoom,
                    (int)tx, (int)ty);
            VMAP_PERF_T1(warm);
            if (!warmed) {
                /* 邻格 heap 暂时放不下：跳过，继续画已在 PSRAM 的瓦片。 */
                if (view->restamp_full_active) {
                    vmap_clear_tile_rect(view, tx, ty);
                }
                if (!pump_advance(view)) {
                    return false;
                }
                continue;
            }
            VMAP_PERF_N(n_warms);
            return true;
        }

        view_canvas(view, &canvas, true);
        base_x = (float)(((double)tx * tile_size - view->pump_cx_d) * scale
            + view->w / 2.0);
        base_y = (float)(((double)ty * tile_size - view->pump_cy_d) * scale
            + view->h / 2.0);

        if (vmap_tile_cache_get(view->cache, view->zoom, (int)tx, (int)ty,
                &tile)) {
            uint32_t st = 0u;
            uint32_t by = 0u;

            VMAP_PERF_T0();
            vmap_render_stat_reset();
            vmap_render_draw_tile(&canvas, &tile, base_x, base_y, (float)scale,
                view->scratch, VMAP_SCRATCH_CAP);
            VMAP_PERF_T1(tile);
            VMAP_PERF_N(n_tiles);
            vmap_render_stat_get(&st, &by);
            s_pf_stores += st;
            s_pf_bytes += by;
            if (view->font) {
                vmap_label_collector_add_tile(&view->label_col, &tile,
                    base_x, base_y, (float)scale);
            }

            ix1 = (int32_t)floor((double)base_x);
            iy1 = (int32_t)floor((double)base_y);
            ix2 = (int32_t)ceil((double)base_x + 256.0 * scale);
            iy2 = (int32_t)ceil((double)base_y + 256.0 * scale);
            if (view->pump_dirty_n == 0) {
                vmap_view_invalidate_canvas_area(view, ix1, iy1, ix2, iy2);
            } else {
                uint8_t di;

                for (di = 0; di < view->pump_dirty_n; di++) {
                    int32_t vx1 = ix1 > view->pump_dirty_x1[di]
                        ? ix1 : view->pump_dirty_x1[di];
                    int32_t vy1 = iy1 > view->pump_dirty_y1[di]
                        ? iy1 : view->pump_dirty_y1[di];
                    int32_t vx2 = ix2 < (view->pump_dirty_x2[di] - 1)
                        ? ix2 : (view->pump_dirty_x2[di] - 1);
                    int32_t vy2 = iy2 < (view->pump_dirty_y2[di] - 1)
                        ? iy2 : (view->pump_dirty_y2[di] - 1);

                    if (vx1 <= vx2 && vy1 <= vy2) {
                        vmap_view_invalidate_canvas_area(view, vx1, vy1, vx2, vy2);
                    }
                }
            }
        } else if (view->restamp_full_active) {
            vmap_clear_tile_rect(view, tx, ty);
        }

        if (!pump_advance(view)) {
            return false;
        }
        return true;
    }

    return false;
}

/*
 * Canvas stack, bottom → top:
 *   map tiles (already in buffer) → nav → labels → REC line → REC numbers.
 * clip_dirty: follow-rebase only restamps the new edge, so interior ink is not
 * redrawn fat. Labels use the same dirty rects.
 */
static void stamp_overlays(vmap_view_t * view, bool labels)
{
    vmap_canvas_t canvas;
    lv_area_t label_drawn;
    bool labels_drawn = false;

    if (!view || !view->canvas || !view->cbuf) {
        return;
    }

    view_canvas(view, &canvas, true);

#if VMAP_ROUTE_ENABLE
    if (view->route) {
        VMAP_PERF_T0();
        vmap_route_paint_to_canvas(view->route, &canvas, view);
        VMAP_PERF_T1P(VMAP_PERF_NAV_SLOT());
    }
#endif

    /* 航向朝上时路名会随画布转成斜字：该模式不烘焙标签（第一版取舍，见
     * vmap_config.h 的说明）。 */
    if (labels && view->font && !view->heading_up
        && view->label_col.count > 0) {
        VMAP_PERF_T0();
        labels_drawn = vmap_label_paint_to_canvas(
            view->canvas, &view->label_col, view->font,
            view->scale, view->pump_dirty_n,
            view->pump_dirty_x1, view->pump_dirty_y1,
            view->pump_dirty_x2, view->pump_dirty_y2, &label_drawn);
        VMAP_PERF_T1(label);
        VMAP_PERF_NB(n_labels, view->label_col.count);
    }

#if VMAP_TRACK_ENABLE
    if (view->track) {
        if (labels_drawn && canvas.clip_n > 0u) {
            int32_t x1 = label_drawn.x1;
            int32_t y1 = label_drawn.y1;
            int32_t x2 = label_drawn.x2 + 1;
            int32_t y2 = label_drawn.y2 + 1;
            uint8_t i;

            /*
             * New labels may be outside the tile dirty edge. Expand the REC
             * clip to their union so track ink is restored above those words.
             */
            for (i = 0; i < canvas.clip_n; i++) {
                if (canvas.clip_x1[i] < x1) {
                    x1 = canvas.clip_x1[i];
                }
                if (canvas.clip_y1[i] < y1) {
                    y1 = canvas.clip_y1[i];
                }
                if (canvas.clip_x2[i] > x2) {
                    x2 = canvas.clip_x2[i];
                }
                if (canvas.clip_y2[i] > y2) {
                    y2 = canvas.clip_y2[i];
                }
            }
            vmap_render_clip_reset(&canvas);
            vmap_render_clip_add(&canvas, x1, y1, x2, y2);
        }
        VMAP_PERF_T0();
        vmap_track_paint_to_canvas(view->track, &canvas);
        VMAP_PERF_T1P(VMAP_PERF_TRACK_SLOT());
    }
#endif
}

void vmap_view_stamp_nav_then_track(vmap_view_t * view)
{
    stamp_overlays(view, false);
    if (view != NULL) {
        if (view->heading_up) {
            /* 屏上是 R、C 是隐藏的：直接作废 C 不会更新画面。这里走和
             * invalidate_canvas_area 同一条记账/限频/兜底路径。 */
            view->rot_pending = true;
            vmap_rot_service(view);
        } else if (view->canvas) {
            lv_obj_invalidate(view->canvas);
        }
    }
    vmap_perf_stamp_end((uint32_t)(view ? view->zoom : 0));
}

static void render_finish(vmap_view_t * view)
{
    if (view->restamp_full_active) {
        view->restamp_full_active = 0;
        if (view->heading_up && view->heading_switch_pending) {
            /* 干净底图已整幅重铺完，下一步只等一次成功的 R 烘焙。 */
            view->heading_switch_ready = 1;
        }
    }
    stamp_overlays(view, true);
    /* 必须在 dvfs_map_busy(false) 之前：MAP hold 期间 HCLK 才是被钉住的那档。
     * 也要在 pump_dirty_n 被清零之前取，脏区条数是判"全量重画 / scroll 补边"的依据。 */
    vmap_perf_frame_end((uint32_t)view->zoom, (uint32_t)view->pump_dirty_n);

#if defined(CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE) && CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE
    bicycle_inval_probe_tag("vmap_render");
#endif
    view->render_rev++;
    view->paint_zoom = view->zoom;
    view->paint_scale = view->scale;
    view->pump_phase = 0;
    view->pump_need_region = 0;
    view->pump_dirty_n = 0;

    if (view->heading_up) {
        if (view->heading_switch_pending) {
            if (view->heading_switch_ready) {
                view->heading_switch_pending = 0;
                if (vmap_rot_full(view)) {
                    view->heading_switch_ready = 0;
                    if (view->canvas) {
                        lv_obj_add_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);
                    }
                    lv_obj_clear_flag(view->rcanvas, LV_OBJ_FLAG_HIDDEN);
                } else {
                    /* 烘焙失败：C 保持可见；5 Hz 航向拍会继续重试。 */
                    view->heading_switch_pending = 1;
                }
            }
        } else {
            vmap_rot_why('E');
            if (vmap_rot_full(view)) {
                if (view->canvas) {
                    lv_obj_add_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);
                }
                lv_obj_clear_flag(view->rcanvas, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        lv_obj_clear_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);
    }

    if (view->restamp_pending && !myvendor_mtp_lfs_quiesce()) {
        view->restamp_pending = 0;
        lv_obj_invalidate(view->canvas);
        render_begin(view, false, false, true);
        return;
    }
    if (view->heading_switch_pending && !view->heading_switch_ready
        && !myvendor_mtp_lfs_quiesce()) {
        /* 整幅重铺没跑成（例如 MTP 刚结束）：补一轮 keep-pan，完成后切 R。 */
        vmap_view_redraw_keep_pan(view);
        return;
    }
    dvfs_map_busy(false);
    if (!view->heading_up) {
        lv_obj_invalidate(view->canvas);
    }
    vmap_view_bring_arrow_to_front(view);
}

/**
 * @brief 开始一帧栅格化（只 begin，由 pump 画完）。
 */
void vmap_view_render(vmap_view_t * view)
{
    if (!view || !view->canvas || !view->cbuf || myvendor_mtp_lfs_quiesce()) {
        return;
    }

    view->restamp_pending = 0;
    render_begin(view, true, false, false);
}

void vmap_view_redraw_keep_pan(vmap_view_t * view)
{
    if (!view || !view->canvas || !view->cbuf || myvendor_mtp_lfs_quiesce()) {
        return;
    }
    if (view->render_rev == 0) {
        return;
    }
    if (view->pump_phase != 0) {
        view->restamp_pending = 1;
        syslog(LOG_NOTICE, "[map] restamp queued (pump busy)");
        return;
    }

    view->restamp_pending = 0;
    render_begin(view, false, false, true);
    syslog(LOG_NOTICE, "[map] restamp keep_pan z=%d tiles tx=%d..%d ty=%d..%d",
        view->zoom, (int)view->pump_tx_min, (int)view->pump_tx_max,
        (int)view->pump_ty_min, (int)view->pump_ty_max);
}

bool vmap_view_render_pump(vmap_view_t * view, unsigned max_tiles)
{
    unsigned n;
    uint32_t t0;

    if (!view || !view->canvas || !view->cbuf || myvendor_mtp_lfs_quiesce()) {
        return true;
    }

    if (view->pump_phase == 0) {
        /* 没有帧在飞 ⇒ 泵本来就不该被调用（帧结束后定时器自己暂停）。
         * **基准必须作废**，否则下一帧的第一拍会把"这一整段空闲"算成"UI 线程被占住"
         * —— 第一版就是这么误报的：`SLOW pump gap 9471ms` / `2228ms` 其实都是
         * "这段时间没渲染"（同期 BLE 每 10 秒的心跳照打，证明系统是活的）。 */
        s_pf_last_pump_cyc = 0u;
        return true;
    }

    if (max_tiles == 0) {
        max_tiles = VMAP_RENDER_PUMP_MAX_TILES;
    }

    t0 = lv_tick_get();
    myvendor_watchdog_ui_beat();
    /* 单拍间隔超长 ⇒ **无条件** WARNING（正常帧不受影响）。只在这一帧确实在飞时量，
     * 所以"没渲染的空闲"不会被算进来；而帧内一次 2 秒以上的拍间隔就只能是系统级
     * 停顿（现场两次 WDT 饥饿复位都伴随 1.6~12 秒的这类间隔，且当时 [vperf] 正被
     * 降到 debug、整场不可诊断）。 */
    {
        const uint32_t now_cyc = vmap_perf_cyc();

        if (s_pf_last_pump_cyc != 0u && now_cyc - s_pf_last_pump_cyc
            > VMAP_PERF_SLOW_GAP_MS * 240u * 1000u) {
            syslog(LOG_WARNING,
                "[vperf] SLOW pump gap %ums (>%ums) —— 帧内拍间隔异常，UI 线程被占住；若同时 WDT 停喂即饥饿族",
                (unsigned)((now_cyc - s_pf_last_pump_cyc) / 240000u),
                (unsigned)VMAP_PERF_SLOW_GAP_MS);
            vmap_perf_dump_runnable();
        }
        s_pf_last_pump_cyc = now_cyc;
    }

    for (n = 0; n < max_tiles && view->pump_phase != 0; n++) {
        if (n > 0 && lv_tick_elaps(t0) >= VMAP_RENDER_PUMP_BUDGET_MS) {
            break;
        }
        if (!render_next_tile(view)) {
            render_finish(view);
            break;
        }
        if (lv_tick_elaps(t0) >= VMAP_RENDER_PUMP_BUDGET_MS) {
            break;
        }
    }

    return view->pump_phase == 0;
}

bool vmap_view_render_busy(const vmap_view_t * view)
{
    return view != NULL && view->pump_phase != 0;
}

/**
 * @brief 地图存储世代（MTP reload 后递增）。
 */
uint32_t vmap_view_map_revision(const vmap_view_t * view)
{
    return view ? view->map_rev : 0;
}

/**
 * @brief 已完成的渲染帧计数；0 表示从未 render_finish。
 */
uint32_t vmap_view_render_revision(const vmap_view_t * view)
{
    return view ? view->render_rev : 0;
}
