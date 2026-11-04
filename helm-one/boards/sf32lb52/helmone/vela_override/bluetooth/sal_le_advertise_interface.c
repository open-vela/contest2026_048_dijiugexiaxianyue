/**
 * @file sal_le_advertise_interface.c
 * @brief 板级抽换 SAL 广播：legacy GAP 与 ADV/scan HCI 串行化。
 *
 * CMake 去掉上游 sal_le_advertise_interface.c。相对上游：SF32 无 Ext Adv HCI，
 * 上游即使用 LEGACY 也会 bt_le_ext_adv_create，导致 Command Disallowed /
 * advertising status=2；本版改 bt_le_adv_start/stop，**成功后才发 STARTED** ——
 * 框架收到 STARTED 才会取消自己的 1 s 定时器。所以 ticket 等待一旦 ≥1 s
 * （scan 占着射频等），即使 bt_le_adv_start 最终返回 0，上层仍会先报 status=2。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/****************************************************************************
 *  Copyright (C) 2024 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Board override: CMake 从 libbluetooth 去掉上游同名文件，改编本文件。
 * 路径：frameworks/connectivity/bluetooth/service/stacks/zephyr/sal_le_advertise_interface.c
 * 上游变更后需对照 rebase。
 *
 * 相对上游的改动：
 * - SF32 没有 Ext Adv HCI。上游即使用 BT_LE_LEGACY_ADV_IND 也会
 *   bt_le_ext_adv_create/start，控制器 Command Disallowed，advertising.c
 *   1 s 后 status=2（START_TIMEOUT），手机扫不到。
 * - 去掉 EXT_ADV / NO_2M，改走 zblue bt_le_adv_start / bt_le_adv_stop
 *   （legacy LE_SET_ADV_*），并 advertising_on_state_changed 取消 1 s 定时器。
 * - 单实例广播（CONFIG_BLUETOOTH_LE_ADVERTISER_MAX_NUM=1）。
 * - 与扫描 SAL 共用 ticket 队列，禁止 libuv worker 并行执行 GAP HCI。
 ***************************************************************************/
#define LOG_TAG "adver"

#include <nuttx/config.h>

#include "sal_le_advertise_interface.h"

/* ---- 抽换件标记（2026-09-18）----------------------------------------------
 * 1) 构建日志里可见：`ninja | grep "override compiled"` 能列出本次构建真的编译了
 *    哪些抽换件 —— 2026-09-18 曾有个抽换件其实根本不在编译库里（已剔除），
 *    改了半天没生效，就靠这种标记一眼看出来；
 * 2) 镜像里可查：`strings nuttx | grep vela_override/`（本符号 used，不会被
 *    --gc-sections 丢掉），不依赖任何编译选项（有些目标带 -w，会把 #warning 压掉）。
 * 见 docs/pitch/README.md。 */
/* ---------------------------------------------------------------------------
 * 抽换件说明（vela_override）
 *   替的是上游 : frameworks/connectivity/bluetooth / service/stacks/zephyr/sal_le_advertise_interface.c
 *   写入时 HEAD: 43945bc1d13f3494a79e3e1d79ca8312c4c0f6a0
 *   上游 blob  : 9d4ba40331337e3b09f675625eb0f0c5ccea54d9     （git -C frameworks/connectivity/bluetooth rev-parse HEAD:service/stacks/zephyr/sal_le_advertise_interface.c 应等于它）
 *   为什么抽换 : SF32 走 legacy bt_le_adv_start（无 Ext Adv）；与 scan 共用跨角色 ticket 队列，等待有界 2 s
 *   版本漂移自查:
 *     git -C frameworks/connectivity/bluetooth rev-parse HEAD:service/stacks/zephyr/sal_le_advertise_interface.c   # 与上面的 blob 比对
 *     git -C frameworks/connectivity/bluetooth diff -- service/stacks/zephyr/sal_le_advertise_interface.c          # 上游若已前进，先看这里再决定还要不要抽换
 *   机制：构建时按**文件名**把这个 .c 顶掉上游同名文件（见同目录 CMakeLists.txt 顶部表），
 *         上游 tree 保持干净、repo sync 收不走 —— 所以本文件不进 docs/pitch。
 * ------------------------------------------------------------------------- */
