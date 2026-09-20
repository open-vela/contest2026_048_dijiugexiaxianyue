/**
 * @file helm_menu.c
 * @brief KEY1 长按菜单：对齐 pages/menu.html 及子页。
 */

#include "helm_menu.h"

#include "Version.h"
#include "bicycle_page_anima.h"
#include "bicycle_page_ids.h"
#include "board_malloc.h"
#include "helm_font.h"
#include "helm_font_lab.h"
#include "helm_palette.h"
#include "helm_pwr.h"
#include "helm_shell.h"
#include "helm_toolbox.h"
#include "helm_widget.h"
#include "live_map/map_page.h"
#include "lv_pm_bar.h"
#include "lv_pm_core.h"
#include "lv_pm_overlay.h"
#include "lv_pm_port.h"
#include "lvgl_page.h"
#include "bicycle_env.h"
#include "bicycle_gpx_sim.h"
#include "bicycle_runtime.h"
#include "bicycle_status_bar.h"
#include "myvendor_devctl.h"
#include "myvendor_gnss.h"
#include "myvendor_sys.h"
#include "myvendor_identity.h"
#include "myvendor_sound.h"
#include "myvendor_gpx.h"
#include "myvendor_mtp.h"
#include "sf32lb_dvfs.h"
#include "bicycle_ride_gpx.h"
#include "gpx_decode.h"
#include "Vendor/Board/lv_port/lv_port_buttons.h"
#include "vmap/vmap_config.h"
#include "vmap/vmap_geo.h"
#include "lvgl/src/draw/lv_draw_line.h"
#include "lvgl/src/draw/lv_draw_rect.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <malloc.h>
#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

#define HELM_MENU_STACK  8
#define HELM_MENU_ROWS   32
/** @brief GPX 文件名 UTF-8 上限（导入中文名；点阵字库盖不住，列表用 TTF）。 */
#define HELM_GPX_NAME_MAX  64

enum {
    HELM_KIND_GO = HELM_ITEM_GO,
    HELM_KIND_SW = HELM_ITEM_SW,
    HELM_KIND_VAL = HELM_ITEM_VAL,
    HELM_KIND_SLIM = HELM_ITEM_SLIM
};

typedef enum {
    HELM_SCR_ROOT = 0,
    HELM_SCR_NAV,
    HELM_SCR_GPX,
    HELM_SCR_SENSORS,
    HELM_SCR_RIDES,
    HELM_SCR_SETTINGS,
    HELM_SCR_TEST,
    HELM_SCR_EPH,
    HELM_SCR_ABOUT,
    HELM_SCR_FONTLAB,
    HELM_SCR_GRADECAL,
    HELM_SCR_INBOX,
    HELM_SCR_NAVPTS,
    HELM_SCR_NAVPT_DETAIL,
    HELM_SCR_NAVPT_ACTIONS,
    HELM_SCR_FAVS,
    HELM_SCR_SCAN,
    HELM_SCR_SCAN_PICK,
    HELM_SCR_PHONE,
    HELM_SCR_GPX_IMPORT,
    HELM_SCR_GPX_RECORD,
    HELM_SCR_RIDE_DETAIL,
    HELM_SCR_TOOLS,
    HELM_SCR_TOOLFACE,
    HELM_SCR_SYSSTAT,
    HELM_SCR_SYS_DISKS,
    HELM_SCR_SYS_MEMORY,
    HELM_SCR_SYS_THREADS,
    /** @brief 系统资源 → 卫星：按星座看"在视 / 锁定 / 参与定位"的颗数。 */
    HELM_SCR_SYS_GNSS
} helm_scr_t;

enum {
    ACT_NOP = 0,
    ACT_ENTER_NAV,
    ACT_ENTER_GPX,
    ACT_ENTER_SENSORS,
    ACT_ENTER_RIDES,
    ACT_ENTER_SETTINGS,
    ACT_ENTER_TEST,
    ACT_ENTER_EPH,
    ACT_ENTER_ABOUT,
    ACT_ENTER_FONTLAB,
    ACT_ENTER_GRADECAL,
    ACT_ENTER_INBOX,
    ACT_ENTER_NAVPTS,
    ACT_ENTER_NAVPT_REC,
    ACT_ENTER_FAVS,
    ACT_ENTER_SCAN_HR,
    ACT_ENTER_SCAN_CAD,
    ACT_ENTER_SCAN_PWR,
    ACT_ENTER_TOOLS,
    ACT_ENTER_SYSSTAT,
    ACT_ENTER_SYS_DISKS,
    ACT_ENTER_SYS_MEMORY,
    ACT_ENTER_SYS_THREADS,
    ACT_ENTER_SYS_GNSS,
    ACT_ENTER_PHONE,
    ACT_PAIR_OPEN,
    ACT_PAIR_UNBIND,
    ACT_TOOL_COMPASS,
    ACT_TOOL_LEVEL,
    ACT_TOOL_GMETER,
    ACT_TOOL_ALT,
    ACT_NAV_COORDS,
    ACT_NAV_FAV,
    ACT_NAV_FAV_START,
    ACT_NAV_STOP,
    ACT_NAV_SKIP,
    ACT_NAV_NEAREST,
    ACT_ENTER_GPX_IMPORT,
    ACT_ENTER_GPX_RECORD,
    ACT_RIDE_OPEN,
    ACT_RIDE_DEL,
    ACT_RIDE_NAV,
    ACT_RIDE_REV,
    ACT_RIDE_CONT,
    ACT_TOGGLE_BT,
    ACT_TOGGLE_SENSOR,
    ACT_CYCLE_BL,
    ACT_CYCLE_THEME,
    ACT_CYCLE_UNIT,
    ACT_CYCLE_TZ,
    ACT_CYCLE_GNSS_SOLVER,
    ACT_TOGGLE_AUTOPAUSE,
    ACT_TOGGLE_NOTIF,
    ACT_TOGGLE_CALLS,
    ACT_TOGGLE_USB,
    ACT_TOGGLE_SOUND,
    ACT_TOGGLE_EPH_AUTO,
    ACT_SCAN_CONNECT,
    ACT_SCAN_AUTO,
    ACT_SCAN_START,
    ACT_INBOX_OPEN,
    ACT_POWEROFF
};

typedef struct {
    char label[HELM_GPX_NAME_MAX];
    char sub[40];
    char value[24];
    uint8_t act;
    uint8_t extra;
    uint8_t kind;
    uint8_t progress;
    uint32_t progress_color;
    helm_ico_id_t ico;
    bool on;
    bool progress_on;
} helm_row_t;

typedef struct {
    helm_scr_t scr;
    uint8_t sel;
} helm_frame_t;

typedef struct {
    lv_obj_t * root;
    lv_obj_t * title;
    lv_obj_t * list;
    lv_obj_t * about;
    lv_obj_t * fontlab;
    lv_obj_t * gradecal;
    lv_obj_t * grade_val;
    lv_obj_t * grade_raw;
    lv_obj_t * grade_off;
    lv_obj_t * grade_imu;
    lv_obj_t * grade_baro;
    lv_obj_t * grade_alt;
    lv_obj_t * empty;
    lv_obj_t * skip_dock;
    lv_obj_t * del_dock;
    lv_obj_t * del_lab;
    lv_obj_t * ride;
    lv_obj_t * ride_track;
    lv_obj_t * ride_dist;
    lv_obj_t * ride_keys;
    lv_obj_t * toolface;
    lv_timer_t * poll;
    lv_timer_t * fav_plan_timer;
    lv_obj_t * plan_mask;
    lv_obj_t * plan_head;
    lv_obj_t * plan_msg;
    lv_obj_t * plan_hint;
    uint8_t fav_plan_phase;
    bool fav_planning;
    bool fav_plan_fail;
    bool fav_plan_kick;
    uint32_t fav_notice_t0;
    uint32_t fav_notice_ms;
    helm_frame_t stack[HELM_MENU_STACK];
    uint8_t sp;
    uint8_t count;
    uint8_t fav_order[MYVENDOR_DEVCTL_FAVORITE_MAX];
    uint8_t fav_order_n;
    helm_row_t items[HELM_MENU_ROWS];
} helm_menu_t;

static helm_menu_t * s_menu;
static int8_t s_list_enter_dir;
static bool s_unit_imperial;
static uint8_t s_scan_kind;
static uint8_t s_scan_phase;
static uint32_t s_scan_t0;
static uint32_t s_sensor_sig;
static uint8_t s_sensor_del_idx;
static bool s_sensor_del;
/** 「手机蓝牙」页：长按右键问过一遍之后，等这一下点击确认解绑。 */
static bool s_phone_unbind_arm;
/** 「手机蓝牙」页：点过「配对新手机」之后，等这一下点击确认开窗口。 */
static bool s_phone_pair_arm;
/** 「手机蓝牙」页上一帧画的是"已配对"还是"没配对"。
 *
 * 配对/解绑都是**异步**的（命令投给 companion 线程，它在下一拍才清记录/写记录），
 * 页面自己不会刷新 —— 实测"解绑后仍显示已配对"。所以在这一页挂一个 1 s 的看门狗
 * 定时器：**状态真的翻了才重画**（不是每秒都画，免得干扰选中/动画）。 */
static bool s_phone_shown_paired;
/** 上一帧显示的单配窗口剩余秒数（倒计时的重画基准）。 */
static unsigned s_phone_shown_sec;
static lv_timer_t * s_phone_watch;
static bool s_scan_jump;
static bool s_scan_link_wait;
static lv_obj_t * s_squash;
static bool s_squash_restore;
static helm_tool_id_t s_tool_id;

#define HELM_SCAN_OFF   0u
#define HELM_SCAN_WAIT  1u
#define HELM_SCAN_RUN   2u
#define HELM_SCAN_DONE  3u
#define HELM_POLL_MS    400u
#define HELM_GPX_LIST_MAX  HELM_MENU_ROWS
#define HELM_RIDE_PT_MAX   96
#define HELM_SYS_DISK_N    3
#define HELM_SYS_HEAP_N    3
#define HELM_SYS_THREAD_N  HELM_MENU_ROWS

/* 系统页"磁盘"那一栏的刷新周期。
 *
 * **别调小**：这个循环对三个挂载点调 statfs()，其中 /mnt/lfs 和 /mnt/kv
 * 是两个 LittleFS 卷，而 littlefs_statfs() 会 lfs_fs_size() **遍历整棵元数据
 * 树且完全不缓存**（~55 ms/卷）。原先这里写的是 2000 ms，也就是每 2 秒在 UI
 * 线程上白烧约 110 ms —— diag 曾因为同一处 API 占 22% CPU，见 sf32lb_sdio.c
 * 里 sf32lb_sd_fs_ok() 的注释。容量变化本来就慢，30 秒足够；进页面时走的是
 * force=true，所以首屏数字仍然是即时的。 */
#define HELM_SYS_DISK_MS   30000u

#define HELM_STAT_MAGIC    0x31545348u /* HST1 */
#define HELM_STAT_VER      4u
#define HELM_STAT_PATH     "/mnt/lfs/ride_stat"
#define HELM_STAT_TMP      "/mnt/lfs/ride_stat.tmp"

typedef struct {
    uint64_t total;
    uint64_t used;
    uint64_t free;
    bool valid;
} helm_sys_usage_t;

typedef struct {
    pid_t pid;
    char name[32];
    size_t stack_total;
    size_t stack_used;
    uint16_t cpu_permille;
    bool cpu_valid;
} helm_sys_thread_t;

typedef struct {
    helm_sys_thread_t thread[HELM_SYS_THREAD_N];
    uint8_t count;
    uint8_t seen;
} helm_sys_threads_t;

static const char * const s_sys_disk_path[HELM_SYS_DISK_N] = {
    "/mnt/lfs", "/mnt/fat", "/mnt/kv"
};

static const char * const s_sys_disk_name[HELM_SYS_DISK_N] = {
    "记录盘", "地图盘", "数据盘"
};

static const helm_ico_id_t s_sys_disk_icon[HELM_SYS_DISK_N] = {
    HELM_ICO_SAVE, HELM_ICO_NAV, HELM_ICO_CHIP
};

static const char * const s_sys_heap_name[HELM_SYS_HEAP_N] = {
    "片内 SRAM", "系统 PSRAM", "图形 PSRAM"
};

static helm_sys_usage_t s_sys_disks[HELM_SYS_DISK_N];
static helm_sys_usage_t s_sys_heaps[HELM_SYS_HEAP_N];
static helm_sys_threads_t s_sys_threads;
static uint32_t s_sys_disk_tick;
static uint32_t s_sys_heap_tick;
static uint32_t s_sys_thread_tick;
static uint32_t s_sys_stack_tick;
static uint32_t s_sys_count_tick;
static uint32_t s_sys_ui_sig;
static uint8_t s_sys_thread_n;
static bool s_sys_disk_ready;
static bool s_sys_heap_ready;
static bool s_sys_thread_ready;
static bool s_sys_stack_ready;
static bool s_sys_count_ready;

static char s_gpx_names[HELM_GPX_LIST_MAX][HELM_GPX_NAME_MAX];
static uint32_t s_gpx_size[HELM_GPX_LIST_MAX];
static uint32_t s_gpx_mtime[HELM_GPX_LIST_MAX];
static uint8_t s_gpx_n;
static const char * s_gpx_dir;

/** 两个目录字符串是否指向同一个目录。
 *
 * 原来这三处用 `==` 比指针：同一个宏字面量还能被编译器合并而"侥幸成立"，
 * 但和运行期传入的路径比就几乎永远为假（-Waddress 已提示）。改成比字符串。
 */
static bool gpx_same_dir(const char * a, const char * b)
{
  if (a == NULL || b == NULL)
    {
      return a == b;
    }

  return strcmp(a, b) == 0;
}
static char s_ride_name[HELM_GPX_NAME_MAX];
static char s_ride_loaded[HELM_GPX_NAME_MAX];
static const char * s_ride_dir;
static const char * s_ride_loaded_dir;
static char s_navpt_path[160];
static char s_navpt_title[HELM_GPX_NAME_MAX];
static float s_ride_lon[HELM_RIDE_PT_MAX];
static float s_ride_lat[HELM_RIDE_PT_MAX];
static uint16_t s_ride_pt_n;
static double s_ride_km;
static uint32_t s_ride_sec;
static bool s_ride_has_time;

typedef struct {
    char name[HELM_GPX_NAME_MAX];
    const char * dir;
    double km;
    uint32_t sec;
    uint32_t size;
    uint32_t mtime;
    bool have_time;
    bool ok;
} helm_gpx_stat_t;

typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t n;
} helm_stat_hdr_t;

typedef struct {
    char name[HELM_GPX_NAME_MAX];
    uint32_t size;
    uint32_t mtime;
    uint32_t dist_m;
    uint32_t sec;
    uint8_t have_time;
    uint8_t pad[3];
} helm_stat_rec_t;

static helm_gpx_stat_t s_gpx_stat[HELM_GPX_LIST_MAX];
static uint8_t s_gpx_stat_n;
static bool s_stat_loaded;
static char s_stat_pend_name[HELM_GPX_NAME_MAX];
static double s_stat_pend_km;
static uint32_t s_stat_pend_sec;
static bool s_stat_pend;

static uint8_t helm_sensor_n(void)
{
    myvendor_sys_sensor_ui_t ui;
    uint8_t n = 0;
    uint8_t i;

    myvendor_sys_sensor_ui_get(&ui);
    for (i = 0; i < MYVENDOR_SYS_SENSOR_KIND_N; i++) {
        if (ui.slot[i].link == MYVENDOR_SYS_SENSOR_LINK_READY) {
            n++;
        }
    }

    return n;
}

static const char * helm_scan_kind_lab(uint8_t kind)
{
    if (kind == MYVENDOR_SYS_SENSOR_KIND_CSC) {
        return "踏频";
    }

    if (kind == MYVENDOR_SYS_SENSOR_KIND_CPS) {
        return "功率";
    }

    return "心率";
}

static const char * helm_scan_anon(uint8_t kind)
{
    if (kind == MYVENDOR_SYS_SENSOR_KIND_CSC) {
        return "踏频器";
    }

    if (kind == MYVENDOR_SYS_SENSOR_KIND_CPS) {
        return "功率计";
    }

    return "心率带";
}

static helm_ico_id_t helm_sensor_ico(uint8_t kind)
{
    if (kind == MYVENDOR_SYS_SENSOR_KIND_CSC) {
        return HELM_ICO_CAD;
    }

    if (kind == MYVENDOR_SYS_SENSOR_KIND_CPS) {
        return HELM_ICO_BOLT;
    }

    return HELM_ICO_HR;
}

static void helm_slot_status(char * val, size_t vn, char * sub, size_t sn,
                             const myvendor_sys_sensor_slot_t * slot,
                             bool radio)
{
    if (val != NULL && vn > 0) {
        val[0] = '\0';
    }

    if (sub != NULL && sn > 0) {
        sub[0] = '\0';
    }

    if (!radio) {
        if (val != NULL) {
            lv_snprintf(val, vn, "外设未开");
        }

        return;
    }

    if (slot == NULL ||
        slot->link == MYVENDOR_SYS_SENSOR_LINK_IDLE) {
        if (val != NULL) {
            lv_snprintf(val, vn, "未连接");
        }

        return;
    }

    if (slot->link == MYVENDOR_SYS_SENSOR_LINK_CONNECTING) {
        if (val != NULL) {
            lv_snprintf(val, vn, "连接中");
        }

        return;
    }

    if (val != NULL) {
        lv_snprintf(val, vn, "已连接");
    }

    if (sub == NULL || sn == 0) {
        return;
    }

    if (slot->bat_pct >= 0 && slot->bat_pct <= 100) {
        lv_snprintf(sub, sn, "电量 %d%%", (int)slot->bat_pct);
    } else {
        lv_snprintf(sub, sn, "电量未知");
    }
}

static void helm_fmt_pct(char * buf, size_t n, float v)
{
    int g10 = (int)(v * 10.0f + (v >= 0.0f ? 0.5f : -0.5f));
    int a = g10 / 10;
    int b = g10 % 10;

    if (b < 0) {
        b = -b;
    }

    if (g10 < 0 && a == 0) {
        lv_snprintf(buf, n, "-0.%d%%", b);
        return;
    }

    lv_snprintf(buf, n, "%d.%d%%", a, b);
}

static void helm_grade_refresh(helm_menu_t * m)
{
    char buf[24];

    if (m == NULL || m->gradecal == NULL) {
        return;
    }

    bicycle_env_tick();
    if (m->grade_val) {
        if (bicycle_env_imu_valid()) {
            helm_fmt_pct(buf, sizeof(buf), bicycle_env_grade_pct());
        } else {
            lv_snprintf(buf, sizeof(buf), "--");
        }

        lv_label_set_text(m->grade_val, buf);
    }

    if (m->grade_raw) {
        if (bicycle_env_imu_valid()) {
            helm_fmt_pct(buf, sizeof(buf), bicycle_env_raw_grade_pct());
        } else {
            lv_snprintf(buf, sizeof(buf), "--");
        }

        lv_label_set_text(m->grade_raw, buf);
    }

    if (m->grade_off) {
        helm_fmt_pct(buf, sizeof(buf), bicycle_env_offset_pct());
        lv_label_set_text(m->grade_off, buf);
    }

    if (m->grade_imu) {
        lv_label_set_text(m->grade_imu,
                          bicycle_env_imu_valid() ? "已就绪" : "等待中");
    }

    if (m->grade_baro) {
        if (bicycle_env_baro_valid()) {
            int h10 = (int)(bicycle_env_hpa() * 10.0f + 0.5f);

            lv_snprintf(buf, sizeof(buf), "%d.%d hPa", h10 / 10, h10 % 10);
            lv_label_set_text(m->grade_baro, buf);
        } else {
            lv_label_set_text(m->grade_baro, "--");
        }
    }

    if (m->grade_alt) {
        if (bicycle_env_baro_valid()) {
            lv_snprintf(buf, sizeof(buf), "%dm",
                        (int)(bicycle_env_altitude_m() + 0.5f));
            lv_label_set_text(m->grade_alt, buf);
        } else {
            lv_label_set_text(m->grade_alt, "--");
        }
    }
}

static void helm_scan_tick(void)
{
    myvendor_sys_sensor_ui_t ui;

    if (s_scan_phase == HELM_SCAN_OFF) {
        return;
    }

    myvendor_sys_sensor_ui_get(&ui);
    if (ui.scanning) {
        s_scan_phase = HELM_SCAN_RUN;
        return;
    }

    if (s_scan_phase == HELM_SCAN_RUN) {
        s_scan_phase = HELM_SCAN_DONE;
        return;
    }

    if (s_scan_phase == HELM_SCAN_WAIT &&
        lv_tick_elaps(s_scan_t0) >= MYVENDOR_DEVCTL_SENSOR_SCAN_MS_DEFAULT) {
        s_scan_phase = HELM_SCAN_DONE;
    }
}

static void helm_scan_begin(void)
{
    s_scan_t0 = lv_tick_get();
    s_sensor_sig = 0;
    s_scan_jump = true;
    if (!myvendor_devctl_sensor_get()) {
        s_scan_phase = HELM_SCAN_OFF;
        return;
    }

    (void)myvendor_devctl_sensor_scan(MYVENDOR_DEVCTL_SENSOR_SCAN_MS_DEFAULT);
    s_scan_phase = HELM_SCAN_WAIT;
}

static void helm_scan_idle_ui(void)
{
    s_scan_phase = HELM_SCAN_OFF;
    s_scan_jump = false;
}

static void helm_scan_end(void)
{
    if (s_scan_phase != HELM_SCAN_OFF) {
        (void)myvendor_devctl_sensor_scan_stop();
    }

    s_scan_link_wait = false;
    helm_scan_idle_ui();
}

static void helm_scan_connect_idx(uint8_t idx)
{
    myvendor_sys_sensor_ui_t ui;
    uint8_t k;

    if (idx == 0) {
        return;
    }

    /** 一地址一槽位：这台设备已经被别的类型占着，本地就拒掉。
     *
     *  服务侧（ble_sensor_connect_index_kind）用同一判据也会拒，但那边只能打
     *  日志，界面会白等一场「正在连接」。用 UI 已拿到的槽位快照先判，能立刻给
     *  用户一句话。正常情况列表会把 linked 的行藏掉，走到这里的是「已绑定、
     *  当前 IDLE 等重连」的边角。 */
    myvendor_sys_sensor_ui_get(&ui);

    {
        static const uint8_t zaddr[MYVENDOR_SYS_SENSOR_ADDR_LEN];

        for (k = 0; k < MYVENDOR_SYS_SENSOR_KIND_N; k++) {
            if (k == s_scan_kind ||
                ui.slot[k].link == MYVENDOR_SYS_SENSOR_LINK_IDLE) {
                continue;
            }

            if (idx <= ui.found_n &&
                memcmp(ui.found[idx - 1].addr, zaddr, sizeof(zaddr)) != 0 &&
                memcmp(ui.found[idx - 1].addr, ui.slot[k].addr,
                       sizeof(zaddr)) == 0) {
                char msg[32];

                lv_snprintf(msg, sizeof(msg), "已被%s占用",
                            helm_scan_kind_lab(k));
                lv_pm_notify_show("蓝牙设备", msg, 2000);
                myvendor_sound_warn();
                return;
            }
        }
    }

    /* 按**当前页面**的类型绑定：同一台设备既广播 CSC 又广播 CPS（甚至 HR）时，
     * 交给服务侧「取第一种匹配」会连到别的槽位上去。
     *
     * 寻址用 **MAC**，不用扫描表下标：`idx` 是这张**活表**的序号 —— 扫描回调
     * 会并发改写它（"更弱的设备被替换"那条规则），而且**每次用户起扫都会整表清空**。
     * 从"界面显示第 idx 项"到"底层按 idx 查表"之间只要表变过，就会连到别的设备
     * （甚至全零地址）。MAC 在显示那一刻就已确定，之后表怎么变都指向同一台。
     * 上面那段"已被 XX 占用"的检查本来就在用 `ui.found[idx-1].addr`。 */
    (void)myvendor_devctl_sensor_connect_addr(ui.found[idx - 1].addr,
                                              ui.found[idx - 1].addr_type,
                                              s_scan_kind,
                                              ui.found[idx - 1].name);
    /* connect_index 自己停观察者。这里再 post SCAN_STOP 会盖掉 CONNECT。 */
    helm_scan_idle_ui();
    s_scan_link_wait = true;
    lv_pm_notify_show("蓝牙设备", "正在连接", 1500);
}

