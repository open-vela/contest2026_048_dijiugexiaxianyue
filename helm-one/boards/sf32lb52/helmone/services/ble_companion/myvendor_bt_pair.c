/**
 * @file myvendor_bt_pair.c
 * @brief BLE 配对（bonding）测试入口：`ctl pair …`
 *
 * 目的：把"开放配对 / 同意配对 / 主动发起配对 / 查看绑定"做成可以在串口上手动操作的
 * 命令，用来验证 IRK/绑定是否真的落在控制器 NVDS 里（配合 `ctl nvds` dump 对比）。
 *
 * 命令：
 *   ctl pair list              列出当前 LE 连接（idx/地址/状态/安全等级）+ 是否已配对
 *   ctl pair connect <idx>     对第 idx 条连接发起配对（bt_conn_set_security L2）
 *   ctl pair accept on|off     是否自动同意配对请求（注册/注销 auth 回调）
 *
 * 说明：zblue 在**没有注册 auth 回调**时走 "Just Works" 且自动接受；注册了回调
 * 就由我们的 `pairing_confirm` 决定同意与否 —— 所以 `accept off` 会让对端看到
 * "配对被拒绝"，方便你测"同意/拒绝"两条路。
 *
 * **边界（用户 2026-09-20 明确）**：IRK / 绑定**只服务手机**这一条线。传感器
 * （HR/CSC/CPS）一律**按地址连**（`ble_sensor` 那条线不认 IRK，也不该认）：它们
 * 不是我们配对的设备，没有密钥、没有身份地址解析可用。所以本文件里所有"加密/
 * 绑定/记录"的判断都要用 `ble_sensor_owns_addr()` 把传感器链路排除掉。
 *
 * **并发**：本模块的回调分布在三个上下文 —— 协议栈线程（`bt_conn_cb.*`）、
 * companion 线程（每拍 poll）、NSH/UI（`ctl pair …`）。共享的暂存一律上锁
 * （见 `g_secure_lock`），并且**不要在非协议栈线程里遍历连接链表**。
 */

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci_types.h>   /* BT_HCI_ERR_AUTH_FAIL */
#include <stdlib.h>
#include <unistd.h>

#include "bluetooth.h"                    /* bt_address_t / BT_TRANSPORT_* */
#include "bt_addr.h"                      /* BT_ADDR_STR_LENGTH / bt_addr_ba2str */

#include "myvendor_mono.h"                /* mono_ms()：配对窗口 */
#include "companion_bridge.h"             /* 投命令给 companion 线程 */

/** @brief 是否自动同意配对请求（`ctl pair accept on|off`；默认开）。
 *
 * 本设备是 NoInputNoOutput，Just Works 之外没有可交互的选项，所以默认直接同意；
 * 关掉它用来复现"对端看到配对被拒绝"这条路。 */
static bool g_auto_accept = true;

/** @brief auth 回调是否已注册（注销后 zblue 回到"无回调 = 自动接受"的行为）。 */
static bool g_auth_registered;

/**
 * @brief SMP 配对确认回调：打印对端与决定，并同意（Just Works）。
 *
 * @param conn    LE 连接；仅回调期间有效，本函数不持有引用。
 * @param passkey 本设备不显示也不输入 passkey，仅用于日志。
 */
static void pair_confirm(struct bt_conn *conn, unsigned int passkey)
{
  char addr[BT_ADDR_LE_STR_LEN];

  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
  printf("pair: confirm %s passkey=%u -> %s\n", addr, passkey,
         g_auto_accept ? "accept" : "reject");
  bt_conn_auth_pairing_confirm(conn);
}

/**
 * @brief SMP 配对取消回调（对端取消 / 流程超时）：只留一行日志。
 *
 * @param conn LE 连接；仅回调期间有效。
 */
static void pair_cancel(struct bt_conn *conn)
{
  char addr[BT_ADDR_LE_STR_LEN];

  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
  printf("pair: cancelled %s\n", addr);
}

__attribute__((unused)) static void pair_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
  char addr[BT_ADDR_LE_STR_LEN];

  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
  printf("pair: passkey %s -> %06u\n", addr, passkey);
}

static const struct bt_conn_auth_cb g_auth_cb = {
  /* **不要**注册 passkey_display：zblue 用"提供了哪些回调"来决定 IO 能力，
   * 一旦有 passkey_display 就声明成 Display Only，协商结果从 Just Works 变成 PIN/
   * passkey 流程 —— 我们既不能显示也不能输入 → 手机报"PIN 异常"（2026-09-20 现场）。
   * 去掉它（也不加 passkey_entry）即回落 NoInputNoOutput(Just Works)，配合下面的
   * pairing_confirm 自动同意即可。 */
  .cancel = pair_cancel,
  .pairing_confirm = pair_confirm,
};

/**
 * @brief 归一化地址文本：去掉 `:`/空格/逗号并把十六进制转成大写。
 *
 * @param in      原始文本（`AA:BB:…` / `aabbcc…` / 一整行 TSV 都行）。
 * @param out     输出缓冲，保证 NUL 结尾。
 * @param out_len 输出缓冲长度（不足则截断）。
 */
static void pair_norm_addr(const char *in, char *out, size_t out_len)
{
  size_t n = 0;

  while (*in != '\0' && n + 1u < out_len)
    {
      const char c = *in++;

      if (c == ':' || c == ' ' || c == '\t' || c == ',')
        {
          continue;
        }
      out[n++] = (c >= 'a' && c <= 'f') ? (char)(c - 'a' + 'A') : c;
    }
  out[n] = '\0';
}

/**
 * @brief 判断一条连接是不是传感器（HR/CSC/CPS），给 `ctl pair list` 的角色列用。
 *
 * 拿连接地址去 `/mnt/kv/bicycle_sensors.tsv`（HR/CSC/CPS 的绑定存档）里找：命中就把
 * 该行首个字段当角色名返回（`hr`/`csc`/`cps`），找不到返回 `"?"`。
 *
 * 匹配放宽两档：TSV 里可能存无冒号形式；对端用 RPA 时连接地址与存档不同，所以还比对
 * "地址后 6 位十六进制"（RPA 前两字节是随机高位）。
 *
 * @param addr_str 连接地址文本（任意分隔形式）。
 * @return 静态缓冲里的角色名；`"?"` = 不是已知传感器 / 读不到存档。
 *
 * @note 返回值指向函数内 static 缓冲，只能当下就用（printf 参数之类）。
 */
static const char *pair_role_hint(const char *addr_str)
{
  static char role[12];
  char want[32];
  FILE *f;
  char line[160];

  role[0] = '?';
  role[1] = '\0';

  pair_norm_addr(addr_str, want, sizeof(want));
  if (strlen(want) < 6u)
    {
      return role;
    }

  f = fopen("/mnt/kv/bicycle_sensors.tsv", "r");
  if (f == NULL)
    {
      return role;
    }

  while (fgets(line, sizeof(line), f) != NULL)
    {
      char have[160];

      pair_norm_addr(line, have, sizeof(have));
      if (strstr(have, want) == NULL && strstr(have, want + strlen(want) - 6u) == NULL)
        {
          continue;
        }

      {
        unsigned n = 0;

        while (line[n] != '\0' && line[n] != '\t' && line[n] != ' '
               && line[n] != ',' && n < sizeof(role) - 1u)
          {
            role[n] = line[n];
            n++;
          }
        role[n] = '\0';
      }
      break;
    }

  fclose(f);
  return role;
}