#pragma message("myvendor override compiled: vela_override/bluetooth/sal_le_advertise_interface.c -- 上游 frameworks/connectivity/bluetooth:service/stacks/zephyr/sal_le_advertise_interface.c@9d4ba4033133 -- SF32 走 legacy bt_le_adv_start（无 Ext Adv）；与 scan 共用跨角色 ticket 队列，等待有界 2 s")
const char myvendor_override_marker_bluetooth_sal_le_advertise_interface_c[] __attribute__((used, section(".myvendor_marker"))) = "vela_override/bluetooth/sal_le_advertise_interface.c -- 上游 frameworks/connectivity/bluetooth:service/stacks/zephyr/sal_le_advertise_interface.c@9d4ba4033133 -- SF32 走 legacy bt_le_adv_start（无 Ext Adv）；与 scan 共用跨角色 ticket 队列，等待有界 2 s";

#include "advertising.h"
#include "sal_interface.h"
#include "service_loop.h"
#include "utils/log.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <zephyr/bluetooth/bluetooth.h>

bool sf32lb52_bt_hci_skip_sync(void);

#ifndef CONFIG_BT_EXT_ADV_MAX_ADV_SEGMENT
#define CONFIG_BT_EXT_ADV_MAX_ADV_SEGMENT 5
#endif

#ifdef CONFIG_BLUETOOTH_BLE_ADV
#define STACK_CALL(func) zblue_##func

typedef void (*sal_func_t)(void* args);

typedef union {
    struct {
        struct bt_le_adv_param param;
        uint8_t* adv_data;
        uint16_t adv_len;
        uint8_t* scan_rsp_data;
        uint16_t scan_rsp_len;
    } start_adv;
} sal_adapter_args_t;

typedef struct {
    bt_controller_id_t id;
    uint8_t adv_id;
    sal_func_t func;
    uint32_t radio_ticket;
    uint32_t issued_ms;          /* 提交时刻：量 ticket 等待是否吃掉框架的 1 s */
    sal_adapter_args_t adpt;
} sal_adapter_req_t;

static uint8_t g_legacy_adv_id;
static pthread_mutex_t g_radio_order_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_radio_order_cond = PTHREAD_COND_INITIALIZER;
static uint32_t g_radio_ticket_next;
static uint32_t g_radio_ticket_serving;

#ifdef CONFIG_MYVENDOR_BLE_TEST_APP
/* 定向广播实验旋钮（只在 CONFIG_MYVENDOR_BLE_TEST_APP 的测试构建里存在）。
 *
 * -1 = 用产品默认（低占空比、不开 DIR_ADDR_RPA）
 *  0 = 强制关
 *  1 = 强制开
 *
 * 为什么旋钮必须放在 SAL：framework 的 ble_adv_params_t 没有
 * "RPA target" / "high duty" 字段（见 bt_le_advertiser.h），而这两件事
 * 恰好都只能在 zblue 的 bt_le_adv_param.options 里表达。 */
static int g_advtest_rpa_opt = -1;
static int g_advtest_low_duty = -1;
static int g_advfault_async;

void myvendor_sal_advtest_set(int rpa_opt, int low_duty)
{
    g_advtest_rpa_opt = rpa_opt;
    g_advtest_low_duty = low_duty;
}

void myvendor_sal_advfault_set(int mode)
{
    g_advfault_async = mode;
}
#endif

/* 等待上界的单位：超过就强制推进并报错。 */
#define MYVENDOR_SAL_TICKET_WAIT_MS 2000u

