/****************************************************************************
 * vendor/HelmOne/boards/sf32lb52/helmone/ble_test_app/ble_test_app.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal BLE peripheral test:
 *   1. Enable Bluetooth adapter (local Framework)
 *   2. Register a private GATT service (0xFF00)
 *   3. Start legacy advertising as "Vela-BT"
 *
 * No SMP pairing is required for read/write on 0xFF05 / 0xFF02.
 * Test with nRF Connect: scan, connect, read 0xFF05, write 0xFF02.
 *
 * Usage (NSH):
 *   ble_test          # run in foreground
 *   ble_test &        # run in background
 *
 * Stop: kill <pid>  (or reboot)
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bluetooth.h"
#include "bt_adapter.h"
#include "bt_device.h"
#include "bt_gatts.h"
#include "bt_le_advertiser.h"

#define LOGI(fmt, ...)  printf("ble_test: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...)  printf("ble_test: ERROR " fmt "\n", ##__VA_ARGS__)

#define ADAPTER_WAIT_SECS       60
#define HCI_DEV_PATH            "/dev/ttyHCI0"
#define MAIN_LOOP_SLEEP_SECS    5

/* Private IOT service (same UUIDs as bttool gatts service 3). */

enum
{
  IOT_SERVICE_ID = 1,
  IOT_RX_CHR_ID,
  IOT_READ_CHR_ID,
};

static uint8_t g_read_char_value[] =
{
  'H', 'e', 'l', 'l', 'o', ' ', 'V', 'E', 'L', 'A', '!'
};

static uint8_t g_rw_char_value[64];
static uint16_t g_rw_char_len;

static bt_instance_t *g_ins;
static void *g_adapter_cookie;
static gatts_handle_t g_gatts_handle;
static bt_advertiser_t *g_adv_handle;

static volatile bool g_running = true;
static volatile bt_adapter_state_t g_adapter_state = BT_ADAPTER_STATE_OFF;

/* advtest 用：框架回调在 service loop 线程，测试任务轮询这两个标志。
 * 只要"有/无 + status"，不需要队列。 */
static volatile bool g_adv_start_done;
static volatile bool g_adv_stop_done;
static volatile uint8_t g_adv_start_status;

/* advtest 与 Companion 共存：适配器是 Companion 开的，测试结束不能替它关。 */
static bool g_advtest_mode;

/* Legacy adv: LE only + manufacturer 0x038F; scan rsp: name "Vela-BT". */

static uint8_t g_adv_data[] =
{
  0x02, 0x01, 0x06, 0x03, 0xff, 0x8f, 0x03
};

static uint8_t g_scan_rsp[] =
{
  0x08, 0x09, 'V', 'e', 'l', 'a', '-', 'B', 'T'
};

static uint16_t rx_char_on_read(gatts_handle_t srv_handle, bt_address_t *addr,
                                uint16_t attr_handle, uint32_t req_handle)
{
  char addr_str[BT_ADDR_STR_LENGTH];

  bt_addr_ba2str(addr, addr_str);
  LOGI("read 0xFF02 from %s", addr_str);

  bt_gatts_response(srv_handle, addr, req_handle, g_rw_char_value, g_rw_char_len);
  return 0;
}

static uint16_t rx_char_on_write(gatts_handle_t srv_handle, bt_address_t *addr,
                                 uint16_t attr_handle, const uint8_t *value,
                                 uint16_t length, uint16_t offset)
{
  char addr_str[BT_ADDR_STR_LENGTH];

  if (offset + length > sizeof(g_rw_char_value))
    {
      return 0;
    }

  if (offset == 0)
    {
      memset(g_rw_char_value, 0, sizeof(g_rw_char_value));
    }

  memcpy(g_rw_char_value + offset, value, length);
  g_rw_char_len = offset + length;

  bt_addr_ba2str(addr, addr_str);
  LOGI("write 0xFF02 from %s, len=%u", addr_str, length);
  return length;
}