/** @brief `ctl pair list` 的遍历游标：当前下标 / 要挑的下标 / 已见条数。 */
static struct bt_conn *g_pick;
static int g_want_idx;
static int g_seen;

/**
 * @brief `bt_conn_foreach` 回调：打印一条 LE 连接，并在下标命中时引用取出。
 *
 * 全 `FF:FF:FF:FF:FF:FF` 的条目是"连接刚建立、地址还没解析出来"的中间态，标
 * `invalid(no addr)` 并跳过 —— 按 idx 挑它会挑到空气（现场踩过两次）。
 *
 * @param conn bt_conn_foreach 已经引用过的连接，本函数不额外持有。
 * @param data 未用。
 */
static void pair_conn_cb(struct bt_conn *conn, void *data)
{
  char addr[BT_ADDR_LE_STR_LEN];
  const bt_security_t sec = bt_conn_get_security(conn);
  const int idx = g_seen++;

  (void)data;
  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
  if (strncmp(addr, "FF:FF:FF:FF:FF:FF", 17) == 0)
    {
      /* 全 FF 的 public 地址不是真对端（连接对象还没解析出地址 / 残留对象），
       * 标出来免得被当成"某个设备"去配对。 */
      printf("pair: [%d] %s invalid(no addr) -> skip\n", idx, addr);
      return;
    }

  printf("pair: [%d] %s role=%s sec=%u bonded=%d\n", idx, addr,
         pair_role_hint(addr), (unsigned)sec, sec >= BT_SECURITY_L2 ? 1 : 0);

  if (idx == g_want_idx)
    {
      g_pick = bt_conn_ref(conn);
    }
}

/**
 * @brief `ctl pair list`：打印当前 LE 连接（下标 / 地址 / 角色 / 安全等级 / 是否绑定）。
 *
 * @return 恒为 0（命令入口，失败也只用日志表达）。
 */
static int pair_cmd_list(void)
{
  g_seen = 0;
  g_pick = NULL;
  g_want_idx = -1;
  bt_conn_foreach(BT_CONN_TYPE_LE, pair_conn_cb, NULL);
  if (g_seen == 0)
    {
      printf("pair: no LE connection\n");
    }
  printf("pair: auto_accept=%d auth_cb=%d conns=%d\n", g_auto_accept ? 1 : 0,
         g_auth_registered ? 1 : 0, g_seen);
  return 0;
}

/* ── 挑一条"可用"的连接（给 `ctl pair connect`）──────────────────────────────
 * 不按裸 idx：链路重建期间那条会显示成 `FF:FF:FF:FF:FF:FF (public) invalid(no addr)`，
 * 按 idx 会挑空（现场两次都这样）。挑法：
 *   `ctl pair connect`              无参数 = auto：挑"不是传感器"的那条（= 手机）
 *   `ctl pair connect <addr 前缀>`   按地址前缀挑（如 `58:81`）
 */
static struct bt_conn *g_pick_out;

/** @brief 挑连接时想匹配的地址前缀（auto 时为 NULL）。 */
static const char    *g_pick_want;
/** @brief 上述前缀的长度。 */
static unsigned       g_pick_wlen;
/** @brief true = auto 模式（挑非传感器的第一条）。 */
static bool           g_pick_auto;

/**
 * @brief `bt_conn_foreach` 回调：按当前模式挑一条连接并引用取出（只挑第一条）。
 *
 * @param conn bt_conn_foreach 已引用的连接。
 * @param data 未用。
 */
static void pair_pick_cb(struct bt_conn *conn, void *data)
{
  char addr[BT_ADDR_LE_STR_LEN];
  const char *role;

  (void)data;
  if (g_pick_out != NULL) {
    return;                                        /* 已经挑到了 */
  }

  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
  if (strncmp(addr, "FF:FF:FF", 8) == 0) {
    return;                                        /* 地址未填：跳过 */
  }
  role = pair_role_hint(addr);

  if (!g_pick_auto) {
    if (g_pick_wlen > 0 && strncasecmp(addr, g_pick_want, g_pick_wlen) == 0) {
      g_pick_out = bt_conn_ref(conn);
    }
    return;
  }

  if (role[0] != '?') {
    return;                                        /* auto：传感器不挑 */
  }
  g_pick_out = bt_conn_ref(conn);
}

/**
 * @brief 遍历连接表挑一条可用的（调用方负责 `bt_conn_unref` 归还）。
 *
 * @param want 地址前缀（大小写不敏感）；NULL 或空串 = auto：挑非传感器的那条。
 * @param wlen want 的长度（want 为 NULL 时忽略）。
 * @return 已引用的连接，没挑到返回 NULL。
 */
static struct bt_conn *pair_pick(const char * want, unsigned wlen)
{
  g_pick_out   = NULL;
  g_pick_want  = want;
  g_pick_wlen  = wlen;
  g_pick_auto  = (want == NULL || want[0] == '\0');
  bt_conn_foreach(BT_CONN_TYPE_LE, pair_pick_cb, NULL);
  return g_pick_out;
}

/**
 * @brief `ctl pair connect <addr 前缀>`：挑连接并 `bt_conn_set_security(L2)` 发起配对。
 *
 * 链路刚重建时地址可能还没填，所以最多重试 5 次（每次隔 1 s）。
 *
 * @param arg 地址前缀；NULL = auto（挑非传感器那条）。
 * @return 0 = 已发起配对；1 = 没挑到可用连接 / 发起失败。
 */
static int pair_cmd_connect_arg(const char * arg)
{
  int attempt;
  int ret = -1;

  for (attempt = 0; attempt < 5; attempt++)
    {
      struct bt_conn *pick = pair_pick(arg, arg == NULL ? 0u : (unsigned)strlen(arg));

      if (pick == NULL)
        {
          printf("pair: no usable connection yet (attempt %d)\n", attempt + 1);
          sleep(1);
          continue;
        }

      ret = bt_conn_set_security(pick, BT_SECURITY_L2);
      printf("pair: set_security(L2) ret=%d (attempt %d)\n", ret, attempt + 1);
      bt_conn_unref(pick);
      if (ret == 0)
        {
          return 0;
        }
      sleep(1);
    }

  return 1;
}

/**
 * @brief `ctl pair connect <idx>`：按 `ctl pair list` 的下标挑连接发起配对。
 *
 * @param idx `ctl pair list` 打印的下标。
 * @return 0 = 已发起；1 = 该下标没有连接 / 发起失败。
 */
