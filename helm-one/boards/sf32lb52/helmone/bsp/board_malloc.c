/**
 * @file board_malloc.c
 * @brief 板级 SRAM / PSRAM 显式内存分配。
 *
 * PSRAM 三块：Umem kumm、BoardPSRAM 独立堆、MTP bump（见 mtp_psram.c）。
 * board_malloc_psram() 供 gpx_port 等大块分配；不计入 NSH `free`。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "board_malloc.h"
#include "board_psram_layout.h"

#include <nuttx/config.h>
#include <nuttx/fs/procfs.h>
#include <nuttx/mm/mm.h>
#include <nuttx/mutex.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#if (CONFIG_MM_REGIONS > 1) || \
    (defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0)

#include "mm_heap/mm.h"

#endif

#if CONFIG_MM_REGIONS > 1

/**
 * Umem 按 region 统计（供 boardmem 显示 Umem SRAM / Umem PSRAM kumm）。
 *
 * NuttX mallinfo() 仅全堆合计；CONFIG_MM_REGIONS&gt;1 时需 walk 各 region。
 */

/** @brief mallinfo 单节点累加回调。 */
static void board_umem_mallinfo_node(FAR struct mm_allocnode_s *node,
                                     FAR void *arg)
{
  FAR struct mallinfo *info = arg;
  size_t nodesize = MM_SIZEOF_NODE(node);

  if (MM_NODE_IS_ALLOC(node))
    {
      info->aordblks++;
      info->uordblks += nodesize;
    }
  else
    {
      info->ordblks++;
      info->fordblks += nodesize;
      if (node->size > (size_t)info->mxordblk)
        {
          info->mxordblk = nodesize;
        }
    }
}

/**
 * @brief 查询 Umem 指定 region 的 mallinfo。
 * @param region 0=片内 SRAM，1=PSRAM kumm。
 * @param[out] info 输出；arena=mm_regionsize[region]。
 * @return true 成功，false 参数或 region 无效。
 */
bool board_umem_region_mallinfo(int region, struct mallinfo *info)
{
  FAR struct mm_heap_s *heap;
  FAR struct mm_allocnode_s *node;
  size_t nodesize;

  if (info == NULL || region < 0)
    {
      return false;
    }

  heap = g_mmheap;
  if (heap == NULL || region >= heap->mm_nregions)
    {
      return false;
    }

  memset(info, 0, sizeof(*info));
  mm_free_delaylist(heap);
  nxrmutex_lock(&heap->mm_lock);

  for (node = heap->mm_heapstart[region];
       node < heap->mm_heapend[region];
       node = (FAR struct mm_allocnode_s *)((FAR char *)node + nodesize))
    {
      nodesize = MM_SIZEOF_NODE(node);
      board_umem_mallinfo_node(node, info);
    }

  /* mm_foreach 对 heapend 哨兵节点也会调用一次 handler */
  board_umem_mallinfo_node(heap->mm_heapend[region], info);
  nxrmutex_unlock(&heap->mm_lock);

  info->arena   = heap->mm_regionsize[region];
  info->usmblks = info->uordblks;
  return true;
}

/**
 * @brief 返回 Umem region 尾部 probe_len 字节地址。
 * @return true 成功。
 */
bool board_umem_region_tail_addr(int region, size_t probe_len,
                                 uintptr_t *addr)
{
  FAR struct mm_heap_s *heap;
  size_t region_size;

  if (addr == NULL || probe_len == 0)
    {
      return false;
    }

  heap = g_mmheap;
  if (heap == NULL || region < 0 || region >= heap->mm_nregions)
    {
      return false;
    }

  region_size = heap->mm_regionsize[region];
  if (region_size < probe_len + 2 * MM_SIZEOF_ALLOCNODE)
    {
      return false;
    }

  *addr = (uintptr_t)heap->mm_heapend[region] - probe_len;
  return true;
}

#else

#include "mm_heap/mm.h"

/* 单 region 板型：region 0 复用全局 mallinfo() */

/**
 * @brief 单 region 板型 Umem mallinfo。
 * @return true 且 region==0 时填充 *info。
 */
bool board_umem_region_mallinfo(int region, struct mallinfo *info)
{
  if (info == NULL || region != 0)
    {
      return false;
    }

  *info = mallinfo();
  return true;
}

