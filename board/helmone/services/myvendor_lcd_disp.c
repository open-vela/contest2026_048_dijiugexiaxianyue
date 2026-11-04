/**
 * @file myvendor_lcd_disp.c
 * @brief 双条带缓冲 LCD flush：LVGL flush_cb 直接启动 LCDC/QSPI DMA，
 *        LCDC EOF 中断回调里调 lv_display_flush_ready（无 ui_flush 线程）。
 *
 * 历史实现把条带作业排给 ui_flush pthread，再由 worker 调 PUTAREA
 * （同步等待 DMA）+ done_cb。现改为：
 *
 *   UI 线程                      LCDC IRQ
 *   ------                       --------
 *   flush_cb(strip A) --+
 *                       +-> sf32lb_lcd_putarea_async() 启动 QSPI DMA
 *   LVGL 渲染 strip B      |            |
 *                          |            +-- EOF -> myvendor_lcd_disp_xfer_done()
 *                          |                        -> lv_display_flush_ready()
 *   flush_cb(strip B) -> 等 A 完成（若仍在传）-> 启动 B
 *
 * A/B 重叠由 LVGL 双缓冲 + 单笔在途 DMA 保证：flush_cb 只启动不等待，
 * UI 渲染 B 时硬件传 A；若 B 渲染先完成，提交路径在启动 B 前等待 A 的
 * 完成中断（semaphore 由 IRQ post），因此不会覆盖 A 正在读取的缓冲。
 *
 * SRAM 条带缓冲在 mpu_config() 里映射为 non-cacheable，提交前无需 cache
 * clean/invalidate；不要在这里加全屏 clean。
 *
 * recover 语义：worker 已删除，原先“detach 卡死的 worker 再重启”不再存在。
 * 这里改成基于 HAL/LCDC 状态的轻量检查：空闲时仅触发一次全屏重绘；DMA
 * 卡住时等待后放弃并返回 -EBUSY（安全 no-op，不撕裂面板状态机）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include "myvendor_lcd_disp.h"

#include "sf32lb_lcd_async.h"

#include <nuttx/lcd/lcd_dev.h>
#include <nuttx/video/fb.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifndef CONFIG_MYVENDOR_LCD_DISP_DEBUG
#  define CONFIG_MYVENDOR_LCD_DISP_DEBUG 0
#endif

#ifndef CONFIG_MYVENDOR_LCD_DISP_STRIP_ROWS
#  define CONFIG_MYVENDOR_LCD_DISP_STRIP_ROWS 60
#endif

#ifndef MYVENDOR_LCD_RECOVER_COOLDOWN_MS
#  define MYVENDOR_LCD_RECOVER_COOLDOWN_MS 8000u
#endif

#ifndef MYVENDOR_LCD_DEBUG_PERIOD_MS
#  define MYVENDOR_LCD_DEBUG_PERIOD_MS 5000u
#endif

struct myvendor_lcd_disp_ctx_s
{
  char              lcd_path[64];
  int               lcd_fd;
  uint16_t          hor_res;
  uint16_t          ver_res;
  uint16_t          strip_rows;
  uint32_t          stride;
  size_t            buf_bytes;
  struct lcddev_area_align_s area_align;

  pthread_mutex_t   lock;
  bool              sync_inited;
  bool              inited;

  atomic_bool       recover_in_progress;
  atomic_bool       recover_unblock_pending;
  atomic_bool       recover_ui_pending;
  atomic_bool       recover_hold_submit;
  atomic_bool       recover_refr_armed;
  atomic_uint       recover_quiet_until_ms;

  atomic_uint       busy_since_ms;

  atomic_uint       stat_submit_ok;
  atomic_uint       stat_submit_busy;
  atomic_uint       stat_submit_err;
  atomic_uint       stat_xfer_done;
  atomic_uint       stat_putarea_fail;
  atomic_uint       stat_flush_ready;
  atomic_uint       stat_wait_calls;
  atomic_uint       stat_wait_ms_max;
  atomic_uint       stat_wait_timeout;
  atomic_uint       stat_flush_ready_force;
  atomic_uint       stat_submit_retry;
  atomic_uint       stat_wait_stall;
  atomic_uint       stat_putarea_ms_max;
  atomic_uint       stat_putarea_slow;
  atomic_uint       stat_recover;
  atomic_uint       stat_xfer_err;
};

static struct myvendor_lcd_disp_ctx_s g_lcd_disp =
{
  .lcd_fd = -1,
};

/* Callbacks are written once at display create, read from the LCDC IRQ.
 * Atomic words avoid a mutex (and its ISR restrictions) on the hot path.
 */