static int pair_cmd_connect(int idx)
{
  int ret;

  g_seen = 0;
  g_pick = NULL;
  g_want_idx = idx;
  bt_conn_foreach(BT_CONN_TYPE_LE, pair_conn_cb, NULL);
  if (g_pick == NULL)
    {
      printf("pair: no connection at idx %d\n", idx);
      return 1;
    }

  ret = bt_conn_set_security(g_pick, BT_SECURITY_L2);
  printf("pair: set_security(L2) ret=%d\n", ret);
  bt_conn_unref(g_pick);
  g_pick = NULL;
  return ret == 0 ? 0 : 1;
}

static int pair_cmd_accept(bool on);
void myvendor_pair_note_bond(const bt_address_t *addr, bool bonded);
static void pair_hex_to_bytes(const char *hex, uint8_t *out, unsigned n);

/* ── 准入 + 手机身份记录（2026-09-20，用户拍板"没配对就不该接受连接"）──────────
 *
 * 改动前：广播是无定向可连接、特征权限全是裸的 `GATT_PERM_READ/WRITE` —— 任何知道
 * 服务 UUID 的中心都能连上并**写**控制/导航/FS 通道（现场实测：手机上 app 已关、
 * 配对已删，仍有一条 `sec=1 bonded=0` 的连接在读写）。现在两道：
 *
 *   ① 连接准入：只有"配对窗口内"或"就是本机记录的那台手机"才放行，其余先给
 *      `PAIR_ADMIT_GRACE_MS` 宽限（系统配对 createBond 也要先建链跑 SMP），到点
 *      仍未绑定就 `bt_conn_disconnect(AUTH_FAIL)`；所有特征/CCCD 都是 ENCRYPT
 *      权限兜底（未加密的 ATT 操作一律被拒 ⇒ 手机因此走隐式配对）。
 *   ② 广播三档（决策在 ble_companion.c 的 `companion_start_advertising()`）：
 *      配对窗口内 = 快广播（200 ms）可发现；有手机记录 = 可发现但闲时间隔拉到
 *      1280 ms（少抢射频）；**没记录 + 窗口外 = 完全不广播**。
 *      定向广播 `ADV_DIRECT_IND` 2026-09-20 已退役：手机配对后改用轮换 RPA 连接，
 *      TargetA 固定的定向广播会被手机忽略（现场"只有配对那次能连"）。详见
 *      `myvendor_pair_directed_peer()` 的注释。
 *
 * **IRK / 绑定只服务手机这条线**（用户 2026-09-20 明确）：传感器（HR/CSC/CPS）一律
 * **按地址连** —— 它们不是我们配对的设备，没有密钥、也没有身份地址可解析。所以本
 * 文件里所有"加密 / 绑定 / 记录"的判断都要用 `ble_sensor_owns_addr()` 把传感器链路
 * 排除掉，别把 IRK 那套套到外设上。
 *
 * 记录文件 `/mnt/kv/bt_phone.tsv`（一行 `ADDR<TAB>TYPE`）：
 *   · 谁写：`myvendor_pair_note_bond()`（bond 回调 / 控制帧路径）与
 *     `myvendor_pair_peer_poll()`（`security_changed` 事件对账，**主力**）；
 *   · 谁删：解绑（`myvendor_pair_unbind_poll()` 里 `bt_unpair` + 删记录）；
 *   · 手机侧"忽略设备"**不会**通知设备，所以 tsv 可能过期：这时靠配对窗口
 *     （设备 UI / `ctl pair open [秒]`）重新配对，窗口内是快广播可发现。
 */
#define PAIR_PHONE_TSV     "/mnt/kv/bt_phone.tsv"
/** `ctl pair open` / BLE 控制帧不带秒数时的窗口长度。
 *  **开机不再自动开窗**（用户 2026-09-20）：只有 UI / NSH / app 显式操作才开。 */
#define PAIR_WINDOW_DEFAULT_S 60u
/* 未配对连接的宽限：系统配对（Android createBond）也要先建一条 LE 链路，
 * 建链后还要跑 SMP —— 立刻掐会导致"永远配不上"。宽限内所有特征都是
 * ENCRYPT 权限 ⇒ 拿不到任何数据；到点还没绑定就断开。
 * 30 s 是"等用户在系统弹框上点确认"用的（createBond 之后系统要弹框）。 */
#define PAIR_ADMIT_GRACE_MS 30000u

/** @brief 配对窗口的截止时刻（mono_ms 基准）；0 = 没开窗。受 `g_window_lock` 保护。 */
static uint32_t g_pair_window_until_ms;
/** @brief "窗口档位变了，请重播广播"的请求；由 companion 线程取走。受 `g_window_lock` 保护。 */
static bool     g_window_restart_req;

/* 窗口状态是**跨线程**的：UI（LVGL 线程）读倒计时、ctl/NSH 开窗、companion 线程
 * 取重启请求、sal/adv 路径读"在不在窗口内"。全部走这把锁（用户 2026-09-20 明确：
 * 不许无保护地跨线程动共享资源）。*/
static pthread_mutex_t g_window_lock = PTHREAD_MUTEX_INITIALIZER;


/** @brief 取走"开窗后要重播广播"的请求（**只能由 companion 线程调**）。
 *
 * `ctl pair open` 跑在 NSH/ctl 任务里，而 `bt_le_stop_advertising()` 之类的框架调用
 * 只能从蓝牙自己的线程走 —— 2026-09-20 实测：在 ctl 任务里直接调会撞
 * `libuv/src/unix/thread.c:358` 断言（板子当场 panic）。所以这里只置标志，
 * 由 companion 主循环取走后再做（和 `ctl radio` 走 bridge take 的既有做法一致）。 */
bool myvendor_pair_window_take_restart(void)
{
  bool r;

  pthread_mutex_lock(&g_window_lock);
  r = g_window_restart_req;
  g_window_restart_req = false;
  pthread_mutex_unlock(&g_window_lock);
  return r;
}

/**
 * @brief 配对窗口是否开着（判定"允许未配对连接"的两块拼图之一）。
 *
 * @return true = 此刻在窗口内。
 *
 * @note 可从任意线程调用（内部持 `g_window_lock`）。
 */
bool myvendor_pair_window_active(void)
{
  bool active;

  pthread_mutex_lock(&g_window_lock);
  active = g_pair_window_until_ms != 0u &&
           (int32_t)(g_pair_window_until_ms - myvendor_mono_ms()) > 0;
  pthread_mutex_unlock(&g_window_lock);
  return active;
}

/** @brief 立刻关掉配对窗口（解绑 / 配对成功时都要收掉）。
 *
 * 用户现场：配对完成后 30 s 还没到，这时解绑 —— 剩下的窗口里**谁都能把"配对中 + 倒计时"
 * 走完再配一次**。语义上"允许配对"这个许可不该比绑定关系活得久，所以：
 *   · 解绑（`myvendor_pair_unbind_poll`）先关窗；
 *   · 配对成功（`myvendor_pair_note_bond(…, true)`）也关窗。
 * 窗口一关，`adv` 那三档会按"有没有记录"重新判定（没记录就不再广播）。 */