/*
 * ADV 与 scan 的 SAL 请求最终由 libuv 线程池执行。用一条跨 SAL 文件的
 * ticket 队列保持提交顺序，避免 stop 和下一角色 start 并行执行。
 *
 * 2026-09-18：**这个队列原来是无限等待的**，一旦某张 ticket 被取走却没
 * 有人 `done()`（请求丢了、work 没排上、对象提前销毁……），`serving` 就
 * 永远停在那里，后面每一个 GAP 请求都在这里自旋 —— 现象是"广播停了、扫描
 * 起不来、HCI 一条命令都不发"，diag 的 cmdsilent 再把适配器整个拆掉
 * （实机 22.6 s 一轮的循环）。现在超时 2 s 就强制把 `serving` 推到本张
 * ticket，让这一条先跑，并打 ERROR 留下证据（谁卡的不重要，重要的是
 * 整条 BLE 栈不能因此永久停摆）。
 */
uint32_t myvendor_sal_radio_ticket_issue(void)
{
    uint32_t ticket;

    pthread_mutex_lock(&g_radio_order_lock);
    ticket = g_radio_ticket_next++;
    pthread_mutex_unlock(&g_radio_order_lock);
    syslog(LOG_INFO, "sal adv ticket n=%u serving=%u\n",
           (unsigned)ticket, (unsigned)g_radio_ticket_serving);
    return ticket;
}

void myvendor_sal_radio_ticket_wait(uint32_t ticket)
{
    uint32_t waited = 0;

    pthread_mutex_lock(&g_radio_order_lock);
    while (ticket != g_radio_ticket_serving) {
        struct timespec ts;

        if (waited >= MYVENDOR_SAL_TICKET_WAIT_MS) {
            syslog(LOG_ERR,
                   "sal radio ticket %u stuck %u ms (serving=%u next=%u) — forcing\n",
                   (unsigned)ticket, (unsigned)waited,
                   (unsigned)g_radio_ticket_serving,
                   (unsigned)g_radio_ticket_next);
            g_radio_ticket_serving = ticket;
            break;
        }

        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&g_radio_order_cond, &g_radio_order_lock, &ts);
        waited += 1000u;
    }
    pthread_mutex_unlock(&g_radio_order_lock);
}

void myvendor_sal_radio_ticket_done(void)
{
    pthread_mutex_lock(&g_radio_order_lock);
    g_radio_ticket_serving++;
    pthread_cond_broadcast(&g_radio_order_cond);
    pthread_mutex_unlock(&g_radio_order_lock);
}