static atomic_uintptr_t g_done_cb;
static atomic_uintptr_t g_done_ud;
static atomic_uintptr_t g_refr_cb;
static atomic_uintptr_t g_unblock_cb;
static atomic_uintptr_t g_recover_ud;

/** @brief CLOCK_MONOTONIC 毫秒。 */
static uint32_t myvendor_lcd_mono_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)((uint64_t)ts.tv_sec * 1000u +
                    (uint64_t)ts.tv_nsec / 1000000u);
}

/** @brief 记录 busy_since 时间戳。 */
static void myvendor_lcd_note_busy(void)
{
  atomic_store_explicit(&g_lcd_disp.busy_since_ms,
      myvendor_lcd_mono_ms(), memory_order_release);
}

/** @brief 调用 LVGL flush_ready 链上的 done 回调（ISR 安全）。 */
static void myvendor_lcd_invoke_done_cb(void)
{
  myvendor_lcd_disp_done_cb_t cb;
  void *user_data;

  cb = (myvendor_lcd_disp_done_cb_t)
       atomic_load_explicit(&g_done_cb, memory_order_acquire);
  user_data = (void *)
       atomic_load_explicit(&g_done_ud, memory_order_acquire);

  if (cb != NULL)
    {
      cb(user_data);
    }
}

/** @brief 调用 recover 全屏 refr 回调（UI 线程）。 */
static void myvendor_lcd_invoke_refr_cb(void)
{
  myvendor_lcd_disp_refr_cb_t cb;
  void *user_data;

  cb = (myvendor_lcd_disp_refr_cb_t)
       atomic_load_explicit(&g_refr_cb, memory_order_acquire);
  user_data = (void *)
       atomic_load_explicit(&g_recover_ud, memory_order_acquire);

  if (cb != NULL)
    {
      cb(user_data);
    }
}

/** @brief 调用 recover unblock 回调（UI 线程）。 */
static void myvendor_lcd_invoke_unblock_cb(void)
{
  myvendor_lcd_disp_unblock_cb_t cb;
  void *user_data;

  cb = (myvendor_lcd_disp_unblock_cb_t)
       atomic_load_explicit(&g_unblock_cb, memory_order_acquire);
  user_data = (void *)
       atomic_load_explicit(&g_recover_ud, memory_order_acquire);

  if (cb != NULL)
    {
      cb(user_data);
    }
}

/**
 * @brief LCDC EOF 完成回调（中断上下文）。
 *
 * 不取锁、不 syslog：只更新计数并调 LVGL 注册的 done 回调
 * （lv_display_flush_ready 只清一个 volatile 标志，允许在 ISR 调用）。
 */
static void myvendor_lcd_disp_xfer_done(void *arg, int status)
{
  (void)arg;

  atomic_fetch_add_explicit(&g_lcd_disp.stat_xfer_done, 1,
      memory_order_relaxed);

  if (status != 0)
    {
      atomic_fetch_add_explicit(&g_lcd_disp.stat_xfer_err, 1,
          memory_order_relaxed);
    }

  /* This is the single flush-ready dispatch point of the IRQ-driven path. */
  atomic_fetch_add_explicit(&g_lcd_disp.stat_flush_ready, 1,
      memory_order_relaxed);

  myvendor_lcd_invoke_done_cb();
}

