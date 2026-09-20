/****************************************************************************
 * vendor/sifli/chips/sf32lb52/sf32lb52_bt_adapter.c
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
 ****************************************************************************/

#include <nuttx/config.h>
#include <fcntl.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include <nuttx/cache.h>
#include <nuttx/clock.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>

#include "bf0_hal.h"
#include "circular_buf.h"
#include "ipc_hw.h"
#include "ipc_queue.h"
#include "mem_map.h"
#include "sf32lb52_bt_adapter.h"
#include "sf32lb52_bd_addr.h"

#define SF32LB52_BT_QID          0
/* Fixed HCPU SRAM top: CH2 0x2007FC00 + CH1 0x2007FE00 (2 x 512 B).
 * Hardware mailbox, not a malloc buffer. Must stay outside Umem
 * (SRAM_HEAP_END in sifli_allocateheap.c). BSP once mapped kumm to
 * SRAM_END; HCI TX into this ring corrupted the SRAM heap (not OOM).
 */
#define SF32LB52_BT_TX_BUF_SIZE  HCPU2LCPU_MB_CH1_BUF_SIZE
#define SF32LB52_BT_TX_BUF_ADDR  HCPU2LCPU_MB_CH1_BUF_START_ADDR
#define SF32LB52_BT_TX_BUF_ALIAS HCPU_ADDR_2_LCPU_ADDR(HCPU2LCPU_MB_CH1_BUF_START_ADDR)
#define SF32LB52_BT_RX_BUF_LEGACY \
  LCPU_ADDR_2_HCPU_ADDR(LCPU2HCPU_MB_CH1_BUF_START_ADDR)
#define SF32LB52_BT_RX_BUF_REV_B \
  LCPU_ADDR_2_HCPU_ADDR(LCPU2HCPU_MB_CH1_BUF_REV_B_START_ADDR)
#define SF32LB52_BT_RX_BUF_SIZE  LCPU2HCPU_MB_CH1_BUF_SIZE
#define SF32LB52_BT_RING_DATA_SIZE \
  ((SF32LB52_BT_RX_BUF_SIZE - sizeof(struct circular_buf)) & ~3UL)
#define SF32LB52_BT_NVDS_BUF_START 0x2040FE00
#define SF32LB52_BT_NVDS_BUF_SIZE  512
#define SF32LB52_BT_NVDS_PATTERN   0x4e564453
#define SF32LB52_BT_NVDS_TAG_BD    0x01
#define SF32LB52_BT_BD_ADDR_LEN    6
#define SF32LB52_BT_TRACE          0
#define SF32LB52_BT_H4_CMD         0x01
#define SF32LB52_BT_RX_FAIL_LOG_MS 1000u
#define SF32LB52_BT_RX_FAIL_GAP_MS 500u
#define SF32LB52_BT_RX_ENOMEM_BACKOFF_US 3000u
#ifdef CONFIG_MYVENDOR_BLE_LOG
#  define BLE_LOG(fmt, ...) syslog(LOG_INFO, "ble: " fmt "\n", ##__VA_ARGS__)
#else
#  define BLE_LOG(...) ((void)0)
#endif

typedef enum
{
  SF32LB52_BT_STATUS_IDLE = 0,
  SF32LB52_BT_STATUS_INITED,
  SF32LB52_BT_STATUS_ENABLED,
} sf32lb52_bt_status_t;

struct sf32lb52_bt_env_s
{
  ipc_queue_handle_t ipc_port;
  uint8_t data_buf[SF32LB52_BT_RX_BUF_SIZE];
  sf32lb52_bt_rx_callback_t notify_host;
  struct work_s rx_work;
  bool queue_open;
  bool wake_held;
  volatile bool rx_work_pending;
  bool rx_worker_running;
  uint32_t rx_read_idx_mirror;
  uint32_t rx_count;
};

static struct sf32lb52_bt_env_s g_sf32lb52_bt_env;
static sf32lb52_bt_status_t g_sf32lb52_bt_status = SF32LB52_BT_STATUS_IDLE;
static volatile uint32_t g_rx_fail_first_ms;
static volatile uint32_t g_rx_fail_last_ms;
static volatile uint32_t g_rx_fail_n;
static uint32_t g_rx_fail_log_ms;
static volatile bool g_hci_skip_sync;
static volatile uint32_t g_hci_last_rx_ms;
static volatile uint32_t g_hci_last_tx_ms;
static volatile uint32_t g_hci_last_ok_tx_ms;
static volatile uint16_t g_hci_last_tx_op;
static volatile uint8_t g_hci_last_rx_evt;

/** 已发到 LCPU、还没等到事件（Command Complete / Status）的操作码，0 = 没有。
 *
 *  2026-09-18：`cmd_stalled()` 原来拿时间戳比大小（`last_rx < last_ok_tx`），
 *  但"发送成功"是 `wait_tx_idle()` 用 `usleep(1000)` 轮询确认 LCPU 取走字节
 *  后才盖章的 —— 控制器回 Command Complete 往往比这个盖章**早几毫秒**，
 *  于是 `last_rx < last_ok_tx` 对一条**已经被回答**的命令也成立，检测器把
 *  健康空闲当"控制器哑了"，4 s 后 diag 把适配器整个拆掉：实机就是
 *  22.6 s 一轮的拆装循环（广播能起、扫描起不来、手机永远连不上）。
 *  改成按操作码配对：只有"某条命令真的没回条"才算静默，空闲不算。
 */
static volatile uint16_t g_hci_pend_op;
static volatile uint32_t g_hci_pend_ms;
#ifdef CONFIG_MYVENDOR_BLE_LOG
static uint32_t g_send_skip_log_ms;
#endif

void sf32lb52_bt_hci_skip_sync_set(bool on)
{
  if (g_hci_skip_sync != on)
    {
      BLE_LOG("skip_sync %d -> %d", g_hci_skip_sync ? 1 : 0, on ? 1 : 0);
    }

  g_hci_skip_sync = on;
}

bool sf32lb52_bt_hci_skip_sync(void)
{
  return g_hci_skip_sync;
}
static uintptr_t g_sf32lb52_bt_rx_buf_addr;
static ipc_hw_q_handle_t g_sf32lb52_bt_tx_hw =
{
  .ch_id = SF32LB52_BT_QID / IPC_HW_QUEUE_NUM,
  .q_idx = SF32LB52_BT_QID % IPC_HW_QUEUE_NUM,
};

struct sf32lb52_bt_nvds_mem_init_s
{
  uint32_t pattern;
  uint16_t used_mem;
  uint16_t writting;
};

static const uint8_t g_sf32lb52_bt_nvds_default_rc10k[] =
{
  0x0d, 0x02, 0x64, 0x19, 0x12, 0x01, 0x01, 0x2f,
  0x04, 0x20, 0x00, 0x00, 0x00, 0x01, 0x06, 0x12,
  0x34, 0x56, 0x78, 0xab, 0xcd, 0x15, 0x01, 0x01
};

static const uint8_t g_sf32lb52_bt_nvds_default_lxt32k[] =
{
  0x2f, 0x04, 0x20, 0x00, 0x00, 0x00, 0x01, 0x06,
  0x12, 0x34, 0x56, 0x78, 0xab, 0xcd, 0x15, 0x01,
  0x01
};

extern uint8_t lcpu_power_on(void);
extern uint8_t lcpu_power_off(void);
extern void ipc_queue_data_ind(uint32_t user_data);

static uint32_t sf32lb52_bt_now_ms(void)
{
  return (uint32_t)TICK2MSEC(clock_systime_ticks());
}

void sf32lb52_bt_hci_rx_stall_clear(void)
{
  g_rx_fail_first_ms = 0;
  g_rx_fail_last_ms = 0;
  g_rx_fail_n = 0;

  /* 适配器重建/显式清障时，"待完成"也要归零：否则上一代的残留会让新的
   * 一轮刚起来就带着一个假的待完成命令。 */
  g_hci_pend_op = 0;
  g_hci_pend_ms = 0;
}