static bt_status_t zblue_le_convert_param(ble_adv_params_t* params, struct bt_le_adv_param* param)
{
    static bt_addr_le_t addr;

    switch (params->adv_type) {
    case BT_LE_ADV_IND:
    case BT_LE_EXT_ADV_IND:
    case BT_LE_LEGACY_ADV_IND:
        param->options |= BT_LE_ADV_OPT_CONN;
        param->options |= BT_LE_ADV_OPT_SCANNABLE;
        break;
    case BT_LE_ADV_SCAN_IND:
    case BT_LE_EXT_ADV_SCAN_IND:
    case BT_LE_LEGACY_ADV_SCAN_IND:
        param->options |= BT_LE_ADV_OPT_SCANNABLE;
        break;
    case BT_LE_ADV_DIRECT_IND:
    case BT_LE_EXT_ADV_DIRECT_IND:
    case BT_LE_LEGACY_ADV_DIRECT_IND:
        param->options |= BT_LE_ADV_OPT_CONN;
        param->options |= BT_LE_ADV_OPT_DIR_MODE_LOW_DUTY;

        /* **不要无条件加 BT_LE_ADV_OPT_DIR_ADDR_RPA。**
         *
         * CONFIG_BT_PRIVACY=1 时，zblue 的 valid_adv_param()
         * （external/zblue/zblue/subsys/bluetooth/host/adv.c:443）对
         * `param->peer && USE_IDENTITY && DIR_ADDR_RPA` 直接返回 false
         * ⇒ bt_le_adv_start() 返回 -EINVAL，连一条 HCI 都不会发出。
         * 而本函数下面的 own_addr_type 映射对 PUBLIC 恰好会置
         * USE_IDENTITY —— 旧代码把这两个无条件置位撞在一起，才是
         * "定向广播必失败"的直接原因，不是控制器能力。
         *
         * 正确做法（Zephyr 提交 a5be0c8228 的说明）：用 RPA 做定向目标
         * 时不能同时用本地 identity 地址，必须让 zblue 生成/使用本地
         * RPA（own_addr_type = RANDOM），并且目标地址本身应是 RPA
         * （peer_addr_type = RANDOM）。 */
        /* **产品规则**：own=RANDOM 的定向广播必须带 DIR_ADDR_RPA。
         * 只置这个位不会引入 USE_IDENTITY（RANDOM 分支不置），所以不会
         * 撞 zblue 的 -EINVAL；控制器据此用 RAL 里的 IRK 把对端身份地址
         * 解析成当前 RPA（Zephyr direct_adv 样例同款）。 */
        if (params->own_addr_type == BT_LE_ADDR_TYPE_RANDOM)
            param->options |= BT_LE_ADV_OPT_DIR_ADDR_RPA;

#ifdef CONFIG_MYVENDOR_BLE_TEST_APP
        if (g_advtest_low_duty == 0)
            param->options &= ~BT_LE_ADV_OPT_DIR_MODE_LOW_DUTY;
        else if (g_advtest_low_duty > 0)
            param->options |= BT_LE_ADV_OPT_DIR_MODE_LOW_DUTY;

        if (g_advtest_rpa_opt > 0)
            param->options |= BT_LE_ADV_OPT_DIR_ADDR_RPA;
        else if (g_advtest_rpa_opt == 0)
            param->options &= ~BT_LE_ADV_OPT_DIR_ADDR_RPA;
#endif
        break;
    case BT_LE_SCAN_RSP:
    case BT_LE_EXT_SCAN_RSP:
    case BT_LE_ADV_NONCONN_IND:
    case BT_LE_EXT_ADV_NONCONN_IND:
    case BT_LE_LEGACY_ADV_NONCONN_IND:
    case BT_LE_LEGACY_SCAN_RSP:
        break;
    default:
        BT_LOGE("%s, convert fail, invalid adv_type:%d", __func__, params->adv_type);
        return BT_STATUS_PARM_INVALID;
    }

    switch (params->own_addr_type) {
    case BT_LE_ADDR_TYPE_PUBLIC:
        param->options |= BT_LE_ADV_OPT_USE_IDENTITY;
        break;
    }

    switch (params->channel_map) {
    case BT_LE_ADV_CHANNEL_37_ONLY:
        param->options |= BT_LE_ADV_OPT_DISABLE_CHAN_38 | BT_LE_ADV_OPT_DISABLE_CHAN_39;
        break;
    case BT_LE_ADV_CHANNEL_38_ONLY:
        param->options |= BT_LE_ADV_OPT_DISABLE_CHAN_37 | BT_LE_ADV_OPT_DISABLE_CHAN_39;
        break;
    case BT_LE_ADV_CHANNEL_39_ONLY:
        param->options |= BT_LE_ADV_OPT_DISABLE_CHAN_37 | BT_LE_ADV_OPT_DISABLE_CHAN_38;
        break;
    case BT_LE_ADV_CHANNEL_DEFAULT:
        break;
    default:
        BT_LOGE("%s, convert fail, invalid channel_map:%d", __func__, params->channel_map);
        return BT_STATUS_PARM_INVALID;
    }

    param->interval_min = params->interval;
    param->interval_max = params->interval;

    if (params->adv_type == BT_LE_ADV_DIRECT_IND
        || params->adv_type == BT_LE_EXT_ADV_DIRECT_IND
        || params->adv_type == BT_LE_LEGACY_ADV_DIRECT_IND) {
        /* HCI LE Set Advertising Parameters 的 Peer_Address_Type 只允许
         * 0x00 public / 0x01 random；Core Vol 4 Part E 7.8.5 规定
         * ADV_DIRECT_IND 不能用 0x02/0x03（identity）。framework 的
         * BT_LE_ADDR_TYPE_PUBLIC_ID / RANDOM_ID 直接透传会被控制器以
         * 0x12 Invalid HCI Command Parameters 拒绝，这里先归一化。
         * 注意：这不改变"目标是身份地址还是 RPA"的语义 —— 身份地址
         * 作为定向目标时，privacy 对端用 RPA 发起连接仍然匹配不上，
         * 见 docs 里的第四组测试。 */
        switch (params->peer_addr_type) {
        case BT_LE_ADDR_TYPE_PUBLIC_ID:
            addr.type = BT_LE_ADDR_TYPE_PUBLIC;
            break;
        case BT_LE_ADDR_TYPE_RANDOM_ID:
            addr.type = BT_LE_ADDR_TYPE_RANDOM;
            break;
        default:
            addr.type = params->peer_addr_type;
            break;
        }

        memcpy(&addr.a, &params->peer_addr, sizeof(bt_address_t));
        param->peer = &addr;

#ifdef CONFIG_MYVENDOR_BLE_TEST_APP
        syslog(LOG_INFO,
               "adver: convert directed peer_type=%u peer=%02x:%02x:%02x:%02x:%02x:%02x\n",
               (unsigned)addr.type, addr.a.val[5], addr.a.val[4],
               addr.a.val[3], addr.a.val[2], addr.a.val[1],
               addr.a.val[0]);
#endif
    }

#ifdef CONFIG_MYVENDOR_BLE_TEST_APP
    syslog(LOG_INFO,
           "adver: convert adv_type=%u own=%u interval=%u options=0x%08x\n",
           (unsigned)params->adv_type, (unsigned)params->own_addr_type,
           (unsigned)params->interval, (unsigned)param->options);
#endif

    return BT_STATUS_SUCCESS;
}