#if CONFIG_MYVENDOR_LCD_DISP_DEBUG
/** @brief 每 N 秒打印 submit/done/flush_ready/wait 窗口统计。 */
static void myvendor_lcd_debug_tick(void)
{
  static uint32_t last_ms;
  static uint32_t prev_submit;
  static uint32_t prev_done;
  static uint32_t prev_ready;
  static uint32_t prev_err;
  static uint32_t prev_wait;
  static uint32_t prev_timeout;
  uint32_t now = myvendor_lcd_mono_ms();
  uint32_t submit;
  uint32_t done;
  uint32_t ready;
  uint32_t err;
  uint32_t wait;
  uint32_t timeout;

  if (last_ms != 0 && now - last_ms < MYVENDOR_LCD_DEBUG_PERIOD_MS)
    {
      return;
    }

  last_ms = now;
  submit  = atomic_load_explicit(&g_lcd_disp.stat_submit_ok,
      memory_order_relaxed);
  done    = atomic_load_explicit(&g_lcd_disp.stat_xfer_done,
      memory_order_relaxed);
  ready   = atomic_load_explicit(&g_lcd_disp.stat_flush_ready,
      memory_order_relaxed);
  err     = atomic_load_explicit(&g_lcd_disp.stat_submit_err,
      memory_order_relaxed) +
            atomic_load_explicit(&g_lcd_disp.stat_xfer_err,
      memory_order_relaxed);
  wait    = atomic_load_explicit(&g_lcd_disp.stat_wait_calls,
      memory_order_relaxed);
  timeout = atomic_load_explicit(&g_lcd_disp.stat_wait_timeout,
      memory_order_relaxed);

  /* The board's default syslog mask is 0xf (only EMERG..ERR pass), so use
   * LOG_ERR for this opt-in diagnostic build; LOG_INFO would be dropped.
   */
  syslog(LOG_ERR,
      "[lcd_flush] 5s: submit=%lu done=%lu flush_ready=%lu err=%lu "
      "wait=%lu timeout=%lu wait_max=%lums recover=%lu busy=%d",
      (unsigned long)(submit - prev_submit),
      (unsigned long)(done - prev_done),
      (unsigned long)(ready - prev_ready),
      (unsigned long)(err - prev_err),
      (unsigned long)(wait - prev_wait),
      (unsigned long)(timeout - prev_timeout),
      (unsigned long)atomic_load_explicit(&g_lcd_disp.stat_wait_ms_max,
          memory_order_relaxed),
      (unsigned long)atomic_load_explicit(&g_lcd_disp.stat_recover,
          memory_order_relaxed),
      (int)sf32lb_lcd_async_busy());

  prev_submit  = submit;
  prev_done    = done;
  prev_ready   = ready;
  prev_err     = err;
  prev_wait    = wait;
  prev_timeout = timeout;
}
#else
#  define myvendor_lcd_debug_tick() do { } while (0)
#endif

/** @brief recover 后标记 UI 工作（不在此调 LVGL）。 */
static void myvendor_lcd_finish_recover_ui(void)
{
  uint32_t now = myvendor_lcd_mono_ms();

  atomic_store_explicit(&g_lcd_disp.recover_quiet_until_ms,
      now + MYVENDOR_LCD_RECOVER_COOLDOWN_MS, memory_order_release);
  atomic_store_explicit(&g_lcd_disp.busy_since_ms, now, memory_order_release);
  atomic_store_explicit(&g_lcd_disp.recover_unblock_pending, true,
      memory_order_release);
  atomic_store_explicit(&g_lcd_disp.recover_ui_pending, true,
      memory_order_release);
  atomic_store_explicit(&g_lcd_disp.recover_hold_submit, true,
      memory_order_release);
  atomic_store_explicit(&g_lcd_disp.recover_refr_armed, true,
      memory_order_release);
  atomic_store_explicit(&g_lcd_disp.recover_in_progress, false,
      memory_order_release);
}

/** @brief LVGL 线程 poll：执行 unblock 回调。 */
void myvendor_lcd_disp_poll_recover_unblock(void)
{
  if (!atomic_exchange_explicit(&g_lcd_disp.recover_unblock_pending, false,
          memory_order_acq_rel))
    {
      return;
    }

  myvendor_lcd_invoke_unblock_cb();
}

/** @brief LVGL 线程 poll：invalidate + refr。 */
void myvendor_lcd_disp_poll_recover_ui(void)
{
  /* unblock 可能已在 flush_wait 执行；此处捕获 idle recover。 */
  myvendor_lcd_disp_poll_recover_unblock();

  if (!atomic_exchange_explicit(&g_lcd_disp.recover_ui_pending, false,
          memory_order_acq_rel))
    {
      return;
    }

  /* 重绘前丢弃中断 refresh 的 stale partial strip。 */
  atomic_store_explicit(&g_lcd_disp.recover_hold_submit, false,
      memory_order_release);

  /* 仅在 LVGL refresh 外做全 invalidate + refr（loop 顶）。 */
  myvendor_lcd_invoke_refr_cb();
}

/** @brief recover 期间是否抑制 submit。 */
bool myvendor_lcd_disp_recover_hold_active(void)
{
  return atomic_load_explicit(&g_lcd_disp.recover_hold_submit,
      memory_order_acquire);
}

/**
 * @brief 打开 LCD 并检查几何（不再创建 worker）。
 * @return 0 成功，负 errno。
 */