void myvendor_pair_window_close(void)
{
  bool was_open;

  pthread_mutex_lock(&g_window_lock);
  was_open = g_pair_window_until_ms != 0u;
  g_pair_window_until_ms = 0u;
  /* 让广播立刻按新档位重判一次：解绑后 → `adv off`（没记录）；配对成功后 → 可发现。 */
  g_window_restart_req   = true;
  pthread_mutex_unlock(&g_window_lock);

  if (was_open)
    {
      printf("pair: window closed\n");
    }
}

/** @brief 配对窗口剩余毫秒（0 = 没开窗）。设备 UI 用它显示"配对中 + 倒计时"。 */
uint32_t myvendor_pair_window_left_ms(void)
{
  int32_t left;

  pthread_mutex_lock(&g_window_lock);
  if (g_pair_window_until_ms == 0u)
    {
      pthread_mutex_unlock(&g_window_lock);
      return 0u;
    }

  left = (int32_t)(g_pair_window_until_ms - myvendor_mono_ms());
  pthread_mutex_unlock(&g_window_lock);
  return left > 0 ? (uint32_t)left : 0u;
}

/** @brief 开配对窗口（秒）；0 = 用开机默认 60 s。窗口内允许未配对连接（首次配对靠它）。 */
void myvendor_pair_window_open(unsigned secs)
{
  if (secs == 0u)
    {
      secs = PAIR_WINDOW_DEFAULT_S;
    }

  pthread_mutex_lock(&g_window_lock);
  g_pair_window_until_ms = myvendor_mono_ms() + secs * 1000u;
  g_window_restart_req   = true;
  pthread_mutex_unlock(&g_window_lock);
  printf("pair: window open %us\n", secs);
}

/* 十六进制串 → 字节；`hex` 至少 2n 个字符。 */
static void pair_hex_to_bytes(const char *hex, uint8_t *out, unsigned n)
{
  unsigned i;

  for (i = 0; i < n; i++)
    {
      unsigned hi = 0;
      unsigned lo = 0;
      const char c1 = hex[i * 2u];
      const char c2 = hex[i * 2u + 1u];

      hi = (c1 >= '0' && c1 <= '9') ? (unsigned)(c1 - '0') : (unsigned)(c1 - 'A' + 10);
      lo = (c2 >= '0' && c2 <= '9') ? (unsigned)(c2 - '0') : (unsigned)(c2 - 'A' + 10);
      out[i] = (uint8_t)(((hi & 15u) << 4) | (lo & 15u));
    }
}

/* 读 `/mnt/kv/bt_phone.tsv` → 归一化地址前 12 个字符（6 字节）+ 地址类型。 */
static bool pair_phone_identity(char *out12, size_t out12_len, uint8_t *out_type)
{
  char      line[96];
  char      raw[40];
  char      norm[40];
  unsigned  type = 1;
  FILE     *f = fopen(PAIR_PHONE_TSV, "r");

  if (f == NULL)
    {
      return false;
    }
  if (fgets(line, sizeof(line), f) == NULL)
    {
      fclose(f);
      return false;
    }
  fclose(f);

  if (sscanf(line, "%39s %u", raw, &type) != 2)
    {
      return false;
    }

  pair_norm_addr(raw, norm, sizeof(norm));
  if (strlen(norm) < 12u)
    {
      return false;
    }
  norm[12] = '\0';

  if (out12_len < 13u)
    {
      return false;
    }
  memcpy(out12, norm, 13u);       /* 含结尾 0 */

  if (out_type != NULL)
    {
      *out_type = (uint8_t)(type & 0xffu);
    }
  return true;
}

/* ── 在 LE 连接里按（归一化）地址找 conn ─────────────────────────────────────
 * 找到就**带引用返回**（调用方负责 `bt_conn_unref`）。
 *
 * @warning 内部走 `bt_conn_foreach()` —— **只能在协议栈线程/会话内用**，不要拿它
 *          做周期性轮询（2026-09-20 的 panic 就是这么来的，见 `myvendor_pair_peer_poll`
 *          的注释）。这里只在 bond 回调 / 控制帧 / 用户命令这些低频路径里调。 */
static char            g_look_hex[13];
/** 命中那条连接在 HCI 层报的地址类型（0 public / 1 random），随查找一起带出。 */
static uint8_t         g_look_type;
/** 命中那条连接的引用（交给调用方 unref）。 */
static struct bt_conn *g_look_conn;

/**
 * @brief `bt_conn_foreach` 回调：地址（归一化后）匹配就记类型并 `bt_conn_ref()`。
 *
 * @param conn bt_conn_foreach 已引用的连接。
 * @param data 未用。
 */
static void pair_look_cb(struct bt_conn *conn, void *data)
{
  char s[BT_ADDR_LE_STR_LEN];
  char norm[40];

  (void)data;
  bt_addr_le_to_str(bt_conn_get_dst(conn), s, sizeof(s));
  pair_norm_addr(s, norm, sizeof(norm));
  if (strncmp(norm, g_look_hex, 12u) == 0)
    {
      g_look_type = (uint8_t)bt_conn_get_dst(conn)->type;
      g_look_conn = bt_conn_ref(conn);
    }
}

/**
 * @brief 按（12 位归一化）地址查一条 LE 连接。
 *
 * @param hex12    12 位十六进制地址（无冒号，大小写不敏感）。
 * @param out_type 可选；命中时写入该连接在 HCI 层的地址类型（0 public / 1 random）。
 * @return 命中时返回**已带引用**的连接（调用方 `bt_conn_unref`）；没命中返回 NULL。
 */
static struct bt_conn *pair_conn_find(const char *hex12, uint8_t *out_type)
{
  snprintf(g_look_hex, sizeof(g_look_hex), "%s", hex12);
  g_look_conn = NULL;
  g_look_type = 0;                /* BT_ADDR_LE_PUBLIC */

  bt_conn_foreach(BT_CONN_TYPE_LE, pair_look_cb, NULL);

  if (out_type != NULL)
    {
      *out_type = g_look_type;
    }

  return g_look_conn;
}


/**
 * @brief 从"应用侧真的有流量"反推"这就是那台已绑定手机"，并把记录补上。
 *
 * 为什么需要它：`on_bond_state_changed` 那条路实测**没能让记录落盘**
 * （2026-09-20：手机配对成功、能连上，但 `/mnt/kv/bt_phone.tsv` 一直是空的）——
 * 记录空 ⇒ 窗口一关准入就会拒绝这台手机。这里换成**实证**：对端能发控制帧说明
 * 链路已加密（所有特征/CCCD 都是 ENCRYPT 权限，未加密写根本到不了这里），
 * 而加密链路必然来自一次成功的配对 ⇒ 记它。地址此时是 IRK 解析后的**身份地址**
 * （已绑定对端 zblue 会解析），正好是记录与定向广播要的那个。
 */