static size_t pack_ad(struct bt_data* out, size_t max, const uint8_t* data, uint16_t len)
{
    size_t n = 0;
    size_t i = 0;

    if (out == NULL || data == NULL || len == 0 || max == 0) {
        return 0;
    }

    while (i < len && n < max) {
        if (data[i] < 1 || (i + 1u + (uint16_t)(data[i] - 1)) > len) {
            break;
        }

        out[n].data_len = data[i] - 1;
        out[n].type = data[i + 1];
        out[n].data = &data[i + 2];
        i += (size_t)out[n].data_len + 2;
        n++;
    }

    return n;
}

static sal_adapter_req_t* sal_adapter_req(bt_controller_id_t id, uint8_t adv_id, sal_func_t func)
{
    sal_adapter_req_t* req = calloc(sizeof(sal_adapter_req_t), 1);

    if (req) {
        req->id = id;
        req->adv_id = adv_id;
        req->func = func;
    }

    return req;
}

static void sal_invoke_async(service_work_t* work, void* userdata)
{
    sal_adapter_req_t* req = userdata;

    SAL_ASSERT(req);
    myvendor_sal_radio_ticket_wait(req->radio_ticket);
    req->func(req);
    myvendor_sal_radio_ticket_done();
    free(userdata);
}

static uint32_t sal_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static bt_status_t sal_send_req(sal_adapter_req_t* req)
{
    if (!req) {
        BT_LOGE("%s, req null", __func__);
        return BT_STATUS_PARM_INVALID;
    }

    req->radio_ticket = myvendor_sal_radio_ticket_issue();
    req->issued_ms = sal_now_ms();
    if (!service_loop_work((void*)req, sal_invoke_async, NULL)) {
        BT_LOGE("%s, service_loop_work failed", __func__);
        myvendor_sal_radio_ticket_wait(req->radio_ticket);
        myvendor_sal_radio_ticket_done();
        free(req);
        return BT_STATUS_FAIL;
    }

    return BT_STATUS_SUCCESS;
}

