/****************************************************************************
 * vendor/sifli/chip/sf32lb52/sifli_uart.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>

#include <stdbool.h>

#if defined(CONFIG_MYVENDOR_MTP_SIMPLE) && defined(CONFIG_BSP_USING_PSRAM)
#  ifndef CONFIG_MYVENDOR_MTP_PSRAM_RESERVE_KB
#    define CONFIG_MYVENDOR_MTP_PSRAM_RESERVE_KB 256
#  endif
#  define MTP_PSRAM_MTP_RESERVE_BYTES \
    ((size_t)CONFIG_MYVENDOR_MTP_PSRAM_RESERVE_KB * 1024u)
#endif

#if defined(CONFIG_BSP_USING_PSRAM)
#  if defined(CONFIG_MYVENDOR_BOARD_PSRAM_POOL_KB)
#    define BOARD_PSRAM_POOL_KB CONFIG_MYVENDOR_BOARD_PSRAM_POOL_KB
#  else
#    define BOARD_PSRAM_POOL_KB 1024
#  endif
#  define BOARD_PSRAM_POOL_BYTES ((size_t)BOARD_PSRAM_POOL_KB * 1024u)
#endif

#include "chip.h"
#include "arm_internal.h"
#include "bf0_hal.h"
#include "mem_map.h"

extern void BSP_PIN_Init(void);
extern void BSP_Power_Up(bool is_deep_sleep);
extern void BSP_Board_PreInit(void);
extern void BSP_Board_EnableSdkFlashClocks(void);
extern void BSP_Board_EnableHclk240(void);

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SRAM memory configuration for SF32LB52 */

#define SRAM_START  0x20000000
#define SRAM_SIZE   0x00080000    /* 512 KB */
#define SRAM_END    (SRAM_START + SRAM_SIZE)

/* HCPU2LCPU mailbox sits at the top of HPSYS SRAM (CH2 then CH1, 2 x 512 B).
 *
 * BSP pitfall (not Vela mm_heap, not OOM): mem_map.h already reserves
 * HPSYS_MBOX_BUF at SRAM top (0x2007FC00, 1 KiB). An earlier port gave
 * Umem [g_idle_topstack, SRAM_END). BLE HCI then wrote HCPU2LCPU_MB_CH1
 * (0x2007FE00) into a live free-chunk header. mallinfo/nsh `free` asserted;
 * malloc itself does not canary-check region overlap.
 *
 * Umem SRAM must end at HPSYS_MBOX_BUF_ADDR. Do not extend kumm to SRAM_END.
 */
#define SRAM_HEAP_END  HPSYS_MBOX_BUF_ADDR

#if SRAM_HEAP_END != (SRAM_END - HPSYS_MBOX_BUF_SIZE)
#  error "HPSYS mailbox is not at the top of SRAM; SRAM_HEAP_END mismatch"
#endif

/* PSRAM: 16MB HyperBus (MPI1 MODE_6). Code uses CBUS 0x10000000 (first 4MB);
 * heap uses PSRAM_DATA @ 0x60400000 (remaining 12MB per SDK ptab.json).
 */

#define PSRAM_HEAP_START  0x60400000
#define PSRAM_HEAP_SIZE   0x00C00000    /* 12 MB */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifdef CONFIG_BSP_USING_PSRAM
static bool g_psram_ready;

static void sifli_psram_preinit(void)
{
  qspi_configure_t qspi_cfg =
  {
    .Instance = hwp_qspi1,
    .SpiMode  = CONFIG_BSP_QSPI1_MODE,
    .msize    = CONFIG_BSP_QSPI1_MEM_SIZE,
    .base     = QSPI1_MEM_BASE,
  };
  static FLASH_HandleTypeDef psram_handle;

  /* Enable 1.8V LDO required by PSRAM. */

  HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO_1V8, true, true);

  /* Use SYSCLK for early boot safety. DLL2 path is enabled later by HAL. */

  HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_FLASH1, RCC_CLK_FLASH_SYSCLK);

  /* Use configured PSRAM mode directly to avoid early-boot PID dependency. */

  if (qspi_cfg.SpiMode == SPI_MODE_NOR)
    {
      g_psram_ready = false;
      return;
    }

  /* Avoid early power-mode query here; HAL_Init will handle PM state later. */

  psram_handle.wakeup = 0;

  /* Keep divider aligned with existing board implementation. */

  g_psram_ready = (HAL_MPI_PSRAM_Init(&psram_handle, &qspi_cfg, 2) == HAL_OK);
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void HAL_MspInit(void)
{
  BSP_PIN_Init();
  BSP_Power_Up(true);
}