bool sf32lb52_bt_hci_rx_stalled(uint32_t stall_ms)
{
  uint32_t first = g_rx_fail_first_ms;
  uint32_t last = g_rx_fail_last_ms;
  uint32_t now;

  if (first == 0 || stall_ms == 0)
    {
      return false;
    }

  now = sf32lb52_bt_now_ms();
  if ((int32_t)(now - last) > (int32_t)SF32LB52_BT_RX_FAIL_GAP_MS)
    {
      return false;
    }

  return (uint32_t)(now - first) >= stall_ms;
}

bool sf32lb52_bt_hci_cmd_stalled(uint32_t stall_ms)
{
  uint32_t pend_age;
  uint16_t pend;
  uint32_t now;

  if (stall_ms == 0 || g_hci_skip_sync || !g_sf32lb52_bt_env.queue_open)
    {
      return false;
    }

  /** 没有任何"已发出、还没回"的命令 → 主机只是没事可发（广播已经在跑、
   *  没人要扫描、也没有链路），**不是**控制器哑。原实现只看时间戳，
   *  这种健康空闲会一路走到 4 s 判死 → 拆适配器（见 g_hci_pend_op 注释）。
   */
  pend = g_hci_pend_op;
  if (pend == 0)
    {
      return false;
    }

  now = sf32lb52_bt_now_ms();
  pend_age = (uint32_t)(now - g_hci_pend_ms);
  return pend_age >= stall_ms;
}

static uintptr_t sf32lb52_bt_rx_buf_addr(void);
static size_t sf32lb52_bt_ring_data_len(uint32_t rd_ptr, uint32_t wr_ptr,
                                        uint32_t buffer_size);
static size_t sf32lb52_bt_tx_pending(struct circular_buf *tx_ring,
                                     uint32_t *rd_ptr,
                                     uint32_t *wr_ptr);

void sf32lb52_bt_hci_dump_stall(void)
{
  uint32_t now = sf32lb52_bt_now_ms();
  uint32_t rx = g_hci_last_rx_ms;
  uint32_t tx = g_hci_last_tx_ms;

  syslog(LOG_ERR,
         "sf32lb52 bt stall skip=%d open=%d status=%d rxn=%lu "
         "txop=0x%04x rxevt=0x%02x rxage=%lu txage=%lu rxf=%lu "
         "pendop=0x%04x pendage=%lu\n",
         g_hci_skip_sync ? 1 : 0,
         g_sf32lb52_bt_env.queue_open ? 1 : 0,
         (int)g_sf32lb52_bt_status,
         (unsigned long)g_sf32lb52_bt_env.rx_count,
         (unsigned)g_hci_last_tx_op,
         (unsigned)g_hci_last_rx_evt,
         (unsigned long)(rx == 0 ? 0xfffffffful : (unsigned long)(now - rx)),
         (unsigned long)(tx == 0 ? 0xfffffffful : (unsigned long)(now - tx)),
         (unsigned long)g_rx_fail_n,
         (unsigned)g_hci_pend_op,
         (unsigned long)(g_hci_pend_op == 0
                             ? 0ul
                             : (unsigned long)(now - g_hci_pend_ms)));

  /** LCPU→host 环的指针：这是区分「LCPU 哑了」和「主机没在抽」的唯一判据。
   *
   *  - `wr` 一直在走、`rd` 不动 → LCPU 在写，是主机侧 RX 抽取链断了
   *    （`rx_worker_running` 闩死 / HPWORK 被占）；
   *  - `wr` 不动 → LCPU 根本没往环里写，控制器/IPC 侧的静默；
   *  - `pend` 是环里积压的字节数，配合 `rxage` 看。
   *
   *  `oktxage` 是"最后一条**成功发出**的 HCI 命令"到现在的毫秒数（与
   *  `txage` 不同：后者在尝试发送时就盖章，失败也算）。
   */
  {
    struct circular_buf *rx_ring =
        (struct circular_buf *)sf32lb52_bt_rx_buf_addr();

    if (rx_ring != NULL)
      {
        uint32_t wr = rx_ring->write_idx_mirror;
        uint32_t rd = rx_ring->read_idx_mirror;

        syslog(LOG_ERR,
               "sf32lb52 bt ring wr=0x%08lx rd=0x%08lx pend=%lu size=%d "
               "oktxage=%lu\n",
               (unsigned long)wr,
               (unsigned long)rd,
               (unsigned long)sf32lb52_bt_ring_data_len(rd, wr,
                                                        rx_ring->buffer_size),
               (int)rx_ring->buffer_size,
               (unsigned long)(g_hci_last_ok_tx_ms == 0
                                   ? 0xfffffffful
                                   : (unsigned long)(now -
                                                     g_hci_last_ok_tx_ms)));
      }
  }

  /** 反方向的判据：TX 环里还有没有我们的字节没被 LCPU 取走。
   *
   *  - `pend==0` → LCPU 把命令读走了，只是不回事件（命令接口/固件的静默）；
   *  - `pend>0`  → LCPU 连 TX 环都不抽了（IPC 或 LCPU 整体停摆）。
   *  这两者要走的恢复动作不同，所以必须分开看。
   */
  {
    struct circular_buf *tx_ring =
        (struct circular_buf *)SF32LB52_BT_TX_BUF_ADDR;
    uint32_t t_rd = 0;
    uint32_t t_wr = 0;
    size_t t_pend;

    t_pend = sf32lb52_bt_tx_pending(tx_ring, &t_rd, &t_wr);
    syslog(LOG_ERR, "sf32lb52 bt txring wr=0x%08lx rd=0x%08lx pend=%lu\n",
           (unsigned long)t_wr, (unsigned long)t_rd, (unsigned long)t_pend);
  }
}

static void sf32lb52_bt_note_host_rx(int ret)
{
  uint32_t now;

  if (ret >= 0)
    {
      g_hci_last_rx_ms = sf32lb52_bt_now_ms();
      sf32lb52_bt_hci_rx_stall_clear();
      return;
    }

  now = sf32lb52_bt_now_ms();
  if (g_rx_fail_first_ms == 0)
    {
      g_rx_fail_first_ms = now;
      BLE_LOG("hci rx fail begin ret=%d skip=%d",
              ret, g_hci_skip_sync ? 1 : 0);
    }

  g_rx_fail_last_ms = now;
  g_rx_fail_n++;
  if (g_rx_fail_log_ms == 0 ||
      (uint32_t)(now - g_rx_fail_log_ms) >= SF32LB52_BT_RX_FAIL_LOG_MS)
    {
      g_rx_fail_log_ms = now;
      syslog(LOG_ERR,
             "sf32lb52 bt rx callback: %d (n=%lu stall=%lu ms skip=%d)\n",
             ret,
             (unsigned long)g_rx_fail_n,
             (unsigned long)(now - g_rx_fail_first_ms),
             g_hci_skip_sync ? 1 : 0);
#ifdef CONFIG_MYVENDOR_BLE_LOG
      BLE_LOG("hci rx stall n=%lu first=%lu last=%lu status=%d open=%d",
              (unsigned long)g_rx_fail_n,
              (unsigned long)g_rx_fail_first_ms,
              (unsigned long)g_rx_fail_last_ms,
              (int)g_sf32lb52_bt_status,
              g_sf32lb52_bt_env.queue_open ? 1 : 0);
#endif
    }
}