static gatt_attr_db_t g_iot_attr_db[] =
{
  GATT_H_PRIMARY_SERVICE(BT_UUID_DECLARE_16(0xff00), IOT_SERVICE_ID),
  GATT_H_CHARACTERISTIC_USER_RSP(BT_UUID_DECLARE_16(0xff02),
                               GATT_PROP_READ | GATT_PROP_WRITE_NR | GATT_PROP_WRITE,
                               GATT_PERM_READ | GATT_PERM_WRITE,
                               rx_char_on_read, rx_char_on_write, IOT_RX_CHR_ID),
  GATT_H_CHARACTERISTIC_AUTO_RSP(BT_UUID_DECLARE_16(0xff05),
                                 GATT_PROP_READ, GATT_PERM_READ,
                                 g_read_char_value, sizeof(g_read_char_value),
                                 IOT_READ_CHR_ID),
};

static gatt_srv_db_t g_iot_service_db =
{
  .attr_db = g_iot_attr_db,
  .attr_num = sizeof(g_iot_attr_db) / sizeof(gatt_attr_db_t),
};

static void on_adapter_state_changed(void *cookie, bt_adapter_state_t state)
{
  g_adapter_state = state;
  LOGI("adapter state -> %d", (int)state);
}

static void on_connection_state_changed(void *cookie, bt_address_t *addr,
                                         bt_transport_t transport,
                                         connection_state_t state)
{
  char addr_str[BT_ADDR_STR_LENGTH];

  bt_addr_ba2str(addr, addr_str);
  LOGI("connection %s transport=%d state=%d", addr_str, (int)transport,
       (int)state);
}

static const adapter_callbacks_t g_adapter_cbs =
{
  .on_adapter_state_changed = on_adapter_state_changed,
  .on_connection_state_changed = on_connection_state_changed,
};

static void gatts_connect_callback(gatts_handle_t srv_handle, bt_address_t *addr)
{
  char addr_str[BT_ADDR_STR_LENGTH];

  bt_addr_ba2str(addr, addr_str);
  LOGI("GATT connected: %s", addr_str);
}

static void gatts_disconnect_callback(gatts_handle_t srv_handle,
                                      bt_address_t *addr)
{
  char addr_str[BT_ADDR_STR_LENGTH];

  bt_addr_ba2str(addr, addr_str);
  LOGI("GATT disconnected: %s", addr_str);
}

static void gatts_attr_table_added(gatts_handle_t srv_handle,
                                   gatt_status_t status, uint16_t attr_handle)
{
  LOGI("GATT table added handle=0x%04x status=%d", attr_handle, (int)status);
}

static const gatts_callbacks_t g_gatts_cbs =
{
  sizeof(g_gatts_cbs),
  gatts_connect_callback,
  gatts_disconnect_callback,
  gatts_attr_table_added,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
};

static void on_advertising_start(bt_advertiser_t *adv, uint8_t adv_id,
                                 uint8_t status)
{
  g_adv_start_status = status;
  g_adv_start_done = true;
  LOGI("advertising started id=%u status=%u handle=%p", adv_id, status, adv);
}

static void on_advertising_stopped(bt_advertiser_t *adv, uint8_t adv_id)
{
  g_adv_stop_done = true;
  if (adv == g_adv_handle)
    {
      /* 框架在 on_advertising_stopped 之后会 destroy_advertiser()；
       * 这个指针不能再拿去 stop。 */
      g_adv_handle = NULL;
    }
  LOGI("advertising stopped id=%u handle=%p", adv_id, adv);
}

static const advertiser_callback_t g_adv_cbs =
{
  sizeof(g_adv_cbs),
  on_advertising_start,
  on_advertising_stopped,
};

static int wait_hci_device(int timeout_secs)
{
  int i;

  for (i = 0; i < timeout_secs * 10; i++)
    {
      if (access(HCI_DEV_PATH, F_OK) == 0)
        {
          LOGI("HCI device %s OK", HCI_DEV_PATH);
          return 0;
        }

      usleep(100 * 1000);
    }

  LOGE("%s missing — enable CONFIG_UART_BTH4 and rebuild", HCI_DEV_PATH);
  return -ENOENT;
}

static int wait_adapter_ready(int timeout_secs)
{
  int i;
  bt_adapter_state_t last = BT_ADAPTER_STATE_OFF;

  for (i = 0; i < timeout_secs * 10; i++)
    {
      bt_adapter_state_t state = bt_adapter_get_state(g_ins);

      if (state != last)
        {
          LOGI("adapter state -> %d", (int)state);
          last = state;
        }

      /* 纯 BLE（没有 BREDR）配置停在 BLE_ON，到不了 Classic ON。
       * 与 ble_companion 的 adapter_is_usable() 保持同一判据。 */
      if (state == BT_ADAPTER_STATE_ON ||
          state == BT_ADAPTER_STATE_BLE_ON)
        {
          return 0;
        }

      usleep(100 * 1000);
    }

  LOGE("adapter not ready, state=%d (expect ON or BLE_ON)",
       (int)bt_adapter_get_state(g_ins));
  return -ETIMEDOUT;
}