void HAL_PreInit(void)
{
  BSP_Board_PreInit();

#ifdef CONFIG_BSP_USING_PSRAM
#if !defined(CONFIG_BSP_USING_SPI_NAND)
  BSP_Board_EnableSdkFlashClocks();
  BSP_Board_EnableHclk240();
#endif

  HAL_MspInit();

#if defined(CONFIG_BSP_USING_SPI_NAND)
  /* PSRAM XIP: do not touch MPI1/DLL2 clocks; bootloader left PSRAM @ ~144MHz. */
  g_psram_ready = true;
#else
  sifli_psram_preinit();
#endif
#endif
}

/****************************************************************************
 * Name: up_allocate_heap/up_allocate_kheap
 *
 * Description:
 *   This function will be called to dynamically set aside the heap region.
 *
 *   - For the normal "flat" build, this function returns the size of the
 *     single heap.
 *   - For the protected build (CONFIG_BUILD_PROTECTED=y) with both kernel-
 *     and user-space heaps (CONFIG_MM_KERNEL_HEAP=y), this function
 *     provides the size of the unprotected, user-space heap.
 *   - For the kernel build (CONFIG_BUILD_KERNEL=y), this function provides
 *     the size of the protected, kernel-space heap.
 *
 *   If a protected kernel-space heap is provided, the kernel heap must be
 *   allocated by an analogous up_allocate_kheap(). A custom version of this
 *   file is needed if memory protection of the kernel heap is required.
 *
 *   The following memory map is assumed for the flat build:
 *
 *     .data region.  Size determined at link time.
 *     .bss  region  Size determined at link time.
 *     IDLE thread stack.  Size determined by CONFIG_IDLETHREAD_STACKSIZE.
 *     Heap.  Extends to SRAM_HEAP_END (HCPU2LCPU mailbox reserved at top).
 *
 *   The following memory map is assumed for the kernel build:
 *
 *     Kernel .data region.  Size determined at link time.
 *     Kernel .bss  region  Size determined at link time.
 *     Kernel IDLE thread stack.  Size determined by
 *       CONFIG_IDLETHREAD_STACKSIZE.
 *     Padding for alignment
 *     User .data region.  Size determined at link time.
 *     User .bss region  Size determined at link time.
 *     Kernel heap.  Size determined by CONFIG_MM_KERNEL_HEAPSIZE.
 *     User heap.  Extends to SRAM_HEAP_END (below the HCPU2LCPU mailbox).
 *
 ****************************************************************************/

void up_allocate_heap(FAR void **heap_start, size_t *heap_size)
{
  /* Heap starts at g_idle_topstack and stops before the BT mailbox. */

  *heap_start = (FAR void *)g_idle_topstack;
  *heap_size  = SRAM_HEAP_END - g_idle_topstack;
}

/******************************************************************************
 * Name: arm_addregion
 *
 * Description:
 *   Memory may be added in non-contiguous chunks.  Additional chunks are
 *   added by calling this function.
 *
 ******************************************************************************/

#if CONFIG_MM_REGIONS > 1
void arm_addregion(void)
{
#ifdef CONFIG_BSP_USING_PSRAM
  if (g_psram_ready)
    {
      size_t psram_heap = PSRAM_HEAP_SIZE;

#if defined(CONFIG_BSP_USING_PSRAM) && defined(BOARD_PSRAM_POOL_BYTES)
      if (BOARD_PSRAM_POOL_KB > 0 && psram_heap > BOARD_PSRAM_POOL_BYTES)
        {
          psram_heap -= BOARD_PSRAM_POOL_BYTES;
        }
#endif

#if defined(CONFIG_MYVENDOR_MTP_SIMPLE)
      if (psram_heap > MTP_PSRAM_MTP_RESERVE_BYTES)
        {
          psram_heap -= MTP_PSRAM_MTP_RESERVE_BYTES;
        }
#endif

      kumm_addregion((void *)PSRAM_HEAP_START, psram_heap);
    }
#endif
}
#endif