static uintptr_t sf32lb52_bt_rx_buf_addr(void)
{
  uint8_t rev_id;

  if (g_sf32lb52_bt_rx_buf_addr != 0)
    {
      return g_sf32lb52_bt_rx_buf_addr;
    }

  rev_id = __HAL_SYSCFG_GET_REVID();
  if (rev_id >= HAL_CHIP_REV_ID_A4)
    {
      g_sf32lb52_bt_rx_buf_addr = SF32LB52_BT_RX_BUF_REV_B;
    }
  else
    {
      g_sf32lb52_bt_rx_buf_addr = SF32LB52_BT_RX_BUF_LEGACY;
    }

  syslog(LOG_INFO, "sf32lb52 bt rx ring addr=0x%08lx rev=%u\n",
         (unsigned long)g_sf32lb52_bt_rx_buf_addr, rev_id);
  return g_sf32lb52_bt_rx_buf_addr;
}

/* ---- NVDS 落盘（2026-09-19）------------------------------------------------
 * 这块 512 B（0x2040FE00）是**控制器共享的非易失信箱**：BD_ADDR、射频校准，
 * 以及（如果它承载的话）配对/绑定记录。它固定在 SRAM 地址上（控制器按地址读写），
 * 所以我们**不搬地址**，只在外面套一层 KV：
 *   开机：先按住默认值 memset，再尝试用 /mnt/kv/bt_nvds.bin 覆盖（pattern 校验通过才用）；
 *   变化：写入点主动存一次 + 低频摘要巡检（控制器自己写的也能被逮到）。
 * 只在**内容真的变了**时写盘，空闲期几乎零写入。 */
#define SF32LB52_BT_NVDS_KV_PATH  "/mnt/kv/bt_nvds.bin"
#define SF32LB52_BT_NVDS_KV_MAGIC 0x564e5442u   /* "BTNV" */
#define SF32LB52_BT_NVDS_POLL_MS  5000u

struct sf32lb52_bt_nvds_kv_hdr_s
{
  uint32_t magic;
  uint32_t len;
  uint32_t crc;      /* 512 B 内容的 crc32（简单按字异或+移位，够用） */
  uint32_t pad;
};

static uint32_t g_nvds_last_crc;
static uint32_t g_nvds_poll_ms;

static uint32_t sf32lb52_bt_nvds_crc(const uint8_t *p, size_t n)
{
  uint32_t c = 0x811c9dc5u;
  size_t i;

  for (i = 0; i < n; i++)
    {
      c ^= p[i];
      c *= 16777619u;
    }
  return c;
}

static void sf32lb52_bt_nvds_kv_save(void)
{
  struct sf32lb52_bt_nvds_kv_hdr_s hdr;
  const uint8_t *blob = (const uint8_t *)SF32LB52_BT_NVDS_BUF_START;
  uint32_t crc = sf32lb52_bt_nvds_crc(blob, SF32LB52_BT_NVDS_BUF_SIZE);
  int fd;

  if (crc == g_nvds_last_crc)
    {
      return;                                    /* 没变就不写盘 */
    }

  hdr.magic = SF32LB52_BT_NVDS_KV_MAGIC;
  hdr.len   = SF32LB52_BT_NVDS_BUF_SIZE;
  hdr.crc   = crc;
  hdr.pad   = 0;

  fd = open(SF32LB52_BT_NVDS_KV_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      syslog(LOG_WARNING, "bt nvds: save open failed %d\n", errno);
      return;
    }

  if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)
      || write(fd, blob, SF32LB52_BT_NVDS_BUF_SIZE)
         != (ssize_t)SF32LB52_BT_NVDS_BUF_SIZE)
    {
      syslog(LOG_WARNING, "bt nvds: save write failed %d\n", errno);
    }
  else
    {
      g_nvds_last_crc = crc;
      syslog(LOG_NOTICE, "bt nvds: saved to kv (crc=%08lx)\n",
             (unsigned long)crc);
    }

  (void)close(fd);
}

/* ---- NVDS dump（`ctl bt nvds`）------------------------------------------------
 * 直接把 512 B 打出来（16 B/行，hex + ASCII）并报 KV 状态和 CRC。
 * 不做 tag 解析：这块是控制器私有的信箱，布局没有公开契约 —— 用"配对前后各 dump
 * 一次、对比 diff"来判断绑定/IRK 是否在其中，比按猜测的布局解析可靠。 */
int sf32lb52_bt_nvds_dump(void)
{
  const uint8_t *blob = (const uint8_t *)SF32LB52_BT_NVDS_BUF_START;
  unsigned off;

  printf("bt nvds: buf=%p size=%u crc=%08lx kv=%s\n",
         (const void *)blob, (unsigned)SF32LB52_BT_NVDS_BUF_SIZE,
         (unsigned long)sf32lb52_bt_nvds_crc(blob, SF32LB52_BT_NVDS_BUF_SIZE),
         access(SF32LB52_BT_NVDS_KV_PATH, F_OK) == 0 ? "present" : "absent");

  for (off = 0; off < SF32LB52_BT_NVDS_BUF_SIZE; off += 16u)
    {
      char asc[17];
      unsigned i;

      for (i = 0; i < 16u; i++)
        {
          const unsigned char c = blob[off + i];

          asc[i] = (c >= 0x20u && c < 0x7fu) ? (char)c : '.';
        }
      asc[16] = '\0';

      printf("  %03x  %02x%02x%02x%02x %02x%02x%02x%02x "
             "%02x%02x%02x%02x %02x%02x%02x%02x  %s\n",
             off,
             blob[off + 0], blob[off + 1], blob[off + 2], blob[off + 3],
             blob[off + 4], blob[off + 5], blob[off + 6], blob[off + 7],
             blob[off + 8], blob[off + 9], blob[off + 10], blob[off + 11],
             blob[off + 12], blob[off + 13], blob[off + 14], blob[off + 15],
             asc);
    }

  return 0;
}

/* 让 dump 之前先把 KV 与内存对齐（若控制器刚改过，顺手落盘）。 */
void sf32lb52_bt_nvds_sync(void)
{
  sf32lb52_bt_nvds_kv_save();
}

static void sf32lb52_bt_nvds_kv_load(void)
{
  struct sf32lb52_bt_nvds_kv_hdr_s hdr;
  uint8_t buf[SF32LB52_BT_NVDS_BUF_SIZE];
  int fd;
  uint32_t pat;

  fd = open(SF32LB52_BT_NVDS_KV_PATH, O_RDONLY);
  if (fd < 0)
    {
      syslog(LOG_NOTICE, "bt nvds: no kv file, keep defaults\n");
      g_nvds_last_crc = sf32lb52_bt_nvds_crc(
          (const uint8_t *)SF32LB52_BT_NVDS_BUF_START,
          SF32LB52_BT_NVDS_BUF_SIZE);
      return;
    }

  if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)
      || hdr.magic != SF32LB52_BT_NVDS_KV_MAGIC
      || hdr.len != SF32LB52_BT_NVDS_BUF_SIZE
      || read(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf))
    {
      syslog(LOG_WARNING, "bt nvds: kv header/short read, keep defaults\n");
      (void)close(fd);
      g_nvds_last_crc = sf32lb52_bt_nvds_crc(
          (const uint8_t *)SF32LB52_BT_NVDS_BUF_START,
          SF32LB52_BT_NVDS_BUF_SIZE);
      return;
    }

  (void)close(fd);

  pat = *(const uint32_t *)buf;
  if (pat != SF32LB52_BT_NVDS_PATTERN
      || sf32lb52_bt_nvds_crc(buf, sizeof(buf)) != hdr.crc)
    {
      syslog(LOG_WARNING, "bt nvds: kv pattern/crc bad (pat=%08lx), defaults\n",
             (unsigned long)pat);
      g_nvds_last_crc = sf32lb52_bt_nvds_crc(
          (const uint8_t *)SF32LB52_BT_NVDS_BUF_START,
          SF32LB52_BT_NVDS_BUF_SIZE);
      return;
    }

  memcpy((void *)SF32LB52_BT_NVDS_BUF_START, buf, sizeof(buf));
  up_clean_dcache((uintptr_t)SF32LB52_BT_NVDS_BUF_START,
                  (uintptr_t)SF32LB52_BT_NVDS_BUF_START + sizeof(buf));
  g_nvds_last_crc = hdr.crc;
  syslog(LOG_NOTICE, "bt nvds: restored from kv (crc=%08lx)\n",
         (unsigned long)hdr.crc);
}