static void STACK_CALL(start_adv)(void* args)
{
    sal_adapter_req_t* req = args;
    struct bt_data ad[CONFIG_BT_EXT_ADV_MAX_ADV_SEGMENT];
    struct bt_data sd[CONFIG_BT_EXT_ADV_MAX_ADV_SEGMENT];
    size_t ad_n;
    size_t sd_n;
    int ret;
    uint32_t wait_ms = sal_now_ms() - req->issued_ms;

    if (sf32lb52_bt_hci_skip_sync()) {
        BT_LOGE("%s, skip HCI (recovering)", __func__);
        g_legacy_adv_id = 0;
        advertising_on_state_changed(req->adv_id, LE_ADVERTISING_STOPPED);
        return;
    }

    ad_n = pack_ad(ad, SAL_ARRAY_SIZE(ad), req->adpt.start_adv.adv_data,
        req->adpt.start_adv.adv_len);
    sd_n = pack_ad(sd, SAL_ARRAY_SIZE(sd), req->adpt.start_adv.scan_rsp_data,
        req->adpt.start_adv.scan_rsp_len);

    if (g_legacy_adv_id != 0) {
        (void)bt_le_adv_stop();
        g_legacy_adv_id = 0;
    }

#ifdef CONFIG_MYVENDOR_BLE_TEST_APP
    if (g_advfault_async && req->adpt.start_adv.param.peer != NULL) {
        /* 延迟 100 ms 再发 STOPPED：framework 的 advertiser_start_event
         * 是先 bt_sal_le_start_adv()、后 list_add_tail()；立即发 STOPPED
         * 会撞上“advertiser 还没登记”的窗口，事件被丢弃。真实 HCI 失败
         * 至少经过 ticket/HCI，不会这么快。 */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        g_advfault_async = 0;
        syslog(LOG_WARNING, "adver: injected async directed failure\n");
        nanosleep(&ts, NULL);
        advertising_on_state_changed(req->adv_id, LE_ADVERTISING_STOPPED);
        goto done;
    }
#endif

    ret = bt_le_adv_start(&req->adpt.start_adv.param,
        ad_n ? ad : NULL, ad_n,
        sd_n ? sd : NULL, sd_n);
    if (ret == -EALREADY) {
        (void)bt_le_adv_stop();
        ret = bt_le_adv_start(&req->adpt.start_adv.param,
            ad_n ? ad : NULL, ad_n,
            sd_n ? sd : NULL, sd_n);
    }

#ifdef CONFIG_MYVENDOR_BLE_TEST_APP
    /* 这一行是区分三种 status=2 的关键：
     *   wait >= 1000 ms  ⇒ 框架 1 s 定时器先到（ticket/扫描占射频），
     *                      即使 ret=0 上层也会报 START_TIMEOUT；
     *   ret != 0         ⇒ 参数/HCI 失败，看紧接着的 err:；
     *   ret = 0          ⇒ 控制器已接受，问题只可能在回调链路。 */
    syslog(LOG_INFO, "adver: start id=%u ticket=%u wait=%u ms ret=%d\n",
           (unsigned)req->adv_id, (unsigned)req->radio_ticket,
           (unsigned)wait_ms, ret);
#endif

    if (ret) {
        BT_LOGE("%s, bt_le_adv_start fail, err:%d", __func__, ret);
        /* 不通知 STARTED → 框架 1 s 超时 status=2。这里立刻 STOPPED，
         * 让上层清句柄并（若已禁定向）下一拍改走无名 ADV。 */
        advertising_on_state_changed(req->adv_id, LE_ADVERTISING_STOPPED);
        goto done;
    }

    g_legacy_adv_id = req->adv_id;
    advertising_on_state_changed(req->adv_id, LE_ADVERTISING_STARTED);

done:
    if (req->adpt.start_adv.adv_data)
        free(req->adpt.start_adv.adv_data);
    if (req->adpt.start_adv.scan_rsp_data)
        free(req->adpt.start_adv.scan_rsp_data);
}