int myvendor_lcd_disp_init(const char *lcd_path)
{
  struct fb_videoinfo_s vinfo;
  int ret;

  if (g_lcd_disp.inited)
    {
      return 0;
    }

  if (lcd_path == NULL)
    {
      lcd_path = "/dev/lcd0";
    }

  memset(&g_lcd_disp, 0, sizeof(g_lcd_disp));
  g_lcd_disp.lcd_fd = -1;

  strncpy(g_lcd_disp.lcd_path, lcd_path, sizeof(g_lcd_disp.lcd_path) - 1u);
  g_lcd_disp.lcd_path[sizeof(g_lcd_disp.lcd_path) - 1u] = '\0';

  atomic_store_explicit(&g_done_cb, 0, memory_order_relaxed);
  atomic_store_explicit(&g_done_ud, 0, memory_order_relaxed);
  atomic_store_explicit(&g_refr_cb, 0, memory_order_relaxed);
  atomic_store_explicit(&g_unblock_cb, 0, memory_order_relaxed);
  atomic_store_explicit(&g_recover_ud, 0, memory_order_relaxed);

  g_lcd_disp.lcd_fd = open(lcd_path, O_RDWR);
  if (g_lcd_disp.lcd_fd < 0)
    {
      syslog(LOG_ERR, "myvendor_lcd_disp: open %s failed errno=%d",
             lcd_path, errno);
      return -errno;
    }

  ret = ioctl(g_lcd_disp.lcd_fd, LCDDEVIO_GETVIDEOINFO,
              (unsigned long)(uintptr_t)&vinfo);
  if (ret < 0)
    {
      syslog(LOG_ERR, "myvendor_lcd_disp: GETVIDEOINFO failed %d", ret);
      goto errout;
    }

  g_lcd_disp.hor_res = (uint16_t)vinfo.xres;
  g_lcd_disp.ver_res = (uint16_t)vinfo.yres;

  ret = ioctl(g_lcd_disp.lcd_fd, LCDDEVIO_GETAREAALIGN,
              (unsigned long)(uintptr_t)&g_lcd_disp.area_align);
  if (ret < 0)
    {
      ret = -errno;
      syslog(LOG_ERR, "myvendor_lcd_disp: GETAREAALIGN failed %d", ret);
      goto errout;
    }

  /* The rounder expands damage before rendering. Screen edges must also
   * satisfy the driver's alignment so that expansion stays inside GRAM.
   */
  if (g_lcd_disp.area_align.col_start_align == 0 ||
      g_lcd_disp.area_align.row_start_align == 0 ||
      g_lcd_disp.area_align.width_align == 0 ||
      g_lcd_disp.area_align.height_align == 0 ||
      g_lcd_disp.area_align.buf_align == 0 ||
      g_lcd_disp.hor_res % g_lcd_disp.area_align.col_start_align != 0 ||
      g_lcd_disp.ver_res % g_lcd_disp.area_align.row_start_align != 0 ||
      g_lcd_disp.hor_res % g_lcd_disp.area_align.width_align != 0 ||
      g_lcd_disp.ver_res % g_lcd_disp.area_align.height_align != 0)
    {
      syslog(LOG_ERR, "myvendor_lcd_disp: unsupported area alignment");
      ret = -EINVAL;
      goto errout;
    }

  if (g_lcd_disp.hor_res == 0 || g_lcd_disp.ver_res == 0)
    {
      syslog(LOG_ERR, "myvendor_lcd_disp: bad resolution %ux%u",
             g_lcd_disp.hor_res, g_lcd_disp.ver_res);
      ret = -EINVAL;
      goto errout;
    }

  g_lcd_disp.strip_rows = (uint16_t)CONFIG_MYVENDOR_LCD_DISP_STRIP_ROWS;

  if (g_lcd_disp.strip_rows < 8)
    {
      g_lcd_disp.strip_rows = 8;
    }

  if (g_lcd_disp.strip_rows > g_lcd_disp.ver_res)
    {
      g_lcd_disp.strip_rows = g_lcd_disp.ver_res;
    }

  /* 单笔 QSPI DMA 必须 < 64 KiB：240x128x2 = 61440 B。Kconfig 允许 160 行
   * 的缓冲高度，但这里把“每次 flush 的条带”限制到 128 行，避免在中断里
   * 再拆第二笔传输（无 worker 后不再有线程可做链式提交）。缓冲本身仍是
   * CONFIG_MYVENDOR_LCD_DISP_STRIP_ROWS 行，只是 LVGL 每次用其中 128 行。
   */
  if (g_lcd_disp.strip_rows > SF32LB_LCD_ASYNC_MAX_ROWS)
    {
      syslog(LOG_INFO,
             "myvendor_lcd_disp: strip %u -> %u rows (single LCDC xfer)",
             (unsigned)g_lcd_disp.strip_rows,
             (unsigned)SF32LB_LCD_ASYNC_MAX_ROWS);
      g_lcd_disp.strip_rows = SF32LB_LCD_ASYNC_MAX_ROWS;
    }

  g_lcd_disp.stride = (uint32_t)g_lcd_disp.hor_res * 2u;
  g_lcd_disp.buf_bytes = (size_t)g_lcd_disp.stride * g_lcd_disp.strip_rows;

  if (g_lcd_disp.buf_bytes == 0)
    {
      syslog(LOG_ERR, "myvendor_lcd_disp: strip size is zero");
      ret = -EINVAL;
      goto errout;
    }

  if (pthread_mutex_init(&g_lcd_disp.lock, NULL) != 0)
    {
      syslog(LOG_ERR, "myvendor_lcd_disp: pthread mutex init failed");
      ret = -EIO;
      goto errout;
    }

  g_lcd_disp.sync_inited = true;
  g_lcd_disp.inited = true;

  syslog(LOG_INFO,
         "myvendor_lcd_disp: %ux%u strip=%u stride=%lu direct-sram path=%s "
         "(IRQ-driven, no worker)",
         g_lcd_disp.hor_res, g_lcd_disp.ver_res, g_lcd_disp.strip_rows,
         (unsigned long)g_lcd_disp.stride, lcd_path);

  return 0;

errout:
  myvendor_lcd_disp_deinit();
  return ret;
}