static int ble_start_peripheral(void)
{
  ble_adv_params_t params;
  bt_status_t ret;
  char addr_str[BT_ADDR_STR_LENGTH];
  bt_address_t le_addr;
  ble_addr_type_t le_addr_type;

  if (wait_hci_device(10) != 0)
    {
      return -1;
    }

  ret = bt_adapter_enable(g_ins);
  LOGI("bt_adapter_enable -> %d", (int)ret);
  if (ret != BT_STATUS_SUCCESS && ret != BT_STATUS_DONE)
    {
      LOGE("bt_adapter_enable failed: %d", (int)ret);
      return -1;
    }

  if (wait_adapter_ready(ADAPTER_WAIT_SECS) != 0)
    {
      return -1;
    }

  bt_device_set_bondable_le(g_ins, false);

  ret = bt_gatts_register_service(g_ins, &g_gatts_handle, (gatts_callbacks_t *)&g_gatts_cbs);
  if (ret != BT_STATUS_SUCCESS || g_gatts_handle == NULL)
    {
      LOGE("bt_gatts_register_service failed: %d", (int)ret);
      return -1;
    }

  ret = bt_gatts_add_attr_table(g_gatts_handle, &g_iot_service_db);
  if (ret != BT_STATUS_SUCCESS)
    {
      LOGE("bt_gatts_add_attr_table failed: %d", (int)ret);
      return -1;
    }

  memset(&params, 0, sizeof(params));
  params.adv_type = BT_LE_LEGACY_ADV_IND;
  params.peer_addr_type = BT_LE_ADDR_TYPE_PUBLIC;
  params.own_addr_type = BT_LE_ADDR_TYPE_PUBLIC;
  params.interval = 320;
  params.tx_power = 0;
  params.channel_map = BT_LE_ADV_CHANNEL_DEFAULT;
  params.filter_policy = BT_LE_ADV_FILTER_WHITE_LIST_FOR_NONE;

  g_adv_handle = bt_le_start_advertising(g_ins, &params,
                                         g_adv_data, sizeof(g_adv_data),
                                         g_scan_rsp, sizeof(g_scan_rsp),
                                         (advertiser_callback_t *)&g_adv_cbs);
  if (g_adv_handle == NULL)
    {
      LOGE("bt_le_start_advertising failed");
      return -1;
    }

  if (bt_adapter_get_le_address(g_ins, &le_addr, &le_addr_type) == BT_STATUS_SUCCESS)
    {
      bt_addr_ba2str(&le_addr, addr_str);
      LOGI("LE address: %s", addr_str);
    }

  LOGI("ready — scan for \"Vela-BT\" (nRF Connect)");
  LOGI("  service 0xFF00, read 0xFF05, write 0xFF02");
  return 0;
}

static void ble_cleanup(void)
{
  if (g_advtest_mode)
    {
      /* advtest 与 Companion 共存：只停掉本测试的广播，**不销毁 instance /
       * 不注销 adapter 回调**。实测在 Companion 仍在用同一套框架对象时
       * bluetooth_delete_instance() 会触发 irq=1 memfault（测试任务退出那
       * 一刻）。测试固件一次性使用，这点泄漏留给重启即可。 */
      if (g_adv_handle != NULL && g_ins != NULL)
        {
          bt_le_stop_advertising(g_ins, g_adv_handle);
          g_adv_handle = NULL;
        }

      return;
    }

  if (g_adv_handle != NULL && g_ins != NULL)
    {
      bt_le_stop_advertising(g_ins, g_adv_handle);
      g_adv_handle = NULL;
    }

  if (g_gatts_handle != NULL)
    {
      bt_gatts_remove_attr_table(g_gatts_handle, IOT_SERVICE_ID);
      bt_gatts_unregister_service(g_gatts_handle);
      g_gatts_handle = NULL;
    }

  if (g_ins != NULL && !g_advtest_mode)
    {
      bt_adapter_disable(g_ins);
    }

  if (g_adapter_cookie != NULL && g_ins != NULL)
    {
      bt_adapter_unregister_callback(g_ins, g_adapter_cookie);
      g_adapter_cookie = NULL;
    }

  if (g_ins != NULL)
    {
      bluetooth_delete_instance(g_ins);
      g_ins = NULL;
    }
}