static bool helm_scr_needs_sensor_poll(helm_scr_t scr)
{
    return scr == HELM_SCR_SCAN || scr == HELM_SCR_SCAN_PICK ||
           scr == HELM_SCR_SENSORS || scr == HELM_SCR_INBOX ||
           scr == HELM_SCR_EPH || scr == HELM_SCR_SYSSTAT ||
           scr == HELM_SCR_SYS_DISKS || scr == HELM_SCR_SYS_MEMORY ||
           scr == HELM_SCR_SYS_THREADS || scr == HELM_SCR_SYS_GNSS;
}

static uint32_t helm_sensor_sig(void)
{
    myvendor_sys_sensor_ui_t ui;
    uint32_t s;
    uint8_t i;

    myvendor_sys_sensor_ui_get(&ui);
    s = (uint32_t)ui.scanning | ((uint32_t)ui.found_n << 1) |
        ((uint32_t)s_scan_phase << 8) |
        (myvendor_devctl_sensor_get() ? 0x1000u : 0);
    {
        uint8_t inbox_n = 0;

        myvendor_sys_inbox_get(NULL, &inbox_n, 0);
        s ^= (uint32_t)inbox_n << 20;
    }
    for (i = 0; i < MYVENDOR_SYS_SENSOR_KIND_N; i++) {
        s ^= (uint32_t)ui.slot[i].link << (12u + i * 2u);
        s ^= (uint32_t)(uint8_t)ui.slot[i].bat_pct << (16u + i);
        s ^= (uint32_t)(uint8_t)ui.slot[i].name[0] << (18u + i);
        s ^= (uint32_t)ui.slot[i].addr[0] << (i & 7u);
        s ^= (uint32_t)ui.slot[i].addr[1] << ((i + 3u) & 7u);
    }

    for (i = 0; i < ui.found_n; i++) {
        s ^= ((uint32_t)ui.found[i].table_idx << (i & 7u)) ^
             (uint32_t)(uint8_t)ui.found[i].name[0];
    }

    s ^= myvendor_devctl_sensor_rec_gen();
    return s;
}

static uint8_t helm_sys_pct(uint64_t part, uint64_t total)
{
    if (total == 0) {
        return 0;
    }

    if (part >= total) {
        return 100;
    }

    return (uint8_t)((part * 100ull + total / 2ull) / total);
}

static void helm_sys_row_bar_ex(helm_menu_t * m, uint8_t row, uint8_t fill,
                                uint8_t used)
{
    if (m == NULL || row >= HELM_MENU_ROWS) {
        return;
    }

    m->items[row].progress_on = true;
    m->items[row].progress = fill > 100u ? 100u : fill;
    if (used >= 90u) {
        m->items[row].progress_color = HELM_COLOR_HR;
    } else if (used >= 70u) {
        m->items[row].progress_color = HELM_COLOR_YEL;
    } else {
        m->items[row].progress_color = HELM_COLOR_CAD;
    }
}

/** @brief 总览条：按已用比例填充。 */
static void helm_sys_row_bar(helm_menu_t * m, uint8_t row, uint8_t used)
{
    helm_sys_row_bar_ex(m, row, used, used);
}

/** @brief 子页条：按剩余比例填充，未使用时满条绿色。 */
static void helm_sys_row_free_bar(helm_menu_t * m, uint8_t row, uint8_t used)
{
    uint8_t fill = (used >= 100u) ? 0u : (uint8_t)(100u - used);

    helm_sys_row_bar_ex(m, row, fill, used);
}

static void helm_sys_fmt_size(char * buf, size_t n, uint64_t bytes)
{
    if (bytes >= 1024ull * 1024ull) {
        uint64_t mb10 = (bytes * 10ull) / (1024ull * 1024ull);

        lv_snprintf(buf, n, "%lu.%luM", (unsigned long)(mb10 / 10ull),
                    (unsigned long)(mb10 % 10ull));
    } else {
        lv_snprintf(buf, n, "%luK",
                    (unsigned long)((bytes + 512ull) / 1024ull));
    }
}

static void helm_sys_fmt_pair(char * buf, size_t n, uint64_t used,
                              uint64_t total)
{
    char a[16];
    char b[16];

    helm_sys_fmt_size(a, sizeof(a), used);
    helm_sys_fmt_size(b, sizeof(b), total);
    lv_snprintf(buf, n, "%s / %s", a, b);
}

static void helm_sys_refresh_disks(bool force)
{
    uint32_t now = lv_tick_get();
    unsigned i;

    if (!force && s_sys_disk_ready &&
        lv_tick_elaps(s_sys_disk_tick) < HELM_SYS_DISK_MS) {
        return;
    }

    for (i = 0; i < HELM_SYS_DISK_N; i++) {
        struct statfs fs;
        uint64_t total;
        uint64_t free;

        if (i == 0u && myvendor_mtp_lfs_quiesce()) {
            continue;
        }

        memset(&fs, 0, sizeof(fs));
        if (statfs(s_sys_disk_path[i], &fs) != 0 || fs.f_bsize == 0 ||
            fs.f_blocks == 0) {
            continue;
        }

        total = (uint64_t)fs.f_bsize * (uint64_t)fs.f_blocks;
        free = (uint64_t)fs.f_bsize * (uint64_t)fs.f_bavail;
        s_sys_disks[i].total = total;
        s_sys_disks[i].free = free;
        s_sys_disks[i].used = total > free ? total - free : 0;
        s_sys_disks[i].valid = true;
    }

    s_sys_disk_tick = now;
    s_sys_disk_ready = true;
}

static void helm_sys_usage_from_mallinfo(helm_sys_usage_t * usage,
                                         const struct mallinfo * info)
{
    if (usage == NULL || info == NULL || info->arena <= 0) {
        return;
    }

    usage->total = (uint64_t)info->arena;
    usage->used = (uint64_t)info->uordblks;
    usage->free = (uint64_t)info->fordblks;
    usage->valid = true;
}

static void helm_sys_refresh_heaps(bool force)
{
    uint32_t now = lv_tick_get();
    struct mallinfo info;

    if (!force && s_sys_heap_ready &&
        lv_tick_elaps(s_sys_heap_tick) < 2000u) {
        return;
    }

    memset(s_sys_heaps, 0, sizeof(s_sys_heaps));
    if (board_umem_region_mallinfo(0, &info)) {
        helm_sys_usage_from_mallinfo(&s_sys_heaps[0], &info);
    }

#if CONFIG_MM_REGIONS > 1
    if (board_umem_region_mallinfo(1, &info)) {
        helm_sys_usage_from_mallinfo(&s_sys_heaps[1], &info);
    }
#endif

    if (board_psram_heap_ready()) {
        info = board_psram_mallinfo();
        helm_sys_usage_from_mallinfo(&s_sys_heaps[2], &info);
    }

    s_sys_heap_tick = now;
    s_sys_heap_ready = true;
}

static bool s_sys_collect_stack;

static void helm_sys_collect_thread(FAR struct tcb_s * tcb, FAR void * arg)
{
    helm_sys_threads_t * out = (helm_sys_threads_t *)arg;
    helm_sys_thread_t * item;

    if (tcb == NULL || out == NULL) {
        return;
    }

    if (out->seen < UINT8_MAX) {
        out->seen++;
    }

    if (out->count >= HELM_SYS_THREAD_N) {
        return;
    }

    item = &out->thread[out->count++];
    memset(item, 0, sizeof(*item));
    item->pid = tcb->pid;
    item->stack_total = tcb->adj_stack_size;
#ifdef CONFIG_STACK_COLORATION
    if (s_sys_collect_stack) {
        item->stack_used = up_check_tcbstack(tcb, tcb->adj_stack_size);
    }
#endif
    lv_snprintf(item->name, sizeof(item->name), "%s", get_task_name(tcb));
}

static void helm_sys_count_one(FAR struct tcb_s * tcb, FAR void * arg)
{
    uint8_t * n = (uint8_t *)arg;

    if (tcb != NULL && n != NULL && *n < UINT8_MAX) {
        (*n)++;
    }
}

static void helm_sys_refresh_thread_count(bool force)
{
    uint32_t now = lv_tick_get();

    if (!force && s_sys_count_ready &&
        lv_tick_elaps(s_sys_count_tick) < 2000u) {
        return;
    }

    s_sys_thread_n = 0;
    nxsched_foreach(helm_sys_count_one, &s_sys_thread_n);
    s_sys_count_tick = now;
    s_sys_count_ready = true;
}

static void helm_sys_refresh_threads(bool force)
{
    pid_t old_pid[HELM_SYS_THREAD_N];
    size_t old_used[HELM_SYS_THREAD_N];
    uint32_t now = lv_tick_get();
    uint8_t old_n;
    uint8_t i;
    uint8_t j;
    bool want_stack;

    if (!force && s_sys_thread_ready &&
        lv_tick_elaps(s_sys_thread_tick) < 1200u) {
        return;
    }

    want_stack = force || !s_sys_stack_ready ||
                 lv_tick_elaps(s_sys_stack_tick) >= 4000u;
    old_n = s_sys_threads.count;
    for (i = 0; i < old_n; i++) {
        old_pid[i] = s_sys_threads.thread[i].pid;
        old_used[i] = s_sys_threads.thread[i].stack_used;
    }

    memset(&s_sys_threads, 0, sizeof(s_sys_threads));
    s_sys_collect_stack = want_stack;
    nxsched_foreach(helm_sys_collect_thread, &s_sys_threads);
    s_sys_collect_stack = false;

    for (i = 0; i < s_sys_threads.count; i++) {
        helm_sys_thread_t * item = &s_sys_threads.thread[i];

#ifdef CONFIG_STACK_COLORATION
        if (!want_stack) {
            uint8_t k;

            for (k = 0; k < old_n; k++) {
                if (old_pid[k] == item->pid) {
                    item->stack_used = old_used[k];
                    break;
                }
            }
        }
#else
        LV_UNUSED(old_n);
        LV_UNUSED(old_pid);
        LV_UNUSED(old_used);
        LV_UNUSED(want_stack);
#endif
#ifndef CONFIG_SCHED_CPULOAD_NONE
        {
            struct cpuload_s load;

            if (clock_cpuload(item->pid, &load) == 0 && load.total > 0) {
                uint64_t p = (uint64_t)load.active * 1000ull /
                             (uint64_t)load.total;

                item->cpu_permille = (uint16_t)(p > 1000ull ? 1000ull : p);
                item->cpu_valid = true;
            }
        }
#endif
    }

    /* PID 顺序不会随负载跳动，按键浏览时选中项保持稳定。 */
    for (i = 1; i < s_sys_threads.count; i++) {
        helm_sys_thread_t cur = s_sys_threads.thread[i];

        j = i;
        while (j > 0 && s_sys_threads.thread[j - 1u].pid > cur.pid) {
            s_sys_threads.thread[j] = s_sys_threads.thread[j - 1u];
            j--;
        }

        s_sys_threads.thread[j] = cur;
    }

    s_sys_thread_n = s_sys_threads.seen;
    s_sys_thread_tick = now;
    s_sys_thread_ready = true;
    s_sys_count_tick = now;
    s_sys_count_ready = true;
    if (want_stack) {
        s_sys_stack_tick = now;
        s_sys_stack_ready = true;
    }
}

static uint8_t helm_sys_cpu_used(void)
{
    uint32_t mhz = sf32lb_dvfs_hclk_mhz();
    uint8_t idle;

    if (mhz != 0) {
        idle = sf32lb_dvfs_idle_pct();
        if (idle > 100u) {
            idle = 100u;
        }

        return (uint8_t)(100u - idle);
    }

#ifndef CONFIG_SCHED_CPULOAD_NONE
    {
        struct cpuload_s load;

        if (clock_cpuload(0, &load) == 0 && load.total > 0) {
            uint64_t idle_pct = (uint64_t)load.active * 100ull /
                                (uint64_t)load.total;

            return (uint8_t)(idle_pct < 100ull ? 100ull - idle_pct : 0ull);
        }
    }
#endif

    return 0;
}

static uint32_t helm_sys_cpu_mhz(void)
{
    return sf32lb_dvfs_hclk_mhz();
}

static uint32_t helm_sys_items_sig(const helm_menu_t * m);

static void helm_row_set(helm_menu_t * m, uint8_t i, helm_ico_id_t ico,
                         const char * lab, const char * sub, const char * val,
                         uint8_t act, uint8_t kind);
static void helm_row_sw(helm_menu_t * m, uint8_t i, helm_ico_id_t ico,
                        const char * lab, bool on, uint8_t act);

static bool helm_addr_nz(const uint8_t *addr)
{
    static const uint8_t z[MYVENDOR_SYS_SENSOR_ADDR_LEN];

    return addr != NULL &&
           memcmp(addr, z, MYVENDOR_SYS_SENSOR_ADDR_LEN) != 0;
}

static bool helm_addr_eq(const uint8_t *a, const uint8_t *b)
{
    return a != NULL && b != NULL &&
           memcmp(a, b, MYVENDOR_SYS_SENSOR_ADDR_LEN) == 0;
}

static bool helm_fill_scan_recs(helm_menu_t * m, uint8_t * n)
{
    myvendor_sys_sensor_ui_t ui;
    myvendor_devctl_sensor_rec_t recs[MYVENDOR_DEVCTL_SENSOR_REC_MAX];
    size_t rec_n = 0;
    uint8_t i;
    char sub[24];
    bool radio;

    myvendor_sys_sensor_ui_get(&ui);
    radio = myvendor_devctl_sensor_get();
    (void)myvendor_devctl_sensor_recs_get(recs, MYVENDOR_DEVCTL_SENSOR_REC_MAX,
                                          &rec_n);
    lv_snprintf(sub, sizeof(sub), "%s · BLE", helm_scan_kind_lab(s_scan_kind));

    if (*n < HELM_MENU_ROWS) {
        helm_row_set(m, *n, HELM_ICO_BLE, "扫描设备", "查找附近设备",
                     radio ? "" : "外设未开",
                     ACT_SCAN_START, HELM_KIND_GO);
        (*n)++;
    }

    for (i = 0; i < rec_n && *n < HELM_MENU_ROWS; i++) {
        const char * nm;
        const char * val = "";
        char row_sub[24];
        helm_ico_id_t ico = helm_sensor_ico(recs[i].kind);
        bool linked = false;

        if (recs[i].kind != s_scan_kind) {
            continue;
        }

        nm = recs[i].name[0] != '\0' ? recs[i].name : helm_scan_anon(s_scan_kind);
        lv_snprintf(row_sub, sizeof(row_sub), "%s", sub);
        if (s_scan_kind < MYVENDOR_SYS_SENSOR_KIND_N &&
            helm_addr_nz(ui.slot[s_scan_kind].addr) &&
            helm_addr_eq(ui.slot[s_scan_kind].addr, recs[i].addr) &&
            ui.slot[s_scan_kind].link != MYVENDOR_SYS_SENSOR_LINK_IDLE) {
            linked = true;
        }

        if (recs[i].autorc) {
            ico = HELM_ICO_STAR;
        }

        if (linked &&
            ui.slot[s_scan_kind].link == MYVENDOR_SYS_SENSOR_LINK_READY) {
            val = "已连接";
            if (recs[i].autorc) {
                lv_snprintf(row_sub, sizeof(row_sub), "回连");
            }
        } else if (linked) {
            val = "连接中";
        } else if (recs[i].autorc) {
            val = "回连";
        }

        helm_row_set(m, *n, ico, nm, row_sub, val, ACT_SCAN_AUTO, HELM_KIND_GO);
        m->items[*n].extra = (uint8_t)i;
        (*n)++;
    }

    if (radio && s_scan_kind < MYVENDOR_SYS_SENSOR_KIND_N &&
        ui.slot[s_scan_kind].link != MYVENDOR_SYS_SENSOR_LINK_IDLE) {
        bool have = false;
        uint8_t r;

        if (helm_addr_nz(ui.slot[s_scan_kind].addr)) {
            for (r = 0; r < rec_n; r++) {
                if (recs[r].kind == s_scan_kind &&
                    helm_addr_eq(recs[r].addr, ui.slot[s_scan_kind].addr)) {
                    have = true;
                    break;
                }
            }
        }

        if (!have && *n < HELM_MENU_ROWS) {
            const char * nm = ui.slot[s_scan_kind].name[0] != '\0' ?
                              ui.slot[s_scan_kind].name :
                              helm_scan_anon(s_scan_kind);
            const char * val = (ui.slot[s_scan_kind].link ==
                                MYVENDOR_SYS_SENSOR_LINK_READY) ?
                               "已连接" : "连接中";

            helm_row_set(m, (*n)++, HELM_ICO_BLE, nm, sub, val, ACT_NOP,
                         HELM_KIND_GO);
        }
    }

    return *n == 0;
}

static bool helm_fill_scan_pick(helm_menu_t * m, uint8_t * n)
{
    myvendor_sys_sensor_ui_t ui;
    uint8_t i;
    uint8_t bit;
    char sub[24];
    bool radio;
    bool wait;

    helm_scan_tick();
    myvendor_sys_sensor_ui_get(&ui);
    radio = myvendor_devctl_sensor_get();
    bit = (uint8_t)(1u << s_scan_kind);
    lv_snprintf(sub, sizeof(sub), "%s · BLE", helm_scan_kind_lab(s_scan_kind));
    wait = s_scan_link_wait ||
           (s_scan_kind < MYVENDOR_SYS_SENSOR_KIND_N &&
            ui.slot[s_scan_kind].link == MYVENDOR_SYS_SENSOR_LINK_CONNECTING);

    if (wait && s_scan_kind < MYVENDOR_SYS_SENSOR_KIND_N &&
        *n < HELM_MENU_ROWS) {
        const char * nm = ui.slot[s_scan_kind].name[0] != '\0' ?
                          ui.slot[s_scan_kind].name :
                          helm_scan_anon(s_scan_kind);
        const char * val = (ui.slot[s_scan_kind].link ==
                            MYVENDOR_SYS_SENSOR_LINK_READY) ?
                           "已连接" : "连接中";

        helm_row_set(m, (*n)++, helm_sensor_ico(s_scan_kind), nm, sub, val,
                     ACT_NOP, HELM_KIND_GO);
        return *n == 0;
    }

    if (radio) {
        for (i = 0; i < ui.found_n && *n < HELM_MENU_ROWS; i++) {
            const char * nm;

            if ((ui.found[i].kind_mask & bit) == 0 || ui.found[i].linked) {
                continue;
            }

            nm = ui.found[i].name[0] != '\0' ? ui.found[i].name :
                 helm_scan_anon(s_scan_kind);
            helm_row_set(m, *n, helm_sensor_ico(s_scan_kind), nm, sub, "",
                         ACT_SCAN_CONNECT, HELM_KIND_GO);
            m->items[*n].extra = ui.found[i].table_idx;
            (*n)++;
        }
    }

    return *n == 0;
}

static const char * helm_bl_name(uint8_t pwm)
{
    static char s[8];

    LV_UNUSED(pwm);
    lv_snprintf(s, sizeof(s), "%u%%", (unsigned)helm_pwr_bl_ui());
    return s;
}

static void helm_tz_fmt(char * buf, size_t n)
{
    int16_t m = myvendor_devctl_tz_min_get();
    unsigned mag;
    unsigned hh;
    unsigned mm;
    char sign;

    if (m < 0) {
        sign = '-';
        mag = (unsigned)(-m);
    } else {
        sign = '+';
        mag = (unsigned)m;
    }

    hh = mag / 60u;
    mm = mag % 60u;
    if (mm == 0) {
        lv_snprintf(buf, n, "UTC%c%u", sign, hh);
    } else {
        lv_snprintf(buf, n, "UTC%c%u:%02u", sign, hh, mm);
    }
}

#define HELM_EPH_MIN_UNIX  1704067200u

static void helm_eph_local(char * buf, size_t n, uint32_t utc, bool with_date)
{
    struct tm tm_buf;
    time_t wall;
    int16_t tz;

    if (buf == NULL || n == 0) {
        return;
    }

    if (utc < HELM_EPH_MIN_UNIX) {
        lv_snprintf(buf, n, "--");
        return;
    }

    tz = myvendor_devctl_tz_min_get();
    wall = (time_t)utc + (time_t)tz * 60;
    if (gmtime_r(&wall, &tm_buf) == NULL) {
        lv_snprintf(buf, n, "--");
        return;
    }

    if (with_date) {
        lv_snprintf(buf, n, "%04d-%02d-%02d %02d:%02d",
                    tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                    tm_buf.tm_hour, tm_buf.tm_min);
    } else {
        lv_snprintf(buf, n, "%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min);
    }
}

static void helm_eph_fill_settings(helm_menu_t * m, uint8_t * n)
{
    uint32_t last = 0;
    uint32_t next = 0;
    char sub[40];
    char val[24];
    time_t now;
    bool have;

    have = myvendor_gnss_eph_times(&last, &next);
    now = time(NULL);
    if (!have) {
        lv_snprintf(sub, sizeof(sub), "未同步");
        lv_snprintf(val, sizeof(val), "--");
    } else {
        helm_eph_local(val, sizeof(val), last, false);
        if (next != 0 && now >= (time_t)next) {
            lv_snprintf(sub, sizeof(sub), "已过期");
        } else if (now >= (time_t)HELM_EPH_MIN_UNIX && next > (uint32_t)now) {
            unsigned left = (unsigned)(next - (uint32_t)now);
            unsigned hh = left / 3600u;

            if (hh >= 1u) {
                lv_snprintf(sub, sizeof(sub), "%u小时后", hh);
            } else {
                unsigned mm = (left % 3600u) / 60u;

                if (mm == 0u) {
                    mm = 1u;
                }

                lv_snprintf(sub, sizeof(sub), "%u分钟后", mm);
            }
        } else {
            char next_hm[8];

            helm_eph_local(next_hm, sizeof(next_hm), next, false);
            lv_snprintf(sub, sizeof(sub), "下次 %s", next_hm);
        }
    }

    helm_row_set(m, (*n)++, HELM_ICO_GPS, "星历", sub, val,
                 ACT_ENTER_EPH, HELM_KIND_GO);
}

static void helm_eph_fill_page(helm_menu_t * m, uint8_t * n)
{
    uint32_t last = 0;
    uint32_t next = 0;
    char last_s[24];
    char next_s[24];
    time_t now;
    bool have;

    helm_row_sw(m, (*n)++, HELM_ICO_GPS, "自动同步",
                myvendor_devctl_eph_auto_get(), ACT_TOGGLE_EPH_AUTO);

    have = myvendor_gnss_eph_times(&last, &next);
    now = time(NULL);
    if (!have) {
        helm_row_set(m, (*n)++, HELM_ICO_TIME, "上次同步",
                     "未同步", "--", ACT_NOP, HELM_KIND_GO);
        helm_row_set(m, (*n)++, HELM_ICO_TIME, "下次同步",
                     "请用 App 同步", "现在", ACT_NOP, HELM_KIND_GO);
        return;
    }

    helm_eph_local(last_s, sizeof(last_s), last, true);
    helm_eph_local(next_s, sizeof(next_s), next, true);
    helm_row_set(m, (*n)++, HELM_ICO_TIME, "上次同步", last_s, "",
                 ACT_NOP, HELM_KIND_GO);
    if (next != 0 && now >= (time_t)next) {
        helm_row_set(m, (*n)++, HELM_ICO_TIME, "下次同步", next_s, "已过期",
                     ACT_NOP, HELM_KIND_GO);
    } else {
        helm_row_set(m, (*n)++, HELM_ICO_TIME, "下次同步", next_s, "",
                     ACT_NOP, HELM_KIND_GO);
    }
}

static const char * helm_notif_kind(uint8_t type)
{
    switch (type) {
    case MYVENDOR_SYS_NOTIF_TYPE_CALL:
        return "来电";
    case MYVENDOR_SYS_NOTIF_TYPE_SMS:
        return "短信";
    case MYVENDOR_SYS_NOTIF_TYPE_CALENDAR:
        return "日历";
    default:
        return "应用";
    }
}

static bool helm_fill_inbox(helm_menu_t * m, uint8_t * n)
{
    myvendor_sys_notif_t box[MYVENDOR_SYS_INBOX_MAX];
    uint8_t nn = 0;
    uint8_t i;

    myvendor_sys_inbox_get(box, &nn, MYVENDOR_SYS_INBOX_MAX);
    for (i = 0; i < nn && *n < HELM_MENU_ROWS; i++) {
        const char * lab = box[i].title[0] ? box[i].title : box[i].body;

        helm_row_set(m, *n,
                     (box[i].type == MYVENDOR_SYS_NOTIF_TYPE_CALL) ?
                         HELM_ICO_CALL : HELM_ICO_BELL,
                     (lab && lab[0]) ? lab : "通知",
                     helm_notif_kind(box[i].type), "",
                     ACT_INBOX_OPEN, HELM_KIND_GO);
        m->items[*n].extra = i;
        (*n)++;
    }

    return *n == 0;
}

static void helm_row_set(helm_menu_t * m, uint8_t i, helm_ico_id_t ico,
                         const char * lab, const char * sub, const char * val,
                         uint8_t act, uint8_t kind)
{
    if (i >= HELM_MENU_ROWS) {
        return;
    }

    lv_snprintf(m->items[i].label, sizeof(m->items[i].label), "%s", lab);
    m->items[i].sub[0] = '\0';
    m->items[i].value[0] = '\0';
    if (sub && sub[0] != '\0') {
        lv_snprintf(m->items[i].sub, sizeof(m->items[i].sub), "%s", sub);
    }

    if (val && val[0] != '\0') {
        lv_snprintf(m->items[i].value, sizeof(m->items[i].value), "%s", val);
    }

    m->items[i].act = act;
    m->items[i].kind = kind;
    m->items[i].ico = ico;
    m->items[i].on = false;
}

static void helm_row_sw(helm_menu_t * m, uint8_t i, helm_ico_id_t ico,
                        const char * lab, bool on, uint8_t act)
{
    helm_row_set(m, i, ico, lab, on ? "已开启" : "已关闭", "", act, HELM_KIND_SW);
    m->items[i].on = on;
}

static helm_scr_t helm_cur_scr(const helm_menu_t * m)
{
    if (m == NULL || m->sp == 0) {
        return HELM_SCR_ROOT;
    }

    return m->stack[m->sp - 1u].scr;
}

static uint8_t * helm_cur_sel(helm_menu_t * m)
{
    if (m == NULL || m->sp == 0) {
        return NULL;
    }

    return &m->stack[m->sp - 1u].sel;
}

static bool helm_name_is_gpx(const char * name)
{
    size_t n;

    if (name == NULL) {
        return false;
    }

    n = strlen(name);
    if (n < 5u) {
        return false;
    }

    return strcasecmp(name + n - 4u, ".gpx") == 0;
}

static bool helm_name_is_tsv(const char * name)
{
    size_t n;

    if (name == NULL) {
        return false;
    }

    n = strlen(name);
    if (n < 5u) {
        return false;
    }

    return strcasecmp(name + n - 4u, ".tsv") == 0;
}

static void helm_navpt_clear_bind(void)
{
    s_navpt_path[0] = '\0';
    s_navpt_title[0] = '\0';
}

static uint8_t helm_navpt_scan(void)
{
    DIR * d;
    struct dirent * de;

    if (myvendor_mtp_lfs_quiesce()) {
        return gpx_same_dir(s_gpx_dir, MYVENDOR_NAVPTS_DIR) ? s_gpx_n : 0;
    }

    s_gpx_n = 0;
    s_gpx_dir = MYVENDOR_NAVPTS_DIR;
    d = opendir(MYVENDOR_NAVPTS_DIR);
    if (d == NULL) {
        return 0;
    }

    while ((de = readdir(d)) != NULL && s_gpx_n < HELM_GPX_LIST_MAX) {
        char path[160];
        struct stat st;

        if (de->d_name[0] == '.' || !helm_name_is_tsv(de->d_name)) {
            continue;
        }

        lv_snprintf(path, sizeof(path), "%s/%s", MYVENDOR_NAVPTS_DIR,
                    de->d_name);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_size < 8) {
            continue;
        }

        lv_snprintf(s_gpx_names[s_gpx_n], HELM_GPX_NAME_MAX, "%s",
                    de->d_name);
        s_gpx_size[s_gpx_n] = (uint32_t)st.st_size;
        s_gpx_mtime[s_gpx_n] = (uint32_t)st.st_mtime;
        s_gpx_n++;
    }

    closedir(d);

    {
        uint8_t i;
        uint8_t j;
        char tmp[HELM_GPX_NAME_MAX];
        uint32_t u;

        for (i = 0; i < s_gpx_n; i++) {
            for (j = (uint8_t)(i + 1u); j < s_gpx_n; j++) {
                if (strcmp(s_gpx_names[j], s_gpx_names[i]) > 0) {
                    lv_snprintf(tmp, sizeof(tmp), "%s", s_gpx_names[i]);
                    lv_snprintf(s_gpx_names[i], HELM_GPX_NAME_MAX, "%s",
                                s_gpx_names[j]);
                    lv_snprintf(s_gpx_names[j], HELM_GPX_NAME_MAX, "%s", tmp);
                    u = s_gpx_size[i];
                    s_gpx_size[i] = s_gpx_size[j];
                    s_gpx_size[j] = u;
                    u = s_gpx_mtime[i];
                    s_gpx_mtime[i] = s_gpx_mtime[j];
                    s_gpx_mtime[j] = u;
                }
            }
        }
    }

    return s_gpx_n;
}