/** @brief 等待在途 DMA 后关闭 LCD。 */
void myvendor_lcd_disp_deinit(void)
{
  if (g_lcd_disp.inited)
    {
      /* Never free the strip buffers while LCDC is still reading them. */
      (void)sf32lb_lcd_async_wait(MYVENDOR_LCD_DISP_WAIT_MS);
    }

  if (g_lcd_disp.lcd_fd >= 0)
    {
      close(g_lcd_disp.lcd_fd);
      g_lcd_disp.lcd_fd = -1;
    }

  if (g_lcd_disp.sync_inited)
    {
      pthread_mutex_destroy(&g_lcd_disp.lock);
    }

  memset(&g_lcd_disp, 0, sizeof(g_lcd_disp));
  g_lcd_disp.lcd_fd = -1;
}

/**
 * @brief 返回分辨率与条带缓冲信息。
 * @return 0 成功。
 */
int myvendor_lcd_disp_get_info(myvendor_lcd_disp_info_t *info)
{
  if (info == NULL || !g_lcd_disp.inited || g_lcd_disp.lcd_fd < 0)
    {
      return -EINVAL;
    }

  info->hor_res    = g_lcd_disp.hor_res;
  info->ver_res    = g_lcd_disp.ver_res;
  info->strip_rows = g_lcd_disp.strip_rows;
  info->stride     = g_lcd_disp.stride;
  info->buf_bytes  = g_lcd_disp.buf_bytes;
  info->area_align = g_lcd_disp.area_align;
  return 0;
}

/** @brief 注册 DMA 完成回调。 */
int myvendor_lcd_disp_register_done_cb(myvendor_lcd_disp_done_cb_t cb,
                                       void *user_data)
{
  if (!g_lcd_disp.inited || g_lcd_disp.lcd_fd < 0)
    {
      return -ENODEV;
    }

  atomic_store_explicit(&g_done_ud, (uintptr_t)user_data,
      memory_order_release);
  atomic_store_explicit(&g_done_cb, (uintptr_t)cb, memory_order_release);
  return 0;
}

/** @brief 注销 done 回调。 */
void myvendor_lcd_disp_unregister_done_cb(void)
{
  atomic_store_explicit(&g_done_cb, 0, memory_order_release);
  atomic_store_explicit(&g_done_ud, 0, memory_order_release);
}

/** @brief 注册 recover refr/unblock 回调。 */
int myvendor_lcd_disp_register_recover_cbs(myvendor_lcd_disp_refr_cb_t refr_cb,
                                           myvendor_lcd_disp_unblock_cb_t
                                               unblock_cb,
                                           void *user_data)
{
  if (!g_lcd_disp.inited)
    {
      return -ENODEV;
    }

  atomic_store_explicit(&g_recover_ud, (uintptr_t)user_data,
      memory_order_release);
  atomic_store_explicit(&g_refr_cb, (uintptr_t)refr_cb, memory_order_release);
  atomic_store_explicit(&g_unblock_cb, (uintptr_t)unblock_cb,
      memory_order_release);
  return 0;
}