static void signal_handler(int signo)
{
  (void)signo;
  g_running = false;
}

/* ---- 定向广播矩阵（CONFIG_MYVENDOR_BLE_TEST_APP 构建专用）----------------
 *
 * 入口在 SAL 里的旋钮：framework 的 ble_adv_params_t 没有 RPA / duty 字段，
 * 所以只有这两个维度需要旁路，其余（peer type、地址、own type、interval）
 * 全部走真实的 framework -> SAL -> zblue -> HCI 路径。
 *
 * 用法（NSH）：
 *   ble_test advtest ind [repeat]
 *   ble_test advtest dir <peer_type> <mac> <rpa 0|1> <low|high> <interval> <repeat> [own_type] [hold_ms]
 *
 * 例：
 *   ble_test advtest dir 0 78D8404BF520 0 low 320 1       # B1
 *   ble_test advtest dir 1 4A1B2C3D4E5F 0 low 320 1       # B2
 *   ble_test advtest dir 0 78D8404BF520 1 low 320 1       # B3（预期 -EINVAL）
 *   ble_test advtest dir 1 4A1B2C3D4E5F 1 low 320 1 1     # B3b（own=RPA）
 *   ble_test advtest dir 0 78D8404BF520 0 high 320 1      # B4
 *   ble_test advtest dir 0 78D8404BF520 0 low 320 10      # B5
 */
extern void myvendor_sal_advtest_set(int rpa_opt, int low_duty);

static uint32_t advtest_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static int hex_nibble(char c)
{
  if (c >= '0' && c <= '9')
    {
      return c - '0';
    }

  if (c >= 'a' && c <= 'f')
    {
      return c - 'a' + 10;
    }

  if (c >= 'A' && c <= 'F')
    {
      return c - 'A' + 10;
    }

  return -1;
}

/** 显示顺序（78:D8:40:4B:F5:20 或 78D8404BF520）→ 框架的最低位在前。 */
static bool advtest_parse_mac(const char *s, bt_address_t *out)
{
  uint8_t bytes[6];
  int n = 0;

  while (*s != '\0' && n < 6)
    {
      int hi;
      int lo;

      if (*s == ':' || *s == '-')
        {
          s++;
          continue;
        }

      hi = hex_nibble(s[0]);
      lo = (s[1] != '\0') ? hex_nibble(s[1]) : -1;
      if (hi < 0 || lo < 0)
        {
          return false;
        }

      bytes[n++] = (uint8_t)((hi << 4) | lo);
      s += 2;
    }

  if (n != 6 || *s != '\0')
    {
      return false;
    }

  for (n = 0; n < 6; n++)
    {
      out->addr[n] = bytes[5 - n];
    }

  return true;
}

static const char *advtest_status_name(uint8_t s)
{
  switch (s)
    {
      case 0:
        return "SUCCESS";
      case 1:
        return "NOMEM";
      case 2:
        return "START_TIMEOUT";
      case 3:
        return "STACK_ERR";
      default:
        return "UNKNOWN";
    }
}