static bool helm_fill_waypoint_rows(helm_menu_t * m, uint8_t * n,
                                    uint8_t ico,
                                    const myvendor_devctl_favorite_t * pts,
                                    size_t pt_n, bool pick)
{
    size_t i;

    for (i = 0; i < pt_n && *n < HELM_MENU_ROWS; i++) {
        char sub[40];
        char fallback[24];
        const char * lab;
        uint8_t order = 0u;
        uint8_t j;

        lv_snprintf(fallback, sizeof(fallback), "第%u站",
                    (unsigned)(i + 1u));
        lab = (pts[i].name[0] != '\0') ? pts[i].name : fallback;
        lv_snprintf(sub, sizeof(sub), "%.6f, %.6f",
                    pts[i].latitude, pts[i].longitude);

        if (!pick) {
            helm_row_set(m, (*n)++, ico, lab, sub, fallback,
                         ACT_NOP, HELM_KIND_GO);
            m->items[*n - 1u].extra = (uint8_t)i;
            continue;
        }

        for (j = 0u; j < m->fav_order_n; j++) {
            if (m->fav_order[j] == (uint8_t)i) {
                order = (uint8_t)(j + 1u);
                break;
            }
        }

        helm_row_set(m, (*n)++, ico, lab, sub,
                     order > 0u ? "" : "选择", ACT_NAV_FAV, HELM_KIND_GO);
        m->items[*n - 1u].extra = (uint8_t)i;
        if (order > 0u) {
            lv_snprintf(m->items[*n - 1u].value,
                        sizeof(m->items[*n - 1u].value), "第%u站",
                        (unsigned)order);
        }
    }

    return *n == 0u;
}

static uint8_t helm_gpx_scan(const char * dir)
{
    DIR * d;
    struct dirent * de;

    if (myvendor_mtp_lfs_quiesce()) {
        return gpx_same_dir(s_gpx_dir, dir) ? s_gpx_n : 0;
    }

    s_gpx_n = 0;
    s_gpx_dir = dir;
    if (dir == NULL) {
        return 0;
    }

    d = opendir(dir);
    if (d == NULL) {
        return 0;
    }

    while ((de = readdir(d)) != NULL && s_gpx_n < HELM_GPX_LIST_MAX) {
        if (de->d_name[0] == '.') {
            continue;
        }

        if (!helm_name_is_gpx(de->d_name)) {
            continue;
        }

        {
            char path[160];
            struct stat st;

            lv_snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
            /* 空壳约 381 字节、0 字节文件不进列表。 */
            if (stat(path, &st) != 0 || st.st_size < 400) {
                continue;
            }

            lv_snprintf(s_gpx_names[s_gpx_n], HELM_GPX_NAME_MAX, "%s",
                        de->d_name);
            s_gpx_size[s_gpx_n] = (uint32_t)st.st_size;
            s_gpx_mtime[s_gpx_n] = (uint32_t)st.st_mtime;
            s_gpx_n++;
        }
    }

    closedir(d);

    /* TRK_YYYYMMDD_HHMMSS：名字倒序即最新在前。 */
    {
        uint8_t i;
        uint8_t j;
        char tmp[HELM_GPX_NAME_MAX];
        uint32_t u;

        for (i = 0; i < s_gpx_n; i++) {
            for (j = (uint8_t)(i + 1u); j < s_gpx_n; j++) {
                if (strcmp(s_gpx_names[j], s_gpx_names[i]) > 0) {
                    lv_snprintf(tmp, sizeof(tmp), "%s", s_gpx_names[i]);
                    lv_snprintf(s_gpx_names[i], HELM_GPX_NAME_MAX, "%s",
                                s_gpx_names[j]);
                    lv_snprintf(s_gpx_names[j], HELM_GPX_NAME_MAX, "%s", tmp);
                    u = s_gpx_size[i];
                    s_gpx_size[i] = s_gpx_size[j];
                    s_gpx_size[j] = u;
                    u = s_gpx_mtime[i];
                    s_gpx_mtime[i] = s_gpx_mtime[j];
                    s_gpx_mtime[j] = u;
                }
            }
        }
    }

    return s_gpx_n;
}

static int helm_gpx_leap(int y)
{
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0;
}

static int64_t helm_gpx_epoch(const gpx_time_t * t)
{
    static const uint8_t dim[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    int y;
    int m;
    int i;
    int64_t days = 0;

    if (t == NULL || t->year < 1970 || t->month < 1 || t->month > 12
        || t->day < 1) {
        return -1;
    }

    y = t->year;
    m = t->month;
    for (i = 1970; i < y; i++) {
        days += 365 + helm_gpx_leap(i);
    }

    for (i = 1; i < m; i++) {
        days += dim[i - 1];
        if (i == 2) {
            days += helm_gpx_leap(y);
        }
    }

    days += (int)t->day - 1;
    return days * 86400 + (int64_t)t->hour * 3600
        + (int64_t)t->minute * 60 + (int64_t)t->second;
}

static void helm_fmt_ride_dur(char * buf, size_t n, uint32_t sec)
{
    unsigned h = sec / 3600u;
    unsigned m = (sec / 60u) % 60u;
    unsigned s = sec % 60u;

    if (buf == NULL || n == 0u) {
        return;
    }

    /* 始终 H:MM:SS，避免 MM:SS 被看成钟点。 */
    lv_snprintf(buf, n, "%u:%02u:%02u", h, m, s);
}

static void helm_fmt_ride_sub(char * buf, size_t n, double km, uint32_t sec,
                             bool have_time)
{
    char dist[16];
    char dur[16];

    if (buf == NULL || n == 0u) {
        return;
    }

    if (s_unit_imperial) {
        lv_snprintf(dist, sizeof(dist), "%.1f mi", km * 0.621371);
    } else {
        lv_snprintf(dist, sizeof(dist), "%.1f km", km);
    }

    if (!have_time) {
        lv_snprintf(buf, n, "%s", dist);
        return;
    }

    helm_fmt_ride_dur(dur, sizeof(dur), sec);
    lv_snprintf(buf, n, "%s  用时%s", dist, dur);
}

static helm_gpx_stat_t * helm_gpx_stat_find(const char * name)
{
    uint8_t i;

    if (name == NULL) {
        return NULL;
    }

    for (i = 0; i < s_gpx_stat_n; i++) {
        if (strcmp(s_gpx_stat[i].name, name) == 0) {
            return &s_gpx_stat[i];
        }
    }

    return NULL;
}

static bool helm_gpx_stat_flush(void)
{
    helm_stat_hdr_t hdr;
    int fd;
    uint8_t i;
    uint16_t n = 0;

    for (i = 0; i < s_gpx_stat_n; i++) {
        if (s_gpx_stat[i].ok) {
            n++;
        }
    }

    fd = open(HELM_STAT_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = HELM_STAT_MAGIC;
    hdr.ver = HELM_STAT_VER;
    hdr.n = n;
    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        close(fd);
        (void)unlink(HELM_STAT_TMP);
        return false;
    }

    for (i = 0; i < s_gpx_stat_n; i++) {
        helm_stat_rec_t rec;

        if (!s_gpx_stat[i].ok) {
            continue;
        }

        memset(&rec, 0, sizeof(rec));
        lv_snprintf(rec.name, sizeof(rec.name), "%s", s_gpx_stat[i].name);
        rec.size = s_gpx_stat[i].size;
        rec.mtime = s_gpx_stat[i].mtime;
        rec.dist_m = (uint32_t)(s_gpx_stat[i].km * 1000.0 + 0.5);
        rec.sec = s_gpx_stat[i].sec;
        rec.have_time = s_gpx_stat[i].have_time ? 1u : 0u;
        if (write(fd, &rec, sizeof(rec)) != (ssize_t)sizeof(rec)) {
            close(fd);
            (void)unlink(HELM_STAT_TMP);
            return false;
        }
    }

    close(fd);
    (void)unlink(HELM_STAT_PATH);
    if (rename(HELM_STAT_TMP, HELM_STAT_PATH) != 0) {
        (void)unlink(HELM_STAT_TMP);
        return false;
    }

    return true;
}

static void helm_gpx_stat_load(void)
{
    helm_stat_hdr_t hdr;
    int fd;
    uint16_t i;

    if (s_stat_loaded) {
        return;
    }

    s_stat_loaded = true;
    s_gpx_stat_n = 0;
    fd = open(HELM_STAT_PATH, O_RDONLY);
    if (fd < 0) {
        return;
    }

    if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)
        || hdr.magic != HELM_STAT_MAGIC || hdr.ver != HELM_STAT_VER
        || hdr.n > HELM_GPX_LIST_MAX) {
        close(fd);
        return;
    }

    for (i = 0; i < hdr.n && s_gpx_stat_n < HELM_GPX_LIST_MAX; i++) {
        helm_stat_rec_t rec;

        if (read(fd, &rec, sizeof(rec)) != (ssize_t)sizeof(rec)) {
            break;
        }

        rec.name[HELM_GPX_NAME_MAX - 1u] = '\0';
        if (rec.name[0] == '\0') {
            continue;
        }

        lv_snprintf(s_gpx_stat[s_gpx_stat_n].name,
                    sizeof(s_gpx_stat[s_gpx_stat_n].name), "%s", rec.name);
        s_gpx_stat[s_gpx_stat_n].km = (double)rec.dist_m / 1000.0;
        s_gpx_stat[s_gpx_stat_n].sec = rec.sec;
        s_gpx_stat[s_gpx_stat_n].size = rec.size;
        s_gpx_stat[s_gpx_stat_n].mtime = rec.mtime;
        s_gpx_stat[s_gpx_stat_n].have_time = (rec.have_time != 0u);
        s_gpx_stat[s_gpx_stat_n].ok = true;
        s_gpx_stat[s_gpx_stat_n].dir = MYVENDOR_GPX_RECORD_DIR;
        s_gpx_stat_n++;
    }

    close(fd);
}

static void helm_gpx_stat_put(const char * name, double km, uint32_t sec,
                             bool have_time, uint32_t size, uint32_t mtime,
                             const char * dir)
{
    helm_gpx_stat_t * s;

    if (name == NULL || name[0] == '\0') {
        return;
    }

    helm_gpx_stat_load();
    s = helm_gpx_stat_find(name);
    if (s == NULL) {
        if (s_gpx_stat_n >= HELM_GPX_LIST_MAX) {
            return;
        }

        s = &s_gpx_stat[s_gpx_stat_n++];
        memset(s, 0, sizeof(*s));
        lv_snprintf(s->name, sizeof(s->name), "%s", name);
    }

    s->km = km;
    s->sec = sec;
    s->have_time = have_time;
    s->size = size;
    s->mtime = mtime;
    s->dir = (dir != NULL) ? dir : MYVENDOR_GPX_RECORD_DIR;
    s->ok = true;
    (void)helm_gpx_stat_flush();
}

static void helm_gpx_stat_forget(const char * name)
{
    helm_gpx_stat_t * s;

    helm_gpx_stat_load();
    s = helm_gpx_stat_find(name);
    if (s == NULL) {
        return;
    }

    {
        uint8_t i = (uint8_t)(s - s_gpx_stat);

        if (i + 1u < s_gpx_stat_n) {
            s_gpx_stat[i] = s_gpx_stat[s_gpx_stat_n - 1u];
        }

        s_gpx_stat_n--;
    }

    (void)helm_gpx_stat_flush();
}

static bool helm_gpx_name_live(const char * name)
{
    uint8_t i;

    if (name == NULL) {
        return false;
    }

    for (i = 0; i < s_gpx_n; i++) {
        if (strcmp(s_gpx_names[i], name) == 0) {
            return true;
        }
    }

    return false;
}

static void helm_gpx_stat_prune(void)
{
    uint8_t i = 0;
    bool dirty = false;

    helm_gpx_stat_load();
    while (i < s_gpx_stat_n) {
        /* 只按当前目录的内存列表淘汰，避免扫盘 stat 和 BLE 落盘抢 LFS。 */
        if (gpx_same_dir(s_gpx_stat[i].dir, s_gpx_dir) &&
            !helm_gpx_name_live(s_gpx_stat[i].name)) {
            if (i + 1u < s_gpx_stat_n) {
                s_gpx_stat[i] = s_gpx_stat[s_gpx_stat_n - 1u];
            }

            s_gpx_stat_n--;
            dirty = true;
            continue;
        }

        i++;
    }

    if (dirty) {
        (void)helm_gpx_stat_flush();
    }
}

static bool helm_gpx_stat_hit(const helm_gpx_stat_t * s, uint32_t size,
                              uint32_t mtime)
{
    if (s == NULL || !s->ok) {
        return false;
    }

    if (size != 0u && s->size != 0u && s->size != size) {
        return false;
    }

    if (mtime != 0u && s->mtime != 0u && s->mtime != mtime) {
        return false;
    }

    return true;
}

void helm_ride_stat_remember(const char * gpx_path, double km, uint32_t sec)
{
    const char * base;

    if (gpx_path == NULL || gpx_path[0] == '\0') {
        return;
    }

    base = strrchr(gpx_path, '/');
    base = (base != NULL && base[1] != '\0') ? base + 1 : gpx_path;
    lv_snprintf(s_stat_pend_name, sizeof(s_stat_pend_name), "%s", base);
    s_stat_pend_km = km;
    s_stat_pend_sec = sec;
    s_stat_pend = true;

    /* 按键路径不碰 LittleFS。已加载的 RAM 表立刻更新，落盘推迟到打开记录列表。 */
    if (s_stat_loaded) {
        helm_gpx_stat_t * s = helm_gpx_stat_find(s_stat_pend_name);

        if (s == NULL && s_gpx_stat_n < HELM_GPX_LIST_MAX) {
            s = &s_gpx_stat[s_gpx_stat_n++];
            memset(s, 0, sizeof(*s));
            lv_snprintf(s->name, sizeof(s->name), "%s", s_stat_pend_name);
        }

        if (s != NULL) {
            s->km = km;
            s->sec = sec;
            s->have_time = true;
            s->dir = MYVENDOR_GPX_RECORD_DIR;
            s->ok = true;
        }
    }
}

static void helm_gpx_stat_commit_pend(void)
{
    if (!s_stat_pend || myvendor_mtp_lfs_quiesce()) {
        return;
    }

    helm_gpx_stat_put(s_stat_pend_name, s_stat_pend_km, s_stat_pend_sec, true,
                      0, 0, MYVENDOR_GPX_RECORD_DIR);
    s_stat_pend = false;
}

static bool helm_fill_gpx_dir(helm_menu_t * m, uint8_t * n, const char * dir,
                              uint8_t act)
{
    uint8_t i;

    /* BLE/MTP 写 /mnt/lfs 时不要在按键路径上 opendir/stat。
     * 否则第一次 lfs_alloc 能把 KEY2 confirm 卡住数秒，列表也打不开。 */
    if (myvendor_mtp_lfs_quiesce()) {
        helm_row_set(m, (*n)++, HELM_ICO_GPX, "写入中",
                     "稍后再打开", "", ACT_NOP, HELM_KIND_GO);
        return false;
    }

    helm_gpx_stat_commit_pend();

    if (helm_gpx_scan(dir) == 0) {
        helm_gpx_stat_prune();
        return true;
    }

    helm_gpx_stat_prune();
    for (i = 0; i < s_gpx_n && *n < HELM_MENU_ROWS; i++) {
        char sub[40];
        helm_gpx_stat_t * st = helm_gpx_stat_find(s_gpx_names[i]);

        if (helm_gpx_stat_hit(st, s_gpx_size[i], s_gpx_mtime[i])) {
            helm_fmt_ride_sub(sub, sizeof(sub), st->km, st->sec,
                              st->have_time);
        } else {
            lv_snprintf(sub, sizeof(sub), "--");
        }

        helm_row_set(m, (*n)++, HELM_ICO_GPX, s_gpx_names[i], sub, "",
                     act, HELM_KIND_GO);
        m->items[*n - 1u].extra = i;
    }

    return false;
}

static const char * helm_ride_dir_path(void)
{
    return (s_ride_dir != NULL) ? s_ride_dir : MYVENDOR_GPX_RECORD_DIR;
}

static bool helm_ride_bind(const char * name, const char * dir)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    lv_snprintf(s_ride_name, sizeof(s_ride_name), "%s", name);
    s_ride_dir = (dir != NULL) ? dir : MYVENDOR_GPX_RECORD_DIR;
    return true;
}

static bool helm_ride_path(char * buf, size_t n)
{
    if (buf == NULL || n == 0u || s_ride_name[0] == '\0') {
        return false;
    }

    lv_snprintf(buf, n, "%s/%s", helm_ride_dir_path(), s_ride_name);
    return true;
}

static bool helm_ride_load(void)
{
    char path[160];
    gpx_decode_t * dec = NULL;
    gpx_decode_cfg_t cfg;
    gpx_decode_stats_t st;
    gpx_point_t batch[16];
    unsigned n;
    int ret;
    float last_lon = 0.0f;
    float last_lat = 0.0f;
    bool have_last = false;
    bool have_t0 = false;
    gpx_time_t t0;
    gpx_time_t t1;
    double len = 0.0;

    memset(&t0, 0, sizeof(t0));
    memset(&t1, 0, sizeof(t1));
    memset(&st, 0, sizeof(st));
    memset(batch, 0, sizeof(batch));

    if (s_ride_name[0] == '\0') {
        s_ride_pt_n = 0;
        s_ride_km = 0.0;
        s_ride_sec = 0;
        s_ride_has_time = false;
        s_ride_loaded[0] = '\0';
        s_ride_loaded_dir = NULL;
        return false;
    }

    if (strcmp(s_ride_loaded, s_ride_name) == 0 &&
        s_ride_loaded_dir == helm_ride_dir_path() && s_ride_pt_n >= 2u) {
        return true;
    }

    s_ride_pt_n = 0;
    s_ride_km = 0.0;
    s_ride_sec = 0;
    s_ride_has_time = false;
    s_ride_loaded[0] = '\0';
    s_ride_loaded_dir = NULL;
    if (!helm_ride_path(path, sizeof(path))) {
        return false;
    }

    gpx_decode_cfg_sparse(&cfg, HELM_RIDE_PT_MAX);
    cfg.batch_max = 16;
    if (gpx_decode_open(path, &cfg, &dec) != 0) {
        return false;
    }

    for (;;) {
        unsigned i;

        memset(batch, 0, sizeof(batch));
        ret = gpx_decode_read(dec, batch, 16, &n);
        if (ret < 0) {
            /* 扩展/尾部损坏时仍用已读到的点画预览。 */
            break;
        }

        for (i = 0; i < n; i++) {
            if (batch[i].latitude == 0.0f && batch[i].longitude == 0.0f) {
                continue;
            }
            if (have_last) {
                const double d = vmap_geo_haversine_m((double)last_lon,
                    (double)last_lat, (double)batch[i].longitude,
                    (double)batch[i].latitude);

                if (d > 100000.0) {
                    continue;
                }
                len += d;
                if (d < 0.5) {
                    last_lon = batch[i].longitude;
                    last_lat = batch[i].latitude;
                    if (batch[i].has_time) {
                        t1 = batch[i].time;
                    }
                    continue;
                }
            }
            last_lon = batch[i].longitude;
            last_lat = batch[i].latitude;
            have_last = true;
            if (batch[i].has_time) {
                if (!have_t0) {
                    t0 = batch[i].time;
                    have_t0 = true;
                }

                t1 = batch[i].time;
            }

            if (s_ride_pt_n < HELM_RIDE_PT_MAX) {
                s_ride_lon[s_ride_pt_n] = last_lon;
                s_ride_lat[s_ride_pt_n] = last_lat;
                s_ride_pt_n++;
            } else {
                s_ride_lon[HELM_RIDE_PT_MAX - 1u] = last_lon;
                s_ride_lat[HELM_RIDE_PT_MAX - 1u] = last_lat;
            }
        }

        if (ret == 1) {
            break;
        }
    }

    (void)gpx_decode_get_stats(dec, &st);
    gpx_decode_close(&dec);

    if (have_last && s_ride_pt_n < HELM_RIDE_PT_MAX) {
        if (s_ride_pt_n == 0u
            || s_ride_lon[s_ride_pt_n - 1u] != last_lon
            || s_ride_lat[s_ride_pt_n - 1u] != last_lat) {
            s_ride_lon[s_ride_pt_n] = last_lon;
            s_ride_lat[s_ride_pt_n] = last_lat;
            s_ride_pt_n++;
        }
    }

    if (s_ride_pt_n < 2u) {
        return false;
    }

    if (st.length_m > len) {
        len = st.length_m;
    }

    s_ride_km = len / 1000.0;
    {
        helm_gpx_stat_t * cache;
        uint32_t gpx_sec = 0;
        bool gpx_has = false;

        if (have_t0) {
            const int64_t a = helm_gpx_epoch(&t0);
            const int64_t b = helm_gpx_epoch(&t1);

            if (a >= 0 && b >= a) {
                gpx_sec = (uint32_t)(b - a);
                gpx_has = true;
            }
        }

        /* 保存时写入的 session 用时优先于 GPX 首末时间差（墙钟跨度）。 */
        helm_gpx_stat_load();
        cache = helm_gpx_stat_find(s_ride_name);
        if (cache != NULL && cache->ok && cache->have_time) {
            s_ride_sec = cache->sec;
            s_ride_has_time = true;
        } else {
            s_ride_sec = gpx_sec;
            s_ride_has_time = gpx_has;
        }
    }

    lv_snprintf(s_ride_loaded, sizeof(s_ride_loaded), "%s", s_ride_name);
    s_ride_loaded_dir = helm_ride_dir_path();
    {
        struct stat fst;

        if (stat(path, &fst) == 0) {
            helm_gpx_stat_put(s_ride_name, s_ride_km, s_ride_sec,
                              s_ride_has_time, (uint32_t)fst.st_size,
                              (uint32_t)fst.st_mtime, helm_ride_dir_path());
        }
    }

    return true;
}