/** 低频巡检：控制器自己改了这块（配对/解绑）也能被逮到并落盘。 */
static void sf32lb52_bt_nvds_kv_poll(uint32_t now_ms)
{
  if (g_nvds_poll_ms != 0u
      && (int32_t)(now_ms - g_nvds_poll_ms) < (int32_t)SF32LB52_BT_NVDS_POLL_MS)
    {
      return;
    }

  g_nvds_poll_ms = now_ms ? now_ms : 1u;
  sf32lb52_bt_nvds_kv_save();
}

static void sf32lb52_bt_clean_nvds_shared(void)
{
  up_clean_dcache((uintptr_t)SF32LB52_BT_NVDS_BUF_START,
                  (uintptr_t)SF32LB52_BT_NVDS_BUF_START +
                  SF32LB52_BT_NVDS_BUF_SIZE);
}

static int sf32lb52_bt_bd_addr_from_uid(uint8_t addr[SF32LB52_BT_BD_ADDR_LEN])
{
  return sf32lb52_bd_addr_from_efuse(addr);
}

static void sf32lb52_bt_nvds_set_bd_addr(uint8_t *blob, size_t len,
                                         const uint8_t *addr)
{
  size_t i = 0;

  while (i + 2 <= len)
    {
      uint8_t tag = blob[i];
      uint8_t tlen = blob[i + 1];

      if (tag == 0xff || i + 2 + tlen > len)
        {
          break;
        }

      if (tag == SF32LB52_BT_NVDS_TAG_BD &&
          tlen == SF32LB52_BT_BD_ADDR_LEN)
        {
          memcpy(&blob[i + 2], addr, SF32LB52_BT_BD_ADDR_LEN);
          return;
        }

      i += 2 + tlen;
    }

  syslog(LOG_ERR, "sf32lb52 bt NVDS has no BD_ADDR tag\n");

  sf32lb52_bt_nvds_kv_save();   /* BD_ADDR 是我们自己写的：立刻落盘 */
}

static bool sf32lb52_bt_rx_ring_valid(struct circular_buf *rx_ring)
{
  uint32_t rd_ptr = rx_ring->read_idx_mirror;
  uint32_t wr_ptr = rx_ring->write_idx_mirror;
  uint32_t rd_idx = CB_GET_PTR_IDX(rd_ptr);
  uint32_t wr_idx = CB_GET_PTR_IDX(wr_ptr);
  long buf_size = rx_ring->buffer_size;

  return buf_size == (long)SF32LB52_BT_RING_DATA_SIZE &&
         rd_idx <= SF32LB52_BT_RING_DATA_SIZE &&
         wr_idx <= SF32LB52_BT_RING_DATA_SIZE;
}

static bool sf32lb52_bt_rx_ring_ready(const char *tag)
{
  struct circular_buf *rx_ring = (struct circular_buf *)sf32lb52_bt_rx_buf_addr();
  uint32_t rd_ptr = rx_ring->read_idx_mirror;
  uint32_t wr_ptr = rx_ring->write_idx_mirror;
  long buf_size = rx_ring->buffer_size;

  if (!sf32lb52_bt_rx_ring_valid(rx_ring))
    {
      syslog(LOG_WARNING,
             "%s rx ring invalid: buf=%ld expected=%lu rd=%08lx wr=%08lx\n",
             tag,
             buf_size,
             (unsigned long)SF32LB52_BT_RING_DATA_SIZE,
             (unsigned long)rd_ptr,
             (unsigned long)wr_ptr);
      return false;
    }

  return true;
}

static int sf32lb52_bt_wait_rx_ring_ready(void)
{
  struct circular_buf *rx_ring =
      (struct circular_buf *)sf32lb52_bt_rx_buf_addr();
  int i;

  for (i = 0; i < 1000; i++)
    {
      up_invalidate_dcache((uintptr_t)sf32lb52_bt_rx_buf_addr(),
                           (uintptr_t)sf32lb52_bt_rx_buf_addr() +
                           SF32LB52_BT_RX_BUF_SIZE);

      if (sf32lb52_bt_rx_ring_valid(rx_ring))
        {
          return OK;
        }

      usleep(1000);
    }

  sf32lb52_bt_rx_ring_ready("sf32lb52 wait");
  return -ETIMEDOUT;
}

static void sf32lb52_bt_prepare_stack_nvds(void)
{
  struct sf32lb52_bt_nvds_mem_init_s *nvds;
  const uint8_t *defaults;
  size_t defaults_len;
  uint8_t blob[sizeof(g_sf32lb52_bt_nvds_default_rc10k)];
  uint8_t mac[SF32LB52_BT_BD_ADDR_LEN];

  if (HAL_LXT_DISABLED())
    {
      defaults = g_sf32lb52_bt_nvds_default_rc10k;
      defaults_len = sizeof(g_sf32lb52_bt_nvds_default_rc10k);
    }
  else
    {
      defaults = g_sf32lb52_bt_nvds_default_lxt32k;
      defaults_len = sizeof(g_sf32lb52_bt_nvds_default_lxt32k);
    }

  memcpy(blob, defaults, defaults_len);
  if (sf32lb52_bt_bd_addr_from_uid(mac) == OK)
    {
      sf32lb52_bt_nvds_set_bd_addr(blob, defaults_len, mac);
    }
  else
    {
      syslog(LOG_WARNING,
             "sf32lb52 bt MAC: eFuse UID unusable, keeping NVDS default\n");
    }

  HAL_HPAON_WakeCore(CORE_ID_LCPU);

  nvds = (struct sf32lb52_bt_nvds_mem_init_s *)SF32LB52_BT_NVDS_BUF_START;
  memset((void *)SF32LB52_BT_NVDS_BUF_START, 0, SF32LB52_BT_NVDS_BUF_SIZE);
  sf32lb52_bt_nvds_kv_load();
  nvds->pattern = SF32LB52_BT_NVDS_PATTERN;
  nvds->used_mem = defaults_len;
  nvds->writting = 0;
  memcpy((void *)(nvds + 1), blob, defaults_len);
  sf32lb52_bt_clean_nvds_shared();

  HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
}

static size_t sf32lb52_bt_ring_data_len(uint32_t rd_ptr, uint32_t wr_ptr,
                                        uint32_t buffer_size)
{
  uint32_t rd_idx = CB_GET_PTR_IDX(rd_ptr);
  uint32_t wr_idx = CB_GET_PTR_IDX(wr_ptr);
  uint32_t rd_mirror = CB_GET_PTR_MIRROR(rd_ptr);
  uint32_t wr_mirror = CB_GET_PTR_MIRROR(wr_ptr);

  if (rd_idx == wr_idx)
    {
      return rd_mirror == wr_mirror ? 0 : buffer_size;
    }

  if (wr_idx > rd_idx)
    {
      return wr_idx - rd_idx;
    }

  return buffer_size - (rd_idx - wr_idx);
}

static size_t sf32lb52_bt_ring_space_len(uint32_t rd_ptr, uint32_t wr_ptr,
                                         uint32_t buffer_size)
{
  return buffer_size - sf32lb52_bt_ring_data_len(rd_ptr, wr_ptr,
                                                buffer_size);
}

static uint32_t sf32lb52_bt_ring_advance(uint32_t ptr, size_t len,
                                         uint32_t buffer_size)
{
  uint32_t idx = CB_GET_PTR_IDX(ptr);
  uint32_t mirror = CB_GET_PTR_MIRROR(ptr);

  idx += len;
  if (idx >= buffer_size)
    {
      idx -= buffer_size;
      mirror = ~mirror;
    }

  return CB_MAKE_PTR_IDX_MIRROR(idx, mirror);
}