bool board_umem_region_tail_addr(int region, size_t probe_len,
                                 uintptr_t *addr)
{
  FAR struct mm_heap_s *heap;
  size_t region_size;

  if (addr == NULL || probe_len == 0 || region != 0)
    {
      return false;
    }

  heap = g_mmheap;
  if (heap == NULL || heap->mm_nregions < 1)
    {
      return false;
    }

  region_size = heap->mm_regionsize[0];
  if (region_size < probe_len + 2 * MM_SIZEOF_ALLOCNODE)
    {
      return false;
    }

  *addr = (uintptr_t)heap->mm_heapend[0] - probe_len;
  return true;
}

#endif

/****************************************************************************
 * BoardPSRAM 独立堆（board_psram_pool_base() 上 mm_initialize）
 ****************************************************************************/

static pthread_mutex_t g_board_psram_init_lock = PTHREAD_MUTEX_INITIALIZER; /**< BoardPSRAM 堆 init 锁。 */
static struct mm_heap_s *g_board_psram_heap;   /**< BoardPSRAM mm 堆指针。 */
static bool g_board_psram_heap_ready;          /**< 堆是否已成功 mm_initialize。 */

/**
 * @brief BoardPSRAM 池尾部地址（堆未就绪时用 layout 推算）。
 * @return true 成功。
 */
bool board_psram_pool_tail_addr(size_t probe_len, uintptr_t *addr)
{
#if defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0

  if (addr == NULL || probe_len == 0)
    {
      return false;
    }

  board_psram_heap_init();

  if (g_board_psram_heap_ready && g_board_psram_heap != NULL)
    {
      if (g_board_psram_heap->mm_regionsize[0] <
          probe_len + 2 * MM_SIZEOF_ALLOCNODE)
        {
          return false;
        }

      *addr = (uintptr_t)g_board_psram_heap->mm_heapend[0] - probe_len;
      return true;
    }

  if (BOARD_PSRAM_POOL_BYTES < probe_len)
    {
      return false;
    }

  *addr = board_psram_pool_base() + BOARD_PSRAM_POOL_BYTES - probe_len;
  return true;

#else

  (void)probe_len;
  (void)addr;
  return false;

#endif
}

/**
 * @brief 初始化 BoardPSRAM 独立堆（幂等）。
 * @return 0 成功，-ENOMEM/-ENOTSUP 失败。
 */
#if defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0

/** @brief 检测 BoardPSRAM 是否与 Umem region 重叠。 */
static bool board_psram_overlaps_umem(uintptr_t pool, size_t bytes)
{
  FAR struct mm_heap_s *heap = g_mmheap;
  uintptr_t pool_end = pool + bytes;
  int nregions;
  int r;

  if (heap == NULL || bytes == 0)
    {
      return false;
    }

#if CONFIG_MM_REGIONS > 1
  nregions = heap->mm_nregions;
#else
  nregions = 1;
#endif

  for (r = 0; r < nregions; r++)
    {
      uintptr_t lo = (uintptr_t)heap->mm_heapstart[r];
      uintptr_t hi = (uintptr_t)heap->mm_heapend[r];

      syslog(LOG_INFO, "board_malloc: umem r%d %p-%p\n",
             r, (void *)lo, (void *)hi);

      if (pool < hi && pool_end > lo)
        {
          return true;
        }
    }

  return false;
}

#endif

/** @brief 初始化 BoardPSRAM 独立堆（幂等，见文件头说明）。 */
int board_psram_heap_init(void)
{
#if defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0

  pthread_mutex_lock(&g_board_psram_init_lock);
  if (!g_board_psram_heap_ready)
    {
      uintptr_t pool = board_psram_pool_base();

      syslog(LOG_INFO, "board_malloc: BoardPSRAM %p +%u KiB\n",
             (void *)pool, (unsigned)(BOARD_PSRAM_POOL_BYTES / 1024u));

      if (board_psram_overlaps_umem(pool, BOARD_PSRAM_POOL_BYTES))
        {
          syslog(LOG_ERR,
                 "board_malloc: BoardPSRAM overlaps Umem, skip "
                 "(BLE/UI will fall back to malloc)\n");
        }
      else
        {
          g_board_psram_heap = mm_initialize("BoardPSRAM",
            (FAR void *)pool, BOARD_PSRAM_POOL_BYTES);
          g_board_psram_heap_ready = (g_board_psram_heap != NULL);

#if defined(CONFIG_FS_PROCFS) && !defined(CONFIG_FS_PROCFS_EXCLUDE_MEMINFO)
          /* NSH `free` walks every procfs heap.  Keep BoardPSRAM out of
           * that list so a PSRAM-side glitch cannot assert nsh_main.
           * Use `boardmem` for this pool. */

          if (g_board_psram_heap != NULL &&
              g_board_psram_heap->mm_procfs != NULL)
            {
              procfs_unregister_meminfo(g_board_psram_heap->mm_procfs);
              g_board_psram_heap->mm_procfs = NULL;
            }
#endif
        }
    }

  pthread_mutex_unlock(&g_board_psram_init_lock);
  return g_board_psram_heap_ready ? 0 : -ENOMEM;

#else

  return -ENOTSUP;

#endif
}