/** @brief 注销 recover 回调。 */
void myvendor_lcd_disp_unregister_recover_cbs(void)
{
  atomic_store_explicit(&g_refr_cb, 0, memory_order_release);
  atomic_store_explicit(&g_unblock_cb, 0, memory_order_release);
  atomic_store_explicit(&g_recover_ud, 0, memory_order_release);
}

/**
 * @brief 等待 flush 空闲（在途 DMA 完成）。
 * @param max_ms 0 表示无限等待。
 */
bool myvendor_lcd_disp_wait_idle(uint32_t max_ms, uint32_t * waited_ms_out)
{
  uint32_t t0;
  int ret;

  if (!g_lcd_disp.inited)
    {
      if (waited_ms_out != NULL)
        {
          *waited_ms_out = 0;
        }

      return true;
    }

  myvendor_lcd_debug_tick();

  t0 = myvendor_lcd_mono_ms();
  ret = sf32lb_lcd_async_wait(max_ms);

  if (waited_ms_out != NULL)
    {
      *waited_ms_out = myvendor_lcd_mono_ms() - t0;
    }

  return ret == 0;
}

/**
 * @brief 提交一条 LVGL 条带：启动 LCDC DMA 后立即返回。
 * @return 0 已启动；负值失败（调用方需自行 flush_ready）。
 */
int myvendor_lcd_disp_submit_flush(const myvendor_lcd_disp_area_t *area,
                                   const void *color_p,
                                   uint32_t stride_bytes,
                                   bool is_last)
{
  uint32_t rows;
  int ret;

  (void)is_last;

  if (!g_lcd_disp.inited || g_lcd_disp.lcd_fd < 0 ||
      area == NULL || color_p == NULL)
    {
      atomic_fetch_add_explicit(&g_lcd_disp.stat_submit_err, 1,
          memory_order_relaxed);
      return -EINVAL;
    }

  if (atomic_load_explicit(&g_lcd_disp.recover_hold_submit,
          memory_order_acquire))
    {
      return 0;
    }

  if (area->x1 < 0 || area->y1 < 0 ||
      area->x2 < area->x1 || area->y2 < area->y1 ||
      area->x2 >= (int16_t)g_lcd_disp.hor_res ||
      area->y2 >= (int16_t)g_lcd_disp.ver_res)
    {
      atomic_fetch_add_explicit(&g_lcd_disp.stat_submit_err, 1,
          memory_order_relaxed);
      return -EINVAL;
    }

  /* LVGL partial mode reshapes the draw buffer to the invalidated area, so
   * the stride is the *area* width * 2 (not necessarily the panel width).
   * The buffer is tightly packed at color_p (row 0 == area->y1).
   */
  if (stride_bytes != (uint32_t)(area->x2 - area->x1 + 1) * 2u)
    {
      atomic_fetch_add_explicit(&g_lcd_disp.stat_submit_err, 1,
          memory_order_relaxed);
      return -EINVAL;
    }

  /* A narrow invalidated area may be taller than the strip height: the
   * hardware limit is the transfer byte count, not the row count.
   */
  rows = (uint32_t)(area->y2 - area->y1 + 1);
  if ((size_t)rows * stride_bytes > SF32LB_LCD_ASYNC_MAX_BYTES)
    {
      atomic_fetch_add_explicit(&g_lcd_disp.stat_submit_err, 1,
          memory_order_relaxed);
      return -E2BIG;
    }

  myvendor_lcd_debug_tick();

  /* The LCDC has a single engine: if the previous strip is still being
   * shifted out, wait for its EOF IRQ.  LVGL normally already did this in
   * wait_for_flushing(); this guard covers the single-buffer/last-area
   * cases and a completion IRQ that has not been taken yet.
   */
  if (sf32lb_lcd_async_busy())
    {
      uint32_t t0 = myvendor_lcd_mono_ms();
      int w = sf32lb_lcd_async_wait(MYVENDOR_LCD_DISP_WAIT_MS);
      uint32_t waited = myvendor_lcd_mono_ms() - t0;

      myvendor_lcd_disp_stats_note_wait(waited, w < 0, false);

      if (w < 0)
        {
          atomic_fetch_add_explicit(&g_lcd_disp.stat_submit_err, 1,
              memory_order_relaxed);
          syslog(LOG_ERR, "[lcd_flush] wait previous strip failed %d", w);
          return -EIO;
        }
    }

  ret = sf32lb_lcd_putarea_async(area->y1, area->y2, area->x1, area->x2,
                                 (const uint8_t *)color_p,
                                 (fb_coord_t)stride_bytes,
                                 myvendor_lcd_disp_xfer_done, NULL);
  if (ret == -EBUSY)
    {
      atomic_fetch_add_explicit(&g_lcd_disp.stat_submit_busy, 1,
          memory_order_relaxed);
      myvendor_lcd_disp_stats_note_submit_retry();

      if (sf32lb_lcd_async_wait(MYVENDOR_LCD_DISP_WAIT_MS) == 0)
        {
          ret = sf32lb_lcd_putarea_async(area->y1, area->y2,
                                         area->x1, area->x2,
                                         (const uint8_t *)color_p,
                                         (fb_coord_t)stride_bytes,
                                         myvendor_lcd_disp_xfer_done, NULL);
        }
    }

  if (ret < 0)
    {
      atomic_fetch_add_explicit(&g_lcd_disp.stat_submit_err, 1,
          memory_order_relaxed);
      syslog(LOG_ERR, "[lcd_flush] putarea_async failed %d", ret);
      return ret;
    }

  atomic_fetch_add_explicit(&g_lcd_disp.stat_submit_ok, 1,
      memory_order_relaxed);
  myvendor_lcd_note_busy();
  return 0;
}