/* 「刚加密的那条链路」的暂存：`bt_conn_cb.security_changed` **在协议栈线程**里写，
 * companion 线程每拍取走落盘。跨线程共享 ⇒ **必须上锁**：裸写一个 13 字节地址 +
 * 一个标志，取走的一侧就可能读到半截地址（用户明确要求：不许无保护地跨线程动资源）。*/
static pthread_mutex_t g_secure_lock = PTHREAD_MUTEX_INITIALIZER;
static char    g_secure_peer[13];
static uint8_t g_secure_peer_type;
static bool    g_secure_peer_pending;

/** @brief 落盘一条手机记录（写前比对；没变就不碰 flash）。
 *
 * 两个调用点：`myvendor_pair_note_bond()`（bond 回调 / 控制帧）和
 * `myvendor_pair_peer_poll()`（security_changed 对账）。**都要先关窗**：配上了剩下的
 * 秒数不该继续允许"再配一次"。 */
static void pair_record_write(const char *norm, uint8_t type)
{
  char    have[13];
  uint8_t have_type = 0;
  FILE   *f;

  myvendor_pair_window_close();

  /* **内容没变就别写盘**：/mnt/kv 是 flash，而这条记录会被每次控制帧和每秒的对账
   * 摸一遍 —— 现场 2026-09-20 手机一连上就 `pair: phone record …` 刷屏，等于拿
   * flash 寿命换一行日志。地址或类型真的变了才落盘。 */
  if (pair_phone_identity(have, sizeof(have), &have_type) &&
      strncmp(have, norm, 12u) == 0 && have_type == type)
    {
      return;
    }

  f = fopen(PAIR_PHONE_TSV, "w");
  if (f == NULL)
    {
      printf("pair: phone record write failed\n");
      return;
    }
  fprintf(f, "%s\t%u\n", norm, (unsigned)type);
  fclose(f);
  printf("pair: phone record %s type=%u\n", norm, (unsigned)type);
}

/**
 * @brief 链路加密了（`bt_conn_cb.security_changed`，跑在 BT 线程）—— 只记地址。
 *
 * **为什么只置标志**：这条回调在协议栈线程上，落盘（/mnt/kv）+ tsv 读都留到
 * companion 线程的每拍去做（`myvendor_pair_peer_poll`）。FS 在栈线程上阻塞会
 * 顶住整个协议栈（"FS 提交饿死心跳"那条教训）。
 */
void myvendor_pair_note_secure_peer(const bt_address_t *addr, uint8_t addr_type)
{
  char s[BT_ADDR_STR_LENGTH];
  char hex[40];

  if (addr == NULL)
    {
      return;
    }

  bt_addr_ba2str(addr, s);
  pair_norm_addr(s, hex, sizeof(hex));
  if (strlen(hex) < 12u)
    {
      return;
    }
  hex[12] = '\0';

  pthread_mutex_lock(&g_secure_lock);
  snprintf(g_secure_peer, sizeof(g_secure_peer), "%s", hex);
  g_secure_peer_type = addr_type;
  g_secure_peer_pending = true;
  pthread_mutex_unlock(&g_secure_lock);
}

/**
 * @brief 对端能发控制帧 = 链路已加密（所有特征/CCCD 都是 ENCRYPT 权限）⇒ 记它。
 *
 * 现在只置标志，真正的落盘交给 `myvendor_pair_peer_poll`（见上）。
 */
void myvendor_pair_note_peer_if_secure(const bt_address_t *addr)
{
  myvendor_pair_note_secure_peer(addr, 0);
}

/**
 * @brief 维护"已绑定手机"记录（`/mnt/kv/bt_phone.tsv`）。
 *
 * 两个方向：
 *   · `bonded = true`：归一化地址 → 关窗 → **内容变了才**落盘（见 `pair_record_write()`）；
 *     调用点：适配器 bond 回调、控制帧路径、`security_changed` 对账。
 *   · `bonded = false`：只在"确实就是记录里那台"时删记录（别的设备配对失败时
 *     适配器也会发 NONE，不该把手机这条抹掉）；`addr == NULL` 是 `ctl pair forget`
 *     的强制清除路径。
 *
 * @param addr   对端地址；`bonded = false` 时允许为 NULL（= 强制清除）。
 * @param bonded true = 记上 / 更新；false = 清除。
 *
 * @note 会做文件 IO（tsv 读写），**不要**从协议栈的高频回调里直接调；跨线程共享
 *       的暂存都已加锁，但落盘仍尽量留在 companion 线程。
 */
void myvendor_pair_note_bond(const bt_address_t *addr, bool bonded)
{
  char    s[BT_ADDR_STR_LENGTH];
  char    norm[40];
  uint8_t type = 0;

  if (!bonded)
    {
      char    have[13];
      char    peer_s[BT_ADDR_STR_LENGTH];
      char    want[40];
      uint8_t want_type = 0;

      /* 只在"确实就是这条记录"时清：BOND_STATE_NONE 也会在**别的设备**配对失败
       * 时发出来（那台陌生设备失败不该把手机这条记录抹掉）。addr == NULL =
       * `ctl pair forget` 的强制清除路径。 */
      if (addr != NULL)
        {
          bt_addr_ba2str(addr, peer_s);
          pair_norm_addr(peer_s, want, sizeof(want));
          if (strlen(want) >= 12u)
            {
              want[12] = '\0';
            }
          if (!pair_phone_identity(have, sizeof(have), &want_type) ||
              strncmp(have, want, 12u) != 0)
            {
              return;
            }
        }

      if (remove(PAIR_PHONE_TSV) == 0)
        {
          printf("pair: phone record cleared\n");
        }
      return;
    }

  bt_addr_ba2str(addr, s);
  pair_norm_addr(s, norm, sizeof(norm));
  if (strlen(norm) < 12u)
    {
      printf("pair: phone record bad addr %s\n", s);
      return;
    }
  norm[12] = '\0';

  {
    struct bt_conn *conn = pair_conn_find(norm, &type);

    if (conn != NULL)
      {
        bt_conn_unref(conn);
      }
    else
      {
        type = 0;                 /* 拿不到连接就按 public 记 */
      }
  }

  pair_record_write(norm, type);
}

/** @brief `myvendor_pair_phone_link_up()` 的遍历结果：是否见到"手机侧"链路。 */
static bool g_link_phone;

/**
 * @brief `bt_conn_foreach` 回调：判断这条链路是不是"手机侧"（非传感器）。
 *
 * @param conn bt_conn_foreach 已引用的连接。
 * @param data 未用。
 */
static void pair_link_cb(struct bt_conn *conn, void *data)
{
  extern bool ble_sensor_owns_addr(const bt_address_t *addr);
  bt_address_t a;

  (void)data;
  memset(&a, 0, sizeof(a));
  memcpy(a.addr, bt_conn_get_dst(conn)->a.val, BT_ADDR_LENGTH);

  if (!ble_sensor_owns_addr(&a))
    {
      g_link_phone = true;        /* 手机侧的链路（传感器链路另算） */
    }
}