static int ble_advtest(int argc, FAR char *argv[])
{
  ble_adv_params_t params;
  bt_status_t ret;
  bt_address_t peer;
  char peer_str[BT_ADDR_STR_LENGTH];
  bool directed;
  uint8_t peer_type = BT_LE_ADDR_TYPE_PUBLIC;
  uint8_t own_type = BT_LE_ADDR_TYPE_PUBLIC;
  int rpa_opt = -1;
  int low_duty = 1;
  int interval = 320;
  int repeat = 1;
  int hold_ms = 0;
  int n_started = 0;
  int n_failed = 0;
  int n_timeout = 0;
  int i;

  g_advtest_mode = true;

  if (argc < 1)
    {
      LOGE("usage: ble_test advtest ind [repeat]");
      LOGE("       ble_test advtest dir <peer_type> <mac> <rpa> <low|high> <interval> <repeat> [own_type]");
      return -EINVAL;
    }

  memset(&peer, 0, sizeof(peer));

  if (strcmp(argv[0], "ind") == 0)
    {
      directed = false;
      if (argc >= 2)
        {
          repeat = atoi(argv[1]);
        }
    }
  else if (strcmp(argv[0], "dir") == 0)
    {
      directed = true;
      if (argc < 7)
        {
          LOGE("dir needs: <peer_type> <mac> <rpa> <low|high> <interval> <repeat> [own_type]");
          return -EINVAL;
        }

      peer_type = (uint8_t)atoi(argv[1]);
      if (!advtest_parse_mac(argv[2], &peer))
        {
          LOGE("bad mac '%s'", argv[2]);
          return -EINVAL;
        }

      rpa_opt = atoi(argv[3]);
      low_duty = (strcmp(argv[4], "high") == 0) ? 0 : 1;
      interval = atoi(argv[5]);
      repeat = atoi(argv[6]);
      if (argc >= 8)
        {
          own_type = (uint8_t)atoi(argv[7]);
        }

      if (argc >= 9)
        {
          hold_ms = atoi(argv[8]);
          if (hold_ms < 0)
            {
              hold_ms = 0;
            }
          else if (hold_ms > 30000)
            {
              hold_ms = 30000;
            }
        }
    }
  else
    {
      LOGE("unknown advtest mode '%s' (want ind|dir)", argv[0]);
      return -EINVAL;
    }

  if (repeat < 1)
    {
      repeat = 1;
    }
  else if (repeat > 100)
    {
      repeat = 100;
    }

  if (interval < 0x20 || interval > 0x4000)
    {
      LOGE("interval %d out of HCI range 0x20..0x4000", interval);
      return -EINVAL;
    }

  if (wait_hci_device(10) != 0)
    {
      return -1;
    }

  ret = bt_adapter_enable(g_ins);
  LOGI("bt_adapter_enable -> %d", (int)ret);
  if (ret != BT_STATUS_SUCCESS && ret != BT_STATUS_DONE)
    {
      LOGE("bt_adapter_enable failed: %d", (int)ret);
      return -1;
    }

  if (wait_adapter_ready(ADAPTER_WAIT_SECS) != 0)
    {
      return -1;
    }

  bt_addr_ba2str(&peer, peer_str);
  LOGI("ADVTEST begin mode=%s peer_type=%u peer=%s rpa=%d duty=%s "
       "interval=%d repeat=%d own=%u",
       directed ? "dir" : "ind", (unsigned)peer_type, peer_str, rpa_opt,
       low_duty ? "low" : "high", interval, repeat, (unsigned)own_type);

  for (i = 0; i < repeat; i++)
    {
      uint32_t elapsed;
      uint32_t t0;
      int j;
      bool started;

      memset(&params, 0, sizeof(params));
      params.adv_type = directed ? BT_LE_LEGACY_ADV_DIRECT_IND
                                 : BT_LE_LEGACY_ADV_IND;
      params.peer_addr_type = peer_type;
      params.own_addr_type = own_type;
      params.interval = (uint32_t)interval;
      params.tx_power = 0;
      params.channel_map = BT_LE_ADV_CHANNEL_DEFAULT;
      params.filter_policy = BT_LE_ADV_FILTER_WHITE_LIST_FOR_NONE;
      if (directed)
        {
          memcpy(&params.peer_addr, &peer, sizeof(peer));
        }

      myvendor_sal_advtest_set(rpa_opt, low_duty);

      g_adv_start_done = false;
      g_adv_stop_done = false;
      g_adv_start_status = 0xff;

      t0 = advtest_now_ms();
      g_adv_handle = bt_le_start_advertising(g_ins, &params,
                                             directed ? NULL : g_adv_data,
                                             directed ? 0 : sizeof(g_adv_data),
                                             directed ? NULL : g_scan_rsp,
                                             directed ? 0 : sizeof(g_scan_rsp),
                                             (advertiser_callback_t *)&g_adv_cbs);
      if (g_adv_handle == NULL)
        {
          n_failed++;
          LOGI("ADVTEST n=%d api=NULL (refused before SAL)", i + 1);
          continue;
        }

      for (j = 0; j < 250; j++)
        {
          if (g_adv_start_done || g_adv_stop_done)
            {
              break;
            }

          usleep(10 * 1000);
        }

      elapsed = advtest_now_ms() - t0;
      started = (g_adv_start_done && g_adv_start_status == BT_ADV_STATUS_SUCCESS);

      if (started)
        {
          n_started++;
          LOGI("ADVTEST n=%d start_cb=SUCCESS elapsed=%u ms", i + 1,
               (unsigned)elapsed);
        }
      else if (g_adv_start_done)
        {
          n_failed++;
          LOGI("ADVTEST n=%d start_cb=%s(%u) elapsed=%u ms", i + 1,
               advtest_status_name(g_adv_start_status),
               (unsigned)g_adv_start_status, (unsigned)elapsed);
        }
      else if (g_adv_stop_done)
        {
          /* 当前 SAL 在 bt_le_adv_start 失败时发 STOPPED，框架不会回调
           * on_advertising_start；errno 在上一行 [adver] 日志里。 */
          n_failed++;
          LOGI("ADVTEST n=%d stopped-before-start elapsed=%u ms "
               "(see [adver] err: above)", i + 1, (unsigned)elapsed);
        }
      else
        {
          n_timeout++;
          LOGI("ADVTEST n=%d TIMEOUT after %u ms (no start/stop callback)",
               i + 1, (unsigned)elapsed);
        }

      if (started && g_adv_handle != NULL)
        {
          uint32_t t1;

          if (hold_ms > 0)
            {
              LOGI("ADVTEST n=%d holding %d ms (keep advertising)", i + 1,
                   hold_ms);
              usleep((useconds_t)hold_ms * 1000);
            }

          t1 = advtest_now_ms();
          bt_le_stop_advertising(g_ins, g_adv_handle);
          for (j = 0; j < 100; j++)
            {
              if (g_adv_stop_done)
                {
                  break;
                }

              usleep(10 * 1000);
            }

          LOGI("ADVTEST n=%d stop_cb=%s elapsed=%u ms", i + 1,
               g_adv_stop_done ? "OK" : "TIMEOUT",
               (unsigned)(advtest_now_ms() - t1));
        }
      else
        {
          /* START_TIMEOUT 时框架已经 destroy_advertiser()；句柄是野的。 */
          g_adv_handle = NULL;
        }

      usleep(200 * 1000);
    }

  LOGI("ADVTEST summary total=%d started=%d failed=%d timeout=%d",
       repeat, n_started, n_failed, n_timeout);
  return (n_failed == 0 && n_timeout == 0) ? 0 : -1;
}