/** @brief flush 是否仍在进行。 */
bool myvendor_lcd_disp_flush_busy(void)
{
  if (!g_lcd_disp.inited)
    {
      return false;
    }

  return sf32lb_lcd_async_busy() ||
      atomic_load_explicit(&g_lcd_disp.recover_in_progress,
          memory_order_acquire);
}

/** @brief 复制诊断计数快照。 */
void myvendor_lcd_disp_stats_snapshot(myvendor_lcd_disp_stats_t * stats)
{
  if (!stats)
    {
      return;
    }

  stats->submit_ok = atomic_load_explicit(&g_lcd_disp.stat_submit_ok,
      memory_order_relaxed);
  stats->submit_busy = atomic_load_explicit(&g_lcd_disp.stat_submit_busy,
      memory_order_relaxed);
  stats->submit_err = atomic_load_explicit(&g_lcd_disp.stat_submit_err,
      memory_order_relaxed);
  stats->worker_done = atomic_load_explicit(&g_lcd_disp.stat_xfer_done,
      memory_order_relaxed);
  stats->putarea_fail = atomic_load_explicit(&g_lcd_disp.stat_putarea_fail,
      memory_order_relaxed);
  stats->flush_ready = atomic_load_explicit(&g_lcd_disp.stat_flush_ready,
      memory_order_relaxed);
  stats->wait_calls = atomic_load_explicit(&g_lcd_disp.stat_wait_calls,
      memory_order_relaxed);
  stats->wait_ms_max = atomic_load_explicit(&g_lcd_disp.stat_wait_ms_max,
      memory_order_relaxed);
  stats->wait_timeout = atomic_load_explicit(&g_lcd_disp.stat_wait_timeout,
      memory_order_relaxed);
  stats->flush_ready_force = atomic_load_explicit(
      &g_lcd_disp.stat_flush_ready_force, memory_order_relaxed);
  stats->submit_retry = atomic_load_explicit(&g_lcd_disp.stat_submit_retry,
      memory_order_relaxed);
  stats->wait_stall = atomic_load_explicit(&g_lcd_disp.stat_wait_stall,
      memory_order_relaxed);
  stats->putarea_ms_max = atomic_load_explicit(&g_lcd_disp.stat_putarea_ms_max,
      memory_order_relaxed);
  stats->putarea_slow = atomic_load_explicit(&g_lcd_disp.stat_putarea_slow,
      memory_order_relaxed);
  stats->recover = atomic_load_explicit(&g_lcd_disp.stat_recover,
      memory_order_relaxed);

  myvendor_lcd_debug_tick();
}

/** @brief 清零诊断计数窗口。 */
void myvendor_lcd_disp_stats_reset_window(void)
{
  atomic_store_explicit(&g_lcd_disp.stat_submit_ok, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_submit_busy, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_submit_err, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_xfer_done, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_putarea_fail, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_flush_ready, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_wait_calls, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_wait_ms_max, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_wait_timeout, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_flush_ready_force, 0,
      memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_submit_retry, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_wait_stall, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_putarea_ms_max, 0,
      memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_putarea_slow, 0,
      memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_recover, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_xfer_err, 0, memory_order_relaxed);
}