/** @brief 现在是否真的有一条"手机侧"LE 链路 —— 给 ble_companion 做状态对账。

 *
 * 为什么要它：GATTS 的 disconnect 回调偶尔**不到**（HCI 已报断开、`g_ctx.connected`
 * 却一直是真），现象就是"设备觉得还在连"：不再广播、手机也连不回来。这里给的是
 * zblue 自己的链路表，比"事件有没有到"可靠。 */
bool myvendor_pair_phone_link_up(void)
{
  g_link_phone = false;
  bt_conn_foreach(BT_CONN_TYPE_LE, pair_link_cb, NULL);
  return g_link_phone;
}



/** @brief 每拍对账：把"刚加密的那条手机链路"补成一条手机记录。
 *
 * 为什么不再等回调：`on_bond_state_changed` 实测不落盘，而 note_peer_if_secure() 要等
 * 手机真发一帧控制帧 —— 手机连上只读不写时记录永远是空的，UI 于是把「配对中 + 倒计时」
 * 一直显示到 0，也不会有「已配对」（2026-09-20 现场）。链路加密（sec>=L2）本身就是配对
 * 完成的证据：补记录 → 窗口收掉、UI 刷新成设备信息。
 *
 * **地址来自 `bt_conn_cb.security_changed`（BT 线程）**，这里只消费标志 —— 以前这里
 * 每拍 `bt_conn_foreach()` 扫连接链表，撞上协议栈正在增删的链表就 panic：
 *  coredump `n574_20260920_121424`：`pc=bt_conn_ref→stlex`、`cfa=0x82`、`mmfar=0x124`、
 *  `pid=35 ble_companion irq=1`（同一个签名早在 `ctl pair list` 上见过 —— 结论是
 *  **别在非协议栈线程里遍历 conn 链表**，晚一拍由事件驱动就够了）。
 */
void myvendor_pair_peer_poll(void)
{
  char    have[13];
  char    want[40];
  uint8_t type = 0;
  uint8_t peer_type;
  bool    pending;

  /* 先在锁里把暂存整份取走（地址+类型+清标志），锁外再做 tsv/flash 那套慢活。 */
  pthread_mutex_lock(&g_secure_lock);
  pending = g_secure_peer_pending;
  if (pending)
    {
      g_secure_peer_pending = false;
      peer_type = g_secure_peer_type;
      snprintf(want, sizeof(want), "%s", g_secure_peer);
    }
  pthread_mutex_unlock(&g_secure_lock);

  if (!pending)
    {
      return;
    }

  if (strlen(want) < 12u)
    {
      return;
    }

  if (pair_phone_identity(have, sizeof(have), &type) &&
      strncmp(have, want, 12u) == 0)
    {
      return;                     /* 已经是它了 */
    }

  printf("pair: secured peer %s -> record\n", want);
  pair_record_write(want, peer_type);
}

/** @brief 有没有"已绑定手机"记录（配对窗口/定向广播/准入都用它）。 */
bool myvendor_pair_has_phone_record(void)
{
  char    hex[13];
  uint8_t type = 0;

  return pair_phone_identity(hex, sizeof(hex), &type);
}

/** @brief 定向广播的目标（已绑定手机 + 不在配对窗口内）；返回 false = 走普通广播。
 *
 * **2026-09-20 起调用点已摘掉**：手机配对后会改用轮换 RPA 连接（HCI `atype=1`，
 * 地址每会话都变），定向广播只能打给一个固定 TargetA ⇒ 手机直接忽略、CONNECT_IND
 * 15 s 超时（用户现象："只有配对那次能连，之后连接被拒绝"）。有记录时改成
 * **可发现 + 可连接**（准入与 ENCRYPT 权限兜底，见 ble_companion.c 的三档注释）。
 * 留着这套是为了将来真遇到"公开地址且稳定"的对端（例如某些 iOS 场景）能直接启用。
 */
bool myvendor_pair_directed_peer(bt_address_t *out, uint8_t *out_type)
{
  char    hex[13];
  uint8_t bytes[6];
  uint8_t type = 0;
  unsigned i;

  if (myvendor_pair_window_active())
    {
      return false;               /* 窗口内保持可发现，方便首次/重配 */
    }
  if (!pair_phone_identity(hex, sizeof(hex), &type))
    {
      return false;
    }

  /* 记录里是**显示顺序**（`78:D8:40:…`），而 framework/zblue 的 `addr[]` 是最低位
   * 在前 —— 这里要倒过来填，否则广播会指向一个反过来的地址（2026-09-20 实测：
   * 打印成 `20:F5:4B:…`，手机永远看不到）。 */
  pair_hex_to_bytes(hex, bytes, 6u);
  memset(out, 0, sizeof(*out));
  for (i = 0; i < 6u; i++)
    {
      out->addr[i] = bytes[5u - i];
    }

  if (out_type != NULL)
    {
      *out_type = type;
    }
  return true;
}

/**
 * @brief 连接准入：窗口内 / 已记录手机 → 放行；否则断开（AUTH_FAIL）。
 *
 * 由 ble_companion 的 GATT connect 回调在"标记 connected"**之前**调用 ——
 * 被拒时调用方不要把它当连上（断开是异步的，连接对象随后会被栈回收）。
 */
static uint32_t g_admit_deadline_ms;   /* 0 = 没有待处理的未配对连接 */
static char     g_admit_peer[13];
static bool     g_unbind_req;          /* ctl/UI 侧请求解绑，交给 companion 线程执行 */

bool myvendor_pair_admit(const bt_address_t *addr)
{
  char    s[BT_ADDR_STR_LENGTH];
  char    want[40];
  char    have[13];
  uint8_t type = 0;

  if (myvendor_pair_window_active())
    {
      return true;
    }

  bt_addr_ba2str(addr, s);
  pair_norm_addr(s, want, sizeof(want));
  if (strlen(want) >= 12u)
    {
      want[12] = '\0';
    }

  if (pair_phone_identity(have, sizeof(have), &type) &&
      strncmp(have, want, 12u) == 0)
    {
      return true;                /* 就是那台已绑定手机 */
    }

  /* **未配对：不再立刻断**（那样手机根本没机会配对：系统配对要先建链路，建链后
   * 还要跑 SMP）。给一段宽限，超时仍未绑定才断开 —— 见 myvendor_pair_admit_poll()。
   * 宽限内数据安全由**特征权限**兜底（全部改成 ENCRYPT：未加密的 ATT 操作一律
   * 被拒，手机因此走隐式配对）。 */
  snprintf(g_admit_peer, sizeof(g_admit_peer), "%s", want);
  g_admit_deadline_ms = myvendor_mono_ms() + PAIR_ADMIT_GRACE_MS;
  printf("pair: unpaired %s -> grace %us (pair now)\n", want,
         (unsigned)(PAIR_ADMIT_GRACE_MS / 1000u));
  return true;
}

/** @brief 请求解除绑定（清密钥池 + 清手机记录）。**只置标志**，执行见 poll。 */
void myvendor_pair_unbind_request(void)
{
  g_unbind_req = true;
}