static void helm_ride_track_draw(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target_obj(e);
    lv_layer_t * layer = lv_event_get_layer(e);
    lv_area_t coords;
    int32_t w;
    int32_t h;
    const int32_t pad = 14;
    float min_lon;
    float max_lon;
    float min_lat;
    float max_lat;
    float sx;
    float sy;
    uint16_t i;
    lv_draw_line_dsc_t ld;
    lv_draw_rect_dsc_t rd;

    if (obj == NULL || layer == NULL || s_ride_pt_n < 2u) {
        return;
    }

    lv_obj_get_coords(obj, &coords);
    w = lv_area_get_width(&coords) - pad * 2;
    h = lv_area_get_height(&coords) - pad * 2;
    if (w < 8 || h < 8) {
        return;
    }

    min_lon = max_lon = s_ride_lon[0];
    min_lat = max_lat = s_ride_lat[0];
    for (i = 1u; i < s_ride_pt_n; i++) {
        if (s_ride_lon[i] < min_lon) {
            min_lon = s_ride_lon[i];
        }
        if (s_ride_lon[i] > max_lon) {
            max_lon = s_ride_lon[i];
        }
        if (s_ride_lat[i] < min_lat) {
            min_lat = s_ride_lat[i];
        }
        if (s_ride_lat[i] > max_lat) {
            max_lat = s_ride_lat[i];
        }
    }

    sx = (max_lon > min_lon) ? ((float)w / (max_lon - min_lon)) : 0.0f;
    sy = (max_lat > min_lat) ? ((float)h / (max_lat - min_lat)) : 0.0f;
    if (sx <= 0.0f && sy <= 0.0f) {
        sx = sy = 1.0f;
    } else if (sx <= 0.0f) {
        sx = sy;
    } else if (sy <= 0.0f) {
        sy = sx;
    } else if (sx < sy) {
        sy = sx;
    } else {
        sx = sy;
    }

    {
        const float used_w = (max_lon - min_lon) * sx;
        const float used_h = (max_lat - min_lat) * sy;
        const float ox = (float)coords.x1 + (float)pad
            + ((float)w - used_w) * 0.5f;
        const float oy = (float)coords.y1 + (float)pad
            + ((float)h - used_h) * 0.5f;

        lv_point_precise_t pts[HELM_RIDE_PT_MAX];
        uint16_t n = 0;

        lv_draw_line_dsc_init(&ld);
        ld.color = helm_color(HELM_COLOR_NAV);
        ld.width = 3;
        ld.opa = LV_OPA_COVER;
        ld.round_start = 1;
        ld.round_end = 1;

        for (i = 0u; i < s_ride_pt_n; i++) {
            const lv_coord_t x = (lv_coord_t)(ox
                + (s_ride_lon[i] - min_lon) * sx);
            const lv_coord_t y = (lv_coord_t)(oy
                + (max_lat - s_ride_lat[i]) * sy);

            if (n > 0u && pts[n - 1u].x == x && pts[n - 1u].y == y) {
                continue;
            }

            pts[n].x = x;
            pts[n].y = y;
            n++;
        }

        if (n >= 2u) {
            ld.points = pts;
            ld.point_cnt = n;
            lv_draw_line(layer, &ld);
        }

        lv_draw_rect_dsc_init(&rd);
        rd.bg_opa = LV_OPA_COVER;
        rd.border_width = 0;
        rd.radius = LV_RADIUS_CIRCLE;

        {
            lv_area_t a;
            const lv_coord_t x = (lv_coord_t)(ox
                + (s_ride_lon[0] - min_lon) * sx);
            const lv_coord_t y = (lv_coord_t)(oy
                + (max_lat - s_ride_lat[0]) * sy);

            rd.bg_color = helm_color(HELM_COLOR_GPS);
            a.x1 = x - 5;
            a.y1 = y - 5;
            a.x2 = x + 5;
            a.y2 = y + 5;
            lv_draw_rect(layer, &rd, &a);

            rd.bg_color = helm_color(HELM_COLOR_INK);
            {
                const lv_coord_t ex = (lv_coord_t)(ox
                    + (s_ride_lon[s_ride_pt_n - 1u] - min_lon) * sx);
                const lv_coord_t ey = (lv_coord_t)(oy
                    + (max_lat - s_ride_lat[s_ride_pt_n - 1u]) * sy);

                a.x1 = ex - 5;
                a.y1 = ey - 5;
                a.x2 = ex + 5;
                a.y2 = ey + 5;
                lv_draw_rect(layer, &rd, &a);
                rd.bg_color = helm_color(HELM_COLOR_PAPER);
                a.x1 = ex - 2;
                a.y1 = ey - 2;
                a.x2 = ex + 2;
                a.y2 = ey + 2;
                lv_draw_rect(layer, &rd, &a);
            }
        }
    }
}

static void helm_ride_refresh(helm_menu_t * m)
{
    uint8_t * sel;
    uint8_t i;
    char buf[40];

    if (m == NULL || m->ride == NULL || m->ride_keys == NULL) {
        return;
    }

    (void)helm_ride_load();
    if (m->ride_dist) {
        if (s_ride_pt_n >= 2u) {
            helm_fmt_ride_sub(buf, sizeof(buf), s_ride_km, s_ride_sec,
                              s_ride_has_time);
        } else {
            lv_snprintf(buf, sizeof(buf), "无轨迹");
        }

        lv_label_set_text(m->ride_dist, buf);
    }

    if (m->ride_track) {
        lv_obj_invalidate(m->ride_track);
    }

    sel = helm_cur_sel(m);
    {
        bool reuse = false;

        if (m->count > 0 &&
            lv_obj_get_child_count(m->ride_keys) == m->count) {
            reuse = true;
            for (i = 0; i < m->count; i++) {
                helm_item_t spec;
                lv_obj_t * row = lv_obj_get_child(m->ride_keys, i);

                memset(&spec, 0, sizeof(spec));
                spec.ico = m->items[i].ico;
                spec.title = m->items[i].label;
                spec.kind = HELM_KIND_SLIM;
                spec.sel = (sel && *sel == i);
                if (!helm_mitem_refresh(row, &spec)) {
                    reuse = false;
                    break;
                }
            }
        }

        if (!reuse) {
            lv_obj_clean(m->ride_keys);
            for (i = 0; i < m->count; i++) {
                helm_item_t spec;

                memset(&spec, 0, sizeof(spec));
                spec.ico = m->items[i].ico;
                spec.title = m->items[i].label;
                spec.kind = HELM_KIND_SLIM;
                spec.sel = (sel && *sel == i);
                helm_mitem_create(m->ride_keys, &spec);
            }
        }

        if (sel && m->count > 0) {
            uint8_t idx = *sel;

            if (idx >= m->count) {
                idx = 0;
                *sel = 0;
            }
            lv_obj_update_layout(m->ride);
            lv_obj_update_layout(m->ride_keys);
            helm_mlist_sel_snap(m->ride_keys, idx);
        }
    }
}

static void helm_ride_close_map(const char * title, const char * sub)
{
    myvendor_sound_ok();
    lv_pm_notify_show(title, sub, 1500);
    (void)lv_pm_close_page_msg(NULL);
}

static bool helm_fill_items(helm_menu_t * m)
{
    myvendor_sys_sensor_ui_t ui;
    helm_scr_t scr = helm_cur_scr(m);
    uint8_t n = 0;
    char tmp[40];
    bool empty = false;

    memset(m->items, 0, sizeof(m->items));
    myvendor_sys_sensor_ui_get(&ui);

    switch (scr) {
    case HELM_SCR_ROOT:
        helm_row_set(m, n++, HELM_ICO_NAV, "导航", "GPX / 坐标点 / 常用点", "",
                     ACT_ENTER_NAV, HELM_KIND_GO);
        lv_snprintf(tmp, sizeof(tmp), "%u 已连接",
                    (unsigned)helm_sensor_n());
        helm_row_set(m, n++, HELM_ICO_BLE, "蓝牙设备", tmp, "",
                     ACT_ENTER_SENSORS, HELM_KIND_GO);
        helm_row_set(m, n++, HELM_ICO_NAV, "工具箱", "指南针 / 水平仪", "",
                     ACT_ENTER_TOOLS, HELM_KIND_GO);
        helm_eph_fill_settings(m, &n);
        {
            uint8_t nrec = helm_gpx_scan(MYVENDOR_GPX_RECORD_DIR);

            if (nrec == 0) {
                helm_row_set(m, n++, HELM_ICO_LIST, "骑行记录", "暂无记录", "",
                             ACT_ENTER_RIDES, HELM_KIND_GO);
            } else {
                lv_snprintf(tmp, sizeof(tmp), "%u 条", (unsigned)nrec);
                helm_row_set(m, n++, HELM_ICO_LIST, "骑行记录", tmp, "",
                             ACT_ENTER_RIDES, HELM_KIND_GO);
            }
        }
        helm_row_set(m, n++, HELM_ICO_CHIP, "系统状态", "磁盘 · 内存 · 线程", "",
                     ACT_ENTER_SYSSTAT, HELM_KIND_GO);
        {
            const char * th = lvgl_page_theme_name();

            lv_snprintf(tmp, sizeof(tmp), "%s · 亮度%s",
                        (th && th[0] == 'o') ? "夜间" : "日光",
                        helm_bl_name(myvendor_devctl_bl_get()));
            helm_row_set(m, n++, HELM_ICO_GEAR, "设置", tmp, "",
                         ACT_ENTER_SETTINGS, HELM_KIND_GO);
        }
        helm_row_set(m, n++, HELM_ICO_POWER, "关机", "断电", "",
                     ACT_POWEROFF, HELM_KIND_GO);
        break;

    case HELM_SCR_NAV:
        if (map_page_review_active(lvgl_page_map())) {
            helm_row_set(m, n++, HELM_ICO_XMARK, "关闭轨迹", "结束回放", "",
                         ACT_NAV_STOP, HELM_KIND_GO);
        } else if (map_page_nav_active(lvgl_page_map())
                   || map_page_nav_planning(lvgl_page_map())) {
            helm_row_set(m, n++, HELM_ICO_PIN, "就近规划", "从最近点向后", "",
                         ACT_NAV_NEAREST, HELM_KIND_GO);
            helm_row_set(m, n++, HELM_ICO_NONE, "跳过本点", "", "当前",
                         ACT_NAV_SKIP, HELM_KIND_GO);
            {
                /* 原来写死「未到」：多站行程里看不出走到第几站了。 */
                uint32_t trip_idx = 0u;
                uint32_t trip_total = 0u;
                char trip_val[16];

                if (map_page_nav_trip_progress(lvgl_page_map(), &trip_idx,
                        &trip_total)) {
                    lv_snprintf(trip_val, sizeof(trip_val), "%u/%u",
                        (unsigned)trip_idx, (unsigned)trip_total);
                } else {
                    lv_snprintf(trip_val, sizeof(trip_val), "未到");
                }
                helm_row_set(m, n++, HELM_ICO_NONE, "途经点", "", trip_val,
                             ACT_NOP, HELM_KIND_GO);
            }
            helm_row_set(m, n++, HELM_ICO_NONE, "停止导航", "", "结束",
                         ACT_NAV_STOP, HELM_KIND_GO);
        } else {
            helm_row_set(m, n++, HELM_ICO_GPX, "GPX导航", "导入 / 记录", "",
                         ACT_ENTER_GPX, HELM_KIND_GO);
            helm_row_set(m, n++, HELM_ICO_NAV, "坐标点导航", "App 下发后点选", "",
                         ACT_ENTER_NAVPTS, HELM_KIND_GO);
            helm_row_set(m, n++, HELM_ICO_STAR, "常用点导航", "App 同步后点选", "",
                         ACT_ENTER_FAVS, HELM_KIND_GO);
        }
        break;

    case HELM_SCR_GPX:
        helm_row_set(m, n++, HELM_ICO_INBOX, "导入", "USB 写入", "",
                     ACT_ENTER_GPX_IMPORT, HELM_KIND_GO);
        helm_row_set(m, n++, HELM_ICO_LIST, "记录", "本机", "",
                     ACT_ENTER_GPX_RECORD, HELM_KIND_GO);
        break;

    case HELM_SCR_GPX_IMPORT:
        empty = helm_fill_gpx_dir(m, &n, MYVENDOR_GPX_IMPORT_DIR, ACT_RIDE_OPEN);
        break;

    case HELM_SCR_GPX_RECORD:
        empty = helm_fill_gpx_dir(m, &n, MYVENDOR_GPX_RECORD_DIR, ACT_RIDE_OPEN);
        break;

    case HELM_SCR_NAVPTS:
        if (myvendor_mtp_lfs_quiesce()) {
            helm_row_set(m, n++, HELM_ICO_NAV, "写入中",
                         "稍后再打开", "", ACT_NOP, HELM_KIND_GO);
            empty = false;
            break;
        }
        if (helm_navpt_scan() == 0) {
            empty = true;
            break;
        }
        {
            uint8_t i;

            for (i = 0; i < s_gpx_n && n < HELM_MENU_ROWS; i++) {
                char title[HELM_GPX_NAME_MAX];
                char * dot;
                char sub[24];

                lv_snprintf(title, sizeof(title), "%s", s_gpx_names[i]);
                dot = strrchr(title, '.');
                if (dot != NULL) {
                    *dot = '\0';
                }

                lv_snprintf(sub, sizeof(sub), "%lu B",
                            (unsigned long)s_gpx_size[i]);
                helm_row_set(m, n++, HELM_ICO_NAV, title, sub, "",
                             ACT_ENTER_NAVPT_REC, HELM_KIND_GO);
                m->items[n - 1u].extra = i;
            }
        }
        empty = n == 0u;
        break;

    case HELM_SCR_NAVPT_DETAIL:
        if (s_navpt_path[0] == '\0') {
            empty = true;
            break;
        }
        {
            myvendor_devctl_favorite_t pts[MYVENDOR_DEVCTL_FAVORITE_MAX];
            size_t pt_n = 0;

            if (myvendor_devctl_waypoints_load(s_navpt_path, pts,
                    MYVENDOR_DEVCTL_FAVORITE_MAX, &pt_n) != 0 ||
                pt_n == 0) {
                empty = true;
                break;
            }

            empty = helm_fill_waypoint_rows(m, &n, HELM_ICO_NAV, pts, pt_n,
                                            false);
        }
        break;

    case HELM_SCR_NAVPT_ACTIONS:
        if (s_navpt_path[0] == '\0') {
            empty = true;
            break;
        }
        helm_row_set(m, n++, HELM_ICO_NAV, "导航", "按文件顺序", "",
                     ACT_RIDE_NAV, HELM_KIND_GO);
        helm_row_set(m, n++, HELM_ICO_REV, "返航", "反向途经点", "",
                     ACT_RIDE_REV, HELM_KIND_GO);
        helm_row_set(m, n++, HELM_ICO_XMARK, "删除", "删除本条", "",
                     ACT_RIDE_DEL, HELM_KIND_GO);
        break;

    case HELM_SCR_FAVS:
        {
            myvendor_devctl_favorite_t favs[MYVENDOR_DEVCTL_FAVORITE_MAX];
            size_t fav_n = 0;

            if (myvendor_devctl_favorites_load(favs,
                    MYVENDOR_DEVCTL_FAVORITE_MAX, &fav_n) != 0 ||
                fav_n == 0) {
                empty = true;
                break;
            }

            empty = helm_fill_waypoint_rows(m, &n, HELM_ICO_STAR, favs, fav_n,
                                            true);
        }
        break;

    case HELM_SCR_SENSORS:
        {
            char st[16];
            char bat[24];

            helm_slot_status(st, sizeof(st), bat, sizeof(bat),
                             &ui.slot[MYVENDOR_SYS_SENSOR_KIND_HR],
                             myvendor_devctl_sensor_get());
            helm_row_set(m, n++, HELM_ICO_HR, "心率带", bat, st,
                         ACT_ENTER_SCAN_HR, HELM_KIND_GO);
            helm_slot_status(st, sizeof(st), bat, sizeof(bat),
                             &ui.slot[MYVENDOR_SYS_SENSOR_KIND_CSC],
                             myvendor_devctl_sensor_get());
            helm_row_set(m, n++, HELM_ICO_CAD, "踏频器", bat, st,
                         ACT_ENTER_SCAN_CAD, HELM_KIND_GO);
            helm_slot_status(st, sizeof(st), bat, sizeof(bat),
                             &ui.slot[MYVENDOR_SYS_SENSOR_KIND_CPS],
                             myvendor_devctl_sensor_get());
            helm_row_set(m, n++, HELM_ICO_BOLT, "功率计", bat, st,
                         ACT_ENTER_SCAN_PWR, HELM_KIND_GO);
            helm_row_set(m, n++, HELM_ICO_BLE, "手机蓝牙", "",
                         myvendor_sys_phone_ble_connected() ? "已连接" : "未连接",
                         ACT_ENTER_PHONE, HELM_KIND_GO);
        }
        break;

    /* 「手机蓝牙」子界面（2026-09-20，按用户设计）：
     *   没配对 → 提示配对（进这一页就已经算"UI 操作"，顺手开一次 30 s 窗口）；
     *   已配对 → 显示已配对设备的状态；长按右键 → 提示解绑；解绑后自动回到"提示配对"。 */
    case HELM_SCR_PHONE:
        {
            char addr[24];
            bool paired = (myvendor_devctl_pair_phone(addr, sizeof(addr)) == 1);

            const uint32_t left_ms = myvendor_devctl_pair_window_left_ms();
            const unsigned left_s  = (left_ms + 999u) / 1000u;

            s_phone_shown_paired = paired;      /* 给看门狗定时器做"状态翻没翻"的基准 */
            s_phone_shown_sec    = left_s;      /* 倒计时也是：秒数变了才重画 */

            if (paired) {
                /* 配对成功 → 这一行变成**设备信息**（地址 + 链路状态）；解绑走长按右键。 */
                helm_row_set(m, n++, HELM_ICO_BLE, "已配对", addr,
                             myvendor_sys_phone_ble_connected() ? "已连接" : "未连接",
                             ACT_NOP, HELM_KIND_GO);
            } else if (left_s > 0u) {
                /* 配对中：右侧显示倒计时（看门狗每秒重画一次）。 */
                char secs[12];

                lv_snprintf(secs, sizeof(secs), "%us", left_s);
                helm_row_set(m, n++, HELM_ICO_BLE, "配对中", "手机可搜索到码表",
                             secs, ACT_NOP, HELM_KIND_GO);
            } else {
                helm_row_set(m, n++, HELM_ICO_BLE, "配对新手机", "按勾号开始配对",
                             "", ACT_PAIR_OPEN, HELM_KIND_SLIM);
            }
        }
        break;

    case HELM_SCR_SCAN:
        empty = helm_fill_scan_recs(m, &n);
        break;

    case HELM_SCR_SCAN_PICK:
        empty = helm_fill_scan_pick(m, &n);
        break;

    case HELM_SCR_RIDES:
        empty = helm_fill_gpx_dir(m, &n, MYVENDOR_GPX_RECORD_DIR, ACT_RIDE_OPEN);
        break;

    case HELM_SCR_RIDE_DETAIL:
        if (s_list_enter_dir > 0) {
            uint8_t * rsel = helm_cur_sel(m);

            if (rsel) {
                *rsel = 0;
            }
        }
        helm_row_set(m, n++, HELM_ICO_PLAY, "继续骑行", "", "",
                     ACT_RIDE_CONT, HELM_KIND_SLIM);
        helm_row_set(m, n++, HELM_ICO_NAV, "导航", "", "",
                     ACT_RIDE_NAV, HELM_KIND_SLIM);
        helm_row_set(m, n++, HELM_ICO_REV, "返航", "", "",
                     ACT_RIDE_REV, HELM_KIND_SLIM);
        helm_row_set(m, n++, HELM_ICO_XMARK, "删除", "", "",
                     ACT_RIDE_DEL, HELM_KIND_SLIM);
        break;

    case HELM_SCR_INBOX:
        empty = helm_fill_inbox(m, &n);
        break;

    case HELM_SCR_SETTINGS:
        helm_row_sw(m, n++, HELM_ICO_BLE, "手机蓝牙", myvendor_devctl_radio_get(),
                    ACT_TOGGLE_BT);
        helm_row_sw(m, n++, HELM_ICO_CHIP, "外设蓝牙", myvendor_devctl_sensor_get(),
                    ACT_TOGGLE_SENSOR);
        helm_row_set(m, n++, HELM_ICO_SUN, "亮度",
                     helm_bl_name(myvendor_devctl_bl_get()),
                     helm_bl_name(myvendor_devctl_bl_get()),
                     ACT_CYCLE_BL, HELM_KIND_VAL);
        helm_row_sw(m, n++, HELM_ICO_SOUND, "蜂鸣器",
                    myvendor_devctl_sound_get(), ACT_TOGGLE_SOUND);
        {
            const char * th = lvgl_page_theme_name();
            const char * name = (th && th[0] == 'o') ? "夜间" : "日光";

            helm_row_set(m, n++, HELM_ICO_EYE, "显示", name, name,
                         ACT_CYCLE_THEME, HELM_KIND_VAL);
        }
        helm_row_set(m, n++, HELM_ICO_UNIT, "单位",
                     s_unit_imperial ? "英里" : "公里",
                     s_unit_imperial ? "英里" : "公里",
                     ACT_CYCLE_UNIT, HELM_KIND_VAL);
        {
            char tz[12];

            helm_tz_fmt(tz, sizeof(tz));
            helm_row_set(m, n++, HELM_ICO_TIME, "时区", tz, tz,
                         ACT_CYCLE_TZ, HELM_KIND_VAL);
        }
        helm_row_sw(m, n++, HELM_ICO_PAUSE, "自动暂停",
                    myvendor_devctl_autopause_get(),
                    ACT_TOGGLE_AUTOPAUSE);
        helm_row_set(m, n++, HELM_ICO_CLIMB, "坡度校准",
                     "水平放置后归零", "",
                     ACT_ENTER_GRADECAL, HELM_KIND_GO);
        helm_row_sw(m, n++, HELM_ICO_BELL, "手机通知",
                    myvendor_devctl_notif_get(), ACT_TOGGLE_NOTIF);
        helm_row_sw(m, n++, HELM_ICO_CALL, "仅来电",
                    myvendor_devctl_notif_calls_only_get(), ACT_TOGGLE_CALLS);
        {
            uint8_t inbox_n = 0;
            char nbuf[8] = "无";

            myvendor_sys_inbox_get(NULL, &inbox_n, 0);
            if (inbox_n > 0) {
                lv_snprintf(nbuf, sizeof(nbuf), "%u", (unsigned)inbox_n);
            }

            helm_row_set(m, n++, HELM_ICO_INBOX, "最近通知", nbuf, "",
                         ACT_ENTER_INBOX, HELM_KIND_GO);
        }
        helm_row_sw(m, n++, HELM_ICO_USB, "USB 传输", myvendor_devctl_mtp_get(),
                    ACT_TOGGLE_USB);
        helm_row_set(m, n++, HELM_ICO_GEAR, "测试配置", "字体 / GNSS 解算", "",
                     ACT_ENTER_TEST, HELM_KIND_GO);
        {
            const char * ver = myvendor_sw_version();
            const char * bld = myvendor_build_date();

            helm_row_set(m, n++, HELM_ICO_CHIP, "固件",
                         (ver && ver[0]) ? ver : VERSION_SOFTWARE,
                         (bld && bld[0]) ? bld : "",
                         ACT_ENTER_ABOUT, HELM_KIND_GO);
        }
        break;

    case HELM_SCR_TEST:
        helm_row_set(m, n++, HELM_ICO_STAR, "字体试验", "对比", "",
                     ACT_ENTER_FONTLAB, HELM_KIND_GO);
        {
            bool rmc = bicycle_runtime_gnss_solver_rmc();

            helm_row_set(m, n++, HELM_ICO_GPS, "GPS解算",
                         rmc ? "完全信 RMC，不过滤" : "自定义滤波",
                         rmc ? "RMC" : "自定义",
                         ACT_CYCLE_GNSS_SOLVER, HELM_KIND_VAL);
        }
        break;

    case HELM_SCR_EPH:
        helm_eph_fill_page(m, &n);
        break;

    case HELM_SCR_TOOLS:
        helm_row_set(m, n++, HELM_ICO_NAV, "指南针", "地磁罗盘", "",
                     ACT_TOOL_COMPASS, HELM_KIND_GO);
        helm_row_set(m, n++, HELM_ICO_CLIMB, "水平仪", "十字圆环", "",
                     ACT_TOOL_LEVEL, HELM_KIND_GO);
        helm_row_set(m, n++, HELM_ICO_BOLT, "加速度计", "G 值", "",
                     ACT_TOOL_GMETER, HELM_KIND_GO);
        helm_row_set(m, n++, HELM_ICO_ALT, "高度计", "气压 / 高度", "",
                     ACT_TOOL_ALT, HELM_KIND_GO);
        break;

    case HELM_SCR_SYSSTAT:
        {
            uint64_t disk_free = 0;
            uint64_t disk_total = 0;
            uint64_t heap_used = 0;
            uint64_t heap_total = 0;
            uint8_t disk_n = 0;
            uint8_t heap_n = 0;
            uint8_t i;
            uint8_t pct;
            char size[16];
            char sub[40];
            char val[16];

            helm_sys_refresh_disks(false);
            helm_sys_refresh_heaps(false);
            helm_sys_refresh_thread_count(false);
            for (i = 0; i < HELM_SYS_DISK_N; i++) {
                if (s_sys_disks[i].valid) {
                    disk_free += s_sys_disks[i].free;
                    disk_total += s_sys_disks[i].total;
                    disk_n++;
                }
            }

            helm_sys_fmt_size(size, sizeof(size), disk_free);
            lv_snprintf(sub, sizeof(sub), "%u 个卷 · 可用 %s",
                        (unsigned)disk_n, size);
            pct = helm_sys_pct(disk_total - disk_free, disk_total);
            lv_snprintf(val, sizeof(val), "%u%%", (unsigned)pct);
            helm_row_set(m, n++, HELM_ICO_SAVE, "磁盘", sub, val,
                         ACT_ENTER_SYS_DISKS, HELM_KIND_GO);
            helm_sys_row_bar(m, (uint8_t)(n - 1u), pct);

            for (i = 0; i < HELM_SYS_HEAP_N; i++) {
                if (s_sys_heaps[i].valid) {
                    heap_used += s_sys_heaps[i].used;
                    heap_total += s_sys_heaps[i].total;
                    heap_n++;
                }
            }

            helm_sys_fmt_size(size, sizeof(size),
                              heap_total > heap_used ?
                              heap_total - heap_used : 0);
            lv_snprintf(sub, sizeof(sub), "%u 个堆 · 可用 %s",
                        (unsigned)heap_n, size);
            pct = helm_sys_pct(heap_used, heap_total);
            lv_snprintf(val, sizeof(val), "%u%%", (unsigned)pct);
            helm_row_set(m, n++, HELM_ICO_CHIP, "内存", sub, val,
                         ACT_ENTER_SYS_MEMORY, HELM_KIND_GO);
            helm_sys_row_bar(m, (uint8_t)(n - 1u), pct);

            lv_snprintf(sub, sizeof(sub), "%u 个运行线程",
                        (unsigned)s_sys_thread_n);
            pct = helm_sys_cpu_used();
            {
                uint32_t mhz = helm_sys_cpu_mhz();

                if (mhz > 0) {
                    lv_snprintf(val, sizeof(val), "%uMHz %u%%",
                                (unsigned)mhz, (unsigned)pct);
                } else {
                    lv_snprintf(val, sizeof(val), "CPU %u%%", (unsigned)pct);
                }
            }
            helm_row_set(m, n++, HELM_ICO_BOLT, "线程", sub, val,
                         ACT_ENTER_SYS_THREADS, HELM_KIND_GO);
            helm_sys_row_bar(m, (uint8_t)(n - 1u), pct);
        }

        {
            /* 系统资源 → 卫星：概览只给总数，按星座的细分在子页。 */
            myvendor_sys_gnss_t g;
            char sub_g[40];
            char val_g[16];
            unsigned view = 0;
            unsigned used = 0;
            unsigned path = 0;
            uint8_t ci;

            if (!myvendor_sys_onboard_gnss_get(&g)) {
                memset(&g, 0, sizeof(g));
            }

            for (ci = 0; ci < MYVENDOR_SYS_GNSS_CONST_N; ci++) {
                view += (unsigned)g.sats_in_view_c[ci];
                used += (unsigned)g.sats_locked_c[ci];
                if (g.sats_in_view_c[ci] > 0u || g.sats_locked_c[ci] > 0u) {
                    path++;
                }
            }

            lv_snprintf(sub_g, sizeof(sub_g), "在视 %u · 参与定位 %u 颗",
                        view, used);
            lv_snprintf(val_g, sizeof(val_g), "%u 路", path);
            helm_row_set(m, n++, HELM_ICO_BOLT, "卫星", sub_g, val_g,
                         ACT_ENTER_SYS_GNSS, HELM_KIND_GO);
        }
        break;

    case HELM_SCR_SYS_DISKS:
        {
            uint8_t i;

            helm_sys_refresh_disks(false);
            for (i = 0; i < HELM_SYS_DISK_N; i++) {
                char sub[40];
                char val[16];

                if (s_sys_disks[i].valid) {
                    char free[16];
                    char total[16];

                    helm_sys_fmt_size(free, sizeof(free), s_sys_disks[i].free);
                    helm_sys_fmt_size(total, sizeof(total),
                                      s_sys_disks[i].total);
                    lv_snprintf(sub, sizeof(sub), "可用 %s · 共 %s",
                                free, total);
                    lv_snprintf(val, sizeof(val), "%u%%",
                                (unsigned)helm_sys_pct(s_sys_disks[i].used,
                                                       s_sys_disks[i].total));
                } else {
                    lv_snprintf(sub, sizeof(sub), "%s", "未挂载");
                    lv_snprintf(val, sizeof(val), "--");
                }

                helm_row_set(m, n++, s_sys_disk_icon[i], s_sys_disk_name[i],
                             sub, val, ACT_NOP, HELM_KIND_VAL);
                if (s_sys_disks[i].valid) {
                    helm_sys_row_free_bar(
                        m, (uint8_t)(n - 1u),
                        helm_sys_pct(s_sys_disks[i].used,
                                     s_sys_disks[i].total));
                } else {
                    helm_sys_row_free_bar(m, (uint8_t)(n - 1u), 0);
                }
            }
        }
        break;

    case HELM_SCR_SYS_MEMORY:
        {
            uint8_t i;

            helm_sys_refresh_heaps(false);
            for (i = 0; i < HELM_SYS_HEAP_N; i++) {
                char sub[40];
                char val[16];

                if (!s_sys_heaps[i].valid) {
                    continue;
                }

                helm_sys_fmt_pair(sub, sizeof(sub), s_sys_heaps[i].used,
                                  s_sys_heaps[i].total);
                lv_snprintf(val, sizeof(val), "%u%%",
                            (unsigned)helm_sys_pct(s_sys_heaps[i].used,
                                                   s_sys_heaps[i].total));
                helm_row_set(m, n++, HELM_ICO_CHIP, s_sys_heap_name[i],
                             sub, val, ACT_NOP, HELM_KIND_VAL);
                helm_sys_row_free_bar(m, (uint8_t)(n - 1u),
                                      helm_sys_pct(s_sys_heaps[i].used,
                                                   s_sys_heaps[i].total));
            }
        }
        empty = n == 0u;
        break;

    case HELM_SCR_SYS_THREADS:
        {
            uint8_t i;

            helm_sys_refresh_threads(false);
            for (i = 0; i < s_sys_threads.count && n < HELM_MENU_ROWS; i++) {
                const helm_sys_thread_t * th = &s_sys_threads.thread[i];
                char sub[40];
                char val[24];
                char used[12];
                char total[12];

                helm_sys_fmt_size(used, sizeof(used), th->stack_used);
                helm_sys_fmt_size(total, sizeof(total), th->stack_total);
#ifdef CONFIG_STACK_COLORATION
                lv_snprintf(sub, sizeof(sub), "PID %d · 栈 %s/%s",
                            (int)th->pid, used, total);
#else
                lv_snprintf(sub, sizeof(sub), "PID %d · 栈 --/%s",
                            (int)th->pid, total);
#endif
#ifndef CONFIG_SCHED_CPULOAD_NONE
                if (th->cpu_valid) {
                    lv_snprintf(val, sizeof(val), "CPU %u.%u%%",
                                (unsigned)(th->cpu_permille / 10u),
                                (unsigned)(th->cpu_permille % 10u));
                } else {
                    lv_snprintf(val, sizeof(val), "CPU --");
                }
#else
                lv_snprintf(val, sizeof(val), "CPU --");
#endif
                helm_row_set(m, n++, HELM_ICO_BOLT,
                             th->name[0] ? th->name : "无名线程",
                             sub, val, ACT_NOP, HELM_KIND_VAL);
                helm_sys_row_free_bar(
                    m, (uint8_t)(n - 1u),
                    th->stack_total > 0u ?
                    helm_sys_pct(th->stack_used, th->stack_total) : 0);
            }
        }
        empty = n == 0u;
        break;

    case HELM_SCR_SYS_GNSS:
        {
            /* 按星座分组：在视（GSV 总数）/ 锁定（GSV 里 SNR>0）/ 参与定位（GSA）。
             * 一行一个星座 —— 哪几路真在跑，看这一页比看 GSV 原文直观。 */
            static const char * const names[MYVENDOR_SYS_GNSS_CONST_N] = {
                "GPS", "GLONASS", "Galileo", "北斗", "QZSS"
            };
            /* 与 GPS/QZSS 同图标：这里是"卫星"语义，不放星座专属图标。 */
            static const uint8_t icons[MYVENDOR_SYS_GNSS_CONST_N] = {
                HELM_ICO_BOLT, HELM_ICO_BOLT, HELM_ICO_BOLT,
                HELM_ICO_BOLT, HELM_ICO_BOLT
            };
            myvendor_sys_gnss_t g;
            uint8_t i;

            if (!myvendor_sys_onboard_gnss_get(&g)) {
                memset(&g, 0, sizeof(g));
            }

            for (i = 0; i < MYVENDOR_SYS_GNSS_CONST_N; i++) {
                char sub[40];
                char val[16];
                unsigned view = (unsigned)g.sats_in_view_c[i];
                unsigned heard = (unsigned)g.sats_heard_c[i];
                unsigned used = (unsigned)g.sats_locked_c[i];
                bool supported = (g.mod_const_mask == 0u) ||
                                 ((g.mod_const_mask & (1u << i)) != 0u);

                if (!supported) {
                    /* 模组自己（MON-VER）就没列这一路：换天线也没用，
                     * 这条提示是给现场看的人一个结论，而不是"0 颗"。 */
                    lv_snprintf(sub, sizeof(sub), "模组不支持（ROM 未列）");
                    lv_snprintf(val, sizeof(val), "%s", "--");
                } else if (view == 0u && heard == 0u && used == 0u) {
                    /* 支持但没数据：可能是当前环境/未启用，标出来别当成 0 颗。 */
                    lv_snprintf(sub, sizeof(sub), "%s", "无数据（未启用/未收到）");
                    lv_snprintf(val, sizeof(val), "%s", "--");
                } else {
                    lv_snprintf(sub, sizeof(sub), "在视 %u · 锁定 %u",
                                view, heard);
                    lv_snprintf(val, sizeof(val), "%u颗", used);
                }

                helm_row_set(m, n++, icons[i], names[i], sub, val,
                             ACT_NOP, HELM_KIND_VAL);
                helm_sys_row_free_bar(
                    m, (uint8_t)(n - 1u),
                    view > 0u ? (heard * 100u) / view : 0u);
            }

            /* 模组身份一行：固件串 + ROM 里声明的星座列表。上一条"模组不支持"
             * 就是拿它判的 —— 现场不用再翻日志。 */
            if (n < HELM_MENU_ROWS) {
                char sub[48];

                if (g.mod_fw[0] != '\0' || g.mod_gnss[0] != '\0') {
                    lv_snprintf(sub, sizeof(sub), "%s",
                                g.mod_fw[0] ? g.mod_fw : "固件未知");
                    helm_row_set(m, n++, HELM_ICO_CHIP, "模组", sub,
                                 g.mod_gnss[0] ? g.mod_gnss : "--",
                                 ACT_NOP, HELM_KIND_VAL);
                }
            }
        }
        empty = n == 0u;
        break;

    default:
        break;
    }

    m->count = n;
    return empty;
}

