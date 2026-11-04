/****************************************************************************
 * vendor/HelmOne/boards/sf32lb52/drivers/lcd/sf32lb_lcd_async.h
 *
 * Interrupt-driven LCDC/QSPI strip submission for the LVGL flush path.
 *
 * Unlike LCDDEVIO_PUTAREA (which waits for the QSPI transfer inside the
 * ioctl), sf32lb_lcd_putarea_async() programs the panel window and starts
 * the LCDC transfer, then returns immediately.  The LCDC EOF interrupt
 * invokes the done callback from ISR context, so the caller can call
 * lv_display_flush_ready() without any intermediate worker thread.
 *
 * Only packed RGB565 strips of at most SF32LB_LCD_ASYNC_MAX_BYTES bytes are
 * accepted in one call: a single QSPI transfer stays below 64 KiB, which is
 * the limit used by the existing synchronous PUTAREA chunking.  LVGL partial
 * mode reshapes its draw buffer to the invalidated area, so a narrow area may
 * be taller than SF32LB_LCD_ASYNC_MAX_ROWS as long as its byte count fits.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __SF32LB_LCD_ASYNC_H
#define __SF32LB_LCD_ASYNC_H

#include <nuttx/video/fb.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum bytes accepted by one async transfer (must stay below 64 KiB). */
#define SF32LB_LCD_ASYNC_MAX_BYTES 61440u

/** Maximum rows for a full-width 240-pixel RGB565 strip. */
#define SF32LB_LCD_ASYNC_MAX_ROWS 128

/**
 * @brief DMA completion callback, invoked from the LCDC interrupt handler.
 *
 * @param arg    Caller cookie.
 * @param status 0 on success, negative errno on transfer error.
 *
 * The callback runs in interrupt context: it must be short and must not
 * block.  lv_display_flush_ready() is safe to call from here (LVGL keeps
 * the flushing flag as a volatile int for exactly this use).
 */
typedef void (*sf32lb_lcd_async_done_t)(void *arg, int status);

/**
 * @brief Start one packed full-width RGB565 strip asynchronously.
 *
 * @param row_start/row_end  Pixel rows (inclusive).
 * @param col_start/col_end  Pixel columns (inclusive), must cover the panel.
 * @param buffer             Pixel buffer (row_start is the first row).
 * @param stride             Row stride in bytes; must equal width * 2.
 * @param done               Completion callback (ISR context), may be NULL.
 * @param arg                Callback cookie.
 * @return 0 started, -EBUSY if a transfer is already in flight,
 *         -EINVAL/-E2BIG/-ENODEV/-ENOTSUP on bad arguments or HW not ready.
 */
int sf32lb_lcd_putarea_async(fb_coord_t row_start, fb_coord_t row_end,
                             fb_coord_t col_start, fb_coord_t col_end,
                             const uint8_t *buffer, fb_coord_t stride,
                             sf32lb_lcd_async_done_t done, void *arg);

/** @brief True while an async transfer (or a synchronous PUTAREA) is busy. */
bool sf32lb_lcd_async_busy(void);

/**
 * @brief Wait until no LCDC transfer is in flight.
 *
 * @param timeout_ms 0 waits forever.
 * @return 0 idle, -ETIMEDOUT on timeout, negative errno on wait failure.
 */
int sf32lb_lcd_async_wait(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* __SF32LB_LCD_ASYNC_H */