/**
 * @brief LVGL 侧记一次 flush_ready（兼容旧调用）。
 *
 * flush_ready 现在由 LCDC 完成中断统一计数，避免 bench/bicycle 两套
 * done 回调有的调、有的不调造成统计偏差；这里保留为空实现。
 */
void myvendor_lcd_disp_stats_note_flush_ready(void)
{
}

/** @brief 记录 wait_idle 统计。 */
void myvendor_lcd_disp_stats_note_wait(uint32_t waited_ms, bool timeout,
    bool forced)
{
  uint32_t prev;

  atomic_fetch_add_explicit(&g_lcd_disp.stat_wait_calls, 1,
      memory_order_relaxed);
  if (timeout)
    {
      atomic_fetch_add_explicit(&g_lcd_disp.stat_wait_timeout, 1,
          memory_order_relaxed);
    }
  if (forced)
    {
      atomic_fetch_add_explicit(&g_lcd_disp.stat_flush_ready_force, 1,
          memory_order_relaxed);
    }
  if (waited_ms >= 100u)
    {
      atomic_fetch_add_explicit(&g_lcd_disp.stat_wait_stall, 1,
          memory_order_relaxed);
    }

  prev = atomic_load_explicit(&g_lcd_disp.stat_wait_ms_max,
      memory_order_relaxed);
  if (waited_ms > prev)
    {
      atomic_store_explicit(&g_lcd_disp.stat_wait_ms_max, waited_ms,
          memory_order_relaxed);
    }
}

/** @brief 记录 submit 重试。 */
void myvendor_lcd_disp_stats_note_submit_retry(void)
{
  atomic_fetch_add_explicit(&g_lcd_disp.stat_submit_retry, 1,
      memory_order_relaxed);
}

/** @brief 记录 submit 错误。 */
void myvendor_lcd_disp_stats_note_submit_err(void)
{
  atomic_fetch_add_explicit(&g_lcd_disp.stat_submit_err, 1,
      memory_order_relaxed);
}

/** @brief 抑制自动 recover 直到指定毫秒。 */
void myvendor_lcd_disp_suppress_recover_ms(uint32_t ms)
{
  uint32_t now;
  uint32_t until;

  if (!g_lcd_disp.sync_inited || ms == 0)
    {
      return;
    }

  now = myvendor_lcd_mono_ms();
  until = now + ms;

  atomic_store_explicit(&g_lcd_disp.recover_quiet_until_ms, until,
      memory_order_release);
}

/**
 * @brief 轻量 recover（原 worker 重启逻辑已删除）。
 *
 * 空闲：没有线程/管道需要重建，仅排队一次全屏重绘（bicycle 的
 * poll_recover_ui 会消费），返回 0。
 *
 * 忙：等待在途 DMA 完成；超时说明 LCDC 状态机卡住。此时从 UI 侧强行
 * 重开 /dev/lcd0 或 HAL_LCDC_DeInit 会撕裂已初始化的面板且没有 worker
 * 可以重新接管，因此安全 no-op 并返回 -EBUSY，由上层决定是否整机复位。
 */
static int myvendor_lcd_disp_recover(void)
{
  if (!g_lcd_disp.inited || !g_lcd_disp.sync_inited)
    {
      return -ENODEV;
    }

  if (atomic_exchange_explicit(&g_lcd_disp.recover_in_progress, true,
          memory_order_acq_rel))
    {
      return -EBUSY;
    }

  if (sf32lb_lcd_async_busy())
    {
      int ret;

      syslog(LOG_WARNING,
             "[lcd_flush] RECOVER: waiting in-flight LCDC DMA (no worker)");

      ret = sf32lb_lcd_async_wait(MYVENDOR_LCD_DISP_WAIT_MS);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "[lcd_flush] RECOVER: LCDC still busy (%d), safe no-op",
                 ret);
          atomic_store_explicit(&g_lcd_disp.recover_in_progress, false,
              memory_order_release);
          return -EBUSY;
        }
    }

  atomic_fetch_add_explicit(&g_lcd_disp.stat_recover, 1,
      memory_order_relaxed);
  myvendor_lcd_finish_recover_ui();

  syslog(LOG_WARNING,
         "[lcd_flush] RECOVER: idle, queued full redraw (IRQ path)");
  return 0;
}

/** @brief 手动触发 recover（NSH/诊断）。 */
int myvendor_lcd_disp_trigger_recover(void)
{
  syslog(LOG_WARNING, "[lcd_flush] manual recover requested");
  return myvendor_lcd_disp_recover();
}