static const char * helm_scr_title(helm_scr_t scr)
{
    switch (scr) {
    case HELM_SCR_NAV:
        return "导航";
    case HELM_SCR_GPX:
        return "GPX导航";
    case HELM_SCR_GPX_IMPORT:
        return "导入";
    case HELM_SCR_GPX_RECORD:
        return "记录";
    case HELM_SCR_SENSORS:
        return "蓝牙设备";
    case HELM_SCR_RIDES:
        return "骑行记录";
    case HELM_SCR_RIDE_DETAIL:
        {
            static char title[HELM_GPX_NAME_MAX];
            char * dot;

            lv_snprintf(title, sizeof(title), "%s", s_ride_name);
            dot = strrchr(title, '.');
            if (dot != NULL) {
                *dot = '\0';
            }

            return title[0] != '\0' ? title : "记录";
        }
    case HELM_SCR_SETTINGS:
        return "设置";
    case HELM_SCR_TEST:
        return "测试配置";
    case HELM_SCR_EPH:
        return "星历";
    case HELM_SCR_GRADECAL:
        return "坡度校准";
    case HELM_SCR_ABOUT:
        return "固件";
    case HELM_SCR_FONTLAB:
        return "字体试验";
    case HELM_SCR_INBOX:
        return "最近通知";
    case HELM_SCR_NAVPTS:
        return "坐标点导航";
    case HELM_SCR_NAVPT_DETAIL:
        {
            static char title[80];

            if (s_navpt_title[0] == '\0') {
                return "途经点：长按右键";
            }

            lv_snprintf(title, sizeof(title), "%s：长按右键", s_navpt_title);
            return title;
        }
    case HELM_SCR_NAVPT_ACTIONS:
        return s_navpt_title[0] != '\0' ? s_navpt_title : "操作";
    case HELM_SCR_FAVS:
        return "常用点导航：长按右键";
    case HELM_SCR_SCAN:
        return helm_scan_anon(s_scan_kind);
    case HELM_SCR_PHONE:
        return "手机蓝牙";
    case HELM_SCR_SCAN_PICK:
        if (s_scan_link_wait) {
            return "正在连接";
        }

        if (s_scan_phase == HELM_SCAN_WAIT || s_scan_phase == HELM_SCAN_RUN) {
            return "扫描中";
        }

        return "选择设备";
    case HELM_SCR_TOOLS:
        return "工具箱";
    case HELM_SCR_TOOLFACE:
        return helm_toolbox_title_for(s_tool_id);
    case HELM_SCR_SYSSTAT:
        return "系统资源";
    case HELM_SCR_SYS_DISKS:
        return "磁盘";
    case HELM_SCR_SYS_MEMORY:
        return "内存堆";
    case HELM_SCR_SYS_THREADS:
        return "线程";
    case HELM_SCR_SYS_GNSS:
        return "卫星";
    default:
        return "菜单";
    }
}

static void helm_empty_for(helm_menu_t * m, helm_scr_t scr)
{
    if (m->empty == NULL) {
        return;
    }

    switch (scr) {
    case HELM_SCR_INBOX:
        helm_empty_set(m->empty, HELM_ICO_BELL, "没有通知",
                        "手机推送会出现在这里");
        break;
    case HELM_SCR_RIDES:
        helm_empty_set(m->empty, HELM_ICO_LIST, "暂无记录",
                        "骑行结束后会出现在这里");
        break;
    case HELM_SCR_GPX_IMPORT:
        helm_empty_set(m->empty, HELM_ICO_GPX, "暂无导入",
                        "USB 写入 import");
        break;
    case HELM_SCR_GPX_RECORD:
        helm_empty_set(m->empty, HELM_ICO_LIST, "暂无记录",
                        "结束后会出现在这里");
        break;
    case HELM_SCR_NAVPTS:
        helm_empty_set(m->empty, HELM_ICO_NAV, "请用 App 同步",
                        "坐标点由手机下发");
        break;
    case HELM_SCR_NAVPT_DETAIL:
    case HELM_SCR_NAVPT_ACTIONS:
        helm_empty_set(m->empty, HELM_ICO_NAV, "没有途经点",
                        "请在 App 编辑后重新同步");
        break;
    case HELM_SCR_FAVS:
        helm_empty_set(m->empty, HELM_ICO_STAR, "暂无常用点",
                        "请用 App 同步");
        break;
    case HELM_SCR_SCAN:
        helm_empty_set(m->empty, HELM_ICO_BLE, "暂无设备",
                        "点扫描查找附近设备");
        break;
    case HELM_SCR_SCAN_PICK:
        helm_scan_tick();
        if (!myvendor_devctl_sensor_get()) {
            helm_empty_set(m->empty, HELM_ICO_BLE, "外设蓝牙未开",
                          "请在设置中打开外设蓝牙");
        } else if (s_scan_link_wait) {
            helm_empty_set(m->empty, HELM_ICO_BLE, "正在连接",
                          "连接成功后返回");
        } else if (s_scan_phase == HELM_SCAN_WAIT ||
                   s_scan_phase == HELM_SCAN_RUN) {
            helm_empty_set(m->empty, HELM_ICO_BLE, "正在扫描",
                          "发现后右键连接");
        } else {
            helm_empty_set(m->empty, HELM_ICO_BLE, "未发现设备",
                          "返回后重新扫描");
        }
        break;
    default:
        helm_empty_set(m->empty, HELM_ICO_LIST, "空", "");
        break;
    }
}

static bool helm_fav_plan_overlay_open(const helm_menu_t * m);

static void helm_menu_enter_vis(helm_menu_t * m)
{
    lv_obj_t * vis = NULL;

    if (m == NULL || s_list_enter_dir == 0) {
        return;
    }

    if (m->list && !lv_obj_has_flag(m->list, LV_OBJ_FLAG_HIDDEN)) {
        vis = m->list;
    } else if (m->empty && !lv_obj_has_flag(m->empty, LV_OBJ_FLAG_HIDDEN)) {
        vis = m->empty;
    } else if (m->about && !lv_obj_has_flag(m->about, LV_OBJ_FLAG_HIDDEN)) {
        vis = m->about;
    } else if (m->fontlab && !lv_obj_has_flag(m->fontlab, LV_OBJ_FLAG_HIDDEN)) {
        vis = m->fontlab;
    } else if (m->gradecal && !lv_obj_has_flag(m->gradecal, LV_OBJ_FLAG_HIDDEN)) {
        vis = m->gradecal;
    } else if (m->ride && !lv_obj_has_flag(m->ride, LV_OBJ_FLAG_HIDDEN)) {
        vis = m->ride;
    }

    if (vis) {
        helm_obj_enter(vis, s_list_enter_dir);
    }

    s_list_enter_dir = 0;
}

static void helm_menu_clear_motion(lv_obj_t * obj)
{
    if (obj == NULL) {
        return;
    }

    lv_anim_delete(obj, NULL);
    lv_obj_set_style_translate_x(obj, 0, 0);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
}

static bool helm_row_stays(const helm_row_t * r)
{
    if (r == NULL) {
        return false;
    }

    if (r->kind == HELM_KIND_SW || r->kind == HELM_KIND_VAL) {
        return true;
    }

    switch (r->act) {
    case ACT_SCAN_AUTO:
    case ACT_SCAN_CONNECT:
    case ACT_NAV_STOP:
    case ACT_NAV_SKIP:
    case ACT_NAV_NEAREST:
    case ACT_RIDE_DEL:
    case ACT_INBOX_OPEN:
        return true;
    default:
        return false;
    }
}

static lv_obj_t * helm_menu_active_list(helm_menu_t * m)
{
    if (m == NULL) {
        return NULL;
    }

    if (m->ride_keys && m->ride &&
        !lv_obj_has_flag(m->ride, LV_OBJ_FLAG_HIDDEN)) {
        return m->ride_keys;
    }

    if (m->list && !lv_obj_has_flag(m->list, LV_OBJ_FLAG_HIDDEN)) {
        return m->list;
    }

    return NULL;
}

static lv_obj_t * helm_menu_sel_row(helm_menu_t * m)
{
    uint8_t * sel;
    lv_obj_t * list;

    sel = helm_cur_sel(m);
    list = helm_menu_active_list(m);
    if (sel == NULL || list == NULL || *sel >= lv_obj_get_child_count(list)) {
        return NULL;
    }

    return lv_obj_get_child(list, *sel);
}

static void helm_menu_squash_drop_obj(void)
{
    if (s_squash) {
        helm_obj_squash_set(s_squash, false);
        s_squash = NULL;
    }
}

static void helm_menu_squash_abort(void)
{
    helm_menu_squash_drop_obj();
    s_squash_restore = false;
}

static void helm_menu_spec_from_row(const helm_menu_t * m, uint8_t i,
                                    const uint8_t * sel,
                                    const lv_font_t * notif_font,
                                    helm_item_t * spec)
{
    memset(spec, 0, sizeof(*spec));
    spec->ico = m->items[i].ico;
    spec->title = m->items[i].label;
    spec->sub = m->items[i].sub;
    spec->value = m->items[i].value;
    spec->kind = m->items[i].kind;
    spec->on = m->items[i].on;
    spec->sel = (sel && *sel == i);
    spec->progress_on = m->items[i].progress_on;
    spec->progress = m->items[i].progress;
    spec->progress_color = m->items[i].progress_color;
    spec->title_font = notif_font;
}

static void helm_menu_squash_after_paint(helm_menu_t * m)
{
    lv_obj_t * row;

    s_squash = NULL;
    if (!s_squash_restore) {
        return;
    }

    s_squash_restore = false;
    row = helm_menu_sel_row(m);
    if (row) {
        helm_obj_squash_set(row, true);
        helm_obj_squash(row, false);
    }
}