static size_t sf32lb52_bt_ring_copy(uint8_t *dst,
                                    const struct circular_buf *rx_ring,
                                    uint32_t rd_ptr,
                                    size_t len)
{
  const uint8_t *pool = (const uint8_t *)(rx_ring + 1);
  uint32_t rd_idx = CB_GET_PTR_IDX(rd_ptr);
  size_t tail;

  if (len == 0)
    {
      return 0;
    }

  tail = rx_ring->buffer_size - rd_idx;
  if (tail >= len)
    {
      memcpy(dst, &pool[rd_idx], len);
      return len;
    }

  memcpy(dst, &pool[rd_idx], tail);
  memcpy(&dst[tail], pool, len - tail);
  return len;
}

static size_t sf32lb52_bt_ring_write(struct circular_buf *tx_ring,
                                     const uint8_t *src, size_t len)
{
  uint8_t *pool = (uint8_t *)(tx_ring + 1);
  uint32_t wr_ptr = tx_ring->write_idx_mirror;
  uint32_t wr_idx = CB_GET_PTR_IDX(wr_ptr);
  size_t space;
  size_t tail;

  space = sf32lb52_bt_ring_space_len(tx_ring->read_idx_mirror,
                                     wr_ptr,
                                     tx_ring->buffer_size);
  if (space == 0)
    {
      return 0;
    }

  if (len > space)
    {
      len = space;
    }

  tail = tx_ring->buffer_size - wr_idx;
  if (tail >= len)
    {
      memcpy(&pool[wr_idx], src, len);
      up_clean_dcache((uintptr_t)&pool[wr_idx],
                      (uintptr_t)&pool[wr_idx] + len);
    }
  else
    {
      memcpy(&pool[wr_idx], src, tail);
      memcpy(pool, &src[tail], len - tail);
      up_clean_dcache((uintptr_t)&pool[wr_idx],
                      (uintptr_t)&pool[wr_idx] + tail);
      up_clean_dcache((uintptr_t)pool,
                      (uintptr_t)pool + len - tail);
    }

  tx_ring->write_idx_mirror = sf32lb52_bt_ring_advance(wr_ptr, len,
                                                       tx_ring->buffer_size);
  up_clean_dcache((uintptr_t)tx_ring,
                  (uintptr_t)tx_ring + sizeof(*tx_ring));
  __DSB();

  return len;
}

static size_t sf32lb52_bt_tx_pending(struct circular_buf *tx_ring,
                                     uint32_t *rd_ptr,
                                     uint32_t *wr_ptr)
{
  uint32_t rd;
  uint32_t wr;

  up_invalidate_dcache((uintptr_t)SF32LB52_BT_TX_BUF_ADDR,
                       (uintptr_t)SF32LB52_BT_TX_BUF_ADDR +
                       sizeof(*tx_ring));

  rd = tx_ring->read_idx_mirror;
  wr = tx_ring->write_idx_mirror;

  if (rd_ptr != NULL)
    {
      *rd_ptr = rd;
    }

  if (wr_ptr != NULL)
    {
      *wr_ptr = wr;
    }

  return sf32lb52_bt_ring_data_len(rd, wr, tx_ring->buffer_size);
}

static void sf32lb52_bt_trigger_tx(void)
{
  __DSB();
  ipc_hw_trigger_interrupt(&g_sf32lb52_bt_tx_hw);
}

static int sf32lb52_bt_wait_tx_idle(struct circular_buf *tx_ring)
{
  uint32_t start_time = HAL_GetTick();
  uint32_t tick_count = 0;
  uint32_t rd_ptr = 0;
  uint32_t wr_ptr = 0;

  while (sf32lb52_bt_tx_pending(tx_ring, &rd_ptr, &wr_ptr) > 0)
    {
      sf32lb52_bt_trigger_tx();

      if (HAL_GetTick() != start_time)
        {
          tick_count++;
          start_time = HAL_GetTick();
        }

      if (tick_count >= 100)
        {
          syslog(LOG_ERR,
                 "sf32lb52 bt tx busy: rd=%08lx wr=%08lx\n",
                 (unsigned long)rd_ptr,
                 (unsigned long)wr_ptr);
          return -ETIMEDOUT;
        }

      usleep(1000);
    }

  return OK;
}

#if SF32LB52_BT_TRACE
static void sf32lb52_bt_log_tx_state(struct circular_buf *tx_ring,
                                     const char *tag,
                                     uint32_t target_wr)
{
  uint32_t rd_ptr = 0;
  uint32_t wr_ptr = 0;
  size_t pending;

  pending = sf32lb52_bt_tx_pending(tx_ring, &rd_ptr, &wr_ptr);
  syslog(LOG_INFO,
         "sf32lb52 bt tx %s: target=%08lx rd=%08lx wr=%08lx pending=%lu\n",
         tag,
         (unsigned long)target_wr,
         (unsigned long)rd_ptr,
         (unsigned long)wr_ptr,
         (unsigned long)pending);
}
#endif

static size_t sf32lb52_bt_tx_chunk_len(const uint8_t *data,
                                       size_t len,
                                       size_t offset)
{
  size_t remaining = len - offset;

  if (offset == 0 && remaining > 1)
    {
      return 1;
    }

  return remaining;
}

