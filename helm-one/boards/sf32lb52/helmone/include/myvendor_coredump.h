/**
 * @file myvendor_coredump.h
 * @brief 把崩溃时的 ARM 寄存器组写到 `/mnt/kv/coredump/nNNN_YYYYMMDD_HHMMSS.txt`。
 * 文件名和 stamp 用本地时（persist.ui.tz_min，默认 UTC+8）；unix= 仍是 UTC。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_COREDUMP_H
#define MYVENDOR_COREDUMP_H

#include <nuttx/config.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYVENDOR_COREDUMP_DIR "/mnt/kv/coredump"

#if defined(CONFIG_MYVENDOR_COREDUMP) && CONFIG_MYVENDOR_COREDUMP

/** @brief 注册 panic notifier，在 dump_assert_info 之前写核心转储。 */
void myvendor_coredump_early(void);

/** @brief mkdir `/mnt/kv/coredump`（KV 挂上之后调用）。 */
void myvendor_coredump_init(void);

/**
 * @brief 抄现场到 RAM，退中断后裸机重初始化 SDIO/LFS 再写盘。
 * @note 成功则不返回（等 IWDT 复位）。
 */
void myvendor_coredump_wdt(void);

/**
 * @brief 把 LVGL 片内条带交给崩溃路径（LCD 分配成功后调用）。
 *
 * 指针记在 SRAM；HardFault 里先停 LCDC/EPIC DMA，再把条带当 note /
 * 崩溃栈 / SD 512 暂存。须为 HCPU SRAM，不能是 PSRAM。
 */
void myvendor_coredump_bind_fb(void *a, size_t a_bytes, void *b,
                               size_t b_bytes);

/** @brief LCD 释放条带时取消绑定。 */
void myvendor_coredump_unbind_fb(void);

#else

static inline void myvendor_coredump_early(void)
{
}

static inline void myvendor_coredump_init(void)
{
}

static inline void myvendor_coredump_wdt(void)
{
}

static inline void myvendor_coredump_bind_fb(void *a, size_t a_bytes,
                                             void *b, size_t b_bytes)
{
  (void)a;
  (void)a_bytes;
  (void)b;
  (void)b_bytes;
}

static inline void myvendor_coredump_unbind_fb(void)
{
}

#endif

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_COREDUMP_H */