/** @brief 执行解绑（companion 线程每拍调）。`bt_unpair` 只能在蓝牙自己的线程里调。 */
void myvendor_pair_unbind_poll(void)
{
  char            hex[13];
  uint8_t         type = 0;
  bt_addr_le_t    addr;
  unsigned        i;
  unsigned        bytes[6];

  if (!g_unbind_req)
    {
      return;
    }
  g_unbind_req = false;

  /* 解绑 = 不再需要"允许配对"这个许可：把可能还开着的窗口收掉。 */
  myvendor_pair_window_close();

  if (!pair_phone_identity(hex, sizeof(hex), &type))
    {
      printf("pair: unbind: no phone record\n");
      return;
    }

  /* tsv 里是显示顺序；zblue 的 bt_addr_le_t 是低位在前 → 倒着填。 */
  pair_hex_to_bytes(hex, (uint8_t *)bytes, 6u);
  memset(&addr, 0, sizeof(addr));
  for (i = 0; i < 6u; i++)
    {
      addr.a.val[i] = ((const uint8_t *)bytes)[5u - i];
    }
  addr.type = type;

  printf("pair: unbind %s (type=%u)\n", hex, (unsigned)type);
  (void)bt_unpair(BT_ID_DEFAULT, &addr);

  /* 记录也清掉（控制器不一定回 bond-deleted 事件）。 */
  myvendor_pair_note_bond(NULL, false);
}

/** @brief 宽限到期仍未绑定 → 断开。由 companion 线程每拍调（框架不能从 ctl 线程碰）。 */
void myvendor_pair_admit_poll(void)
{
  struct bt_conn *conn;
  char            have[13];
  uint8_t         type = 0;

  if (g_admit_deadline_ms == 0u ||
      (int32_t)(g_admit_deadline_ms - myvendor_mono_ms()) > 0)
    {
      return;
    }

  g_admit_deadline_ms = 0u;

  /* 宽限期间配好了？记录里出现这个地址就算通过（bond 回调写记录）。 */
  if (pair_phone_identity(have, sizeof(have), &type) &&
      strncmp(have, g_admit_peer, 12u) == 0)
    {
      printf("pair: %s paired in grace\n", g_admit_peer);
      return;
    }

  conn = pair_conn_find(g_admit_peer, NULL);
  if (conn != NULL)
    {
      printf("pair: reject unpaired %s (grace over)\n", g_admit_peer);
      (void)bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
      bt_conn_unref(conn);
    }
}


/* ── ctl 侧的"看"和"动作"都改走 companion 线程（2026-09-20）─────────────────
 *
 * 现场：`ctl pair list` 在 **ctl 线程**里 `bt_conn_foreach()` → 踩到被 BT 线程改到
 * 一半的连接链表 → `bt_conn_ref()` 的 `stlex` 打在野指针上 → MemManage panic
 * （dump: cfsr=0x82 mmfar=0x124 pc=bt_conn_ref←bt_conn_foreach_mc pid=ctl）。
 * 所以下面这两个函数**只能由 companion 线程调**（节点里的 COMPANION_TEST_PAIR_*），
 * ctl 侧只负责投命令 + 打印。
 */

/** @brief 拼 `ctl pair list` 输出的累积器（buf/cap/写入偏移/当前下标）。 */
struct pair_report_s
{
  char    *buf;
  size_t   cap;
  unsigned off;
  int      idx;
};

/**
 * @brief 往报告缓冲追加一行（printf 风格）；写满就丢弃后续内容。
 *
 * @param r   报告累积器。
 * @param fmt printf 格式串。
 */
static void pair_report_add(struct pair_report_s *r, const char *fmt, ...)
{
  va_list ap;
  int     n;

  if (r->off + 8u >= r->cap)
    {
      return;
    }

  va_start(ap, fmt);
  n = vsnprintf(r->buf + r->off, r->cap - r->off, fmt, ap);
  va_end(ap);

  if (n > 0)
    {
      r->off += (unsigned)n;
      if (r->off >= r->cap)
        {
          r->off = (unsigned)(r->cap - 1u);
        }
    }
}

/**
 * @brief `bt_conn_foreach` 回调：把一条 LE 连接渲染成报告行（给 `ctl pair list`）。
 *
 * @param conn bt_conn_foreach 已引用的连接。
 * @param data `struct pair_report_s *`（累积器）。
 */
static void pair_report_cb(struct bt_conn *conn, void *data)
{
  struct pair_report_s *r = data;
  char                  addr[BT_ADDR_LE_STR_LEN];
  const bt_security_t   sec = bt_conn_get_security(conn);

  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

  if (strncmp(addr, "FF:FF:FF:FF:FF:FF", 17) == 0)
    {
      pair_report_add(r, "pair: [%d] %s invalid(no addr) -> skip\n", r->idx++, addr);
      return;
    }

  pair_report_add(r, "pair: [%d] %s role=%s sec=%u bonded=%d\n", r->idx++, addr,
                  pair_role_hint(addr), (unsigned)sec,
                  sec >= BT_SECURITY_L2 ? 1 : 0);
}

/** @brief 填"配对/连接快照"文本（**companion 线程**；ctl 只打印）。 */
void myvendor_pair_report(char *buf, size_t cap)
{
  struct pair_report_s r;

  if (buf == NULL || cap == 0u)
    {
      return;
    }

  buf[0] = '\0';
  r.buf = buf;
  r.cap = cap;
  r.off = 0;
  r.idx = 0;

  bt_conn_foreach(BT_CONN_TYPE_LE, pair_report_cb, &r);

  if (r.idx == 0)
    {
      pair_report_add(&r, "pair: no LE connection\n");
    }
  pair_report_add(&r, "pair: conns=%d\n", r.idx);
}

/* 挑一条"可用"的连接（跳过全 FF 的中间态、跳过传感器）。 */
static struct bt_conn *g_auto_pick;

/**
 * @brief `bt_conn_foreach` 回调：挑第一条"不是传感器"的连接（供设备侧主动配对）。
 *
 * @param conn bt_conn_foreach 已引用的连接。
 * @param data 未用。
 */
static void pair_auto_cb(struct bt_conn *conn, void *data)
{
  char addr[BT_ADDR_LE_STR_LEN];
  bt_address_t a;

  (void)data;
  if (g_auto_pick != NULL)
    {
      return;
    }

  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
  if (strncmp(addr, "FF:FF:FF:FF:FF:FF", 17) == 0)
    {
      return;
    }

  memset(&a, 0, sizeof(a));
  memcpy(a.addr, bt_conn_get_dst(conn)->a.val, BT_ADDR_LENGTH);
  {
    extern bool ble_sensor_owns_addr(const bt_address_t *addr);

    if (ble_sensor_owns_addr(&a))
      {
        return;                 /* 心率带/踏频/功率计不是配对对象 */
      }
  }

  g_auto_pick = bt_conn_ref(conn);
}

/** @brief 设备侧发起配对：挑"不是传感器"的那条连接做 set_security(L2)。
 *  @return true = 请求已提交。 */