int main(int argc, FAR char *argv[])
{
  int rc = EXIT_SUCCESS;

  /* 故障注入（仅 nsh-btadv 测试构建；不创建 BT instance）。 */
  if (argc >= 2 && strcmp(argv[1], "advfault") == 0)
    {
      const char *mode = argc >= 3 ? argv[2] : "off";
      extern void myvendor_adv_fault_set(int mode);
      extern void myvendor_sal_advfault_set(int mode);
      int m = 0;

      if (strcmp(mode, "null") == 0)
        {
          m = 1;
        }
      else if (strcmp(mode, "async") == 0)
        {
          m = 2;
        }
      else if (strcmp(mode, "expire") == 0)
        {
          m = 3;
        }

      myvendor_adv_fault_set(m);
      myvendor_sal_advfault_set(m == 2 ? 1 : 0);
      LOGI("advfault mode=%s (null=1 async=2 expire=3)", mode);
      return EXIT_SUCCESS;
    }

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  memcpy(g_rw_char_value, g_read_char_value, sizeof(g_read_char_value));
  g_rw_char_len = sizeof(g_read_char_value);

  g_ins = bluetooth_create_instance();
  if (g_ins == NULL)
    {
      LOGE("bluetooth_create_instance failed");
      return EXIT_FAILURE;
    }

  g_adapter_cookie = bt_adapter_register_callback(g_ins, &g_adapter_cbs);
  if (g_adapter_cookie == NULL)
    {
      LOGE("bt_adapter_register_callback failed");
      ble_cleanup();
      return EXIT_FAILURE;
    }

  if (argc >= 2 && strcmp(argv[1], "advtest") == 0)
    {
      rc = (ble_advtest(argc - 2, argv + 2) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
      ble_cleanup();
      return rc;
    }

  if (ble_start_peripheral() != 0)
    {
      ble_cleanup();
      return EXIT_FAILURE;
    }

  while (g_running)
    {
      sleep(MAIN_LOOP_SLEEP_SECS);
    }

  LOGI("shutting down...");
  ble_cleanup();
  LOGI("done");
  return EXIT_SUCCESS;
}
