/**
 * @file board_psram_layout.h
 * @brief PSRAM_DATA 分区布局（低地址 → 高地址）的单一来源。
 *
 * board_malloc、mtp_psram、boardmem 经此处宏/内联计算各区基址与大小。
 *
 * 扣减顺序与字节数必须与 chips/sf32lb52/sifli_allocateheap.c 中
 * arm_addregion() 完全一致，否则 kumm 与 BoardPSRAM / MTP 会重叠。
 * 物理分区须与 ptab（PSRAM_DATA @ 0x60400000, 12 MiB）一致。
 *
 * 默认布局（12 MiB，Kconfig 可改 pool / MTP 预留，kumm 为余量）：
 *
 *   0x60400000  Umem PSRAM kumm（malloc/free，NSH free）— 产品 1 MiB
 *   board_psram_pool_base()  BoardPSRAM 独立堆（mm_initialize）— 余量
 *   board_mtp_psram_arena_base()  MTP bump 区（尾端固定，无 free）
 *   0x61000000
 *
 * 产品 nsh：pool 11008 KiB + MTP 256 KiB → kumm 1024 KiB。
 *
 * Kconfig：MYVENDOR_BOARD_PSRAM_POOL_KB、MYVENDOR_MTP_PSRAM_RESERVE_KB。
 * 宏：*_KB 供 #if；*_BYTES 供运行时 size_t。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_BOARD_PSRAM_LAYOUT_H
#define MY_VENDOR_BOARD_PSRAM_LAYOUT_H

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

/* PSRAM_DATA 分区：HyperBus 堆窗口，与 sifli_allocateheap.c / ptab 一致 */

#define BOARD_PSRAM_DATA_BASE  0x60400000u
#define BOARD_PSRAM_DATA_SIZE  0x00C00000u  /* 12 MiB (16 MiB chip − 4 MiB code XIP) */

/*
 * MTP 尾端 bump 预留（KiB）。
 * 由 mtp_psram.c 顺序分配大缓存；不参与 NuttX 堆，一般无对应 free。
 * 未启用 MYVENDOR_MTP_SIMPLE 时为 0。
 */

#if defined(CONFIG_MYVENDOR_MTP_PSRAM_RESERVE_KB)
#  define BOARD_MTP_PSRAM_RESERVE_KB CONFIG_MYVENDOR_MTP_PSRAM_RESERVE_KB
#elif defined(CONFIG_MYVENDOR_MTP_SIMPLE)
#  define BOARD_MTP_PSRAM_RESERVE_KB 256
#else
#  define BOARD_MTP_PSRAM_RESERVE_KB 0
#endif

#define BOARD_MTP_PSRAM_RESERVE_BYTES \
  ((size_t)BOARD_MTP_PSRAM_RESERVE_KB * 1024u)

/*
 * BoardPSRAM 独立堆大小（KiB）。
 * board_malloc.c 在此区域 mm_initialize()；从 kumm 之前切出，与 NSH free 无关。
 * MYVENDOR_BOARD_PSRAM_POOL_KB=0 时整段 PSRAM 仅 kumm（及可选 MTP 尾）。
 */

#if defined(CONFIG_MYVENDOR_BOARD_PSRAM_POOL_KB)
#  define BOARD_PSRAM_POOL_KB CONFIG_MYVENDOR_BOARD_PSRAM_POOL_KB
#elif defined(CONFIG_BSP_USING_PSRAM)
#  define BOARD_PSRAM_POOL_KB 11008
#else
#  define BOARD_PSRAM_POOL_KB 0
#endif

#define BOARD_PSRAM_POOL_BYTES ((size_t)BOARD_PSRAM_POOL_KB * 1024u)

/**
 * @brief 交给 kumm 的 PSRAM 字节数（region 1 长度）。
 *
 * 算法：PSRAM_DATA 总长 − BoardPSRAM 池 − MTP 尾预留。
 * 须与 sifli_allocateheap.c::arm_addregion() 中 psram_heap 扣减相同。
 */
static inline size_t board_psram_kumm_bytes(void)
{
  size_t avail = BOARD_PSRAM_DATA_SIZE;

  if (BOARD_PSRAM_POOL_BYTES > 0 && avail > BOARD_PSRAM_POOL_BYTES)
    {
      avail -= BOARD_PSRAM_POOL_BYTES;
    }

  if (BOARD_MTP_PSRAM_RESERVE_BYTES > 0 &&
      avail > BOARD_MTP_PSRAM_RESERVE_BYTES)
    {
      avail -= BOARD_MTP_PSRAM_RESERVE_BYTES;
    }

  return avail;
}

/**
 * @brief BoardPSRAM 独立堆起始物理地址（紧接 kumm 之后）。
 */
static inline uintptr_t board_psram_pool_base(void)
{
  return BOARD_PSRAM_DATA_BASE + board_psram_kumm_bytes();
}

/**
 * @brief MTP bump 区起始（PSRAM_DATA 末尾向上预留）。
 */
static inline uintptr_t board_mtp_psram_arena_base(void)
{
  return BOARD_PSRAM_DATA_BASE + BOARD_PSRAM_DATA_SIZE -
         BOARD_MTP_PSRAM_RESERVE_BYTES;
}

#endif /* MY_VENDOR_BOARD_PSRAM_LAYOUT_H */