static void sf32lb52_bt_rx_worker(FAR void *arg)
{
  struct sf32lb52_bt_env_s *env = arg;

  for (;;)
    {
      irqstate_t flags;
      int empty_retries = 0;

      for (;;)
        {
          size_t size;
          size_t read_len;
          int ret;
          struct circular_buf *rx_ring;
          uint32_t wr_ptr;

          flags = enter_critical_section();

          if (!env->queue_open || env->ipc_port == IPC_QUEUE_INVALID_HANDLE)
            {
              env->rx_work_pending = false;
              env->rx_worker_running = false;
              leave_critical_section(flags);
              return;
            }

          leave_critical_section(flags);

          up_invalidate_dcache((uintptr_t)sf32lb52_bt_rx_buf_addr(),
                               (uintptr_t)sf32lb52_bt_rx_buf_addr() +
                               SF32LB52_BT_RX_BUF_SIZE);

          flags = enter_critical_section();

          if (!env->queue_open || env->ipc_port == IPC_QUEUE_INVALID_HANDLE)
            {
              env->rx_work_pending = false;
              env->rx_worker_running = false;
              leave_critical_section(flags);
              return;
            }

          rx_ring = (struct circular_buf *)sf32lb52_bt_rx_buf_addr();
          wr_ptr = rx_ring->write_idx_mirror;
          size = sf32lb52_bt_ring_data_len(env->rx_read_idx_mirror,
                                          wr_ptr,
                                          rx_ring->buffer_size);
          if (size == 0)
            {
              uint32_t rd_ptr = rx_ring->read_idx_mirror;
              uint32_t local_rd = env->rx_read_idx_mirror;

              leave_critical_section(flags);

              if (empty_retries++ < 20)
                {
                  usleep(1000);
                  continue;
                }

          #if SF32LB52_BT_TRACE
              syslog(LOG_INFO,
                "sf32lb52 bt rx empty: local=%08lx rd=%08lx wr=%08lx\n",
                (unsigned long)local_rd,
                (unsigned long)rd_ptr,
                (unsigned long)wr_ptr);
          #endif
              break;
            }

          empty_retries = 0;

          if (size > sizeof(env->data_buf))
            {
              size = sizeof(env->data_buf);
            }

          read_len = sf32lb52_bt_ring_copy(env->data_buf, rx_ring,
                                           env->rx_read_idx_mirror, size);
          if (read_len == 0)
            {
              leave_critical_section(flags);
              break;
            }

          env->rx_read_idx_mirror = sf32lb52_bt_ring_advance(
              env->rx_read_idx_mirror, read_len, rx_ring->buffer_size);
          rx_ring->read_idx_mirror = env->rx_read_idx_mirror;
          __DSB();

          up_clean_dcache((uintptr_t)sf32lb52_bt_rx_buf_addr(),
                          (uintptr_t)sf32lb52_bt_rx_buf_addr() +
                          sizeof(*rx_ring));

          env->rx_count++;

          leave_critical_section(flags);

        #if SF32LB52_BT_TRACE
             syslog(LOG_INFO,
               "sf32lb52 bt rx drain: len=%lu local=%08lx wr=%08lx\n",
               (unsigned long)read_len,
               (unsigned long)env->rx_read_idx_mirror,
               (unsigned long)wr_ptr);
        #endif

          if (env->notify_host == NULL)
            {
              continue;
            }

          if (read_len >= 2 && env->data_buf[0] == 0x04)
            {
              g_hci_last_rx_evt = env->data_buf[1];

              /* 待完成配对：Command Complete(0x0e) 与 Command Status(0x0f)
               * 都算"控制器活着并且理了这条命令"。参数布局不同：
               *   0x0e: [2]=plen [3]=ncmd [4..5]=opcode
               *   0x0f: [2]=plen [3]=status [4]=ncmd [5..6]=opcode
               */
              if (g_hci_pend_op != 0)
                {
                  if (g_hci_last_rx_evt == 0x0e && read_len >= 6 &&
                      ((uint16_t)env->data_buf[4] |
                       ((uint16_t)env->data_buf[5] << 8)) == g_hci_pend_op)
                    {
                      g_hci_pend_op = 0;
                    }
                  else if (g_hci_last_rx_evt == 0x0f && read_len >= 7 &&
                           ((uint16_t)env->data_buf[5] |
                            ((uint16_t)env->data_buf[6] << 8)) ==
                           g_hci_pend_op)
                    {
                      g_hci_pend_op = 0;
                    }
                }

              /* 事件级追踪：与 `hci tx` 配对，就能看出"哪条命令没有回条"。 */
              if (g_hci_last_rx_evt == 0x0e && read_len >= 6)
                {
                  syslog(LOG_INFO,
                         "hci rx evt=0x0e ncmd=%u op=0x%04x len=%lu\n",
                         (unsigned)env->data_buf[3],
                         (unsigned)((uint16_t)env->data_buf[4] |
                                    ((uint16_t)env->data_buf[5] << 8)),
                         (unsigned long)read_len);
                }
              else
                {
                  syslog(LOG_INFO, "hci rx evt=0x%02x len=%lu\n",
                         (unsigned)g_hci_last_rx_evt, (unsigned long)read_len);
                }
            }

          ret = env->notify_host(env->data_buf, read_len);
          sf32lb52_bt_note_host_rx(ret);
          if (ret < 0)
            {
              /* 不要在 HPWORK 上空转：会饿死 H4 RX / GNSS。 */
              usleep(SF32LB52_BT_RX_ENOMEM_BACKOFF_US);
              flags = enter_critical_section();
              if (!env->queue_open)
                {
                  env->rx_work_pending = false;
                  env->rx_worker_running = false;
                  leave_critical_section(flags);
                  return;
                }

              leave_critical_section(flags);
            }
        }

      flags = enter_critical_section();
      if (!env->rx_work_pending)
        {
          env->rx_worker_running = false;
          leave_critical_section(flags);
          break;
        }

      env->rx_work_pending = false;
      leave_critical_section(flags);
    }
}

static int32_t sf32lb52_bt_rx_ind(ipc_queue_handle_t handle, size_t size)
{
  struct sf32lb52_bt_env_s *env = &g_sf32lb52_bt_env;
  irqstate_t flags;
  bool queue_work;

  if (handle != env->ipc_port)
    {
      return -EINVAL;
    }

  if (!env->queue_open)
    {
      struct circular_buf *rx_ring = (struct circular_buf *)sf32lb52_bt_rx_buf_addr();
      #if SF32LB52_BT_TRACE
            syslog(LOG_INFO, "sf32lb52 rx_ind (pre-open) wr=%08lx rd=%08lx\n",
              (unsigned long)rx_ring->write_idx_mirror,
              (unsigned long)rx_ring->read_idx_mirror);
      #endif
      return OK;
    }

  flags = enter_critical_section();
  env->rx_work_pending = true;
  queue_work = !env->rx_worker_running && work_available(&env->rx_work);
  if (queue_work)
    {
      env->rx_worker_running = true;
    }
  leave_critical_section(flags);

#if SF32LB52_BT_TRACE
  {
    struct circular_buf *rx_ring =
        (struct circular_buf *)sf32lb52_bt_rx_buf_addr();

    up_invalidate_dcache((uintptr_t)sf32lb52_bt_rx_buf_addr(),
                         (uintptr_t)sf32lb52_bt_rx_buf_addr() +
                         sizeof(*rx_ring));

    syslog(LOG_INFO,
           "sf32lb52 rx_ind: size=%lu local=%08lx rd=%08lx wr=%08lx\n",
           (unsigned long)size,
           (unsigned long)env->rx_read_idx_mirror,
           (unsigned long)rx_ring->read_idx_mirror,
           (unsigned long)rx_ring->write_idx_mirror);
  }
#endif

  if (queue_work)
    {
      int ret;

      ret = work_queue(HPWORK, &env->rx_work, sf32lb52_bt_rx_worker,
                       env, 0);
      if (ret < 0)
        {
          flags = enter_critical_section();
          env->rx_worker_running = false;
          leave_critical_section(flags);

          syslog(LOG_ERR, "sf32lb52 bt queue rx work failed: %d\n", ret);
          return ret;
        }
    }

  return OK;
}

static int sf32lb52_bt_mailbox_init(void)
{
  struct sf32lb52_bt_env_s *env = &g_sf32lb52_bt_env;
  ipc_queue_cfg_t q_cfg;

  memset(&q_cfg, 0, sizeof(q_cfg));
  q_cfg.qid = SF32LB52_BT_QID;
  q_cfg.tx_buf_size = SF32LB52_BT_TX_BUF_SIZE;
  q_cfg.tx_buf_addr = SF32LB52_BT_TX_BUF_ADDR;
  q_cfg.tx_buf_addr_alias = SF32LB52_BT_TX_BUF_ALIAS;
  q_cfg.rx_buf_addr = sf32lb52_bt_rx_buf_addr();
  q_cfg.rx_ind = sf32lb52_bt_rx_ind;

  env->ipc_port = ipc_queue_init(&q_cfg);
  if (env->ipc_port == IPC_QUEUE_INVALID_HANDLE)
    {
      return -ENODEV;
    }

  syslog(LOG_INFO,
         "sf32lb52 bt tx ring addr=0x%08lx size=%u\n",
         (unsigned long)SF32LB52_BT_TX_BUF_ADDR,
         (unsigned)SF32LB52_BT_TX_BUF_SIZE);
  return OK;
}

int sf32lb52_bt_controller_init(void)
{
  int ret;

  if (g_sf32lb52_bt_status != SF32LB52_BT_STATUS_IDLE)
    {
      return OK;
    }

  memset(&g_sf32lb52_bt_env, 0, sizeof(g_sf32lb52_bt_env));
  ret = sf32lb52_bt_mailbox_init();
  if (ret < 0)
    {
      return ret;
    }

  g_sf32lb52_bt_status = SF32LB52_BT_STATUS_INITED;
  return OK;
}