/** @brief BoardPSRAM 堆是否已就绪。 */
bool board_psram_heap_ready(void)
{
  return g_board_psram_heap_ready;
}

/** @brief 返回 BoardPSRAM 堆 mallinfo。 */
struct mallinfo board_psram_mallinfo(void)
{
  struct mallinfo info;

  memset(&info, 0, sizeof(info));
  if (board_psram_heap_init() == 0 && g_board_psram_heap != NULL)
    {
      info = mm_mallinfo(g_board_psram_heap);
    }

  return info;
}

/****************************************************************************
 * 板级分配 / 释放 API
 ****************************************************************************/

/** @brief 从 Umem SRAM（malloc）分配。 */
void *board_malloc_sram(size_t size)
{
  if (size == 0)
    {
      return NULL;
    }

  return malloc(size);
}

/**
 * @brief 优先 BoardPSRAM 分配，失败或未就绪则回退 SRAM。
 * @return 指针或 NULL。
 */
void *board_malloc_psram(size_t size)
{
  void *ptr;

  if (size == 0)
    {
      return NULL;
    }

#if defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0

  if (board_psram_heap_init() != 0 || g_board_psram_heap == NULL)
    {
      return board_malloc_sram(size);
    }

  ptr = mm_malloc(g_board_psram_heap, size);
  if (ptr != NULL)
    {
      return ptr;
    }

#endif

  return board_malloc_sram(size);
}

/** @brief PSRAM 堆内 realloc，否则标准 realloc。 */
void *board_realloc_psram(void *ptr, size_t size)
{
  if (size == 0)
    {
      board_mem_free(ptr);
      return NULL;
    }

  if (ptr == NULL)
    {
      return board_malloc_psram(size);
    }

#if defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0

  if (board_psram_heap_init() == 0 && g_board_psram_heap != NULL &&
      mm_heapmember(g_board_psram_heap, ptr))
    {
      return mm_realloc(g_board_psram_heap, ptr, size);
    }

#endif

  return realloc(ptr, size);
}

/** @brief 释放 SRAM 指针（free）。 */
void board_free_sram(void *ptr)
{
  if (ptr != NULL)
    {
      free(ptr);
    }
}

/** @brief 释放 PSRAM 池指针，非池内则走 board_free_sram。 */
void board_free_psram(void *ptr)
{
  if (ptr == NULL)
    {
      return;
    }

#if defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0

  if (g_board_psram_heap_ready &&
      g_board_psram_heap != NULL &&
      mm_heapmember(g_board_psram_heap, ptr))
    {
      mm_free(g_board_psram_heap, ptr);
      return;
    }

#endif

  board_free_sram(ptr);
}

/** @brief 自动判别 PSRAM 池或 SRAM 并释放。 */
void board_mem_free(void *ptr)
{
  if (ptr == NULL)
    {
      return;
    }

#if defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0

  if (g_board_psram_heap_ready &&
      g_board_psram_heap != NULL &&
      mm_heapmember(g_board_psram_heap, ptr))
    {
      board_free_psram(ptr);
      return;
    }

#endif

  board_free_sram(ptr);
}

/** @brief 判断指针是否在 BoardPSRAM 堆内。 */
bool board_ptr_in_psram_pool(const void *ptr)
{
#if defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0

  if (ptr != NULL && g_board_psram_heap_ready && g_board_psram_heap != NULL) {
    return mm_heapmember(g_board_psram_heap, ptr);
  }

#endif

  return false;
}