bt_status_t bt_sal_le_start_adv(bt_controller_id_t id, uint8_t adv_id, ble_adv_params_t* params, uint8_t* adv_data, uint16_t adv_len, uint8_t* scan_rsp_data, uint16_t scan_rsp_len)
{
    sal_adapter_req_t* req;
    int ret;

    req = sal_adapter_req(id, adv_id, STACK_CALL(start_adv));
    if (!req) {
        BT_LOGE("%s, req null", __func__);
        return BT_STATUS_NOMEM;
    }

    ret = zblue_le_convert_param(params, &req->adpt.start_adv.param);
    if (ret) {
        BT_LOGE("%s, convert fail, err:%d", __func__, ret);
        ret = BT_STATUS_PARM_INVALID;
        goto error;
    }

    req->adpt.start_adv.param.options &= ~(BT_LE_ADV_OPT_EXT_ADV | BT_LE_ADV_OPT_NO_2M);

    if (adv_len && adv_data) {
        req->adpt.start_adv.adv_data = malloc(adv_len);
        if (!req->adpt.start_adv.adv_data) {
            BT_LOGE("%s, malloc fail", __func__);
            ret = BT_STATUS_NOMEM;
            goto error;
        }

        memcpy(req->adpt.start_adv.adv_data, adv_data, adv_len);
        req->adpt.start_adv.adv_len = adv_len;
    }

    if (scan_rsp_len && scan_rsp_data) {
        req->adpt.start_adv.scan_rsp_data = malloc(scan_rsp_len);
        if (!req->adpt.start_adv.scan_rsp_data) {
            BT_LOGE("%s, malloc fail", __func__);
            ret = BT_STATUS_NOMEM;
            goto error;
        }

        memcpy(req->adpt.start_adv.scan_rsp_data, scan_rsp_data, scan_rsp_len);
        req->adpt.start_adv.scan_rsp_len = scan_rsp_len;
    }

    ret = sal_send_req(req);
#ifdef CONFIG_MYVENDOR_BLE_TEST_APP
    syslog(LOG_INFO, "adver: request id=%u accepted=%d\n",
           (unsigned)adv_id, ret);
#endif
    return ret;

error:
    if (req->adpt.start_adv.adv_data)
        free(req->adpt.start_adv.adv_data);
    if (req->adpt.start_adv.scan_rsp_data)
        free(req->adpt.start_adv.scan_rsp_data);
    free(req);
    return ret;
}

static void STACK_CALL(stop_adv)(void* args)
{
    sal_adapter_req_t* req = args;
    int ret;

    if (sf32lb52_bt_hci_skip_sync()) {
        g_legacy_adv_id = 0;
        advertising_on_state_changed(req->adv_id, LE_ADVERTISING_STOPPED);
        return;
    }

    ret = bt_le_adv_stop();
    if (ret && ret != -EALREADY) {
        BT_LOGE("%s, bt_le_adv_stop fail, err:%d", __func__, ret);
    }

    g_legacy_adv_id = 0;
    advertising_on_state_changed(req->adv_id, LE_ADVERTISING_STOPPED);
}

bt_status_t bt_sal_le_stop_adv(bt_controller_id_t id, uint8_t adv_id)
{
    sal_adapter_req_t* req;

    req = sal_adapter_req(id, adv_id, STACK_CALL(stop_adv));
    if (!req) {
        BT_LOGE("%s, req null", __func__);
        return BT_STATUS_NOMEM;
    }

    return sal_send_req(req);
}
#endif /* CONFIG_BLUETOOTH_BLE_ADV */