static void helm_menu_paint(helm_menu_t * m)
{
    helm_scr_t scr;
    uint8_t * sel;
    uint8_t i;
    bool about;
    bool empty;

    if (m == NULL || m->list == NULL || m->title == NULL) {
        return;
    }

    helm_menu_squash_drop_obj();
    helm_menu_clear_motion(m->list);
    helm_menu_clear_motion(m->empty);
    helm_menu_clear_motion(m->about);
    helm_menu_clear_motion(m->fontlab);
    helm_menu_clear_motion(m->gradecal);
    helm_menu_clear_motion(m->ride);
    helm_menu_clear_motion(m->toolface);

    scr = helm_cur_scr(m);
    about = (scr == HELM_SCR_ABOUT);
    if (m->fav_planning) {
        helm_mhead_set(m->title, "正在规划…");
    } else if (m->fav_plan_fail) {
        helm_mhead_set(m->title, "规划失败");
    } else {
        helm_mhead_set(m->title, helm_scr_title(scr));
        if (scr == HELM_SCR_RIDE_DETAIL) {
            helm_mhead_set_font(m->title,
                                helm_font_sys(15, helm_font_title()));
        }
    }
    empty = helm_fill_items(m);
    if (scr == HELM_SCR_SYSSTAT || scr == HELM_SCR_SYS_DISKS ||
        scr == HELM_SCR_SYS_MEMORY || scr == HELM_SCR_SYS_THREADS ||
        scr == HELM_SCR_SYS_GNSS) {
        s_sys_ui_sig = helm_sys_items_sig(m);
    }

    if (m->skip_dock) {
        lv_obj_add_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN);
    }

    if (m->del_dock) {
        lv_obj_add_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);
    }

    if (about || scr == HELM_SCR_FONTLAB || scr == HELM_SCR_GRADECAL
        || scr == HELM_SCR_RIDE_DETAIL || scr == HELM_SCR_TOOLFACE) {
        lv_obj_add_flag(m->list, LV_OBJ_FLAG_HIDDEN);
        if (m->empty) {
            lv_obj_add_flag(m->empty, LV_OBJ_FLAG_HIDDEN);
        }

        if (m->about) {
            if (about) {
                lv_obj_clear_flag(m->about, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(m->about, LV_OBJ_FLAG_HIDDEN);
            }
        }

        if (m->fontlab) {
            if (scr == HELM_SCR_FONTLAB) {
                lv_obj_clear_flag(m->fontlab, LV_OBJ_FLAG_HIDDEN);
                helm_font_lab_refresh();
            } else {
                lv_obj_add_flag(m->fontlab, LV_OBJ_FLAG_HIDDEN);
            }
        }

        if (m->gradecal) {
            if (scr == HELM_SCR_GRADECAL) {
                lv_obj_clear_flag(m->gradecal, LV_OBJ_FLAG_HIDDEN);
                helm_grade_refresh(m);
            } else {
                lv_obj_add_flag(m->gradecal, LV_OBJ_FLAG_HIDDEN);
            }
        }

        if (m->ride) {
            if (scr == HELM_SCR_RIDE_DETAIL) {
                uint8_t * rsel = helm_cur_sel(m);

                if (rsel && m->count > 0 && *rsel >= m->count) {
                    *rsel = 0;
                }
                lv_obj_clear_flag(m->ride, LV_OBJ_FLAG_HIDDEN);
                helm_ride_refresh(m);
            } else {
                lv_obj_add_flag(m->ride, LV_OBJ_FLAG_HIDDEN);
                helm_mlist_cursor_off(m->ride_keys);
            }
        }

        if (m->toolface) {
            if (scr == HELM_SCR_TOOLFACE) {
                helm_toolbox_open(s_tool_id);
                helm_mhead_set(m->title, helm_toolbox_title());
            } else {
                helm_toolbox_hide();
            }
        }

        s_sensor_sig = helm_sensor_sig();
        if (m->poll) {
            if (scr == HELM_SCR_GRADECAL) {
                lv_timer_resume(m->poll);
            } else {
                lv_timer_pause(m->poll);
            }
        }

        helm_menu_squash_after_paint(m);
        helm_menu_enter_vis(m);
        return;
    }

    if (m->about) {
        lv_obj_add_flag(m->about, LV_OBJ_FLAG_HIDDEN);
    }

    if (m->fontlab) {
        lv_obj_add_flag(m->fontlab, LV_OBJ_FLAG_HIDDEN);
    }

    if (m->gradecal) {
        lv_obj_add_flag(m->gradecal, LV_OBJ_FLAG_HIDDEN);
    }

    if (m->ride) {
        lv_obj_add_flag(m->ride, LV_OBJ_FLAG_HIDDEN);
        helm_mlist_cursor_off(m->ride_keys);
    }

    helm_toolbox_hide();

    if (empty) {
        lv_obj_add_flag(m->list, LV_OBJ_FLAG_HIDDEN);
        if (m->empty) {
            helm_empty_for(m, scr);
            lv_obj_clear_flag(m->empty, LV_OBJ_FLAG_HIDDEN);
        }

        s_sensor_sig = helm_sensor_sig();
        if (m->poll) {
            if (helm_scr_needs_sensor_poll(scr)) {
                lv_timer_resume(m->poll);
            } else {
                lv_timer_pause(m->poll);
            }
        }

        helm_menu_squash_after_paint(m);
        helm_menu_enter_vis(m);
        return;
    }

    if (m->empty) {
        lv_obj_add_flag(m->empty, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clear_flag(m->list, LV_OBJ_FLAG_HIDDEN);
    sel = helm_cur_sel(m);
    if (sel && m->count > 0 && *sel >= m->count) {
        *sel = 0;
    }

    if (scr == HELM_SCR_SCAN_PICK && s_scan_jump && sel != NULL) {
        uint8_t pick;

        for (pick = 0; pick < m->count; pick++) {
            if (m->items[pick].act == ACT_SCAN_CONNECT &&
                m->items[pick].extra != 0) {
                *sel = pick;
                s_scan_jump = false;
                break;
            }
        }
    }

    {
        const lv_font_t * notif_font = NULL;
        bool reuse = false;

        if (scr == HELM_SCR_INBOX || scr == HELM_SCR_GPX_IMPORT ||
            scr == HELM_SCR_GPX_RECORD || scr == HELM_SCR_RIDES ||
            scr == HELM_SCR_NAVPTS ||
            scr == HELM_SCR_NAVPT_DETAIL ||
            scr == HELM_SCR_FAVS) {
            notif_font = helm_font_sys(15, helm_font_title());
        }

        if (m->count > 0 && lv_obj_get_child_count(m->list) == m->count) {
            reuse = true;
            for (i = 0; i < m->count; i++) {
                helm_item_t spec;
                lv_obj_t * row = lv_obj_get_child(m->list, i);

                helm_menu_spec_from_row(m, i, sel, notif_font, &spec);
                if (!helm_mitem_refresh(row, &spec)) {
                    reuse = false;
                    break;
                }
            }
        }

        if (!reuse) {
            lv_obj_clean(m->list);
            for (i = 0; i < m->count; i++) {
                helm_item_t spec;

                helm_menu_spec_from_row(m, i, sel, notif_font, &spec);
                helm_mitem_create(m->list, &spec);
            }
        }

        if (sel && m->count > 0) {
            uint8_t idx = *sel;

            if (idx >= m->count) {
                idx = 0;
                *sel = 0;
            }
            lv_obj_update_layout(m->list);
            helm_mlist_sel_snap(m->list, idx);
        }
    }

    s_sensor_sig = helm_sensor_sig();
    if (m->poll) {
        if (helm_scr_needs_sensor_poll(scr)) {
            lv_timer_resume(m->poll);
        } else {
            lv_timer_pause(m->poll);
        }
    }

    helm_menu_squash_after_paint(m);
    helm_menu_enter_vis(m);
}

static void helm_scan_pick_finish(helm_menu_t * m)
{
    s_scan_link_wait = false;
    helm_scan_idle_ui();
    myvendor_sound_ok();
    lv_pm_notify_show("蓝牙设备", "已连接", 1500);
    if (m != NULL && m->sp > 1 && helm_cur_scr(m) == HELM_SCR_SCAN_PICK) {
        m->sp--;
        s_list_enter_dir = -1;
    }

    helm_menu_paint(m);
}

static uint32_t helm_sys_items_sig(const helm_menu_t * m)
{
    uint32_t s = 2166136261u;
    uint8_t i;

    if (m == NULL) {
        return 0;
    }

    s ^= m->count;
    s *= 16777619u;
    for (i = 0; i < m->count; i++) {
        const helm_row_t * r = &m->items[i];
        const char * p;

        s ^= r->progress;
        s *= 16777619u;
        s ^= r->progress_color;
        s *= 16777619u;
        for (p = r->value; *p != '\0'; p++) {
            s ^= (uint8_t)*p;
            s *= 16777619u;
        }

        for (p = r->sub; *p != '\0'; p++) {
            s ^= (uint8_t)*p;
            s *= 16777619u;
        }
    }

    return s;
}

static bool helm_sys_live_paint(helm_menu_t * m)
{
    uint8_t * sel;
    uint8_t i;
    uint32_t sig;

    if (m == NULL || m->list == NULL ||
        lv_obj_has_flag(m->list, LV_OBJ_FLAG_HIDDEN) ||
        lv_obj_get_child_count(m->list) != m->count) {
        return false;
    }

    sig = helm_sys_items_sig(m);
    if (sig == s_sys_ui_sig) {
        return true;
    }

    sel = helm_cur_sel(m);
    for (i = 0; i < m->count; i++) {
        helm_item_t spec;
        lv_obj_t * row = lv_obj_get_child(m->list, i);

        helm_menu_spec_from_row(m, i, sel, NULL, &spec);
        spec.live = true;
        if (!helm_mitem_refresh(row, &spec)) {
            return false;
        }
    }

    s_sys_ui_sig = sig;
    return true;
}

static bool helm_sys_poll_paint(helm_menu_t * m)
{
    if (m == NULL) {
        return false;
    }

    (void)helm_fill_items(m);
    return helm_sys_live_paint(m);
}

static void helm_poll_cb(lv_timer_t * t)
{
    helm_menu_t * m = s_menu;
    myvendor_sys_sensor_ui_t ui;
    static uint32_t sys_ui_tick;
    helm_scr_t scr;

    LV_UNUSED(t);
    if (m == NULL) {
        return;
    }

    scr = helm_cur_scr(m);
    helm_scan_tick();
    if (s_scan_link_wait && scr == HELM_SCR_SCAN_PICK &&
        s_scan_kind < MYVENDOR_SYS_SENSOR_KIND_N) {
        myvendor_sys_sensor_ui_get(&ui);
        if (ui.slot[s_scan_kind].link == MYVENDOR_SYS_SENSOR_LINK_READY) {
            helm_scan_pick_finish(m);
            return;
        }
    }
    if (scr == HELM_SCR_GRADECAL) {
        helm_grade_refresh(m);
        return;
    }

    if (scr == HELM_SCR_SYSSTAT || scr == HELM_SCR_SYS_DISKS ||
        scr == HELM_SCR_SYS_MEMORY || scr == HELM_SCR_SYS_THREADS ||
        scr == HELM_SCR_SYS_GNSS) {
        uint32_t wait = 1000u;

        if (scr == HELM_SCR_SYS_DISKS) {
            wait = 2000u;
        } else if (scr == HELM_SCR_SYS_MEMORY) {
            wait = 1500u;
        } else if (scr == HELM_SCR_SYS_THREADS) {
            wait = 1200u;
        }

        if (lv_tick_elaps(sys_ui_tick) >= wait) {
            sys_ui_tick = lv_tick_get();
            if (!helm_sys_poll_paint(m)) {
                helm_menu_paint(m);
            }
        }

        return;
    }

    if (scr == HELM_SCR_EPH) {
        static uint32_t s_eph_sig;
        uint32_t last = 0;
        uint32_t next = 0;
        uint32_t sig;

        (void)myvendor_gnss_eph_times(&last, &next);
        sig = last ^ next ^ (uint32_t)(time(NULL) / 60) ^
              (myvendor_devctl_eph_auto_get() ? 1u : 2u);
        if (sig == s_eph_sig) {
            return;
        }

        s_eph_sig = sig;
        helm_menu_paint(m);
        return;
    }

    if (helm_sensor_sig() == s_sensor_sig) {
        return;
    }

    helm_menu_paint(m);
}

static void helm_fav_plan_timer_cb(lv_timer_t * timer);
static void helm_fav_plan_on_input(helm_menu_t * m);

static bool helm_fav_plan_overlay_open(const helm_menu_t * m)
{
    return m != NULL && (m->fav_planning || m->fav_plan_fail);
}

static void helm_fav_plan_raise_chrome(void)
{
    lv_obj_t * chrome = lv_pm_status_bar_cont();

    if (chrome != NULL && !lv_obj_has_flag(chrome, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(chrome);
    }
}

static void helm_fav_plan_place_mask(helm_menu_t * m)
{
    if (!m || !m->plan_mask) {
        return;
    }
    lv_obj_set_size(m->plan_mask, PAGE_HOR_RES, HELM_PAGE_H - HELM_MHEAD_H);
    lv_obj_align(m->plan_mask, LV_ALIGN_TOP_LEFT, 0, HELM_MHEAD_H);
}

static void helm_fav_plan_mask_clicked(lv_event_t * e)
{
    helm_fav_plan_on_input((helm_menu_t *)lv_event_get_user_data(e));
}

static void helm_fav_plan_hide(helm_menu_t * m)
{
    if (!m) {
        return;
    }
    if (m->fav_plan_timer) {
        lv_timer_delete(m->fav_plan_timer);
        m->fav_plan_timer = NULL;
    }
    m->fav_planning = false;
    m->fav_plan_fail = false;
    m->fav_plan_kick = false;
    m->fav_plan_phase = 0u;
    m->fav_notice_t0 = 0u;
    m->fav_notice_ms = 0u;
    if (m->plan_mask) {
        lv_obj_add_flag(m->plan_mask, LV_OBJ_FLAG_HIDDEN);
    }
    if (m->title && (helm_cur_scr(m) == HELM_SCR_FAVS ||
                     helm_cur_scr(m) == HELM_SCR_NAVPT_DETAIL ||
                     helm_cur_scr(m) == HELM_SCR_NAVPT_ACTIONS)) {
        helm_mhead_set(m->title, helm_scr_title(helm_cur_scr(m)));
    }
}

static void helm_fav_plan_present(helm_menu_t * m, const char * msg,
    const char * hint)
{
    if (!m || !m->plan_mask) {
        return;
    }
    if (m->plan_msg) {
        lv_label_set_text(m->plan_msg, msg ? msg : "");
    }
    if (m->plan_hint) {
        lv_label_set_text(m->plan_hint, hint ? hint : "");
    }
    if (m->plan_head) {
        lv_label_set_text(m->plan_head,
            m->fav_planning ? "正在规划" : "规划失败");
    }
    lv_obj_clear_flag(m->plan_mask, LV_OBJ_FLAG_HIDDEN);
    helm_fav_plan_place_mask(m);
    lv_obj_move_foreground(m->plan_mask);
    if (m->title) {
        helm_mhead_set(m->title, m->fav_planning ? "正在规划…" :
            (msg ? msg : "规划失败"));
    }
    if (m->root) {
        lv_obj_update_layout(m->root);
    }
    helm_fav_plan_place_mask(m);
    if (m->plan_mask) {
        lv_obj_update_layout(m->plan_mask);
        lv_obj_move_foreground(m->plan_mask);
    }
    helm_fav_plan_raise_chrome();
}

static void helm_fav_plan_update(helm_menu_t * m)
{
    static const char spin[] = "|/-\\";
    char text[40];

    if (!m || !m->plan_msg) {
        return;
    }
    lv_snprintf(text, sizeof(text), "正在规划路线 %c",
        spin[m->fav_plan_phase & 3u]);
    m->fav_plan_phase++;
    lv_label_set_text(m->plan_msg, text);
}

static void helm_fav_notice(helm_menu_t * m, const char * text, uint32_t ms)
{
    if (!m || !m->title || !text) {
        return;
    }
    m->fav_planning = false;
    m->fav_plan_fail = false;
    m->fav_plan_kick = false;
    m->fav_notice_t0 = lv_tick_get();
    m->fav_notice_ms = ms;
    if (m->plan_mask) {
        lv_obj_add_flag(m->plan_mask, LV_OBJ_FLAG_HIDDEN);
    }
    helm_mhead_set(m->title, text);
    if (!m->fav_plan_timer) {
        m->fav_plan_timer = lv_timer_create(helm_fav_plan_timer_cb, 250u, m);
    }
}

static void helm_fav_plan_fail(helm_menu_t * m, const char * msg)
{
    if (!m) {
        return;
    }
    if (m->fav_plan_timer) {
        lv_timer_delete(m->fav_plan_timer);
        m->fav_plan_timer = NULL;
    }
    m->fav_planning = false;
    m->fav_plan_fail = true;
    m->fav_plan_kick = false;
    m->fav_notice_t0 = 0u;
    m->fav_notice_ms = 0u;
    helm_fav_plan_present(m, msg ? msg : "路线规划失败", "按键关闭");
    myvendor_sound_warn();
    lv_refr_now(NULL);
}

static void helm_fav_plan_do_submit(helm_menu_t * m)
{
    myvendor_devctl_favorite_t favs[MYVENDOR_DEVCTL_FAVORITE_MAX];
    vmap_route_waypoint_t wps[MYVENDOR_DEVCTL_FAVORITE_MAX];
    const char * names[MYVENDOR_DEVCTL_FAVORITE_MAX];
    size_t fav_n = 0;
    uint8_t i;

    bicycle_gnss_fix_t fix;

    if (!m || !m->fav_planning) {
        return;
    }
    if (!bicycle_runtime_poll_fix(&fix) || !fix.valid) {
        helm_fav_plan_fail(m, "无定位，无法规划");
        return;
    }
    if (myvendor_devctl_waypoints_load(
            (s_navpt_path[0] != '\0') ? s_navpt_path :
            MYVENDOR_DEVCTL_FAVORITES_PATH,
            favs, MYVENDOR_DEVCTL_FAVORITE_MAX, &fav_n) != 0) {
        helm_fav_plan_fail(m, s_navpt_path[0] != '\0' ?
            "坐标点读取失败" : "常用点读取失败");
        return;
    }
    for (i = 0u; i < m->fav_order_n; i++) {
        const uint8_t idx = m->fav_order[i];

        if ((size_t)idx >= fav_n) {
            m->fav_order_n = 0u;
            helm_menu_paint(m);
            helm_fav_plan_fail(m, s_navpt_path[0] != '\0' ?
                "坐标点已变化，请重试" : "常用点已变化，请重试");
            return;
        }
        wps[i].lon = favs[idx].longitude;
        wps[i].lat = favs[idx].latitude;
        names[i] = favs[idx].name;
    }
    /* 站在某站上＝那一站已经过掉（trip 的语义是「当前坐标不算」），规划时会
     * 自动跳过它（见 map_page_nav_trip_plan 里的 skip）。所以要报「已在终点
     * 附近」，判据必须是**整条行程一站都还没到**，而不是只看第 1 个点 ——
     * 以前盯 wps[0]，于是站在第 1 个途经点上就误报「已在终点附近」。 */
    if (vmap_route_trip_first_unreached(wps, 0u, m->fav_order_n,
            (double)fix.longitude, (double)fix.latitude) >= m->fav_order_n) {
        helm_fav_plan_fail(m, "已在终点附近");
        return;
    }
    if (!map_page_nav_trip_plan(lvgl_page_map(), wps, m->fav_order_n)) {
        helm_fav_plan_fail(m, "无法开始规划");
        return;
    }
    map_page_nav_trip_set_names(lvgl_page_map(), names, m->fav_order_n);
    myvendor_sound_ok();
}

static void helm_fav_plan_timer_cb(lv_timer_t * timer)
{
    helm_menu_t * m = (helm_menu_t *)lv_timer_get_user_data(timer);
    map_page_t * map = lvgl_page_map();

    if (!m) {
        return;
    }
    if (m->fav_plan_kick) {
        m->fav_plan_kick = false;
        lv_timer_set_period(timer, 250u);
        helm_fav_plan_do_submit(m);
        return;
    }
    if (!m->fav_planning) {
        if (m->fav_notice_ms != 0u
            && lv_tick_elaps(m->fav_notice_t0) >= m->fav_notice_ms) {
            helm_fav_plan_hide(m);
        }
        return;
    }
    if (map_page_nav_active(map)) {
        helm_fav_plan_hide(m);
        (void)lv_pm_close_page_msg(NULL);
        return;
    }
    if (!map_page_nav_planning(map)) {
        helm_fav_plan_fail(m, "路线规划失败");
        return;
    }
    helm_fav_plan_update(m);
}

static void helm_fav_plan_show(helm_menu_t * m)
{
    bicycle_gnss_fix_t fix;

    if (!m) {
        return;
    }
    if (!bicycle_runtime_poll_fix(&fix) || !fix.valid) {
        helm_fav_plan_fail(m, "无定位，无法规划");
        return;
    }
    m->fav_planning = true;
    m->fav_plan_fail = false;
    m->fav_plan_kick = true;
    m->fav_plan_phase = 0u;
    m->fav_notice_t0 = 0u;
    m->fav_notice_ms = 0u;
    helm_fav_plan_present(m, "正在规划路线", "单击停止");
    helm_fav_plan_update(m);
    if (m->fav_plan_timer) {
        lv_timer_delete(m->fav_plan_timer);
        m->fav_plan_timer = NULL;
    }
    m->fav_plan_timer = lv_timer_create(helm_fav_plan_timer_cb, 30u, m);
    lv_refr_now(NULL);
}

static bool helm_navpt_prepare_order(helm_menu_t * m, bool reverse)
{
    myvendor_devctl_favorite_t pts[MYVENDOR_DEVCTL_FAVORITE_MAX];
    size_t pt_n = 0;
    uint8_t i;

    if (m == NULL || s_navpt_path[0] == '\0') {
        return false;
    }

    if (myvendor_devctl_waypoints_load(s_navpt_path, pts,
            MYVENDOR_DEVCTL_FAVORITE_MAX, &pt_n) != 0 ||
        pt_n == 0) {
        return false;
    }

    m->fav_order_n = (uint8_t)pt_n;
    for (i = 0u; i < m->fav_order_n; i++) {
        m->fav_order[i] = reverse ? (uint8_t)(m->fav_order_n - 1u - i) : i;
    }

    return true;
}

static void helm_navpt_start(helm_menu_t * m, bool reverse)
{
    if (helm_fav_plan_overlay_open(m)) {
        return;
    }

    if (!helm_navpt_prepare_order(m, reverse)) {
        myvendor_sound_warn();
        helm_fav_notice(m, "坐标点读取失败", 2000u);
        return;
    }

    helm_fav_plan_show(m);
}

static void helm_fav_plan_on_input(helm_menu_t * m)
{
    if (!m) {
        return;
    }
    if (m->fav_planning) {
        m->fav_planning = false;
        map_page_nav_stop(lvgl_page_map());
        helm_fav_plan_hide(m);
        myvendor_sound_back();
        return;
    }
    if (m->fav_plan_fail) {
        helm_fav_plan_hide(m);
        myvendor_sound_back();
    }
}

static void helm_push(helm_menu_t * m, helm_scr_t scr)
{
    if (m->sp >= HELM_MENU_STACK) {
        return;
    }

    m->stack[m->sp].scr = scr;
    m->stack[m->sp].sel = 0;
    m->sp++;
    s_list_enter_dir = 1;
    helm_menu_paint(m);
}

static void helm_back(helm_menu_t * m)
{
    if (m->del_dock && !lv_obj_has_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);
        s_sensor_del = false;
        return;
    }

    if (m->skip_dock && !lv_obj_has_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (helm_cur_scr(m) == HELM_SCR_SCAN_PICK) {
        helm_scan_end();
    }

    if (m->sp <= 1) {
        (void)lv_pm_close_page_msg(NULL);
        return;
    }

    m->sp--;
    s_list_enter_dir = -1;
    helm_menu_paint(m);
}

static void helm_wait_app(const char * title)
{
    myvendor_sound_warn();
    lv_pm_notify_show(title, "等待 App 下发", 2000);
}

static void helm_show_skip(helm_menu_t * m)
{
    if (m->skip_dock) {
        lv_obj_clear_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(m->skip_dock);
    }
}

static void helm_show_del(helm_menu_t * m)
{
    if (m == NULL || m->del_dock == NULL) {
        return;
    }

    myvendor_sound_warn();
    if (m->del_lab) {
        lv_label_set_text(m->del_lab, "删除此记录？");
    }

    lv_obj_clear_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(m->del_dock);
}

/**
 * @brief 「手机蓝牙」页看门狗（1 s）：状态翻了才重画。
 *
 * 配对（记录被 companion 写出来）和解绑（记录被清掉）都是**异步**的：UI 只投命令，
 * 真正的读写发生在 companion 线程下一拍 —— 所以确认之后立刻重画读到的还是旧状态
 * （实测"解绑后仍显示已配对"）。这里只在**当前就在这一页、且状态真的和上一帧不同**
 * 时才重画：不每秒都画（免得干扰选中/开关动画），配对成功后也能自动变成"已配对"。
 */
void helm_phone_watch_cb(lv_timer_t * t)
{
    helm_menu_t * m = s_menu;

    LV_UNUSED(t);

    if (m == NULL || helm_cur_scr(m) != HELM_SCR_PHONE) {
        return;
    }

    {
        const unsigned left_s =
            (myvendor_devctl_pair_window_left_ms() + 999u) / 1000u;
        const bool paired_now = (myvendor_devctl_pair_phone(NULL, 0) == 1);

        /* 配对状态翻了 或 倒计时秒数变了 → 重画（倒计时每秒一次，别的都不动）。 */
        if (paired_now != s_phone_shown_paired || left_s != s_phone_shown_sec) {
            helm_menu_paint(s_menu);
        }
    }
}

static bool helm_ride_sel_bind(helm_menu_t * m)
{
    uint8_t * sel;
    uint8_t idx;

    if (m == NULL) {
        return false;
    }

    sel = helm_cur_sel(m);
    if (sel == NULL || *sel >= m->count) {
        return false;
    }

    if (m->items[*sel].act != ACT_RIDE_OPEN) {
        return false;
    }

    idx = m->items[*sel].extra;
    if (idx >= s_gpx_n) {
        return false;
    }

    return helm_ride_bind(s_gpx_names[idx], s_gpx_dir);
}

static void helm_ride_do_del(helm_menu_t * m)
{
    char path[160];
    helm_scr_t scr;

    scr = (m != NULL) ? helm_cur_scr(m) : HELM_SCR_ROOT;
    if (m != NULL && m->del_dock != NULL) {
        lv_obj_add_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);
    }

    if (scr == HELM_SCR_NAVPT_DETAIL || scr == HELM_SCR_NAVPT_ACTIONS) {
        if (s_navpt_path[0] == '\0' || unlink(s_navpt_path) != 0) {
            myvendor_sound_warn();
            lv_pm_notify_show("删除", "失败", 2000);
            return;
        }

        helm_navpt_clear_bind();
        myvendor_sound_ok();
        lv_pm_notify_show("删除", "已移除", 1500);
        if (m != NULL) {
            while (m->sp > 1) {
                helm_scr_t cur = m->stack[m->sp - 1u].scr;

                if (cur != HELM_SCR_NAVPT_DETAIL &&
                    cur != HELM_SCR_NAVPT_ACTIONS) {
                    break;
                }

                m->sp--;
            }

            s_list_enter_dir = -1;
            helm_menu_paint(m);
        }

        return;
    }

    if (!helm_ride_path(path, sizeof(path))) {
        return;
    }

    if (unlink(path) != 0) {
        myvendor_sound_warn();
        lv_pm_notify_show("删除", "失败", 2000);
        return;
    }

    helm_gpx_stat_forget(s_ride_name);
    s_ride_name[0] = '\0';
    s_ride_loaded[0] = '\0';
    s_ride_dir = NULL;
    s_ride_loaded_dir = NULL;
    s_ride_pt_n = 0;
    s_ride_km = 0.0;
    s_ride_sec = 0;
    s_ride_has_time = false;
    myvendor_sound_ok();
    lv_pm_notify_show("删除", "已移除", 1500);
    if (m == NULL) {
        return;
    }

    if (scr == HELM_SCR_RIDE_DETAIL) {
        helm_back(m);
    } else {
        helm_menu_paint(m);
    }
}

static void helm_sensor_do_del(helm_menu_t * m)
{
    if (m != NULL && m->del_dock != NULL) {
        lv_obj_add_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);
    }

    if (!s_sensor_del) {
        return;
    }

    s_sensor_del = false;
    if (myvendor_devctl_sensor_delete(s_sensor_del_idx) != 0) {
        myvendor_sound_warn();
        lv_pm_notify_show("删除", "失败", 2000);
        return;
    }

    myvendor_sound_ok();
    lv_pm_notify_show("删除", "已移除", 1500);
    if (m != NULL) {
        helm_menu_paint(m);
    }
}

static void helm_do_act(helm_menu_t * m, uint8_t act)
{
    switch (act) {
    case ACT_ENTER_PHONE:
        /* 只进页面，**不开窗**：开配对必须由用户在页内点击 + 弹窗确认
         * （`开始配对？` → 确认；见点击路径里 HELM_SCR_PHONE 分支）。
         * 顺带清掉上次留下的"待确认"，免得一进来第一下点击就把旧确认吃掉了。 */
        s_phone_pair_arm = false;
        s_phone_unbind_arm = false;
        printf("helm: phone page enter\n");
        helm_push(m, HELM_SCR_PHONE);
        /* 看门狗：配对/解绑是异步的，状态翻了就重画（只在这一页生效）。 */
        if (s_phone_watch == NULL) {
            extern void helm_phone_watch_cb(lv_timer_t * t);

            s_phone_watch = lv_timer_create(helm_phone_watch_cb, 1000, NULL);
        }
        break;
    /* 「手机蓝牙」子界面里的动作（2026-09-20）。都在 companion 线程执行：
     * 开窗会把广播从"不广播/定向"切到可发现；解绑清密钥 + 清手机记录。 */
    case ACT_PAIR_OPEN:
        (void)myvendor_devctl_pair_open(30);
        lv_pm_notify_show("手机蓝牙", "已开配对 30 秒", 1500);
        break;
    case ACT_PAIR_UNBIND:
        (void)myvendor_devctl_pair_unbind();
        lv_pm_notify_show("手机蓝牙", "已解除绑定", 1500);
        break;
    case ACT_ENTER_NAV:
        helm_push(m, HELM_SCR_NAV);
        break;
    case ACT_ENTER_GPX:
        helm_push(m, HELM_SCR_GPX);
        break;
    case ACT_ENTER_SENSORS:
        helm_push(m, HELM_SCR_SENSORS);
        break;
    case ACT_ENTER_RIDES:
        helm_push(m, HELM_SCR_RIDES);
        break;
    case ACT_ENTER_SETTINGS:
        helm_push(m, HELM_SCR_SETTINGS);
        break;
    case ACT_ENTER_TEST:
        helm_push(m, HELM_SCR_TEST);
        break;
    case ACT_ENTER_EPH:
        helm_push(m, HELM_SCR_EPH);
        break;
    case ACT_ENTER_ABOUT:
        helm_push(m, HELM_SCR_ABOUT);
        break;
    case ACT_ENTER_FONTLAB:
        helm_push(m, HELM_SCR_FONTLAB);
        break;
    case ACT_ENTER_GRADECAL:
        helm_push(m, HELM_SCR_GRADECAL);
        break;
    case ACT_ENTER_INBOX:
        helm_push(m, HELM_SCR_INBOX);
        break;
    case ACT_ENTER_NAVPTS:
        helm_navpt_clear_bind();
        helm_push(m, HELM_SCR_NAVPTS);
        break;
    case ACT_ENTER_NAVPT_REC:
        {
            uint8_t * sel = helm_cur_sel(m);
            uint8_t idx;
            myvendor_devctl_favorite_t pts[MYVENDOR_DEVCTL_FAVORITE_MAX];
            size_t pt_n = 0;
            char * dot;

            if (sel == NULL || *sel >= m->count) {
                break;
            }

            idx = m->items[*sel].extra;
            if (idx >= s_gpx_n) {
                break;
            }

            lv_snprintf(s_navpt_path, sizeof(s_navpt_path), "%s/%s",
                        MYVENDOR_NAVPTS_DIR, s_gpx_names[idx]);
            lv_snprintf(s_navpt_title, sizeof(s_navpt_title), "%s",
                        s_gpx_names[idx]);
            dot = strrchr(s_navpt_title, '.');
            if (dot != NULL) {
                *dot = '\0';
            }

            if (myvendor_devctl_waypoints_load(s_navpt_path, pts,
                    MYVENDOR_DEVCTL_FAVORITE_MAX, &pt_n) != 0 ||
                pt_n == 0) {
                myvendor_sound_warn();
                helm_navpt_clear_bind();
                break;
            }

            helm_push(m, HELM_SCR_NAVPT_DETAIL);
        }
        break;
    case ACT_ENTER_FAVS:
        helm_navpt_clear_bind();
        m->fav_order_n = 0u;
        helm_push(m, HELM_SCR_FAVS);
        break;
    case ACT_ENTER_TOOLS:
        helm_push(m, HELM_SCR_TOOLS);
        break;
    case ACT_ENTER_SYSSTAT:
        helm_push(m, HELM_SCR_SYSSTAT);
        break;
    case ACT_ENTER_SYS_DISKS:
        helm_sys_refresh_disks(true);
        helm_push(m, HELM_SCR_SYS_DISKS);
        break;
    case ACT_ENTER_SYS_MEMORY:
        helm_sys_refresh_heaps(true);
        helm_push(m, HELM_SCR_SYS_MEMORY);
        break;
    case ACT_ENTER_SYS_THREADS:
        helm_sys_refresh_threads(true);
        helm_push(m, HELM_SCR_SYS_THREADS);
        break;
    case ACT_ENTER_SYS_GNSS:
        helm_push(m, HELM_SCR_SYS_GNSS);
        break;
    case ACT_TOOL_COMPASS:
        s_tool_id = HELM_TOOL_COMPASS;
        helm_push(m, HELM_SCR_TOOLFACE);
        break;
    case ACT_TOOL_LEVEL:
        s_tool_id = HELM_TOOL_LEVEL;
        helm_push(m, HELM_SCR_TOOLFACE);
        break;
    case ACT_TOOL_GMETER:
        s_tool_id = HELM_TOOL_GMETER;
        helm_push(m, HELM_SCR_TOOLFACE);
        break;
    case ACT_TOOL_ALT:
        s_tool_id = HELM_TOOL_ALTIMETER;
        helm_push(m, HELM_SCR_TOOLFACE);
        break;
    case ACT_ENTER_SCAN_HR:
        s_scan_kind = 0;
        helm_push(m, HELM_SCR_SCAN);
        break;
    case ACT_ENTER_SCAN_CAD:
        s_scan_kind = 1;
        helm_push(m, HELM_SCR_SCAN);
        break;
    case ACT_ENTER_SCAN_PWR:
        s_scan_kind = 2;
        helm_push(m, HELM_SCR_SCAN);
        break;
    case ACT_NAV_COORDS:
        helm_wait_app("导航");
        break;
    case ACT_NAV_FAV:
        {
            uint8_t * sel = helm_cur_sel(m);
            uint8_t idx;
            uint8_t i;

            if (sel == NULL || *sel >= m->count) {
                break;
            }
            idx = m->items[*sel].extra;
            for (i = 0u; i < m->fav_order_n; i++) {
                if (m->fav_order[i] == idx) {
                    memmove(&m->fav_order[i], &m->fav_order[i + 1u],
                        (size_t)(m->fav_order_n - i - 1u)
                            * sizeof(m->fav_order[0]));
                    m->fav_order_n--;
                    myvendor_sound_back();
                    helm_menu_paint(m);
                    return;
                }
            }
            if (m->fav_order_n >= MYVENDOR_DEVCTL_FAVORITE_MAX) {
                myvendor_sound_warn();
                break;
            }
            m->fav_order[m->fav_order_n++] = idx;
            myvendor_sound_ok();
            helm_menu_paint(m);
        }
        break;
    case ACT_NAV_FAV_START:
        {
            myvendor_devctl_favorite_t favs[MYVENDOR_DEVCTL_FAVORITE_MAX];
            size_t fav_n = 0;
            uint8_t i;

            if (helm_fav_plan_overlay_open(m)) {
                break;
            }
            if (m->fav_order_n == 0u) {
                myvendor_sound_warn();
                helm_fav_notice(m, s_navpt_path[0] != '\0' ?
                    "请先选择途经点" : "请先选择常用点", 1500u);
                break;
            }
            {
                const char * src = (s_navpt_path[0] != '\0') ?
                    s_navpt_path : MYVENDOR_DEVCTL_FAVORITES_PATH;
                size_t pt_n = 0;

                if (myvendor_devctl_waypoints_load(src, favs,
                        MYVENDOR_DEVCTL_FAVORITE_MAX, &pt_n) != 0) {
                    myvendor_sound_warn();
                    helm_fav_notice(m, s_navpt_path[0] != '\0' ?
                        "坐标点读取失败" : "常用点读取失败", 2000u);
                    break;
                }

                fav_n = pt_n;
            }
            for (i = 0u; i < m->fav_order_n; i++) {
                if ((size_t)m->fav_order[i] >= fav_n) {
                    myvendor_sound_warn();
                    m->fav_order_n = 0u;
                    helm_menu_paint(m);
                    helm_fav_notice(m, s_navpt_path[0] != '\0' ?
                        "坐标点已变化，请重试" : "常用点已变化，请重试", 2000u);
                    return;
                }
            }
            helm_fav_plan_show(m);
        }
        break;
    case ACT_ENTER_GPX_IMPORT:
        helm_push(m, HELM_SCR_GPX_IMPORT);
        break;
    case ACT_ENTER_GPX_RECORD:
        helm_push(m, HELM_SCR_GPX_RECORD);
        break;
    case ACT_RIDE_OPEN:
        {
            uint8_t * sel = helm_cur_sel(m);
            uint8_t idx;

            if (sel == NULL || *sel >= m->count) {
                break;
            }

            idx = m->items[*sel].extra;
            if (idx >= s_gpx_n) {
                break;
            }

            if (!helm_ride_bind(s_gpx_names[idx], s_gpx_dir)) {
                break;
            }

            helm_push(m, HELM_SCR_RIDE_DETAIL);
        }
        break;
    case ACT_RIDE_DEL:
        helm_show_del(m);
        break;
    case ACT_RIDE_CONT:
        {
            char path[160];
            const bicycle_runtime_t * rt = bicycle_runtime_get();

            if (helm_shell_paused() || (rt && rt->recording) ||
                bicycle_ride_gpx_active()) {
                myvendor_sound_warn();
                lv_pm_notify_show("骑行", "请先结束当前骑行", 2000);
                break;
            }

            if (myvendor_mtp_lfs_quiesce()) {
                myvendor_sound_warn();
                lv_pm_notify_show("骑行", "USB 传输中", 2000);
                break;
            }

            if (!helm_ride_load() || s_ride_pt_n < 2u) {
                myvendor_sound_warn();
                lv_pm_notify_show("骑行", "无轨迹", 2000);
                break;
            }

            if (!helm_ride_path(path, sizeof(path))) {
                break;
            }

            if (bicycle_ride_gpx_prepare_continue(path) != 0) {
                myvendor_sound_warn();
                lv_pm_notify_show("骑行", "复制轨迹失败", 2000);
                break;
            }

            helm_shell_continue_ride((float)s_ride_km, s_ride_sec,
                                     s_ride_lon, s_ride_lat, s_ride_pt_n);
            helm_ride_close_map("骑行", "继续记录");
        }
        break;
    case ACT_RIDE_NAV:
    case ACT_RIDE_REV:
        if (helm_cur_scr(m) == HELM_SCR_NAVPT_DETAIL ||
            helm_cur_scr(m) == HELM_SCR_NAVPT_ACTIONS) {
            helm_navpt_start(m, act == ACT_RIDE_REV);
            break;
        }
        {
            char path[160];
            bool reverse = (act == ACT_RIDE_REV);

            if (!helm_ride_path(path, sizeof(path))) {
                break;
            }

            if (!map_page_nav_from_gpx(lvgl_page_map(), path, reverse)) {
                myvendor_sound_warn();
                lv_pm_notify_show(reverse ? "返航" : "导航", "打开失败", 2000);
                break;
            }

            helm_ride_close_map(reverse ? "返航" : "导航", s_ride_name);
        }
        break;
    case ACT_NAV_STOP:
#if VMAP_ROUTE_ENABLE
        {
            const bool review = map_page_review_active(lvgl_page_map());

            map_page_nav_stop(lvgl_page_map());
            myvendor_sound_back();
            lv_pm_notify_show(review ? "回放" : "导航",
                              review ? "已关闭" : "已停止", 1500);
            {
                uint8_t * nsel = helm_cur_sel(m);

                if (nsel) {
                    *nsel = 0;
                }
            }
            helm_menu_paint(m);
        }
#endif
        break;
    case ACT_NAV_SKIP:
        helm_show_skip(m);
        break;
    case ACT_NAV_NEAREST:
#if VMAP_ROUTE_ENABLE
        if (!map_page_nav_plan_nearest(lvgl_page_map())) {
            myvendor_sound_warn();
        } else {
            myvendor_sound_ok();
        }
#endif
        break;
    case ACT_TOGGLE_BT:
        (void)myvendor_devctl_radio_set(!myvendor_devctl_radio_get());
        helm_menu_paint(m);
        break;
    case ACT_TOGGLE_SENSOR:
        (void)myvendor_devctl_sensor_set(!myvendor_devctl_sensor_get());
        helm_menu_paint(m);
        break;
    case ACT_CYCLE_BL:
        (void)helm_pwr_bl_cycle();
        helm_menu_paint(m);
        break;
    case ACT_CYCLE_THEME:
        (void)lvgl_page_theme_cycle();
        helm_menu_paint(m);
        break;
    case ACT_CYCLE_UNIT:
        s_unit_imperial = !s_unit_imperial;
        helm_menu_paint(m);
        break;
    case ACT_CYCLE_TZ:
        {
            int16_t tzm = myvendor_devctl_tz_min_get();

            tzm = (int16_t)(tzm + 60);
            if (tzm > MYVENDOR_DEVCTL_TZ_MAX) {
                tzm = (int16_t)MYVENDOR_DEVCTL_TZ_MIN;
            }

            (void)myvendor_devctl_tz_min_set(tzm);
            bicycle_status_bar_refresh();
            helm_menu_paint(m);
        }
        break;
    case ACT_CYCLE_GNSS_SOLVER:
        bicycle_runtime_set_gnss_solver_rmc(!bicycle_runtime_gnss_solver_rmc());
        helm_menu_paint(m);
        break;
    case ACT_TOGGLE_AUTOPAUSE:
        (void)myvendor_devctl_autopause_set(!myvendor_devctl_autopause_get());
        helm_menu_paint(m);
        break;
    case ACT_TOGGLE_NOTIF:
        (void)myvendor_devctl_notif_set(!myvendor_devctl_notif_get());
        helm_menu_paint(m);
        break;
    case ACT_TOGGLE_CALLS:
        (void)myvendor_devctl_notif_calls_only_set(
            !myvendor_devctl_notif_calls_only_get());
        helm_menu_paint(m);
        break;
    case ACT_TOGGLE_USB:
        (void)myvendor_devctl_mtp_set(!myvendor_devctl_mtp_get());
        helm_menu_paint(m);
        break;
    case ACT_TOGGLE_SOUND:
        {
            bool on = !myvendor_devctl_sound_get();

            (void)myvendor_devctl_sound_set(on);
            if (on) {
                myvendor_sound_ok();
            }

            helm_menu_paint(m);
        }
        break;
    case ACT_TOGGLE_EPH_AUTO:
        {
            bool on = !myvendor_devctl_eph_auto_get();

            (void)myvendor_devctl_eph_auto_set(on);
            if (on) {
                myvendor_gnss_eph_reload();
            }

            helm_menu_paint(m);
        }
        break;
    case ACT_INBOX_OPEN:
        {
            myvendor_sys_notif_t box[MYVENDOR_SYS_INBOX_MAX];
            uint8_t nn = 0;
            uint8_t idx;
            uint8_t * sel = helm_cur_sel(m);

            if (sel == NULL || *sel >= m->count) {
                break;
            }

            idx = m->items[*sel].extra;
            myvendor_sys_inbox_get(box, &nn, MYVENDOR_SYS_INBOX_MAX);
            if (idx >= nn) {
                break;
            }

            lv_pm_notify_show_ex(box[idx].title[0] ? box[idx].title : "通知",
                                 box[idx].body[0] ? box[idx].body :
                                 (box[idx].title[0] ? box[idx].title : "通知"),
                                 box[idx].icon[0] ? box[idx].icon : NULL,
                                 3000);
        }
        break;
    case ACT_SCAN_CONNECT:
        {
            uint8_t * sel = helm_cur_sel(m);
            uint8_t idx;

            if (sel == NULL || *sel >= m->count) {
                break;
            }

            idx = m->items[*sel].extra;
            if (s_scan_link_wait) {
                break;
            }

            if (!myvendor_devctl_sensor_get()) {
                myvendor_sound_warn();
                lv_pm_notify_show("蓝牙设备", "外设蓝牙未开", 1500);
                break;
            }

            helm_scan_connect_idx(idx);
            helm_menu_paint(m);
        }
        break;
    case ACT_SCAN_START:
        {
            myvendor_sys_sensor_ui_t ui;

            if (!myvendor_devctl_sensor_get()) {
                myvendor_sound_warn();
                lv_pm_notify_show("蓝牙设备", "外设蓝牙未开", 1500);
                break;
            }

            myvendor_sys_sensor_ui_get(&ui);
            if (s_scan_link_wait ||
                (s_scan_kind < MYVENDOR_SYS_SENSOR_KIND_N &&
                 ui.slot[s_scan_kind].link ==
                 MYVENDOR_SYS_SENSOR_LINK_CONNECTING)) {
                lv_pm_notify_show("蓝牙设备", "正在连接", 1500);
                break;
            }

            helm_scan_begin();
            helm_push(m, HELM_SCR_SCAN_PICK);
        }
        break;
    case ACT_SCAN_AUTO:
        {
            uint8_t * sel = helm_cur_sel(m);
            myvendor_devctl_sensor_rec_t recs[MYVENDOR_DEVCTL_SENSOR_REC_MAX];
            size_t rec_n = 0;
            uint8_t idx;
            bool on;

            if (sel == NULL || *sel >= m->count) {
                break;
            }

            idx = m->items[*sel].extra;
            if (myvendor_devctl_sensor_recs_get(recs,
                    MYVENDOR_DEVCTL_SENSOR_REC_MAX, &rec_n) != 0 ||
                idx >= rec_n) {
                break;
            }

            on = !recs[idx].autorc;
            if (myvendor_devctl_sensor_auto_set(idx, on) != 0) {
                myvendor_sound_warn();
                lv_pm_notify_show("蓝牙设备", "保存失败", 1500);
                break;
            }

            myvendor_sound_ok();
            lv_pm_notify_show("蓝牙设备", on ? "开机回连" : "已取消回连", 1500);
            helm_menu_paint(m);
        }
        break;
    case ACT_POWEROFF:
        helm_pwr_exec();
        break;
    default:
        break;
    }
}

static void helm_menu_step_sel(helm_menu_t * m, int8_t dir)
{
    uint8_t * sel;
    uint8_t old;
    helm_scr_t scr;

    if (m == NULL || dir == 0) {
        return;
    }

    if (helm_fill_items(m) || m->count == 0) {
        return;
    }

    sel = helm_cur_sel(m);
    if (sel == NULL || m->count <= 1u) {
        return;
    }

    old = *sel;
    if (dir > 0) {
        *sel = (uint8_t)((*sel + 1u) % m->count);
    } else {
        *sel = (*sel == 0u) ? (uint8_t)(m->count - 1u) :
               (uint8_t)(*sel - 1u);
    }

    scr = helm_cur_scr(m);
    if (scr == HELM_SCR_RIDE_DETAIL && m->ride_keys &&
        lv_obj_get_child_count(m->ride_keys) == m->count) {
        helm_mlist_move_sel(m->ride_keys, old, *sel);
        return;
    }

    if (m->list && !lv_obj_has_flag(m->list, LV_OBJ_FLAG_HIDDEN) &&
        lv_obj_get_child_count(m->list) == m->count) {
        helm_mlist_move_sel(m->list, old, *sel);
        return;
    }

    helm_menu_paint(m);
}

static void helm_on_next_item(void * ud)
{
    helm_menu_t * m = s_menu;

    LV_UNUSED(ud);
    helm_menu_squash_abort();
    if (m == NULL) {
        return;
    }

    if (helm_cur_scr(m) == HELM_SCR_TOOLFACE) {
        if (helm_toolbox_key1()) {
            helm_mhead_set(m->title, helm_toolbox_title());
        }

        return;
    }

    if (helm_cur_scr(m) == HELM_SCR_ABOUT ||
        helm_cur_scr(m) == HELM_SCR_GRADECAL) {
        return;
    }
    if (helm_fav_plan_overlay_open(m)) {
        helm_fav_plan_on_input(m);
        return;
    }

    if (helm_cur_scr(m) == HELM_SCR_FONTLAB) {
        helm_font_lab_next();
        return;
    }

    if (m->del_dock && !lv_obj_has_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);
        s_sensor_del = false;
        myvendor_sound_back();
        return;
    }

    if (m->skip_dock && !lv_obj_has_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN);
        myvendor_sound_back();
        return;
    }

    helm_menu_step_sel(m, 1);
}