int sf32lb52_bt_controller_deinit(void)
{
  int ret;

  if (g_sf32lb52_bt_status == SF32LB52_BT_STATUS_ENABLED)
    {
      ret = sf32lb52_bt_controller_disable();
      if (ret < 0)
        {
          return ret;
        }
    }

  if (g_sf32lb52_bt_status != SF32LB52_BT_STATUS_INITED)
    {
      return -EPERM;
    }

  g_sf32lb52_bt_env.queue_open = false;
  g_sf32lb52_bt_env.rx_work_pending = false;
  g_sf32lb52_bt_env.rx_worker_running = false;
  if (!work_available(&g_sf32lb52_bt_env.rx_work))
    {
      ret = work_cancel_sync(HPWORK, &g_sf32lb52_bt_env.rx_work);
      if (ret < 0 && ret != -ENOENT)
        {
          return ret;
        }
    }

  ret = ipc_queue_deinit(g_sf32lb52_bt_env.ipc_port);
  if (ret < 0)
    {
      return ret;
    }

  memset(&g_sf32lb52_bt_env, 0, sizeof(g_sf32lb52_bt_env));
  g_sf32lb52_bt_env.ipc_port = IPC_QUEUE_INVALID_HANDLE;
  g_sf32lb52_bt_status = SF32LB52_BT_STATUS_IDLE;
  return OK;
}

int sf32lb52_hci_register_callback(sf32lb52_bt_rx_callback_t callback)
{
  if (callback == NULL)
    {
      return -EINVAL;
    }

  if (g_sf32lb52_bt_status == SF32LB52_BT_STATUS_IDLE)
    {
      return -EPERM;
    }

  g_sf32lb52_bt_env.notify_host = callback;
  return OK;
}

int sf32lb52_bt_controller_enable(void)
{
  int ret;

  if (g_sf32lb52_bt_status == SF32LB52_BT_STATUS_ENABLED)
    {
      return OK;
    }

  if (g_sf32lb52_bt_status != SF32LB52_BT_STATUS_INITED)
    {
      return -EPERM;
    }

  sf32lb52_bt_prepare_stack_nvds();
  HAL_LCPU_ASSERT_INFO_clear();

  ret = lcpu_power_on();
  if (ret != 0)
    {
      return -EIO;
    }

  /* The L2H_MAILBOX (0x40002000) is on the LPSYS APB bus.  Its CxIER
   * register is only writable when the LPSYS bus bridge is active.
   * Wake the LCPU first so the bus bridge is powered, THEN open the
   * IPC queue (which writes CxIER to unmask the mailbox RX interrupt).
   * The SDK reference ipc_hw_enable_interrupt2() has the same
   * HAL_HPAON_WakeCore() before the UNMASK call.
   */

  HAL_HPAON_WakeCore(CORE_ID_LCPU);
  g_sf32lb52_bt_env.wake_held = true;

  usleep(500000);

  ret = ipc_queue_open(g_sf32lb52_bt_env.ipc_port);
  if (ret < 0)
    {
      HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
      g_sf32lb52_bt_env.wake_held = false;
      return ret;
    }

  /* Flush H2L TX ring to SRAM immediately after ipc_queue_open so
   * LCPU sees the reset indices (read_idx=write_idx=0) before it
   * tries to process any stale data from a previous session.
   */
  up_clean_dcache((uintptr_t)SF32LB52_BT_TX_BUF_ADDR,
                  (uintptr_t)SF32LB52_BT_TX_BUF_ADDR +
                  SF32LB52_BT_TX_BUF_SIZE);
  __DSB();

  /* After every lcpu_power_on(), LCPU resets its TX ring write pointer
   * (write_idx_mirror) to 0 and writes its own boot responses into the
   * ring.  HCPU's read pointer (read_idx_mirror) may still hold the
   * non-zero value from the previous session -- dirty in DCache or
   * stored in SRAM.  If we do not synchronise them:
   *
   *   circular_buf_data_len() = wrap(write_idx - read_idx)
   *
   * returns a large garbage value, and LCPU's fresh boot events
   * plus ring garbage all replay as "new" HCI responses.  These
   * ghost events are consumed by the host as replies to real commands,
   * so HCI_Reset (0x0c01) is never actually executed by the controller,
   * and bt_le_adv_start later gets "Command Disallowed" (0x07).
   *
   * Fix: flush DCache to write back any dirty read_idx and to load
   * LCPU's current write_idx from SRAM, then advance read_idx to
   * write_idx (discard ALL pending LCPU boot data), then clean the
   * DCache so LCPU sees the updated read pointer in SRAM.
   */

  {
    struct circular_buf *rx_ring =
        (struct circular_buf *)sf32lb52_bt_rx_buf_addr();

    ret = sf32lb52_bt_wait_rx_ring_ready();
    if (ret < 0)
      {
        syslog(LOG_ERR, "sf32lb52 rx ring not ready: %d\n", ret);
        ipc_queue_close(g_sf32lb52_bt_env.ipc_port);
        HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
        g_sf32lb52_bt_env.wake_held = false;
        return ret;
      }

    rx_ring->read_idx_mirror = rx_ring->write_idx_mirror;
    g_sf32lb52_bt_env.rx_read_idx_mirror = rx_ring->write_idx_mirror;

    up_clean_dcache((uintptr_t)sf32lb52_bt_rx_buf_addr(),
                    (uintptr_t)sf32lb52_bt_rx_buf_addr() +
                    sizeof(*rx_ring));
    __DSB();
  }

  {
    volatile MAILBOX_CH_TypeDef *l2h = L2H_MAILBOX;
    uint32_t nvic_bit = (1UL << (LCPU2HCPU_IRQn & 0x1F));
    uint32_t nvic_en  = NVIC->ISER[LCPU2HCPU_IRQn >> 5];

    (void)l2h;
    (void)nvic_bit;
    (void)nvic_en;
  }

  g_sf32lb52_bt_env.queue_open = true;
  g_sf32lb52_bt_status = SF32LB52_BT_STATUS_ENABLED;

  return OK;
}

int sf32lb52_bt_controller_disable(void)
{
  int ret = OK;
  int tmpret;
  bool queue_open;

  if (g_sf32lb52_bt_status != SF32LB52_BT_STATUS_ENABLED)
    {
      return -EPERM;
    }

  queue_open = g_sf32lb52_bt_env.queue_open;
  g_sf32lb52_bt_env.queue_open = false;
  g_sf32lb52_bt_env.rx_work_pending = false;
  g_sf32lb52_bt_env.rx_worker_running = false;
  if (!work_available(&g_sf32lb52_bt_env.rx_work))
    {
      uint32_t t0 = sf32lb52_bt_now_ms();

      while (!work_available(&g_sf32lb52_bt_env.rx_work) &&
             (uint32_t)(sf32lb52_bt_now_ms() - t0) < 250u)
        {
          usleep(2000);
        }

      if (!work_available(&g_sf32lb52_bt_env.rx_work))
        {
          syslog(LOG_WARNING, "sf32lb52 bt rx worker still busy, async cancel\n");
          (void)work_cancel(HPWORK, &g_sf32lb52_bt_env.rx_work);
        }
    }

  if (queue_open)
    {
      tmpret = ipc_queue_close(g_sf32lb52_bt_env.ipc_port);
      if (tmpret < 0 && ret == OK)
        {
          ret = tmpret;
        }
    }

  if (g_sf32lb52_bt_env.wake_held)
    {
      HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
      g_sf32lb52_bt_env.wake_held = false;
    }

  tmpret = lcpu_power_off();
  if (tmpret != 0 && ret == OK)
    {
      ret = -EIO;
    }

  g_sf32lb52_bt_env.notify_host = NULL;
  g_sf32lb52_bt_status = SF32LB52_BT_STATUS_INITED;
  return ret;
}