bool myvendor_pair_connect_auto(void)
{
  char addr[BT_ADDR_LE_STR_LEN];

  g_auto_pick = NULL;
  bt_conn_foreach(BT_CONN_TYPE_LE, pair_auto_cb, NULL);

  if (g_auto_pick == NULL)
    {
      return false;
    }

  bt_addr_le_to_str(bt_conn_get_dst(g_auto_pick), addr, sizeof(addr));
  printf("pair: connect %s set_security(L2) ret=%d\n", addr,
         bt_conn_set_security(g_auto_pick, BT_SECURITY_L2));
  bt_conn_unref(g_auto_pick);
  g_auto_pick = NULL;
  return true;
}

/**
 * @brief `ctl pair accept on|off`：注册/注销 auth 回调并设置"自动同意"。
 *
 * 注销（`on=false`）后 zblue 回到"无回调 = 自动接受"，`g_auto_accept` 也跟着变，
 * 用来复现"对端看到配对被拒绝"的路径。
 *
 * @param on true = 注册回调并自动同意；false = 注销并拒绝。
 * @return 0 = 成功；1 = 框架返回错误。
 */
static int pair_cmd_accept(bool on)
{
  int ret = 0;

  if (on && !g_auth_registered)
    {
      ret = bt_conn_auth_cb_register(&g_auth_cb);
      if (ret == 0)
        {
          g_auth_registered = true;
        }
    }
  else if (!on && g_auth_registered)
    {
      ret = bt_conn_auth_cb_register(NULL);
      if (ret == 0)
        {
          g_auth_registered = false;
        }
    }

  g_auto_accept = on;
  printf("pair: accept=%d auth_cb=%d ret=%d\n", on ? 1 : 0,
         g_auth_registered ? 1 : 0, ret);
  return ret == 0 ? 0 : 1;
}

/**
 * @brief `ctl pair …` 的 NSH 入口（`list` / `connect` / `accept` / `open` / `phone` /
 *        `unbind` / `forget`）。
 *
 * 两条纪律在这里体现：
 *   · **不在 ctl 线程遍历连接链表** —— `list` 只投
 *     `COMPANION_TEST_PAIR_LIST`，等 companion 线程把快照填进 bridge 后打印
 *     （2026-09-20 的 `bt_conn_ref→stlex` panic 就是 ctl 线程直接遍历造成的）；
 *   · **框架调用只置标志** —— `open` / `unbind` 落到
 *     `myvendor_pair_window_take_restart()` / `myvendor_pair_unbind_poll()`，
 *     由 companion 线程真正执行（在 ctl 线程直接调会撞 libuv 断言）。
 *
 * @param argc 参数个数（不含 "pair"）。
 * @param argv 参数数组；argv[0] 为子命令。
 * @return 0 = 成功；1 = 用法错误 / 失败。
 */
int myvendor_bt_pair_cmd(int argc, char ** argv)
{
  /* 默认就自动同意配对：`g_auth_registered` 是 RAM 里的（重启后清零），
   * 所以每次用这个命令前先把回调注册上 —— 否则重启后 `accept=1` 但 `auth_cb=0`，
   * 实际走的是 zblue 的默认（无人应答）路径。 */
  if (!g_auth_registered)
    {
      (void)pair_cmd_accept(true);
    }

  if (argc < 1)
    {
      printf("usage: ctl pair list | connect <idx|addr> | accept on|off | open [秒] | phone | unbind | forget\n");
      return 1;
    }

  if (strcmp(argv[0], "list") == 0)
    {
      /* **不要在 ctl 线程遍历连接**（会踩半截链表 panic）：让 companion 线程填快照，
       * 这里只等一拍再打印（见 myvendor_pair_report 的注释 + dump 证据）。 */
      char     out[320];
      uint32_t gen = 0;
      unsigned i;

      companion_bridge_sensor_cmd_post(COMPANION_TEST_PAIR_LIST);
      for (i = 0; i < 30u; i++)
        {
          if (companion_bridge_pair_report_get(out, sizeof(out), &gen))
            {
              break;
            }
          usleep(20000);        /* 20 ms × 30 = 最长等 600 ms */
        }
      companion_bridge_pair_report_get(out, sizeof(out), &gen);
      printf("%s", out);
      printf("pair: auto_accept=%d auth_cb=%d\n", g_auto_accept ? 1 : 0,
             g_auth_registered ? 1 : 0);
      return 0;
    }

  if (strcmp(argv[0], "open") == 0)
    {
      myvendor_pair_window_open(argc >= 2 ? (unsigned)atoi(argv[1]) : 0u);
      return 0;
    }

  if (strcmp(argv[0], "phone") == 0)
    {
      char    hex[13];
      uint8_t type = 0;

      if (pair_phone_identity(hex, sizeof(hex), &type))
        {
          printf("pair: phone %s type=%u window=%d\n", hex, (unsigned)type,
                 myvendor_pair_window_active() ? 1 : 0);
        }
      else
        {
          printf("pair: phone record absent (window=%d)\n",
                 myvendor_pair_window_active() ? 1 : 0);
        }
      return 0;
    }

  if (strcmp(argv[0], "unbind") == 0)
    {
      /* 真解绑（清密钥池）+ 清记录；实际执行在 companion 线程（框架约束）。 */
      myvendor_pair_unbind_request();
      return 0;
    }

  if (strcmp(argv[0], "forget") == 0)
    {
      /* 只清"已绑定手机"这条记录（定向广播/准入用），不动 zblue 的密钥池 —— 真解绑
       * 还得让手机侧"忽略设备"或走 bt_unpair。 */
      myvendor_pair_note_bond(NULL, false);
      return 0;
    }

  if (strcmp(argv[0], "connect") == 0)
    {
      /* 设备侧发起配对：**同样交给 companion 线程**（要遍历连接挑一条，ctl 线程不能碰）。
       * 现在的语义是 auto（跳过全 FF 中间态、跳过传感器）；带地址前缀的挑法留给设备 UI。 */
      (void)argc;
      (void)argv;
      companion_bridge_sensor_cmd_post(COMPANION_TEST_PAIR_CONNECT);
      printf("pair: connect requested (companion 执行；结果看日志 / ctl pair list)\n");
      return 0;
    }

  if (strcmp(argv[0], "accept") == 0 && argc >= 2)
    {
      return pair_cmd_accept(strcmp(argv[1], "on") == 0);
    }

  printf("usage: ctl pair list | connect <idx|addr> | accept on|off | open [秒] | phone | unbind | forget\n");
  return 1;
}

/* 开机默认：注册 auth 回调 + 自动同意。这样 app / 系统的配对请求会被自动确认，
 * 不必每次重启后手工 `ctl pair accept on`（2026-09-19 实测需要它才能配上手机）。 */
void myvendor_bt_pair_autoinit(void)
{
  if (g_auth_registered)
    {
      return;
    }

  if (bt_conn_auth_cb_register(&g_auth_cb) == 0)
    {
      g_auth_registered = true;
      g_auto_accept     = true;
      printf("pair: autoinit ok (auth_cb=1 auto_accept=1)\n");
    }
}