static void helm_on_confirm(void * ud)
{
    helm_menu_t * m = s_menu;
    uint8_t * sel;
    bool stays = false;

    LV_UNUSED(ud);
    if (m == NULL || helm_cur_scr(m) == HELM_SCR_ABOUT) {
        helm_menu_squash_abort();
        return;
    }

    /* **「手机蓝牙」页的确认**（KEY2 短按 = 这一步；"询问"在 helm_on_key2_long）。
     *
     * ⚠️ 必须放在**最前**：下面紧跟着的 `del_dock` 守卫会在弹窗可见时
     * `helm_ride_do_del()` / `helm_sensor_do_del()` 一嗓子就 `return` ——
     * 之前这段写在守卫后面，于是**弹窗上的 ✓ 被当成"删记录"的确认**，
     * 配对/解绑永远不生效（用户现场："长按弹窗后按 ✓ 没能启动广播"）。
     */
    if (helm_cur_scr(m) == HELM_SCR_PHONE) {
        if (s_phone_pair_arm) {
            s_phone_pair_arm = false;
            if (m->del_dock != NULL) {
                lv_obj_add_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);
            }

            printf("helm: phone pair confirmed -> open window\n");
            (void)myvendor_devctl_pair_open(30);
            myvendor_sound_ok();
            lv_pm_notify_show("手机蓝牙", "已开配对 30 秒", 1500);
            return;
        }

        if (s_phone_unbind_arm) {
            s_phone_unbind_arm = false;
            if (m->del_dock != NULL) {
                lv_obj_add_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);
            }

            printf("helm: phone unbind confirmed\n");
            (void)myvendor_devctl_pair_unbind();
            myvendor_sound_ok();
            lv_pm_notify_show("手机蓝牙", "已解除绑定", 1500);
            helm_menu_paint(m);     /* 状态变了也没关系：看门狗还会兜一次 */
            return;
        }

        /* 已配对：短按没有动作（解绑要长按）—— 但也别静默，给一句提示。 */
        if (myvendor_devctl_pair_phone(NULL, 0) == 1) {
            printf("helm: phone press while paired (long-press to unbind)\n");
            myvendor_sound_warn();
            lv_pm_notify_show("手机蓝牙", "长按右键可解绑", 1200);
            return;
        }

        /* 没配对时按确认 = 想配对：**先弹确认**，别一点就开窗口。 */
        printf("helm: phone pair ask\n");
        s_phone_pair_arm = true;
        helm_show_del(m);
        if (m->del_lab != NULL) {
            lv_label_set_text(m->del_lab, "开始配对？");
        }
        return;
    }
    if (helm_fav_plan_overlay_open(m)) {
        helm_menu_squash_abort();
        helm_fav_plan_on_input(m);
        return;
    }

    if (helm_cur_scr(m) == HELM_SCR_TOOLFACE) {
        helm_menu_squash_abort();
        if (helm_toolbox_key2()) {
            helm_mhead_set(m->title, helm_toolbox_title());
        }

        return;
    }

    if (helm_cur_scr(m) == HELM_SCR_GRADECAL) {
        helm_menu_squash_abort();
        if (bicycle_env_calibrate()) {
            helm_grade_refresh(m);
            lv_pm_notify_show("坡度", "已归零", 1500);
        } else {
            myvendor_sound_warn();
            lv_pm_notify_show("坡度", "IMU 未就绪", 2000);
        }

        return;
    }

    if (helm_cur_scr(m) == HELM_SCR_FONTLAB) {
        helm_menu_squash_abort();
        helm_font_lab_prev();
        return;
    }

    if (m->del_dock && !lv_obj_has_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN)) {
        helm_menu_squash_abort();
        if (helm_cur_scr(m) == HELM_SCR_SCAN) {
            helm_sensor_do_del(m);
        } else {
            helm_ride_do_del(m);
        }

        return;
    }

    if (m->skip_dock && !lv_obj_has_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN)) {
        helm_menu_squash_abort();
        lv_obj_add_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN);
        lv_pm_notify_show("导航", "已跳过", 1500);
        return;
    }

    /* 坐标点详情：左右键都只翻点位，长按右键才出操作。 */
    if (helm_cur_scr(m) == HELM_SCR_NAVPT_DETAIL) {
        helm_menu_squash_abort();
        helm_menu_step_sel(m, -1);
        return;
    }

    if (helm_fill_items(m)) {
        helm_menu_squash_abort();
        return;
    }

    sel = helm_cur_sel(m);
    if (sel != NULL && m->count > 0 && *sel >= m->count) {
        *sel = 0;
    }

    if (sel == NULL || *sel >= m->count) {
        helm_menu_squash_abort();
        return;
    }

    if (helm_cur_scr(m) == HELM_SCR_SCAN_PICK && s_scan_jump) {
        uint8_t i;

        for (i = 0; i < m->count; i++) {
            if (m->items[i].act == ACT_SCAN_CONNECT &&
                m->items[i].extra != 0) {
                *sel = i;
                s_scan_jump = false;
                break;
            }
        }
    }

    stays = helm_row_stays(&m->items[*sel]);
    if (stays) {
        s_squash_restore = true;
        s_squash = helm_menu_sel_row(m);
    } else {
        helm_menu_squash_drop_obj();
        helm_mlist_press_sel(helm_menu_active_list(m), *sel);
    }

    helm_do_act(m, m->items[*sel].act);
    if (s_squash_restore) {
        if (s_squash) {
            helm_obj_squash_pulse(s_squash);
        }

        s_squash_restore = false;
        s_squash = NULL;
    }
}