int sf32lb52_bt_controller_force_reset(void)
{
  sf32lb52_bt_rx_callback_t cb;
  int ret = OK;
  int en = -EIO;
  bool keep_skip;
  bool tried_enable;

  cb = g_sf32lb52_bt_env.notify_host;
  keep_skip = g_hci_skip_sync;
  syslog(LOG_ERR, "sf32lb52 bt force reset (LCPU)\n");
  BLE_LOG("force_reset begin keep_skip=%d status=%d open=%d cb=%d",
          keep_skip ? 1 : 0,
          (int)g_sf32lb52_bt_status,
          g_sf32lb52_bt_env.queue_open ? 1 : 0,
          cb != NULL ? 1 : 0);
  sf32lb52_bt_hci_skip_sync_set(true);
  sf32lb52_bt_hci_rx_stall_clear();

  if (g_sf32lb52_bt_status == SF32LB52_BT_STATUS_ENABLED)
    {
      ret = sf32lb52_bt_controller_disable();
    }

  usleep(20000);

  /* 记下 enable 到底有没有被调用：下面"跳过 enable"那一栏靠它区分
   * 「enable 失败」和「根本没人可 enable」。 */
  tried_enable = (g_sf32lb52_bt_status == SF32LB52_BT_STATUS_INITED);

  if (tried_enable)
    {
      en = sf32lb52_bt_controller_enable();

      if (en < 0 && ret == OK)
        {
          ret = en;
        }
    }

  /** skip_sync 必须**无条件**还回去（除非调用方明确要求保持）。
   *
   *  旧写法是 `if (en >= 0 && !keep_skip)`，于是"enable 压根没被调用"
   *  这条路 —— `status` 既不是 ENABLED 也不是 INITED（例如已被
   *  `sf32lb52_bt_controller_deinit()` 降到 IDLE），`en` 一直是初值 -EIO ——
   *  就会把 skip 永远留着。
   *
   *  **skip=1 的后果是致命的**：`sf32lb52_host_send_packet()` 对一切 HCI
   *  返回 -ENODEV，上层 adapter 永远到不了 ON，之后 respawn 出来的
   *  companion 停在 `adapter not ready, state=0` /
   *  `bt_gatts_register_service failed: 2`，再也没有人去清它（判据就是
   *  "LCPU 复位成功也不要清 skip" 那条注释所依赖的前提 —— 那前提只在
   *  enable 真的跑过时成立）。
   *
   *  挡住误发的其实是 `host_send_packet()` 里的 `status != ENABLED ||
   *  !queue_open` 两道判据，不需要 skip 来兜底：这里清掉，复位失败也只是
   *  回到"发不出去"，不会是"永远发不出去"。
   */
  if (!keep_skip)
    {
      sf32lb52_bt_hci_skip_sync_set(false);
    }

  if (!tried_enable)
    {
      syslog(LOG_ERR,
             "sf32lb52 bt force reset: status=%d (not INITED), enable skipped%s\n",
             (int)g_sf32lb52_bt_status,
             keep_skip ? " (keep skip)" : "");
    }
  else if (en < 0)
    {
      syslog(LOG_ERR, "sf32lb52 bt force reset: enable failed %d\n", en);
    }

  if (cb != NULL)
    {
      g_sf32lb52_bt_env.notify_host = cb;
    }

  BLE_LOG("force_reset done keep_skip=%d tried_enable=%d enable=%d ret=%d "
          "skip=%d status=%d",
          keep_skip ? 1 : 0, tried_enable ? 1 : 0, en, ret,
          g_hci_skip_sync ? 1 : 0, (int)g_sf32lb52_bt_status);
  return ret;
}

int sf32lb52_host_send_packet(const uint8_t *data, uint16_t len)
{
  struct circular_buf *tx_ring =
      (struct circular_buf *)SF32LB52_BT_TX_BUF_ADDR;
  uint32_t start_time;
  uint32_t tick_count;
  size_t written;
  size_t remaining;
  size_t offset;
  size_t chunk;
  int ret;

  if (data == NULL || len == 0)
    {
      return -EINVAL;
    }

  if (g_sf32lb52_bt_status != SF32LB52_BT_STATUS_ENABLED ||
      !g_sf32lb52_bt_env.queue_open ||
      g_hci_skip_sync)
    {
#ifdef CONFIG_MYVENDOR_BLE_LOG
      uint32_t now = sf32lb52_bt_now_ms();

      if (g_send_skip_log_ms == 0 ||
          (uint32_t)(now - g_send_skip_log_ms) >= 1000u)
        {
          g_send_skip_log_ms = now;
          BLE_LOG("host_send skip status=%d open=%d skip_sync=%d len=%u",
                  (int)g_sf32lb52_bt_status,
                  g_sf32lb52_bt_env.queue_open ? 1 : 0,
                  g_hci_skip_sync ? 1 : 0,
                  (unsigned)len);
        }
#endif
      return -ENODEV;
    }

  if (data[0] == SF32LB52_BT_H4_CMD && len >= 3)
    {
      g_hci_last_tx_op = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
      g_hci_last_tx_ms = sf32lb52_bt_now_ms();

      /* 待完成必须在这里置位（**入环时刻**），不能等下面 wait_tx_idle 确认
       * 之后再置：那是 usleep(1000) 轮询 LCPU 取走字节，控制器回 Command
       * Complete 往往比它**早**（实机 2026-09-18：tx 18.132 / rx 18.140，
       * 而我们 18.14x 才盖章），于是"置位"发生在"清零"之后，配对永远配不上，
       * 一条已被回答的命令被当成"发了没人应"。 */
      if (g_hci_last_tx_op != 0)
        {
          g_hci_pend_op = g_hci_last_tx_op;
          g_hci_pend_ms = g_hci_last_tx_ms;
        }

      /* 命令级追踪：排查"发出去没人应"时，必须知道**是哪一条**命令、
       * 参数是什么（同一个 opcode 的 enable=0/1 行为完全不同）。 */
      syslog(LOG_INFO, "hci tx op=0x%04x plen=%u p=%02x%02x%02x%02x len=%u\n",
             (unsigned)g_hci_last_tx_op,
             len >= 4 ? (unsigned)data[3] : 0u,
             len > 4 ? (unsigned)data[4] : 0u,
             len > 5 ? (unsigned)data[5] : 0u,
             len > 6 ? (unsigned)data[6] : 0u,
             len > 7 ? (unsigned)data[7] : 0u,
             (unsigned)len);
    }

  offset = 0;
  remaining = len;
  start_time = HAL_GetTick();
  tick_count = 0;

  ret = sf32lb52_bt_wait_tx_idle(tx_ring);
  if (ret < 0)
    {
      return ret;
    }

  while (remaining > 0)
    {
      irqstate_t flags;

      chunk = sf32lb52_bt_tx_chunk_len(data, len, offset);

      up_invalidate_dcache((uintptr_t)SF32LB52_BT_TX_BUF_ADDR,
                           (uintptr_t)SF32LB52_BT_TX_BUF_ADDR +
                           sizeof(*tx_ring));

      flags = enter_critical_section();
      written = sf32lb52_bt_ring_write(tx_ring, data + offset, chunk);
      leave_critical_section(flags);

      if (written == 0)
        {
          if (HAL_GetTick() != start_time)
            {
              tick_count++;
              start_time = HAL_GetTick();
            }

          if (tick_count >= 10)
            {
              syslog(LOG_ERR,
                     "sf32lb52 bt tx timeout: remaining=%lu\n",
                     (unsigned long)remaining);
              return -ETIMEDOUT;
            }

          continue;
        }

      offset += written;
      remaining -= written;

      __DSB();
      sf32lb52_bt_trigger_tx();
    }

#if SF32LB52_BT_TRACE
  sf32lb52_bt_log_tx_state(tx_ring, "queued", tx_ring->write_idx_mirror);
#endif

  if (data[0] == SF32LB52_BT_H4_CMD)
    {
      ret = sf32lb52_bt_wait_tx_idle(tx_ring);
      if (ret < 0)
        {
          return ret;
        }

      /* 命令确实进了环（wait_tx_idle 确认被 LCPU 取走）才盖章。
       * 只用于 `oktxage` 展示；"控制器是否哑"的判据是上面的 pendop 配对
       * （时间戳盖章会输给控制器的回包速度，见那里的注释）。 */
      g_hci_last_ok_tx_ms = sf32lb52_bt_now_ms();
    }

  return OK;
}