static void helm_on_back(void * ud)
{
    LV_UNUSED(ud);
    helm_menu_squash_abort();
    if (s_menu) {
        if (helm_fav_plan_overlay_open(s_menu)) {
            helm_fav_plan_on_input(s_menu);
            return;
        }
        myvendor_sound_back();
        helm_back(s_menu);
    }
}

static void helm_on_key2_long(void * ud)
{
    helm_menu_t * m = s_menu;
    helm_scr_t scr;
    uint8_t * sel;

    LV_UNUSED(ud);
    helm_menu_squash_abort();
    if (m == NULL) {
        return;
    }
    if (helm_fav_plan_overlay_open(m)) {
        return;
    }

    /* 菜单里 KEY2 长按不得开停 REC。记录页长按询问删除；
     * 连接页长按删除已记设备，其它行仍断开当前类型。 */
    scr = helm_cur_scr(m);

    /* 「手机蓝牙」页：长按右键 = 提示解绑（已配对时）；没配对就提示还没配对。
     * 确认那一下走点击路径（同一个确认 dock，避免和别页的删记录确认打架）。 */
    if (scr == HELM_SCR_PHONE) {
        char addr[24];

        if (s_phone_unbind_arm) {
            return;                 /* 已经在问"解除绑定？"了 */
        }
        if (myvendor_devctl_pair_phone(addr, sizeof(addr)) == 1) {
            printf("helm: phone unbind ask\n");
            s_phone_unbind_arm = true;
            helm_show_del(m);
            if (m->del_lab != NULL) {
                lv_label_set_text(m->del_lab, "解除绑定？");
            }
        } else {
            myvendor_sound_warn();
            lv_pm_notify_show("手机蓝牙", "还没配对", 1200);
        }
        return;
    }
    if (scr == HELM_SCR_RIDE_DETAIL || scr == HELM_SCR_RIDES
        || scr == HELM_SCR_GPX_RECORD || scr == HELM_SCR_GPX_IMPORT) {
        if (m->del_dock && !lv_obj_has_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN)) {
            return;
        }

        if (scr != HELM_SCR_RIDE_DETAIL && !helm_ride_sel_bind(m)) {
            return;
        }

        if (s_ride_name[0] == '\0') {
            return;
        }

        s_sensor_del = false;
        helm_show_del(m);
        return;
    }

    if (scr == HELM_SCR_SENSORS) {
        sel = helm_cur_sel(m);
        if (sel != NULL && *sel < MYVENDOR_SYS_SENSOR_KIND_N) {
            (void)myvendor_devctl_sensor_disconnect_kind(*sel);
            myvendor_sound_warn();
            lv_pm_notify_show("蓝牙设备", "已断开", 1500);
            helm_menu_paint(m);
        } else if (sel != NULL) {
            /* 非传感器行（就是「手机蓝牙」那行）走它自己的动作：进子界面。 */
            helm_do_act(m, m->items[*sel].act);
        }

        return;
    }

    if (scr == HELM_SCR_FAVS) {
        helm_do_act(m, ACT_NAV_FAV_START);
        return;
    }

    if (scr == HELM_SCR_NAVPT_DETAIL) {
        if (m->del_dock && !lv_obj_has_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN)) {
            return;
        }

        if (s_navpt_path[0] == '\0') {
            return;
        }

        myvendor_sound_ok();
        helm_push(m, HELM_SCR_NAVPT_ACTIONS);
        return;
    }

    if (scr == HELM_SCR_SCAN_PICK) {
        if (s_scan_link_wait) {
            s_scan_link_wait = false;
            (void)myvendor_devctl_sensor_disconnect_kind(s_scan_kind);
            myvendor_sound_warn();
            lv_pm_notify_show("蓝牙设备", "已取消", 1500);
            helm_menu_paint(m);
        }

        return;
    }

    if (scr == HELM_SCR_SCAN) {
        (void)helm_fill_items(m);
        sel = helm_cur_sel(m);
        if (sel != NULL && *sel < m->count &&
            m->items[*sel].act == ACT_SCAN_AUTO) {
            if (m->del_dock && !lv_obj_has_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN)) {
                return;
            }

            s_sensor_del = true;
            s_sensor_del_idx = m->items[*sel].extra;
            helm_show_del(m);
            return;
        }

        (void)myvendor_devctl_sensor_disconnect_kind(s_scan_kind);
        myvendor_sound_warn();
        lv_pm_notify_show("蓝牙设备", "已断开", 1500);
        helm_menu_paint(m);
    }
}

static void helm_bind_keys(void)
{
    lv_port_buttons_set_page_scroll_cb(helm_on_next_item, NULL);
    lv_port_buttons_set_page_confirm_cb(helm_on_confirm, NULL);
    lv_port_buttons_set_page_longpress_cb(helm_on_back, NULL);
    lv_port_buttons_set_page_longpress2_cb(helm_on_key2_long, NULL);
    lv_port_buttons_set_page_longpress2_up_cb(NULL, NULL);
    printf("helm: menu keys bound KEY1=next/back KEY2=ok\n");
}

static void helm_build_about(helm_menu_t * m)
{
    lv_obj_t * hero;
    lv_obj_t * kv;
    char line[48];
    const char * id = myvendor_identity_name();
    const char * ver = myvendor_sw_version();
    const char * hwv = myvendor_hw_version();
    const char * bld = myvendor_build_date();

    m->about = lv_obj_create(m->root);
    lv_obj_remove_style_all(m->about);
    helm_grow_y(m->about);
    lv_obj_set_flex_flow(m->about, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(m->about, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m->about, LV_OBJ_FLAG_SCROLLABLE);

    hero = helm_sheet_hero(m->about, HELM_ICO_CHIP, true);
    helm_label(hero, helm_font_val(), HELM_COLOR_INK,
               (id && id[0]) ? "Helm One" : VERSION_FIRMWARE_NAME);
    helm_label(hero, helm_font_lab(), HELM_COLOR_INK, "码表 V1");

    kv = helm_kvbox_create(m->about);
    lv_snprintf(line, sizeof(line), "%s",
                (ver && ver[0]) ? ver : VERSION_SOFTWARE);
    helm_kv_add(kv, HELM_ICO_CHIP, "固件", line);
    lv_snprintf(line, sizeof(line), "%s",
                (bld && bld[0]) ? bld : "-");
    helm_kv_add(kv, HELM_ICO_CHIP, "Build", line);
    lv_snprintf(line, sizeof(line), "%s",
                (hwv && hwv[0]) ? hwv : VERSION_HARDWARE);
    helm_kv_add(kv, HELM_ICO_CHIP, "硬件", line);
    helm_kv_add(kv, HELM_ICO_BLE, "蓝牙名",
                (id && id[0]) ? id : VERSION_FIRMWARE_NAME);
    helm_kvbox_seal(kv);
}

static void helm_kv_set_val(lv_obj_t * row, lv_obj_t ** out)
{
    uint32_t n;

    if (row == NULL || out == NULL) {
        return;
    }

    n = lv_obj_get_child_count(row);
    if (n == 0) {
        return;
    }

    *out = lv_obj_get_child(row, (int32_t)n - 1);
}

static void helm_build_gradecal(helm_menu_t * m)
{
    lv_obj_t * hero;
    lv_obj_t * kv;
    lv_obj_t * hint;

    m->gradecal = lv_obj_create(m->root);
    lv_obj_remove_style_all(m->gradecal);
    helm_grow_y(m->gradecal);
    lv_obj_set_flex_flow(m->gradecal, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(m->gradecal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m->gradecal, LV_OBJ_FLAG_SCROLLABLE);

    hero = helm_sheet_hero(m->gradecal, HELM_ICO_CLIMB, true);
    m->grade_val = helm_label(hero, helm_font_quad(), HELM_COLOR_CLIMB, "--");
    helm_label(hero, helm_font_lab(), HELM_COLOR_INK, "当前坡度");

    kv = helm_kvbox_create(m->gradecal);
    helm_kv_set_val(helm_kv_add(kv, HELM_ICO_CLIMB, "原始", "--"), &m->grade_raw);
    helm_kv_set_val(helm_kv_add(kv, HELM_ICO_GEAR, "偏置", "0.0%"), &m->grade_off);
    helm_kv_set_val(helm_kv_add(kv, HELM_ICO_CHIP, "IMU", "等待中"), &m->grade_imu);
    helm_kv_set_val(helm_kv_add(kv, HELM_ICO_ALT, "气压", "--"), &m->grade_baro);
    helm_kv_set_val(helm_kv_add(kv, HELM_ICO_ALT, "海拔", "--"), &m->grade_alt);
    helm_kvbox_seal(kv);

    hint = helm_label(m->gradecal, helm_font_lab(), HELM_COLOR_HAIR,
                      "水平放置后归零");
    lv_obj_set_style_pad_hor(hint, 12, 0);
    lv_obj_set_style_pad_top(hint, 8, 0);
}

static void helm_build_ride(helm_menu_t * m)
{
    m->ride = lv_obj_create(m->root);
    lv_obj_remove_style_all(m->ride);
    helm_grow_y(m->ride);
    lv_obj_set_flex_flow(m->ride, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(m->ride, 6, 0);
    lv_obj_set_style_pad_row(m->ride, 4, 0);
    lv_obj_add_flag(m->ride, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m->ride, LV_OBJ_FLAG_SCROLLABLE);

    m->ride_dist = helm_label(m->ride, helm_font_title(), HELM_COLOR_NAV, "--");
    lv_obj_set_width(m->ride_dist, lv_pct(100));
    lv_obj_set_style_pad_hor(m->ride_dist, 4, 0);
    lv_obj_set_style_pad_bottom(m->ride_dist, 2, 0);

    m->ride_track = lv_obj_create(m->ride);
    lv_obj_remove_style_all(m->ride_track);
    helm_grow_y(m->ride_track);
    lv_obj_set_style_bg_color(m->ride_track, helm_color(HELM_COLOR_NAV_FILL), 0);
    lv_obj_set_style_bg_opa(m->ride_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(m->ride_track, HELM_RADIUS, 0);
    lv_obj_clear_flag(m->ride_track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(m->ride_track, helm_ride_track_draw, LV_EVENT_DRAW_MAIN,
                        NULL);

    m->ride_keys = lv_obj_create(m->ride);
    lv_obj_remove_style_all(m->ride_keys);
    lv_obj_set_width(m->ride_keys, lv_pct(100));
    lv_obj_set_height(m->ride_keys, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(m->ride_keys, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(m->ride_keys, 4, 0);
    lv_obj_clear_flag(m->ride_keys, LV_OBJ_FLAG_SCROLLABLE);
    helm_mlist_sel_snap(m->ride_keys, 0);
}

static void helm_build_skip(helm_menu_t * m)
{
    lv_obj_t * head;
    lv_obj_t * ico;
    lv_obj_t * nav;

    m->skip_dock = helm_dock_create(m->root, false);
    lv_obj_add_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN);

    head = lv_obj_create(m->skip_dock);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), HELM_DOCK_HEAD_H);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, HELM_DOCK_COL_GAP, 0);
    ico = helm_icon_create(head, HELM_ICO_SKIP, HELM_DOCK_ICO_HEAD);
    helm_icon_set_color(ico, helm_color(HELM_COLOR_INK));
    helm_label(head, helm_font_title(), HELM_COLOR_INK, "跳过本点？");

    nav = lv_obj_create(m->skip_dock);
    lv_obj_remove_style_all(nav);
    lv_obj_set_width(nav, lv_pct(100));
    lv_obj_set_height(nav, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(nav, 5, 0);
    lv_obj_set_style_margin_top(nav, 6, 0);
    ico = helm_icon_create(nav, HELM_ICO_PIN, 14);
    helm_icon_set_color(ico, helm_color(HELM_COLOR_INK));
    helm_label(nav, helm_font_lab(), HELM_COLOR_INK, "下一未到：终点");
}

static void helm_build_del(helm_menu_t * m)
{
    lv_obj_t * head;
    lv_obj_t * ico;

    m->del_dock = helm_dock_create(m->root, false);
    lv_obj_add_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);

    head = lv_obj_create(m->del_dock);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), HELM_DOCK_HEAD_H);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, HELM_DOCK_COL_GAP, 0);
    ico = helm_icon_create(head, HELM_ICO_XMARK, HELM_DOCK_ICO_HEAD);
    helm_icon_set_color(ico, helm_color(HELM_COLOR_INK));
    m->del_lab = helm_label(head, helm_font_title(), HELM_COLOR_INK, "删除此记录？");
    helm_softkeys_create(m->del_dock, HELM_COLOR_INK);
}

static void helm_build_plan(helm_menu_t * m)
{
    lv_obj_t * card;
    lv_obj_t * body;

    m->plan_mask = helm_mask_create(m->root);
    lv_obj_add_flag(m->plan_mask, LV_OBJ_FLAG_IGNORE_LAYOUT);
    helm_fav_plan_place_mask(m);
    lv_obj_add_flag(m->plan_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(m->plan_mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m->plan_mask, helm_fav_plan_mask_clicked,
        LV_EVENT_CLICKED, m);

    card = helm_card_create(m->plan_mask, HELM_ICO_NAV, "正在规划", false);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, helm_fav_plan_mask_clicked, LV_EVENT_CLICKED, m);
    {
        lv_obj_t * head = lv_obj_get_child(card, 0);
        uint32_t n = head ? lv_obj_get_child_count(head) : 0u;

        m->plan_head = (n > 0u) ? lv_obj_get_child(head, n - 1u) : NULL;
    }
    body = helm_card_body(card);
    m->plan_msg = helm_label(body, helm_font_title(), HELM_COLOR_INK,
        "正在规划路线");
    lv_obj_set_width(m->plan_msg, lv_pct(100));
    lv_label_set_long_mode(m->plan_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(m->plan_msg, LV_TEXT_ALIGN_CENTER, 0);
    m->plan_hint = helm_label(body, helm_font_lab(), HELM_COLOR_HAIR,
        "单击停止");
    lv_obj_set_width(m->plan_hint, lv_pct(100));
    lv_obj_set_style_text_align(m->plan_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(m->plan_hint, 6, 0);
    helm_softkeys_create(body, HELM_COLOR_INK);
}

static helm_menu_t * helm_ui_ensure(lv_pm_page_t page)
{
    helm_menu_t * m;

    if (page == NULL || page->page == NULL) {
        return NULL;
    }

    if (page->user_data) {
        s_menu = (helm_menu_t *)page->user_data;
        return s_menu;
    }

    m = (helm_menu_t *)lv_pm_malloc(sizeof(*m));
    if (m == NULL) {
        return NULL;
    }

    memset(m, 0, sizeof(*m));

    m->root = lv_obj_create(page->page);
    lv_obj_remove_style_all(m->root);
    lv_obj_set_size(m->root, PAGE_HOR_RES, HELM_PAGE_H);
    lv_obj_align(m->root, LV_ALIGN_TOP_LEFT, 0, 0);
    helm_style_scr(m->root);
    lv_obj_set_flex_flow(m->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(m->root, LV_OBJ_FLAG_SCROLLABLE);

    m->title = helm_mhead_create(m->root, "菜单");
    m->list = helm_mlist_create(m->root);

    m->empty = lv_obj_create(m->root);
    lv_obj_remove_style_all(m->empty);
    helm_grow_y(m->empty);
    lv_obj_set_flex_flow(m->empty, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(m->empty, LV_OBJ_FLAG_HIDDEN);

    helm_build_about(m);
    helm_build_gradecal(m);
    helm_build_ride(m);
    m->toolface = helm_toolbox_build(m->root);
    m->fontlab = helm_font_lab_build(m->root);
    helm_build_skip(m);
    helm_build_del(m);
    helm_build_plan(m);

    m->poll = lv_timer_create(helm_poll_cb, HELM_POLL_MS, NULL);
    lv_timer_pause(m->poll);

    m->stack[0].scr = HELM_SCR_ROOT;
    m->stack[0].sel = 0;
    m->sp = 1;
    page->user_data = m;
    s_menu = m;
    helm_menu_paint(m);
    return m;
}

static void helm_on_load(void * pm_page)
{
    (void)helm_ui_ensure(lv_pm_get_pm_page(pm_page));
}

static void helm_will_appear(void * pm_page)
{
    helm_menu_t * m = helm_ui_ensure(lv_pm_get_pm_page(pm_page));

    if (m) {
        helm_bind_keys();
        helm_menu_paint(m);
    }
}

static void helm_will_disappear(void * pm_page)
{
    LV_UNUSED(pm_page);
    helm_scan_end();
    helm_menu_squash_abort();
    if (s_menu) {
        helm_mlist_cursor_off(s_menu->list);
        helm_mlist_cursor_off(s_menu->ride_keys);
        helm_fav_plan_hide(s_menu);
        helm_toolbox_hide();
    }
    if (s_menu && s_menu->poll) {
        lv_timer_pause(s_menu->poll);
    }
}

static void helm_did_disappear(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);

    /* Fade 结束 opa=0 但仍会因子对象 invalidate 打到 LCD。 */
    if (page && page->page) {
        lv_obj_add_flag(page->page, LV_OBJ_FLAG_HIDDEN);
    }
}

static void helm_on_unload(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);
    helm_menu_t * m;

    if (page == NULL) {
        return;
    }

    m = (helm_menu_t *)page->user_data;
    helm_scan_end();
    helm_menu_squash_abort();
    if (m) {
        helm_mlist_cursor_off(m->list);
        helm_mlist_cursor_off(m->ride_keys);
        helm_fav_plan_hide(m);
        helm_toolbox_unload();
        if (m->poll) {
            lv_timer_delete(m->poll);
            m->poll = NULL;
        }

        lv_pm_free(m);
        page->user_data = NULL;
    }

    if (s_menu == m) {
        s_menu = NULL;
    }
}

void helm_menu_page_register(void)
{
    lv_pm_page_t page = lv_pm_create_page((lv_pm_id)BICYCLE_PM_ID_MENU, "Menu");

    if (page == NULL) {
        LV_LOG_ERROR("helm_menu: register failed");
        return;
    }

    lv_pm_set_open(page, helm_on_load);
    lv_pm_set_will_appear(page, helm_will_appear);
    lv_pm_set_will_disappear(page, helm_will_disappear);
    lv_pm_set_dis_disappear(page, helm_did_disappear);
    lv_pm_set_close(page, helm_on_unload);
    bicycle_page_anima_apply(page, BICYCLE_PM_ID_MENU);
}

int helm_menu_open(void)
{
    int rc;

    if (lvgl_page_current_id() == BICYCLE_PM_ID_MENU) {
        return 0;
    }

    /* lv_pm_open_page_msg preempts an in-flight transition; do not refuse. */
    rc = lv_pm_open_page_msg((lv_pm_id)BICYCLE_PM_ID_MENU, NULL);
    printf("helm: menu open rc=%d\n", rc);
    return rc;
}

void helm_menu_refresh(void)
{
    if (s_menu != NULL) {
        helm_menu_paint(s_menu);
    }
}
