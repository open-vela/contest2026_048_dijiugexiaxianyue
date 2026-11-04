/**
 * @file bicycle_demo.c
 * @brief 自行车 UI — 演示模式。四步流程、循环模式（random/linear/minghu）、
 *        单场景与横幅见 bicycle_demo.h。
 */

#include <nuttx/config.h>

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "bicycle_demo.h"

#include "bicycle_config.h"
#include "bicycle_gpx_sim.h"
#include "bicycle_page_ids.h"
#include "bicycle_ride_gpx.h"
#include "bicycle_runtime.h"
#include "helm_anim.h"
#include "helm_font.h"
#include "helm_palette.h"
#include "helm_shell.h"
#include "helm_widget.h"
#include "lv_pm_core.h"
#include "lvgl_page.h"
#include "map_page.h"
#include "myvendor_bicycle_ctl.h"
#include "myvendor_gpx.h"

/* ============================ 四步的料 ============================ */

typedef enum {
    STEP_GPX_NAV = 0,   /**< ①模拟 GPX 导航。 */
    STEP_NAV_END,       /**< ①→② 之间：**结束导航**（清干净，别让上一个导航影响下一个）。 */
    STEP_PLAN_NAV,      /**< ②模拟坐标点导航。 */
    STEP_PAUSE,         /**< ③暂停/继续（真实中键长按动作 + 假 GPS 冻结）。 */
    STEP_REROUTE,       /**< ④城区导航偏航 → 真重规划。 */
    STEP_NATION,        /**< ⑤全国地图解析（跳点）。 */
    STEP_THEME,         /**< ⑥主题切换。 */
    STEP_COUNT
} demo_step_t;

/** @brief 每一步跑完接哪一步（最后一步回到 ① 循环）。 */
static const uint8_t k_demo_next[STEP_COUNT] = {
    STEP_NAV_END, STEP_PLAN_NAV, STEP_PAUSE, STEP_REROUTE,
    STEP_NATION, STEP_THEME, STEP_GPX_NAV,
};

/**
 * @brief 第 ③ 步跳的点。
 *
 * @note 三条口径（都在 2026-09-29 上板翻车后定的）：
 *  · **必须落在这张卡真有瓦片的格子里** —— 全国图是按 3 km 格打包的，格与格之间会有洞
 *    （实测：`(100.45,36.85)` 青海湖、`(128.06,42.02)` 长白山天池那两个格子**根本没有 vpk**，
 *    跳过去整屏白）。下面这 5 个坐标是**离线按 `mkfs/fat/map` 逐格核过**的：
 *    所在格的 **3×3 邻格全都有 vpk**（2 km 视野怎么摆都不露白边），用的是网格坐标。
 *  · **"在路上 + 有水 + 有林"是实测挑的**（工具 `zcode/analysis/demo_spot_scan.py`，
 *    直接解 VTIL 的 ROAD/WATER/WATERWAY/FOREST 四层）：最近道路 1~44 m、窗内几百条路迹、
 *    自带水面/水线/林地。**青海湖、洱海、千岛湖、长白山、泸沽湖都不合格**（湖边 1~2 km 内
 *    没有可画的路，或没有林地）—— 换点前先跑一遍那个脚本。
 *  · 顺序就是跳的顺序（引擎按表循环，见 `bicycle_gnss_hop_set_pts`）。
 */
static const bicycle_gnss_hop_pt_t k_nation_pts[] = {
    /* ⚠ 字段顺序是 **lat 在前**（与内置城市表一致）。 */
    { "太湖",       31.4900f, 120.2900f },
    { "阳朔",       24.7800f, 110.4900f },
    { "九寨沟",     33.2690f, 103.9117f },
    { "呼伦贝尔",   49.1960f, 119.7361f },
    { "三亚湾",     18.3121f, 109.4058f },
};

#define DEMO_NATION_N (sizeof(k_nation_pts) / sizeof(k_nation_pts[0]))

/**
 * @brief random 模式的**已验证安全点**（20 个）。
 *
 * @note 只用完全随机经纬度是**不行**的：全国图按 3 km 格打包、格间有洞，
 *       跳到没瓦片的格子会整屏白（见上面第 ③ 步的教训）。这里全部是城市/
 *       景点，`random` 只在它们之间乱序跳，宁可重复也不越界。
 * @note 周期与表长都是"够展会看"的量：64 次 × 8 秒 ≈ 8.5 分钟一轮。
 */
static const bicycle_gnss_hop_pt_t k_random_safe[] = {
    /* ⚠ 字段顺序是 **name, lat, lon**。 */
    { "北京",       39.9042f, 116.4074f },
    { "上海",       31.2304f, 121.4737f },
    { "广州",       23.1291f, 113.2644f },
    { "成都",       30.5728f, 104.0668f },
    { "乌鲁木齐",   43.8256f,  87.6168f },
    { "哈尔滨",     45.8038f, 126.5350f },
    { "拉萨",       29.6520f,  91.1721f },
    { "三亚",       18.2528f, 109.5119f },
    { "昆明",       25.0389f, 102.7183f },
    { "西安",       34.3416f, 108.9398f },
    { "太湖",       31.4900f, 120.2900f },
    { "阳朔",       24.7800f, 110.4900f },
    { "九寨沟",     33.2690f, 103.9117f },
    { "呼伦贝尔",   49.1960f, 119.7361f },
    { "三亚湾",     18.3121f, 109.4058f },
    { "南京",       32.0603f, 118.7969f },
    { "杭州",       30.2741f, 120.1551f },
    { "武汉",       30.5931f, 114.3054f },
    { "重庆",       29.5630f, 106.5516f },
    { "青岛",       36.0671f, 120.3826f },
};

#define DEMO_RANDOM_SAFE_N (sizeof(k_random_safe) / sizeof(k_random_safe[0]))
/** @brief random 跳点表长度（从 20 个安全点里有放回地随机挑）。 */
#define DEMO_RANDOM_HOP_N  64u

/**
 * @brief random 实际交给跳点引擎的表。
 * @note 必须**静态活到演示结束**：`bicycle_gnss_hop_set_pts()` 只存指针，不拷贝。
 *       名字都指向 `k_random_safe[]` 里的字符串字面量，同样静态有效。
 */
static bicycle_gnss_hop_pt_t s_random_hop[DEMO_RANDOM_HOP_N];

/** @brief 自己的 xorshift32 状态（不碰全局 rand/srand）。 */
static uint32_t s_random_seed;

/** @brief 每一步的横幅文案 + 跑多久（全国跳 = 每个点 10 s × 点数 + 一拍余量）。 */
static const struct {
    const char * text;
    uint32_t ms;
} k_steps[STEP_COUNT] = {
    { "模拟 GPX 导航",    BICYCLE_DEMO_NAV_MS },
    { "导航结束",          BICYCLE_DEMO_NAV_END_MS },
    { "模拟坐标点导航",    BICYCLE_DEMO_NAV_MS },
    { "暂停 / 继续",      BICYCLE_DEMO_PAUSE_MS },
    { "城区偏航重规划",    BICYCLE_DEMO_REROUTE_MS },
    { "全国地图解析",      BICYCLE_DEMO_HOP_PERIOD_MS * (uint32_t)DEMO_NATION_N + 1000u },
    { "主题切换",          BICYCLE_DEMO_THEME_MS },
};

/**
 * @brief 全量循环的状态槽标识（`bicycle_nsh demo status` 用）。
 * @note 必须与 `demo_step_t` **一一对应**（含中间的 `nav_end`）；旧表只有 4 项，
 *       会让 nav_end 之后整体错位、最后一步落到 NULL。
 */
static const char * const k_step_ids[STEP_COUNT] = {
    "gpx_nav", "nav_end", "plan_nav", "pause", "reroute", "nation", "theme",
};

/* ============================ 状态 ============================ */

static bool s_on;
static demo_step_t s_step;
/** @brief 本步起点（`lv_tick_get()`；只在 UI 线程读写）。 */
static uint32_t s_step_ms;
/** @brief 本步跑多久（毫秒）。 */
static uint32_t s_step_len_ms;
/** @brief 已经跑完几遍（日志里对进度用）。 */
static uint32_t s_cycle;
/** @brief 本次演示回放哪条轨迹；空 = 没起来。 */
static char s_gpx[192];
/** @brief 开演示之前的主题名（停止时恢复）。 */
static char s_theme0[24];
static bool s_theme_hold;

/** @brief 单场景模式：当前场景下标（-1 = 不在单场景）。 */
static int s_scene = -1;
/** @brief 单场景模式是否生效（与四步循环的 `s_on` 互斥）。 */
static bool s_scene_active;
/** @brief `alert_hr` 场景：每 250 ms 重写一次假传感器。 */
static bool s_scene_alert_hr;
/** @brief `alert_hr` 场景上次重写假传感器的时刻（`lv_tick_get()`）。 */
static uint32_t s_scene_hr_ms;

/** @brief 演示横幅（挂 `lv_layer_top()`；只在演示期间存在，每次露脸 2 秒）。 */
static lv_obj_t * s_ban;
static lv_obj_t * s_ban_lab;
static char s_ban_txt[48];
/** @brief 退场动画进行中（1 s 轮询会反复调 `demo_banner_clear()`，靠它幂等）。 */
static bool s_banner_hiding;
/** @brief 本次露脸的起点（`lv_tick_get()`）；到期由 `bicycle_demo_poll()` 收掉。 */
static uint32_t s_ban_ms;
/** @brief 本次露脸停留多久（默认 `BICYCLE_DEMO_BAN_MS`；跳点点名要挂久一点）。 */
static uint32_t s_ban_len_ms = BICYCLE_DEMO_BAN_MS;
/** @brief 跳点段上一个点的名字（换点才打一行日志）。 */
static char s_hop_last[24];
/** @brief 第 ③ 步"规划落地"那一行是否已打（每步只打一次）。 */
static bool s_plan_logged;

/* ---------------------- 暂停/继续 + 城区偏航重规划 ---------------------- */

/** @brief 演示自己开的那条骑行（停止/换步要收掉，别留一条 recording）。 */
static bool s_demo_ride;
/** @brief 暂停步阶段：0=骑行中，1=已暂停，2=已继续。 */
static uint8_t s_pause_phase;
/** @brief 暂停步本阶段起点（`lv_tick_get()`）。 */
static uint32_t s_pause_ms;
/** @brief 偏航步阶段：0=跟线骑，1=已偏航，2=重规划中，3=已落地/已收偏移。 */
static uint8_t s_reroute_phase;
/** @brief 偏航步：off_route 是否已经真的超过阈值（兜"重规划太快、没看到 planning 帧"）。 */
static bool s_reroute_off_seen;
/** @brief 偏航步进导航的时刻（`lv_tick_get()`）。 */
static uint32_t s_reroute_ms;
/** @brief 提交规划的时刻（`lv_tick_get()`；0 = 还没提交）。 */
static uint32_t s_reroute_plan_ms;
/** @brief 本次偏航步已提交过几次规划（冷机地图没落位时重试，最多 2 次）。 */
static uint8_t s_reroute_tries;
/** @brief 路线 sim 第一次真的在跑的时刻（0 = 还没跑起来）。 */
static uint32_t s_reroute_move_ms;

/* ---------------------- 页面巡览（全量演示把每个主界面都带到） ---------------------- */

/** @brief 骑行态巡览顺序：导航/骑行/仪表台/全数字（地图在最前，导航横幅先露脸）。 */
static const char * const k_tour_ride[] = { "map", "ride", "dash", "data" };
/** @brief 暂停步巡览顺序：表盘（速度最醒目）→ 仪表台 → 全数字。 */
static const char * const k_tour_pause[] = { "ride", "dash", "data" };
/** @brief 待机态巡览顺序：主页面（地图+信息卡）→ 表盘 → 数据。 */
static const char * const k_tour_idle[] = { "standby", "ride", "data" };

/** @brief 是否在巡览。 */
static bool s_tour_on;
/** @brief 当前巡览表（静态表，调用方持有）。 */
static const char * const * s_tour_list;
/** @brief 巡览页数。 */
static uint8_t s_tour_n;
/** @brief 当前页下标。 */
static uint8_t s_tour_idx;
/** @brief 上次翻页时刻。 */
static uint32_t s_tour_ms;
/** @brief 每页停留（毫秒）。 */
static uint32_t s_tour_period_ms;

/* ---------------------- 新增循环演示（random/linear/minghu） ---------------------- */

/** @brief 是否在跑新增循环模式（与 `s_on`/`s_scene_active` 互斥）。 */
static bool s_mode_active;
/** @brief 当前新增循环模式（`s_mode_active` 为真时有效）。 */
static bicycle_demo_mode_t s_mode;

/**
 * @brief random 已进入、但还没把点表交给跳点引擎（冷机 warmup 中）。
 * @note 见 #BICYCLE_DEMO_RANDOM_WARMUP_MS：先让地图页首帧画完再起跳，
 *       避免“首帧 + 第一次大跳”叠在同一个 UI 拍里触发看门狗。
 */
static bool s_mode_random_pending;
/** @brief random warmup 的起点（`lv_tick_get()`；只在 UI 线程读写）。 */
static uint32_t s_mode_random_warmup_ms;

/**
 * @brief random 命令来得太早（地图页还没建好）：先记下模式，等地图可用再进入。
 * @note 冷机 3~4 s 就能在 NSH 里敲命令，而 LiveMap 要到开机 6~8 s 才建好；
 *       旧逻辑这时直接返回 #BICYCLE_DEMO_ERR_NO_MAP，用户看到的是“命令没反应”。
 */
static bool s_mode_enter_pending;
/** @brief 等待地图页的起点（`lv_tick_get()`）。 */
static uint32_t s_mode_enter_wait_ms;
/** @brief 等地图页的上限（毫秒）；超时就放弃，免得一直显示“准备”。 */
#define DEMO_MODE_WAIT_MAP_MS 30000u

/** @brief linear：起点 + 向东 30 km 的终点（静态，route sim 只存指针）。 */
static bicycle_nav_pt_t s_linear_pts[2];
/** @brief minghu：72 点环线（静态，route sim 只存指针）。 */
#define DEMO_MINGHU_PTS 72u
static bicycle_nav_pt_t s_minghu_pts[DEMO_MINGHU_PTS];

/** @brief 当前模式是否由路线模拟驱动（linear / 生成的明湖环线；GPX 版为假）。 */
static bool s_mode_route;
/** @brief 路线已有一次跑到终点、正在等重开。 */
static bool s_mode_route_ended;
/** @brief 本轮路线起点时刻（`lv_tick_get()`）。 */
static uint32_t s_mode_route_start_ms;
/** @brief 本轮路线按里程/速度估算的时长（毫秒；到点兜底判结束）。 */
static uint32_t s_mode_route_dur_ms;
/** @brief 检测到终点、开始计停顿的时刻。 */
static uint32_t s_mode_route_end_ms;
/** @brief 上次做"到终点了吗"轮询的时刻。 */
static uint32_t s_mode_route_check_ms;
/** @brief 本轮路线终点的经纬度。 */
static double s_mode_route_end_lon;
static double s_mode_route_end_lat;

/** @brief linear/minghu 终点检测的轮询周期（毫秒）。 */
#define DEMO_ROUTE_CHECK_MS 500u
/** @brief 终点判定半径（米）：路线模拟停在最后一点，定位落进来就算到。 */
#define DEMO_ROUTE_END_M    20.0

/** @brief 横幅相对屏幕中心的下移量（px）。0 = 正中间。 */
#define DEMO_BAN_DY   0
/** @brief 横幅入场/退场时长（只动牌子自己，地图不受影响）。 */
#define DEMO_BAN_ENTER_MS 140
#define DEMO_BAN_EXIT_MS  120

/**
 * @brief 循环演示模式表（状态槽只有 16 字节，name 用 ASCII；desc 是中文文案）。
 * @note **静态只读**：NSH/菜单也会读，运行期不要改。
 */
static const struct {
    const char * name;
    const char * desc;
} k_modes[BICYCLE_DEMO_MODE_COUNT] = {
    { "full",   "全量循环（导航 / 暂停 / 偏航重规划 / 跳点 / 主题）" },
    { "random", "全国坐标随机跳转" },
    { "linear", "30km 直线巡航" },
    { "minghu", "绕明湖骑行（环线）" },
};

/* ============================ 单场景的表 ============================ */

/**
 * @brief 单场景下标（顺序即场景表顺序，next/prev 按它走）。
 * @note 表尾加新场景要放在 #DEMO_SCENE_COUNT 之前，并补 `demo_scene_enter()` 分支。
 */
typedef enum {
    DEMO_SCENE_STANDBY = 0,
    DEMO_SCENE_RIDE,
    DEMO_SCENE_DASH,
    DEMO_SCENE_DATA,
    DEMO_SCENE_MAP,
    DEMO_SCENE_CLIMB,
    DEMO_SCENE_NAV,
    DEMO_SCENE_NAV_STRAIGHT,
    DEMO_SCENE_NAV_LEFT,
    DEMO_SCENE_NAV_RIGHT,
    DEMO_SCENE_NAV_UTURN,
    DEMO_SCENE_NAV_WARN,
    DEMO_SCENE_NAV_REPLAN,
    DEMO_SCENE_NAV_ARRIVED,
    DEMO_SCENE_ALERT_SPEED,
    DEMO_SCENE_ALERT_HR,
    DEMO_SCENE_PAUSED,
    DEMO_SCENE_SAVE,
    DEMO_SCENE_NOTIFY,
    DEMO_SCENE_TOAST,
    DEMO_SCENE_THEME_CLASSIC,
    DEMO_SCENE_THEME_NIGHT,
    DEMO_SCENE_GPX,
    DEMO_SCENE_PLAN,
    DEMO_SCENE_NATION,
    DEMO_SCENE_IDLE,
    DEMO_SCENE_WAKE,
    DEMO_SCENE_PAUSE_RUN,
    DEMO_SCENE_REROUTE_RUN,
    DEMO_SCENE_COUNT
} demo_scene_t;

/**
 * @brief 单场景表：名字（终端/状态槽） + 中文说明（列表/横幅）。
 * @note **静态只读**：NSH 任务的 `demo list` 也会读这张表，运行期不要改。
 */
static const struct {
    const char * name;
    const char * desc;
} k_scenes[DEMO_SCENE_COUNT] = {
    { "standby",       "主页面（地图 + 信息卡）" },
    { "ride",          "骑行页（速度/时间/心率）" },
    { "dash",          "骑行页（老版 X1 仪表台）" },
    { "data",          "完整数据页（10 项）" },
    { "map",           "地图页" },
    { "climb",         "海拔剖面页" },
    { "nav",           "GPX 导航 + 导航横幅" },
    { "nav_straight",  "横幅：直行" },
    { "nav_left",      "横幅：左转" },
    { "nav_right",     "横幅：右转" },
    { "nav_uturn",     "横幅：掉头" },
    { "nav_warn",      "横幅：偏航" },
    { "nav_replan",    "横幅：重新规划" },
    { "nav_arrived",   "横幅：已到达" },
    { "alert_speed",   "超速告警" },
    { "alert_hr",      "心率告警" },
    { "paused",        "暂停条" },
    { "save",          "保存提示" },
    { "notify",        "顶部通知" },
    { "toast",         "底部提示" },
    { "theme_classic", "日光主题" },
    { "theme_night",   "夜间主题" },
    { "gpx",           "原步骤①：GPX 导航" },
    { "plan",          "原步骤②：坐标点导航" },
    { "nation",        "原步骤③：全国跳点" },
    { "idle",          "静止时钟" },
    { "wake",          "唤醒" },
    { "pause_run",     "暂停 / 继续（真实中键动作）" },
    { "reroute_run",   "城区偏航 → 重规划（真实）" },
};

/**
 * @brief 把当前状态写进 ctl 状态槽（`bicycle_nsh demo status` 读它）。
 * @note 单场景报场景名；新模式报模式名（random/linear/minghu）；全量四步仍报
 *       **步骤 id**（gpx_nav/plan_nav/nation/theme），保持旧 `demo status` 口径；
 *       关着报 off。`demo start full` 在 NSH 侧按"状态变化"等待，不依赖 `full` 这个值。
 */
static void demo_publish(void)
{
    const char * value;

    if (s_scene_active && s_scene >= 0 && s_scene < (int)DEMO_SCENE_COUNT) {
        value = k_scenes[s_scene].name;
    } else if (s_mode_active && (unsigned)s_mode < (unsigned)BICYCLE_DEMO_MODE_COUNT) {
        value = k_modes[s_mode].name;
    } else if (s_on) {
        value = k_step_ids[s_step];
    } else {
        value = "off";
    }

    myvendor_bicycle_ctl_state_set(MYVENDOR_BICYCLE_CTL_STATE_DEMO, value);
}

/* ============================ 横幅 ============================ */

/** @brief 横幅入场：translate_y 的 exec。 */
static void demo_banner_y_exec(void * var, int32_t v)
{
    if (helm_anim_is_suspended()) {
        return;
    }

    lv_obj_set_style_translate_y((lv_obj_t *)var, v, 0);
}

/** @brief 横幅入场/退场：opa 的 exec。 */
static void demo_banner_opa_exec(void * var, int32_t v)
{
    if (helm_anim_is_suspended()) {
        return;
    }

    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

/** @brief 入场结束：落在终态（suspend 跳过帧时也不会停在半透明/偏移上）。 */
static void demo_banner_enter_done(lv_anim_t * a)
{
    lv_obj_t * ban = (a != NULL) ? (lv_obj_t *)a->var : NULL;

    if (ban != NULL) {
        lv_obj_set_style_translate_y(ban, 0, 0);
        lv_obj_set_style_opa(ban, LV_OPA_COVER, 0);
    }
}

/** @brief 横幅入场：从下 10px、全透明处浮上来（只在新建时播一次）。 */
static void demo_banner_enter(void)
{
    if (s_ban == NULL) {
        return;
    }

    lv_obj_set_style_translate_y(s_ban, 10, 0);
    lv_obj_set_style_opa(s_ban, LV_OPA_TRANSP, 0);
    helm_anim_run(s_ban, demo_banner_y_exec, 10, 0, DEMO_BAN_ENTER_MS,
                  lv_anim_path_ease_out, demo_banner_enter_done);
    helm_anim_run(s_ban, demo_banner_opa_exec, 0, 255, DEMO_BAN_ENTER_MS,
                  lv_anim_path_ease_out, demo_banner_enter_done);
}

/** @brief 退场结束：这时候才真的删对象（期间再来 show 会就地接管，不闪）。 */
static void demo_banner_hide_done(lv_anim_t * a)
{
    lv_obj_t * ban = (a != NULL) ? (lv_obj_t *)a->var : NULL;

    if (ban != NULL && ban == s_ban) {
        s_ban = NULL;
        s_ban_lab = NULL;
        lv_obj_delete(ban);
    }

    s_ban_txt[0] = '\0';
    s_banner_hiding = false;
}

/**
 * @brief 显示/更新演示横幅（**不**重新计时；计时见 `demo_banner_show()`）。
 * @note 文案没变就不动 LVGL（第 ③ 步文案里有当前点名字，每圈都会进这里）。
 */
static void demo_banner_set(const char * text)
{
    lv_obj_t * top;

    if (text == NULL) {
        text = "";
    }

    /* 退场还没走完又要露脸：停掉退场、就地复位，别删了再建（会闪一下）。 */
    if (s_ban != NULL && s_banner_hiding) {
        helm_anim_stop(s_ban, demo_banner_y_exec);
        helm_anim_stop(s_ban, demo_banner_opa_exec);
        s_banner_hiding = false;
        lv_obj_set_style_translate_y(s_ban, 0, 0);
        lv_obj_set_style_opa(s_ban, LV_OPA_COVER, 0);
    }

    if (s_ban != NULL && strcmp(s_ban_txt, text) == 0) {
        return;
    }

    strncpy(s_ban_txt, text, sizeof(s_ban_txt) - 1u);
    s_ban_txt[sizeof(s_ban_txt) - 1u] = '\0';

    if (s_ban == NULL) {
        top = lv_layer_top();
        if (top == NULL) {
            return;
        }

        s_ban = lv_obj_create(top);
        lv_obj_remove_style_all(s_ban);
        lv_obj_set_size(s_ban, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        /* 深色实心牌 + 白字：与全局通知横幅同一套观感（半透 200，底下的地图还看得见在动）。 */
        lv_obj_set_style_bg_color(s_ban, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_ban, 200, 0);
        lv_obj_set_style_radius(s_ban, 10, 0);
        lv_obj_set_style_pad_hor(s_ban, 16, 0);
        lv_obj_set_style_pad_ver(s_ban, 9, 0);
        lv_obj_clear_flag(s_ban, LV_OBJ_FLAG_SCROLLABLE);
        /* 不吃按键/滑动：横幅只是看的，底下的菜单与地图照常收事件。 */
        lv_obj_remove_flag(s_ban, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_ban, LV_OBJ_FLAG_FLOATING);
        /* ⚠ 顺序：先建 label 让内容宽定下来，再居中。空对象先 `align` 会拿 padding
         * 宽落位，等文案撑开后就"从左边缘往右长"⇒ 横幅整体偏左（2026-11-03 现场）。 */
        s_ban_lab = helm_label(s_ban, helm_font_sys(15, helm_font_title()),
            0xFFFFFF, s_ban_txt);
        lv_obj_update_layout(s_ban);
        lv_obj_align(s_ban, LV_ALIGN_CENTER, 0, DEMO_BAN_DY);
        lv_obj_update_layout(s_ban);
        demo_banner_enter();
    } else {
        lv_label_set_text(s_ban_lab, s_ban_txt);
        lv_obj_update_layout(s_ban);
        lv_obj_align(s_ban, LV_ALIGN_CENTER, 0, DEMO_BAN_DY);
        lv_obj_update_layout(s_ban);
        lv_obj_move_foreground(s_ban);
    }

    /* 半透底色会跟着底下的地图一起重画：换个新文案后要请它重画一次。 */
    lv_obj_invalidate(s_ban);
}

static void demo_banner_clear(void)
{
    if (s_ban == NULL || s_banner_hiding) {
        return;
    }

    if (helm_anim_is_suspended()) {
        /* 屏幕不可见：不做退场，直接收掉（动画只会白跑一遍）。 */
        lv_obj_delete(s_ban);
        s_ban = NULL;
        s_ban_lab = NULL;
        s_banner_hiding = false;
        s_ban_txt[0] = '\0';
        return;
    }

    s_banner_hiding = true;
    helm_anim_run(s_ban, demo_banner_y_exec, 0, 10, DEMO_BAN_EXIT_MS,
                  lv_anim_path_ease_in, demo_banner_hide_done);
    helm_anim_run(s_ban, demo_banner_opa_exec, 255, 0, DEMO_BAN_EXIT_MS,
                  lv_anim_path_ease_in, demo_banner_hide_done);
}

/** @brief 露脸一次（写文案 + 重新计时 `BICYCLE_DEMO_BAN_MS`）。 */
static void demo_banner_show(const char * text)
{
    demo_banner_set(text);
    s_ban_ms = lv_tick_get();
    s_ban_len_ms = BICYCLE_DEMO_BAN_MS;
}

/** @brief 露脸一次，但自己指定停留时长（跳点那步要"切换前报 + 切换后还看得见"）。 */
static void demo_banner_show_for(const char * text, uint32_t ms)
{
    demo_banner_set(text);
    s_ban_ms = lv_tick_get();
    s_ban_len_ms = ms;
}

/**
 * @brief 到点就收横幅。
 */
static void demo_banner_tick(void)
{
    if (s_ban != NULL && lv_tick_elaps(s_ban_ms) >= s_ban_len_ms) {
        demo_banner_clear();
    }
}

/**
 * @brief 第 ③ 步：**切换前**就把下一个点的名字挂出来（用户要求"告诉是什么位置"）。
 *
 * @details 观众面对的是 2 km 窗口里的一块地图，不点名根本不知道跳到哪了。这里的做法是
 *          按**自己的时钟**算"现在该是第几个点、还剩多久换点"：快到切换点时（提前
 *          `BICYCLE_DEMO_ANNOUNCE_MS`）就报**下一个**点的名字，横幅一直挂到切换后
 *          `BICYCLE_DEMO_BAN_MS`。跳点引擎（`bicycle_gnss_hop_*`）按同一周期推进，
 *          两者相差不超过一次定位更新的间隔（~200 ms），提前量足够盖住。
 */
static void demo_nation_announce(void)
{
    uint32_t el = lv_tick_elaps(s_step_ms);
    uint32_t period = BICYCLE_DEMO_HOP_PERIOD_MS;
    uint32_t idx = el / period;
    const char * name;
    char buf[48];

    if (idx >= (uint32_t)DEMO_NATION_N) {
        idx = (uint32_t)DEMO_NATION_N - 1u;
    } else if (((el % period) + BICYCLE_DEMO_ANNOUNCE_MS) >= period
               && idx + 1u < (uint32_t)DEMO_NATION_N) {
        idx += 1u;                       /* 快到换点了：先报下一个 */
    }

    name = k_nation_pts[idx].name;

    /* 换一个点打一行日志：这是"跳到哪了"唯一的复查依据。 */
    if (strcmp(name, s_hop_last) != 0) {
        strncpy(s_hop_last, name, sizeof(s_hop_last) - 1u);
        s_hop_last[sizeof(s_hop_last) - 1u] = '\0';
        syslog(LOG_INFO, "demo: hop -> %s\n", name);

        lv_snprintf(buf, sizeof(buf), "%s · %s", k_steps[STEP_NATION].text, name);
        demo_banner_show_for(buf, BICYCLE_DEMO_ANNOUNCE_MS + BICYCLE_DEMO_BAN_MS);
    }
}

/* ============================ 主题 / 缩放 ============================ */

static void demo_theme_save(void)
{
    const char * n = lvgl_page_theme_name();

    s_theme0[0] = '\0';
    if (n != NULL) {
        strncpy(s_theme0, n, sizeof(s_theme0) - 1u);
        s_theme0[sizeof(s_theme0) - 1u] = '\0';
    }

    s_theme_hold = true;
}

/** @brief 恢复进演示之前那个主题（翻转过才动）。 */
static void demo_theme_restore(void)
{
    const char * n;

    if (!s_theme_hold) {
        return;
    }

    s_theme_hold = false;
    n = lvgl_page_theme_name();
    if (s_theme0[0] == '\0' || (n != NULL && strcmp(n, s_theme0) == 0)) {
        return;
    }

    /* 演示翻出来的主题会被写进 /mnt/kv/ui_theme ⇒ 停止时还原。 */
    (void)lvgl_page_theme_set_by_name(s_theme0);
}

/** @brief 翻主题：判据与动作都跟设置「显示」那一行同源，别自己造第三套。 */
static void demo_theme_flip(void)
{
    (void)lvgl_page_theme_set_by_name(helm_pal_night() ? "classic" : "night");
}

/* ============================ 回放起手 ============================ */

/**
 * @brief 挑一条用来回放的轨迹：用户指定的那条 → 记录/导入目录里最新的那条。
 * @param[out] out 命中的完整路径（失败时空串）。
 * @return 找到为 true。
 * @note 写死的路径**不能是唯一来源**：App 重导一次、换张卡就没了，
 *       演示不该因为一个文件名整个起不来。
 */
static bool demo_pick_gpx(char * out, size_t n)
{
    static const char * const dirs[] = {
        MYVENDOR_GPX_RECORD_DIR,
        MYVENDOR_GPX_IMPORT_DIR,
    };
    char best[192];
    unsigned d;

    best[0] = '\0';

    if (access(BICYCLE_DEMO_GPX, R_OK) == 0) {
        strncpy(best, BICYCLE_DEMO_GPX, sizeof(best) - 1u);
    }

    for (d = 0; d < sizeof(dirs) / sizeof(dirs[0]) && best[0] == '\0'; d++) {
        DIR * dir = opendir(dirs[d]);
        struct dirent * e;
        time_t best_t = 0;

        if (dir == NULL) {
            continue;
        }

        while ((e = readdir(dir)) != NULL) {
            struct stat st;
            char path[192];
            size_t len = strlen(e->d_name);
            int m;

            if (len < 5u || strcasecmp(e->d_name + len - 4u, ".gpx") != 0) {
                continue;
            }

            m = snprintf(path, sizeof(path), "%s/%s", dirs[d], e->d_name);
            if (m < 0 || (size_t)m >= sizeof(path)) {
                continue;
            }

            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }

            if (best[0] == '\0' || st.st_mtime > best_t) {
                best_t = st.st_mtime;
                strncpy(best, path, sizeof(best) - 1u);
                best[sizeof(best) - 1u] = '\0';
            }
        }

        closedir(dir);
    }

    if (best[0] == '\0') {
        out[0] = '\0';
        return false;
    }

    strncpy(out, best, n - 1u);
    out[n - 1u] = '\0';
    return true;
}

/**
 * @brief 起回放（当作模拟 GNSS）。
 * @param[out] path 实际回放的那条（失败时空串）。
 * @return 0 成功。
 */
static int demo_replay_start(map_page_t * page, char * path, size_t n)
{
    float kph = bicycle_config_nav_sim_speed_kph();

    path[0] = '\0';

    if (demo_pick_gpx(path, n)
        && map_page_gnss_sim(page, path, false, kph, 0.0f) == 0) {
        return 0;
    }

    path[0] = '\0';
    return -1;
}

/* ============================ 四步 ============================ */

/**
 * @brief ①：开指定 GPX 的跟线导航 + 回放当模拟定位；path 为空则自动挑一条。
 * @note 明湖模式要**点名**回放含"明湖"的 GPX，所以把"挑文件"与"起导航"拆开；
 *       传 NULL 的路径与旧 `demo_enter_gpx_nav()` 完全一致。
 */
static bool demo_enter_gpx_nav_file(const char * path)
{
    map_page_t * page = lvgl_page_map();
    char picked[192];

    if (page == NULL) {
        syslog(LOG_WARNING, "demo: no map page\n");
        return false;
    }

    /* 每一步都从干净状态起：清掉上一轮/用户留下的导航（含到达锁存）。 */
    map_page_nav_stop(page);

    if (path == NULL || path[0] == '\0') {
        if (!demo_pick_gpx(picked, sizeof(picked))) {
            syslog(LOG_WARNING, "demo: no gpx to play\n");
            return false;
        }
        path = picked;
    }

    /* ⚠ 顺序不能反：`map_page_nav_from_gpx()` 开头会 `bicycle_gpx_sim_stop()`
     * （它默认"跟线用真实定位"）⇒ 先起回放的话会被它一脚踢掉。反过来先开导航、
     * 再起回放，两者才同时在线。 */
    if (!map_page_nav_from_gpx(page, path, false)) {
        syslog(LOG_WARNING, "demo: gpx nav failed (%s)\n", path);
        return false;
    }

    if (map_page_gnss_sim(page, path, false,
            bicycle_config_nav_sim_speed_kph(), 0.0f) != 0) {
        syslog(LOG_WARNING, "demo: replay failed (%s)\n", path);
        map_page_nav_stop(page);
        return false;
    }

    strncpy(s_gpx, path, sizeof(s_gpx) - 1u);
    s_gpx[sizeof(s_gpx) - 1u] = '\0';
    syslog(LOG_INFO, "demo: step1 gpx nav %s @%.0f km/h\n", path,
        (double)bicycle_config_nav_sim_speed_kph());
    return true;
}

/** @brief ①：自动挑一条轨迹（四步/单场景的旧入口）。 */
static bool demo_enter_gpx_nav(void)
{
    return demo_enter_gpx_nav_file(NULL);
}

/**
 * @brief 读 GPX 里**第一个轨迹点**（演示规划要一个确定的"从哪儿出发"）。
 * @return 取到为 true。
 *
 * @note 为什么不问 `bicycle_runtime_poll_fix()`（2026-09-29 上板翻车的原因）：
 *       回放引擎是"全国跳 → 路线模拟 → GPX 回放"三级优先，**回放不是永远说了算**。
 *       实测日志里那一次，规划起点取到的是**上一条路线的终点**（= 上一次规划的目的地），
 *       于是"从目的地规划到目的地"→ 21 m 路线 → 一开就报到达。
 *       轨迹起点是这条回放文件本身的事实，直接读文件最稳；读不到才退回"当前定位"。
 * @note 文件是**一整行**的超长 XML（Fly/小米导出都这样），所以按块读、在块里找首点。
 */
static bool demo_gpx_first_pt(const char * path, double * lon, double * lat)
{
    char buf[4096];
    FILE * f = fopen(path, "r");
    size_t n;
    const char * p;
    bool ok = false;

    if (f == NULL) {
        return false;
    }

    n = fread(buf, 1u, sizeof(buf) - 1u, f);
    buf[n] = '\0';
    (void)fclose(f);

    p = strstr(buf, "<trkpt");
    if (p != NULL) {
        const char * la = strstr(p, "lat=\"");
        const char * lo = strstr(p, "lon=\"");

        if (la != NULL && lo != NULL && la < p + 256 && lo < p + 256) {
            double vlat = strtod(la + 5, NULL);
            double vlon = strtod(lo + 5, NULL);

            if (vlat >= -90.0 && vlat <= 90.0 && vlon >= -180.0 && vlon <= 180.0) {
                *lat = vlat;
                *lon = vlon;
                ok = true;
            }
        }
    }

    return ok;
}

/**
 * @brief ③：**假 GPS 按 GPX 从头骑** + 规划到固定坐标点（导航叠在上面）。
 *
 * @details 用户 2026-09-29：**"坐标点导航的轨迹是错的，不应该重头开始那条 GPX 吗"**。
 *          原因是我先前用了 `map_page_nav_plan_sim()` —— sim 版会
 *          `map_page_nav_gnss_for_sim()`：**停掉 GPX 回放、把定位源切成路线模拟**，
 *          于是屏上记下来的"骑行轨迹"是那条规划路线，而不是回放的 GPX。
 *          现在改用 `map_page_nav_plan()`（非 sim 版）：它只停**路线模拟**
 *          （`map_page_nav_gnss_for_gps()`），GPX 回放照跑 ⇒ 假 GPS 从轨迹起点一路骑，
 *          导航线/转向/里程是叠在上面的一层。回放先起，规划起点用文件里第一个
 *          `<trkpt>`，两者落在同一个地方。
 */
static bool demo_enter_plan_nav(void)
{
    map_page_t * page = lvgl_page_map();
    char path[192];
    double from_lon = NAN;
    double from_lat = NAN;

    if (page == NULL) {
        return false;
    }

    if (demo_replay_start(page, path, sizeof(path)) != 0) {
        syslog(LOG_WARNING, "demo: replay for plan failed\n");
        return false;
    }

    strncpy(s_gpx, path, sizeof(s_gpx) - 1u);
    s_gpx[sizeof(s_gpx) - 1u] = '\0';

    /* 起点显式用**轨迹起点**（= 这段假 GPS 的出发点）；读不到才交给"当前定位"。 */
    if (!demo_gpx_first_pt(path, &from_lon, &from_lat)) {
        from_lon = NAN;
        from_lat = NAN;
        syslog(LOG_WARNING, "demo: no trkpt in %s, plan from current fix\n", path);
    }

    /* 方向箭头兜底：以前规划导航不会自己把箭头打开，演示这里手动补一刀；
     * 现在 `map_page_nav_plan_begin()` 已显式恢复箭头，这里保留无副作用。 */
    map_page_set_arrow_visible(page, true);

    if (!map_page_nav_plan(page, from_lon, from_lat,
            BICYCLE_DEMO_PLAN_LON, BICYCLE_DEMO_PLAN_LAT)) {
        syslog(LOG_WARNING, "demo: plan failed\n");
        return false;
    }

    s_plan_logged = false;          /* 等规划落地后再打"路线就绪"那一行 */
    syslog(LOG_INFO, "demo: step2 plan %.7f,%.7f -> %.7f,%.7f (replay keeps driving)\n",
        from_lon, from_lat, BICYCLE_DEMO_PLAN_LON, BICYCLE_DEMO_PLAN_LAT);
    return true;
}

/* ====================== ③ 暂停/继续 + ④ 城区偏航重规划 ====================== */

/** @brief `demo_scene_clear_ride()` 定义在单场景段（文件后半），这里先声明。 */
static void demo_scene_clear_ride(void);

/**
 * @brief 收掉合成定位上的演示修饰（回放冻结）。
 * @note 所有"清干净"的路径都要调：漏掉一处，下一段演示就会带着上一次的冻结。
 */
static void demo_sim_reset(void)
{
    bicycle_gpx_sim_set_hold(false);
}

/**
 * @brief 停掉页面巡览（换步/停演示时必须调，否则下一步会被旧表继续翻页）。
 */
static void demo_tour_stop(void)
{
    s_tour_on = false;
}

/**
 * @brief 开始页面巡览：按表轮着锁页面。
 *
 * @details 用户 2026-11-03："展示的还不够全，没有自动在每个页面跳转…最起码在主界面每个页面
 *          展示一下，最好带着页面一起触发。" 所以全量循环的导航/骑行/暂停/主题几步都要把
 *          主界面轮一遍，而不是一直停在地图页或表盘页。
 *          · 骑行态表 `k_tour_ride`：地图 → 表盘 → 仪表台 → 全数字；
 *          · 暂停步 `k_tour_pause`：表盘 → 仪表台 → 全数字（暂停条是全局 chrome，每页都看得到）；
 *          · 主题步（已收骑行，回到待机态）`k_tour_idle`：主页面 → 表盘 → 数据。
 * @param list      页面名表（`helm_shell_demo_page()` 认的名字）。
 * @param n         页数。
 * @param period_ms 每页停留（毫秒）。
 */
static void demo_tour_start(const char * const * list, uint8_t n, uint32_t period_ms)
{
    if (list == NULL || n == 0u) {
        demo_tour_stop();
        return;
    }

    s_tour_on = true;
    s_tour_list = list;
    s_tour_n = n;
    s_tour_idx = 0;
    s_tour_ms = lv_tick_get();
    s_tour_period_ms = period_ms > 0u ? period_ms : 6000u;

    if (!helm_shell_demo_page(list[0])) {
        syslog(LOG_WARNING, "demo: tour page %s failed\n", list[0]);
    }
}

/** @brief 巡览节拍：到点翻下一页（循环）。 */
static void demo_tour_tick(void)
{
    if (!s_tour_on || s_tour_list == NULL || s_tour_n == 0u) {
        return;
    }

    if (lv_tick_elaps(s_tour_ms) < s_tour_period_ms) {
        return;
    }

    s_tour_ms = lv_tick_get();
    s_tour_idx = (uint8_t)((s_tour_idx + 1u) % s_tour_n);
    if (!helm_shell_demo_page(s_tour_list[s_tour_idx])) {
        syslog(LOG_WARNING, "demo: tour page %s failed\n", s_tour_list[s_tour_idx]);
    } else {
        syslog(LOG_INFO, "demo: tour page %s\n", s_tour_list[s_tour_idx]);
    }
}

/**
 * @brief ③暂停/继续：走真实按键路径，不是只把暂停条画出来。
 *
 * @details 用户 2026-11-03："没有把骑行暂停，继续的状态演示出来（主要是 gps 一直在跑，
 *          中键不会停下来一会）"。所以：
 *          · 暂停调 `helm_shell_toggle_ride()` —— 与 **KEY2 长按同一条函数**
 *            （暂停录制 + 弹暂停条）；
 *          · 同时冻结假 GPS（`bicycle_gpx_sim_set_hold(true)`）⇒ 速度归 0、位置不动；
 *          · 继续调 `helm_shell_ensure_ride()` —— 与暂停态下 **KEY1 短按同一条恢复路径**；
 *          · 再放开假 GPS，从原位置接着骑。
 */
static bool demo_enter_pause_demo(void)
{
    map_page_t * page = lvgl_page_map();
    char path[192];

    if (page == NULL) {
        syslog(LOG_WARNING, "demo: no map page for pause\n");
        return false;
    }

    demo_scene_clear_ride();
    demo_sim_reset();

    if (demo_replay_start(page, path, sizeof(path)) != 0) {
        syslog(LOG_WARNING, "demo: replay for pause failed\n");
        return false;
    }

    strncpy(s_gpx, path, sizeof(s_gpx) - 1u);
    s_gpx[sizeof(s_gpx) - 1u] = '\0';

    /* 真起一条演示骑行：`toggle_ride()` 只在 recording 时才走暂停分支。 */
    helm_shell_ensure_ride();
    if (!helm_shell_demo_page("ride")) {
        syslog(LOG_WARNING, "demo: pause page lock failed\n");
    }

    s_demo_ride = true;
    s_pause_phase = 0;
    s_pause_ms = lv_tick_get();
    syslog(LOG_INFO, "demo: pause demo start (%s)\n", path);
    return true;
}

/** @brief ③的节拍：骑 → 暂停（+冻结）→ 继续（+放开）。 */
static void demo_pause_tick(void)
{
    uint32_t el = lv_tick_elaps(s_pause_ms);

    if (s_pause_phase == 0 && el >= BICYCLE_DEMO_PAUSE_AT_MS) {
        /* 用户抢先按了暂停就不要再来一下 —— 暂停态下 `toggle_ride` 会弹保存框。 */
        if (!helm_shell_paused()) {
            helm_shell_toggle_ride();      /* == KEY2 长按 */
        }
        bicycle_gpx_sim_set_hold(true);    /* 假 GPS 停住：速度 0 */
        s_pause_phase = 1;
        syslog(helm_shell_paused() ? LOG_INFO : LOG_WARNING,
            "demo: pause (KEY2-long path) paused=%d hold=%d\n",
            helm_shell_paused() ? 1 : 0, bicycle_gpx_sim_hold() ? 1 : 0);
    } else if (s_pause_phase == 1
        && el >= BICYCLE_DEMO_PAUSE_AT_MS + BICYCLE_DEMO_PAUSE_HOLD_MS) {
        bicycle_gpx_sim_set_hold(false);   /* 先放开定位再继续 */
        helm_shell_ensure_ride();          /* == 暂停态下 KEY1 短按 */
        s_pause_phase = 2;
        syslog(LOG_INFO, "demo: resume paused=%d hold=%d\n",
            helm_shell_paused() ? 1 : 0, bicycle_gpx_sim_hold() ? 1 : 0);
    }
}

/**
 * @brief ④城区偏航重规划：坐标点导航 + 城区 GPX 假 GPS，真实偏航 → 真实 reroute。
 *
 * @details 用户 2026-11-03 定稿：
 *          · 导航 = 坐标点导航 `39.919940,116.350961 → 39.916015,116.358281`（北京城区）；
 *          · 假 GPS = 回放 `/mnt/lfs/mtp/import/北京.gpx`（gpx.studio 生成，点稀疏、
 *            **故意绕路**）⇒ 骑手自己偏离规划路线，不再人为加坐标偏移；
 *          · map_page 的偏航判定（阈值 50 m、持续 3 s）触发 reroute，横幅进"重新规划"、
 *            新线从当前位置重画。
 *
 * @note 顺序必须是「先起回放、再规划」：`map_page_nav_plan()` 会
 *       `map_page_nav_gnss_for_gps()`（只停路线模拟，不动 GPX 回放）；与第 2/3 步同序，
 *       规划起点与回放起点对齐。
 */
static bool demo_enter_reroute_demo(void)
{
    map_page_t * page = lvgl_page_map();

    if (page == NULL) {
        syslog(LOG_WARNING, "demo: no map page for reroute\n");
        return false;
    }

    demo_scene_clear_ride();               /* 停上一段导航/回放/骑行 */
    demo_sim_reset();

    /* **先把页面切到地图，再起回放**（用户 2026-11-04 反馈"跳北京没能打开地图"）：
     * 原来顺序反了，回放先跑、页面后切，切页那一下的 cover/resume 会把前几拍的
     * 跟车丢掉，等 2.5 s 去规划时地图还停在上一站。 */
    if (!helm_shell_demo_page("map")) {
        syslog(LOG_WARNING, "demo: reroute page lock failed\n");
    }

    /* 假 GPS：北京城区 GPX 回放（点稀疏 + 故意绕路 ⇒ 自然偏航）。 */
    if (map_page_gnss_sim(page, BICYCLE_DEMO_REROUTE_GPX, false,
            bicycle_config_nav_sim_speed_kph(), 0.0f) != 0) {
        syslog(LOG_WARNING, "demo: reroute gpx replay failed (%s)\n",
            BICYCLE_DEMO_REROUTE_GPX);
        return false;
    }
    strncpy(s_gpx, BICYCLE_DEMO_REROUTE_GPX, sizeof(s_gpx) - 1u);
    s_gpx[sizeof(s_gpx) - 1u] = '\0';

    /* **直接把地图搬过去**，别指望回放跟车：实测从上一站（明湖）到北京，
     * 跟车用了 33 s 才把视图挪过去，而规划在 6 s 就提交了 —— 规划拿的是旧
     * 区域的路由图，连试 3 次都是 `no route`（2026-11-04 用户复现）。 */
    map_page_center_on(page, BICYCLE_DEMO_REROUTE_FROM_LON,
        BICYCLE_DEMO_REROUTE_FROM_LAT);

    s_reroute_phase = 0;                   /* 先等地图跟到目标城区 */
    s_reroute_off_seen = false;
    s_reroute_tries = 0;
    s_reroute_ms = lv_tick_get();
    s_reroute_plan_ms = 0;
    s_reroute_move_ms = 0;
    syslog(LOG_INFO, "demo: reroute gpx replay %s; plan after map settle\n",
        BICYCLE_DEMO_REROUTE_GPX);
    return true;
}

/** @brief ④的节拍：等地图落位 → 提交规划 → 等规划落地 → 等 GPX 绕路判出偏航。 */
static void demo_reroute_tick(void)
{
    map_page_t * page = lvgl_page_map();
    const bicycle_runtime_t * rt = bicycle_runtime_get();
    const double off_m = rt ? rt->nav_off_route_m : 0.0;
    const double off_lim = bicycle_config_nav_off_route_m() > 0.0
        ? bicycle_config_nav_off_route_m() : 50.0;
    bool replanning;
    uint32_t el;

    if (page == NULL) {
        return;
    }

    el = lv_tick_elaps(s_reroute_ms);
    replanning = map_page_nav_replanning(page);

    if (s_reroute_phase == 0) {
        /* ⚠ 必须等地图跟到目标城区再规划（2026-11-03 上板实测）：
         * `map_page_nav_plan()` 用的是**当前显示区域**的路由图；地图还在上一站时
         * 会拿错图 —— 北京的点被吸到 869 km 外，日志 `both ends off-road`。
         * 假 GPS 回放先把地图带过去，停 SETTLE_MS 再提交规划。 */
        /* 等**地图真的画出来**再规划，而不是死等一个固定时长：
         * 判据 = 至少出过一帧且渲染泵空闲（`map_page_render_ready`）。
         * 15 s 硬兜底，避免卡着不动。 */
        if ((el >= BICYCLE_DEMO_REROUTE_SETTLE_MS
                && map_page_render_ready(page))
            || el >= 15000u) {
            if (!map_page_render_ready(page)) {
                syslog(LOG_WARNING,
                    "demo: reroute plan without settled map (%ums)\n",
                    (unsigned)el);
            }
            map_page_set_arrow_visible(page, true);
            if (!map_page_nav_plan(page,
                    BICYCLE_DEMO_REROUTE_FROM_LON, BICYCLE_DEMO_REROUTE_FROM_LAT,
                    BICYCLE_DEMO_REROUTE_TO_LON, BICYCLE_DEMO_REROUTE_TO_LAT)) {
                syslog(LOG_WARNING, "demo: reroute plan submit failed\n");
                s_reroute_phase = 4;
                demo_tour_start(k_tour_ride, 4u, 4000u);
                return;
            }

            s_reroute_plan_ms = lv_tick_get();
            if (s_reroute_plan_ms == 0u) {
                s_reroute_plan_ms = 1u;
            }
            s_reroute_phase = 1;
            syslog(LOG_INFO, "demo: reroute plan %.6f,%.6f -> %.6f,%.6f at %ums\n",
                BICYCLE_DEMO_REROUTE_FROM_LON, BICYCLE_DEMO_REROUTE_FROM_LAT,
                BICYCLE_DEMO_REROUTE_TO_LON, BICYCLE_DEMO_REROUTE_TO_LAT,
                (unsigned)el);
        }
    } else if (s_reroute_phase == 1) {
        /* 等坐标点导航落地（假 GPS 回放已经在跑）。 */
        if (map_page_nav_active(page)) {
            s_reroute_move_ms = lv_tick_get();
            if (s_reroute_move_ms == 0u) {
                s_reroute_move_ms = 1u;
            }
            s_reroute_phase = 2;
            syslog(LOG_INFO, "demo: reroute riding at %ums\n", (unsigned)el);
        } else if (lv_tick_elaps(s_reroute_plan_ms) >= 8000u
            && !map_page_nav_planning(page)) {
            /* 冷机第一次规划可能还拿着旧区域图（地图没落位）：重试一次，
             * 这时地图肯定已经跟到目标城区。 */
            if (s_reroute_tries < 2u) {
                s_reroute_tries++;
                syslog(LOG_WARNING,
                    "demo: reroute plan retry %u (map may have just settled)\n",
                    (unsigned)s_reroute_tries);
                if (map_page_nav_plan(page,
                        BICYCLE_DEMO_REROUTE_FROM_LON, BICYCLE_DEMO_REROUTE_FROM_LAT,
                        BICYCLE_DEMO_REROUTE_TO_LON, BICYCLE_DEMO_REROUTE_TO_LAT)) {
                    s_reroute_plan_ms = lv_tick_get();
                    if (s_reroute_plan_ms == 0u) {
                        s_reroute_plan_ms = 1u;
                    }
                    return;
                }
            }

            syslog(LOG_WARNING, "demo: reroute plan failed (no route)\n");
            s_reroute_phase = 4;
            demo_tour_start(k_tour_ride, 4u, 4000u);
        }
    } else if (s_reroute_phase == 2) {
        /* 发生偏航时留一行判据（GPX 故意绕路，正常应在几秒内出现）。 */
        if (!s_reroute_off_seen && off_m >= off_lim * 1.2) {
            s_reroute_off_seen = true;
            syslog(LOG_INFO, "demo: reroute off_route=%.0fm (gpx detour)\n", off_m);
        }

        if (replanning) {
            s_reroute_phase = 3;
            syslog(LOG_INFO, "demo: reroute replanning started\n");
        } else if (s_reroute_move_ms != 0u
            && lv_tick_elaps(s_reroute_move_ms) >= BICYCLE_DEMO_REROUTE_WAIT_MS) {
            syslog(LOG_WARNING, "demo: reroute not triggered in %ums (off=%.0fm)\n",
                (unsigned)lv_tick_elaps(s_reroute_move_ms), off_m);
            s_reroute_phase = 4;
            demo_tour_start(k_tour_ride, 4u, 4000u);
        }
    } else if (s_reroute_phase == 3) {
        if (!replanning) {
            s_reroute_phase = 4;
            /* 偏航/重规划已经演完，剩下的时间把主界面其余页轮一遍。 */
            demo_tour_start(k_tour_ride, 4u, 4000u);
            syslog(LOG_INFO, "demo: reroute done at %ums\n", (unsigned)el);
        }
    }
}

/**
 * @brief ①→② 之间：**结束导航**。
 *
 * @details 用户现场（2026-09-29）："我建议中间增加上导航结束吧，不然上一个导航可能对
 *          下一个有影响"。GPX 跟线导航留下的不只是叠层 —— 还有 `nav_gpx_course`、
 *          GPX 窗口、"到达锁存"、以及**仍然在跑的回放/路线模拟**；直接在上面叠一次
 *          `nav_plan_sim`，规划起点和到达判定都可能被上一条路线带跑。
 *          所以单独留一步，把导航/回放/演示骑行**全部收干净**再进下一步。
 */
static void demo_enter_nav_end(void)
{
    map_page_t * page = lvgl_page_map();

    demo_sim_reset();

    if (page != NULL) {
        map_page_nav_stop(page);
        (void)map_page_gnss_sim(page, NULL, false, 0.0f, 0.0f);
        map_page_end_ride(page);       /* 演示骑行不落盘 */
    } else {
        bicycle_gpx_sim_stop();
        bicycle_route_sim_stop();
    }
    s_demo_ride = false;

    syslog(LOG_INFO, "demo: nav end (clean slate for step2)\n");
}

/** @brief ⑤：结束导航与回放，收掉演示骑行，开始跳点。 */
static void demo_enter_nation(void)
{
    map_page_t * page = lvgl_page_map();

    demo_sim_reset();

    if (page != NULL) {
        map_page_nav_stop(page);                                /* 结束导航 */
        (void)map_page_gnss_sim(page, NULL, false, 0.0f, 0.0f);  /* 停回放/路线模拟 */
        /* 收掉这次演示骑行：未保存的 GPX 丢弃、轨迹线清掉。跳点动辄上千公里，
         * 不收就会往轨迹里灌一条横跨全国的长线、里程也涨到几千公里。 */
        map_page_end_ride(page);
        /* ⚠ **不要动瓦片缩放**（2026-09-29 上板翻车）：全国图是**只按 z14 打包**的
         * （逐个 vpk 核过，里面全是 z14 瓦片），把视图拉到 z11 就是请求一批不存在的
         * 瓦片 ⇒ 整段跳点全白屏。要"看宽一点"只用 `s,F` 缩放，别改 z。 */
    } else {
        bicycle_gpx_sim_stop();
        bicycle_route_sim_stop();
    }
    s_demo_ride = false;

    bicycle_gnss_hop_set_pts(k_nation_pts, (uint32_t)DEMO_NATION_N);
    bicycle_gnss_hop_start(BICYCLE_DEMO_HOP_PERIOD_MS, (unsigned)DEMO_NATION_N);
    s_hop_last[0] = '\0';

    syslog(LOG_INFO, "demo: nation %u pts x %u ms\n",
        (unsigned)DEMO_NATION_N, (unsigned)BICYCLE_DEMO_HOP_PERIOD_MS);
}

/** @brief ⑥：停跳点（跳的是合成定位），换另一套主题。 */
static void demo_enter_theme(void)
{
    bicycle_gnss_hop_stop();
    demo_theme_flip();
    syslog(LOG_INFO, "demo: theme -> %s\n", helm_pal_night() ? "night" : "classic");
}

/** @brief 进某一步：设时长 + 干这一步的活 + 换横幅。 */
static void demo_goto(demo_step_t step)
{
    bool ok = true;

    /* 上一步的页面锁/巡览都收掉，再按这一步重新安排。 */
    demo_tour_stop();
    helm_shell_demo_page_clear();

    s_step = step;
    s_step_ms = lv_tick_get();
    s_step_len_ms = k_steps[step].ms;

    switch (step) {
    case STEP_GPX_NAV:
        ok = demo_enter_gpx_nav();
        if (ok) {
            demo_tour_start(k_tour_ride, 4u, 7000u);
        }
        break;

    case STEP_NAV_END:
        demo_enter_nav_end();
        break;

    case STEP_PLAN_NAV:
        ok = demo_enter_plan_nav();
        if (ok) {
            demo_tour_start(k_tour_ride, 4u, 7000u);
        }
        break;

    case STEP_PAUSE:
        ok = demo_enter_pause_demo();
        if (ok) {
            /* 20 s / 3 页：表盘 → 仪表台 → 全数字（暂停条与 0 km/h 每页都能看到）。 */
            demo_tour_start(k_tour_pause, 3u, BICYCLE_DEMO_PAUSE_MS / 3u);
        }
        break;

    case STEP_REROUTE:
        ok = demo_enter_reroute_demo();
        /* 偏航步先留在地图页（要看得见偏航与 reroute）；巡览在 tick 里落地后启动。 */
        break;

    case STEP_NATION:
        demo_enter_nation();
        break;

    case STEP_THEME:
    default:
        demo_enter_theme();
        /* 全国跳已经收掉骑行/导航 ⇒ 这里是待机轮转：主页面 → 表盘 → 数据。 */
        demo_tour_start(k_tour_idle, 3u, 5000u);
        break;
    }

    if (!ok) {
        /* 这一步起不来也照常提示这一步的横幅（看得见"到哪一步了"）。 */
        syslog(LOG_WARNING, "demo: step %d did not start\n", (int)step);
    }

    demo_banner_show(k_steps[step].text);
    demo_publish();
}

/* ============================ 循环演示（random/linear/minghu） ============================ */

/** @brief xorshift32：状态只在本文件里，避免 `srand()` 影响程序别处。 */
static uint32_t demo_rand32(void)
{
    uint32_t x = s_random_seed;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_random_seed = x;
    return x;
}

/** @brief 用时间 + 系统 tick 播种，并空转几拍把弱种子打散。 */
static void demo_rand_seed(void)
{
    uint32_t seed = (uint32_t)time(NULL) ^ 0x9E3779B9u;
    unsigned i;

    seed ^= lv_tick_get() * 2654435761u;
    if (seed == 0u) {
        seed = 0x6D2B79F5u;
    }

    s_random_seed = seed;
    for (i = 0; i < 4u; i++) {
        (void)demo_rand32();
    }
}

/**
 * @brief 生成 64 项随机跳点表：从 20 个安全点里**有放回**地挑。
 * @note 允许重复（用户口径），但不让同一个点连着出现两次 —— 否则那一拍
 *       定位/横幅都不变，看起来像卡住了。
 */
static void demo_random_build(void)
{
    unsigned prev = (unsigned)DEMO_RANDOM_SAFE_N;
    unsigned i;

    demo_rand_seed();

    for (i = 0; i < (unsigned)DEMO_RANDOM_HOP_N; i++) {
        unsigned k;

        if (i == 0u) {
            k = demo_rand32() % (uint32_t)DEMO_RANDOM_SAFE_N;
        } else {
            k = (prev + 1u
                + demo_rand32() % ((uint32_t)DEMO_RANDOM_SAFE_N - 1u))
                % (uint32_t)DEMO_RANDOM_SAFE_N;
        }

        s_random_hop[i] = k_random_safe[k];
        prev = k;
    }
}

/** @brief 等距圆柱近似两点距离（米）；只用于终点判定，不参与路线生成。 */
static double demo_dist_m(double lat1, double lon1, double lat2, double lon2)
{
    const double r = 6371000.0;
    const double dlat = (lat2 - lat1) * M_PI / 180.0;
    const double dlon = (lon2 - lon1) * M_PI / 180.0;
    const double mlat = (lat1 + lat2) * 0.5 * M_PI / 180.0;
    const double x = dlon * cos(mlat);

    return sqrt(x * x + dlat * dlat) * r;
}

/** @brief 按里程/速度估算路线跑完要多久（毫秒）；只做终点检测的兜底。 */
static uint32_t demo_route_duration_ms(double meters, float kph)
{
    double sec;

    if (kph < 1.0f) {
        kph = 1.0f;
    }

    sec = meters / ((double)kph / 3.6);
    if (sec < 1.0) {
        sec = 1.0;
    } else if (sec > 86400.0) {
        sec = 86400.0;
    }

    return (uint32_t)(sec * 1000.0 + 0.5);
}

/**
 * @brief 新模式进入前的清场：停跳点/路线模拟/GPX 回放/导航，收掉演示骑行。
 * @note 顺序与四步「导航结束」同口径。`bicycle_gpx_sim_set_loop(false)` 一定要清，
 *       否则明湖 GPX 的往返回放会漏到下一场。
 */
static void demo_mode_reset_ride(void)
{
    map_page_t * page = lvgl_page_map();

    bicycle_gnss_hop_stop();
    bicycle_route_sim_stop();
    bicycle_gpx_sim_set_loop(false);
    bicycle_gpx_sim_stop();

    if (page != NULL) {
        map_page_nav_stop(page);
        (void)map_page_gnss_sim(page, NULL, false, 0.0f, 0.0f);
        map_page_end_ride(page);       /* 演示骑行不落盘 */
    }
}

/**
 * @brief 起（或重开）linear/minghu 的路线模拟，并记下终点/预计时长。
 * @return #BICYCLE_DEMO_OK；#BICYCLE_DEMO_ERR_MODE / #BICYCLE_DEMO_ERR_NAV。
 */
static int demo_mode_route_start(bicycle_demo_mode_t mode)
{
    const bicycle_nav_pt_t * pts;
    uint32_t n;
    float kph;
    double len_m;

    switch (mode) {
    case BICYCLE_DEMO_MODE_LINEAR:
        pts = s_linear_pts;
        n = 2u;
        kph = BICYCLE_DEMO_LINEAR_KPH;
        len_m = BICYCLE_DEMO_LINEAR_KM * 1000.0;
        break;

    case BICYCLE_DEMO_MODE_MINGHU:
        pts = s_minghu_pts;
        n = DEMO_MINGHU_PTS;
        kph = BICYCLE_DEMO_MINGHU_KPH;
        len_m = 2.0 * M_PI * BICYCLE_DEMO_MINGHU_RADIUS_M;
        break;

    default:
        return BICYCLE_DEMO_ERR_MODE;
    }

    if (bicycle_route_sim_start(pts, n, kph, 0) != 0) {
        syslog(LOG_WARNING, "demo: %s route sim failed\n", k_modes[mode].name);
        return BICYCLE_DEMO_ERR_NAV;
    }

    s_mode_route_ended = false;
    s_mode_route_start_ms = lv_tick_get();
    s_mode_route_check_ms = 0;
    s_mode_route_end_lon = pts[n - 1u].lon;
    s_mode_route_end_lat = pts[n - 1u].lat;
    s_mode_route_dur_ms = demo_route_duration_ms(len_m, kph);
    return BICYCLE_DEMO_OK;
}

/** @brief random：点表洗好并交给跳点引擎，`count=0` 持续跳。 */
static int demo_mode_random_start(void)
{
    uint32_t t0 = lv_tick_get();

    bicycle_gnss_hop_set_pts(s_random_hop, (uint32_t)DEMO_RANDOM_HOP_N);
    bicycle_gnss_hop_start(BICYCLE_DEMO_RANDOM_PERIOD_MS, 0);
    if (!bicycle_gnss_hop_active()) {
        return BICYCLE_DEMO_ERR_NAV;
    }

    s_mode_route = false;
    s_hop_last[0] = '\0';
    syslog(LOG_INFO, "demo: random start (%u pts x %u ms, setup %ums)\n",
        (unsigned)DEMO_RANDOM_HOP_N, (unsigned)BICYCLE_DEMO_RANDOM_PERIOD_MS,
        (unsigned)lv_tick_elaps(t0));
    return BICYCLE_DEMO_OK;
}

/**
 * @brief random：进模式时只洗点表、先不起跳，等地图像素稳住（见 warmup 宏）。
 * @note 冷机立刻起跳会和地图页首帧叠在同一个 UI 拍里，超过看门狗门限。
 */
static int demo_mode_enter_random(void)
{
    demo_random_build();

    s_mode_route = false;
    s_hop_last[0] = '\0';
    s_mode_random_pending = true;
    s_mode_random_warmup_ms = lv_tick_get();
    if (s_mode_random_warmup_ms == 0u) {
        /* lv_tick_elaps(0) 会当成已经等了很久；0 这一拍换成 1。 */
        s_mode_random_warmup_ms = 1u;
    }

    syslog(LOG_INFO, "demo: random warmup %u ms (%u pts x %u ms)\n",
        (unsigned)BICYCLE_DEMO_RANDOM_WARMUP_MS,
        (unsigned)DEMO_RANDOM_HOP_N, (unsigned)BICYCLE_DEMO_RANDOM_PERIOD_MS);
    return BICYCLE_DEMO_OK;
}

/** @brief linear：从当前定位（无效则明湖中心）向东 30 km 直线巡航。 */
static int demo_mode_enter_linear(void)
{
    bicycle_gnss_fix_t fix;
    double lat = BICYCLE_DEMO_MINGHU_LAT;
    double lon = BICYCLE_DEMO_MINGHU_LON;
    double coslat;
    double dlon;
    int rc;

    if (bicycle_runtime_poll_fix(&fix) && fix.valid
        && fix.latitude >= -85.0f && fix.latitude <= 85.0f
        && fix.longitude >= -180.0f && fix.longitude <= 180.0f) {
        lat = (double)fix.latitude;
        lon = (double)fix.longitude;
    } else {
        syslog(LOG_INFO, "demo: linear no fix, start from minghu center\n");
    }

    coslat = cos(lat * M_PI / 180.0);
    if (coslat < 0.1) {
        coslat = 0.1;
    }
    dlon = (BICYCLE_DEMO_LINEAR_KM * 1000.0) / (111320.0 * coslat);

    s_linear_pts[0].lon = lon;
    s_linear_pts[0].lat = lat;
    s_linear_pts[1].lon = lon + dlon;
    s_linear_pts[1].lat = lat;

    rc = demo_mode_route_start(BICYCLE_DEMO_MODE_LINEAR);
    if (rc != BICYCLE_DEMO_OK) {
        return rc;
    }

    s_mode_route = true;
    syslog(LOG_INFO, "demo: linear %.6f,%.6f -> %.6f,%.6f @%.0f km/h\n",
        lon, lat, s_linear_pts[1].lon, s_linear_pts[1].lat,
        (double)BICYCLE_DEMO_LINEAR_KPH);
    return BICYCLE_DEMO_OK;
}

/**
 * @brief 明湖演示回放哪条 GPX。
 *
 * @details 优先**固定回放** `BICYCLE_DEMO_GPX`（用户指定的
 *          `/mnt/lfs/mtp/import/20260529户外骑行.gpx`）；文件不在时再退回
 *          import/record 目录里文件名含“明湖”的最新一条；都没有才用生成的环线。
 * @return 找到为 true，out 写完整路径。
 */
static bool demo_find_minghu_gpx(char * out, size_t n)
{
    static const char * const dirs[] = {
        MYVENDOR_GPX_IMPORT_DIR,
        MYVENDOR_GPX_RECORD_DIR,
    };
    char best[192];
    time_t best_t = 0;
    unsigned d;

    /* 用户点名的文件优先（存在即用，不参与“最新”比较）。 */
    if (access(BICYCLE_DEMO_GPX, R_OK) == 0) {
        strncpy(out, BICYCLE_DEMO_GPX, n - 1u);
        out[n - 1u] = '\0';
        return true;
    }

    best[0] = '\0';

    for (d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        DIR * dir = opendir(dirs[d]);
        struct dirent * e;

        if (dir == NULL) {
            continue;
        }

        while ((e = readdir(dir)) != NULL) {
            struct stat st;
            char path[192];
            size_t len = strlen(e->d_name);
            int m;

            if (len < 5u || strcasecmp(e->d_name + len - 4u, ".gpx") != 0) {
                continue;
            }

            if (strstr(e->d_name, "明湖") == NULL) {
                continue;
            }

            m = snprintf(path, sizeof(path), "%s/%s", dirs[d], e->d_name);
            if (m < 0 || (size_t)m >= sizeof(path)) {
                continue;
            }

            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }

            if (best[0] == '\0' || st.st_mtime > best_t) {
                best_t = st.st_mtime;
                strncpy(best, path, sizeof(best) - 1u);
                best[sizeof(best) - 1u] = '\0';
            }
        }

        closedir(dir);
    }

    if (best[0] == '\0') {
        out[0] = '\0';
        return false;
    }

    strncpy(out, best, n - 1u);
    out[n - 1u] = '\0';
    return true;
}

/** @brief 生成绕明湖中心的 72 点环线（等距圆柱近似；东向按 cos(lat) 修正）。 */
static void demo_minghu_build(void)
{
    const double lat0 = BICYCLE_DEMO_MINGHU_LAT;
    const double lon0 = BICYCLE_DEMO_MINGHU_LON;
    const double dlat = BICYCLE_DEMO_MINGHU_RADIUS_M / 110540.0;
    const double dlon = BICYCLE_DEMO_MINGHU_RADIUS_M
        / (111320.0 * cos(lat0 * M_PI / 180.0));
    unsigned i;

    for (i = 0; i < (unsigned)DEMO_MINGHU_PTS; i++) {
        const double a = 2.0 * M_PI * (double)i / (double)DEMO_MINGHU_PTS;

        s_minghu_pts[i].lon = lon0 + dlon * cos(a);
        s_minghu_pts[i].lat = lat0 + dlat * sin(a);
    }
}

/**
 * @brief minghu：优先回放含“明湖”的 GPX（往返循环），否则用生成的 72 点环线。
 * @note 环线用 `helm_shell_ensure_ride()` 开一条演示骑行把轨迹画出来；
 *       退出时 `map_page_end_ride()` 会把未保存的 GPX 丢弃。
 */
static int demo_mode_enter_minghu(void)
{
    char path[192];
    int rc;

    if (demo_find_minghu_gpx(path, sizeof(path))) {
        if (!demo_enter_gpx_nav_file(path)) {
            syslog(LOG_WARNING, "demo: minghu gpx nav failed (%s)\n", path);
            return BICYCLE_DEMO_ERR_NAV;
        }

        /* 回放到端点掉头继续，别停住。 */
        bicycle_gpx_sim_set_loop(true);
        s_mode_route = false;
        syslog(LOG_INFO, "demo: minghu gpx loop %s\n", path);
        return BICYCLE_DEMO_OK;
    }

    demo_minghu_build();
    rc = demo_mode_route_start(BICYCLE_DEMO_MODE_MINGHU);
    if (rc != BICYCLE_DEMO_OK) {
        return rc;
    }

    s_mode_route = true;
    helm_shell_ensure_ride();
    syslog(LOG_INFO, "demo: minghu ring %u pts r=%.0fm @%.0f km/h\n",
        (unsigned)DEMO_MINGHU_PTS, BICYCLE_DEMO_MINGHU_RADIUS_M,
        (double)BICYCLE_DEMO_MINGHU_KPH);
    return BICYCLE_DEMO_OK;
}

/** @brief 进模式时露一次横幅（random 顺手记住第一个点的名字）。 */
static void demo_mode_banner(void)
{
    char buf[48];

    switch (s_mode) {
    case BICYCLE_DEMO_MODE_RANDOM: {
        const char * name;

        if (s_mode_random_pending) {
            /* warmup 期间还没起跳：先报“准备”，别显示未跳时的 "-"。 */
            demo_banner_show("随机跳转 · 准备");
            break;
        }

        name = bicycle_gnss_hop_cur_name();
        if (name == NULL || name[0] == '\0') {
            name = "-";
        }

        lv_snprintf(buf, sizeof(buf), "随机跳转 · %s", name);
        strncpy(s_hop_last, name, sizeof(s_hop_last) - 1u);
        s_hop_last[sizeof(s_hop_last) - 1u] = '\0';
        demo_banner_show(buf);
        break;
    }

    case BICYCLE_DEMO_MODE_LINEAR:
        demo_banner_show("30km 直线巡航");
        break;

    case BICYCLE_DEMO_MODE_MINGHU:
        demo_banner_show("绕明湖骑行");
        break;

    default:
        break;
    }
}

/** @brief 取消 random 的 warmup（退出/切模式时用；跳点引擎已经停掉才调）。 */
static void demo_mode_random_cancel(void)
{
    s_mode_random_pending = false;
    s_mode_random_warmup_ms = 0;
}

/** @brief 新模式的公共进入流程：清场 → 强制地图页 → 起料 → 横幅/状态槽。 */
static int demo_mode_enter(bicycle_demo_mode_t mode)
{
    int rc;

    demo_mode_random_cancel();
    demo_mode_reset_ride();
    (void)helm_shell_demo_page("map");

    switch (mode) {
    case BICYCLE_DEMO_MODE_RANDOM:
        rc = demo_mode_enter_random();
        break;

    case BICYCLE_DEMO_MODE_LINEAR:
        rc = demo_mode_enter_linear();
        break;

    case BICYCLE_DEMO_MODE_MINGHU:
        rc = demo_mode_enter_minghu();
        break;

    default:
        rc = BICYCLE_DEMO_ERR_MODE;
        break;
    }

    if (rc != BICYCLE_DEMO_OK) {
        return rc;
    }

    demo_mode_banner();
    demo_publish();
    return BICYCLE_DEMO_OK;
}

/**
 * @brief 退出新增循环模式：停模拟 → 收骑行 → 清页面锁/横幅 → 还原主题。
 * @note 停止/切模式都走这里；`bicycle_gpx_sim_set_loop(false)` 保证明湖 GPX 的
 *       往返开关不会漏到下一场。
 */
static void demo_mode_leave(void)
{
    if (!s_mode_active) {
        return;
    }

    demo_mode_reset_ride();
    demo_mode_random_cancel();
    s_mode_enter_pending = false;
    s_mode_enter_wait_ms = 0;
    bicycle_gpx_sim_set_loop(false);

    s_mode_active = false;
    s_mode = BICYCLE_DEMO_MODE_FULL;
    s_mode_route = false;
    s_mode_route_ended = false;
    s_mode_route_start_ms = 0;
    s_mode_route_dur_ms = 0;
    s_mode_route_end_ms = 0;
    s_mode_route_check_ms = 0;
    s_gpx[0] = '\0';

    helm_shell_demo_page_clear();
    demo_banner_clear();
    demo_theme_restore();
    demo_publish();
    syslog(LOG_NOTICE, "demo: mode stop\n");
}

/** @brief random：warmup 到点后起跳；之后点名字变了就报 `随机跳转 · <name>`。 */
static void demo_mode_random_tick(void)
{
    const char * name;
    char buf[48];

    if (s_mode_random_pending) {
        uint32_t waited = (s_mode_random_warmup_ms != 0u)
            ? lv_tick_elaps(s_mode_random_warmup_ms) : BICYCLE_DEMO_RANDOM_WARMUP_MS;

        if (waited < BICYCLE_DEMO_RANDOM_WARMUP_MS) {
            return;
        }

        /* 先把 pending 清掉：起跳只尝试一次，失败也不在每一拍重试。 */
        demo_mode_random_cancel();
        syslog(LOG_INFO, "demo: random warmup done (%ums), start now\n",
            (unsigned)waited);
        if (demo_mode_random_start() != BICYCLE_DEMO_OK) {
            syslog(LOG_WARNING, "demo: random start failed\n");
            return;
        }

        /* 起跳这一拍不点名，下一拍名字变化再报（避免和“准备”叠在一起）。 */
        return;
    }

    name = bicycle_gnss_hop_cur_name();
    if (name == NULL || name[0] == '\0' || strcmp(name, s_hop_last) == 0) {
        return;
    }

    strncpy(s_hop_last, name, sizeof(s_hop_last) - 1u);
    s_hop_last[sizeof(s_hop_last) - 1u] = '\0';
    lv_snprintf(buf, sizeof(buf), "随机跳转 · %s", name);
    demo_banner_show(buf);
    syslog(LOG_INFO, "demo: random -> %s\n", name);
}

/**
 * @brief linear/minghu：到终点后隔 `BICYCLE_DEMO_LOOP_PAUSE_MS` 重开一轮。
 *
 * @note `bicycle_route_sim_active()` 的语义是"线程还在"，**跑完终点也不会变 false**
 *       （只是把定位钉在最后一点）。所以这里除了看 active，还用两条兜底：
 *       ① 当前定位落在终点 `DEMO_ROUTE_END_M` 内；② 按里程/速度估算的时长已到。
 *       三者任一成立即视为到终点，停 2 秒再从头开一轮。
 */
static void demo_mode_route_tick(void)
{
    bicycle_gnss_fix_t fix;
    uint32_t now;

    if (!s_mode_route) {
        return;
    }

    now = lv_tick_get();

    if (s_mode_route_ended) {
        if (lv_tick_elaps(s_mode_route_end_ms) >= BICYCLE_DEMO_LOOP_PAUSE_MS) {
            int rc = demo_mode_route_start(s_mode);

            if (rc != BICYCLE_DEMO_OK) {
                /* 起不来就再等一轮，别把 UI 线程卡在重试上。 */
                s_mode_route_end_ms = now;
                syslog(LOG_WARNING, "demo: %s restart failed (%d)\n",
                    k_modes[s_mode].name, rc);
            } else {
                syslog(LOG_INFO, "demo: %s loop restart\n", k_modes[s_mode].name);
            }
        }
        return;
    }

    if (!bicycle_route_sim_active()) {
        s_mode_route_ended = true;
        s_mode_route_end_ms = now;
        syslog(LOG_INFO, "demo: %s route sim stopped, loop in %u ms\n",
            k_modes[s_mode].name, (unsigned)BICYCLE_DEMO_LOOP_PAUSE_MS);
        return;
    }

    if (s_mode_route_dur_ms > 0u
        && lv_tick_elaps(s_mode_route_start_ms) > s_mode_route_dur_ms + 5000u) {
        s_mode_route_ended = true;
        s_mode_route_end_ms = now;
        syslog(LOG_INFO, "demo: %s route timeout, loop in %u ms\n",
            k_modes[s_mode].name, (unsigned)BICYCLE_DEMO_LOOP_PAUSE_MS);
        return;
    }

    if (s_mode_route_check_ms != 0u
        && lv_tick_elaps(s_mode_route_check_ms) < DEMO_ROUTE_CHECK_MS) {
        return;
    }

    s_mode_route_check_ms = now;
    if (bicycle_runtime_poll_fix(&fix) && fix.valid
        && demo_dist_m((double)fix.latitude, (double)fix.longitude,
            s_mode_route_end_lat, s_mode_route_end_lon) <= DEMO_ROUTE_END_M) {
        s_mode_route_ended = true;
        s_mode_route_end_ms = now;
        syslog(LOG_INFO, "demo: %s arrived, loop in %u ms\n",
            k_modes[s_mode].name, (unsigned)BICYCLE_DEMO_LOOP_PAUSE_MS);
    }
}

/** @brief 新增循环模式的每圈节拍（poll 里 scene 之后、full 之前）。 */
static void demo_mode_poll(void)
{
    demo_banner_tick();

    switch (s_mode) {
    case BICYCLE_DEMO_MODE_RANDOM:
        demo_mode_random_tick();
        break;

    case BICYCLE_DEMO_MODE_LINEAR:
    case BICYCLE_DEMO_MODE_MINGHU:
        demo_mode_route_tick();
        break;

    default:
        break;
    }
}

/**
 * @brief 冷机 random 早到：地图页建好后补做真正的进入（清场/切页/warmup）。
 * @note 只在 `s_mode_enter_pending` 时干活；超时 `#DEMO_MODE_WAIT_MAP_MS`
 *       还没地图就放弃，别让状态槽一直停在“准备”。
 */
static void demo_mode_enter_tick(void)
{
    const char * name;
    int rc;

    if (!s_mode_enter_pending) {
        return;
    }

    if (lvgl_page_map() == NULL) {
        if (lv_tick_elaps(s_mode_enter_wait_ms) < DEMO_MODE_WAIT_MAP_MS) {
            return;
        }

        name = k_modes[s_mode].name;
        syslog(LOG_WARNING, "demo: no map page, cancel %s\n", name);
        demo_mode_leave();
        return;
    }

    s_mode_enter_pending = false;
    s_mode_enter_wait_ms = 0;
    name = k_modes[s_mode].name;
    rc = demo_mode_enter(s_mode);
    if (rc != BICYCLE_DEMO_OK) {
        syslog(LOG_WARNING, "demo: mode %s failed (deferred, %d)\n", name, rc);
        demo_mode_leave();
        return;
    }

    syslog(LOG_NOTICE, "demo: mode %s (deferred after map)\n", name);
}

unsigned bicycle_demo_mode_count(void)
{
    return (unsigned)BICYCLE_DEMO_MODE_COUNT;
}

const char * bicycle_demo_mode_name(bicycle_demo_mode_t mode)
{
    if ((unsigned)mode >= (unsigned)BICYCLE_DEMO_MODE_COUNT) {
        return NULL;
    }

    return k_modes[mode].name;
}

const char * bicycle_demo_mode_desc(bicycle_demo_mode_t mode)
{
    if ((unsigned)mode >= (unsigned)BICYCLE_DEMO_MODE_COUNT) {
        return NULL;
    }

    return k_modes[mode].desc;
}

int bicycle_demo_mode_from_name(const char * name)
{
    unsigned i;

    if (name == NULL || name[0] == '\0') {
        return -1;
    }

    for (i = 0; i < (unsigned)BICYCLE_DEMO_MODE_COUNT; i++) {
        if (strcasecmp(name, k_modes[i].name) == 0) {
            return (int)i;
        }
    }

    return -1;
}

bicycle_demo_mode_t bicycle_demo_current_mode(void)
{
    if (s_mode_active && (unsigned)s_mode < (unsigned)BICYCLE_DEMO_MODE_COUNT) {
        return s_mode;
    }

    if (s_on) {
        return BICYCLE_DEMO_MODE_FULL;
    }

    return (bicycle_demo_mode_t)-1;
}

/* ============================ 单场景 ============================ */

/**
 * @brief 清掉导航/回放（含路线模拟）/演示骑行，并把传感器归零。
 * @note 顺序与四步里的「导航结束」同口径：先停导航，再停回放，最后收骑行。
 *       跳点也一并停掉（跳的是合成定位，留着会把下一个场景带跑）。
 */
static void demo_scene_clear_ride(void)
{
    map_page_t * page = lvgl_page_map();

    bicycle_gnss_hop_stop();
    demo_sim_reset();

    if (page != NULL) {
        map_page_nav_stop(page);
        (void)map_page_gnss_sim(page, NULL, false, 0.0f, 0.0f);
        map_page_end_ride(page);
    } else {
        bicycle_gpx_sim_stop();
        bicycle_route_sim_stop();
    }

    s_demo_ride = false;
    bicycle_runtime_update_sensors(0, false, 0, false, 0, false);
}

/** @brief 纯横幅场景 → `helm_shell_demo_banner()` 的模式（0 = 不是纯横幅场景）。 */
static int demo_scene_banner_mode(int idx)
{
    switch ((demo_scene_t)idx) {
    case DEMO_SCENE_NAV_STRAIGHT:
        return 1;
    case DEMO_SCENE_NAV_LEFT:
        return 2;
    case DEMO_SCENE_NAV_WARN:
        return 3;
    case DEMO_SCENE_NAV_ARRIVED:
        return 4;
    case DEMO_SCENE_NAV_RIGHT:
        return 5;
    case DEMO_SCENE_NAV_UTURN:
        return 6;
    case DEMO_SCENE_NAV_REPLAN:
        return 7;
    default:
        return 0;
    }
}

/** @brief 页面锁：进不去就按进入失败处理。 */
static int demo_scene_page(const char * name)
{
    if (!helm_shell_demo_page(name)) {
        syslog(LOG_WARNING, "demo: scene page %s failed\n", name);
        return BICYCLE_DEMO_SCENE_ERR_ENTER;
    }

    return 0;
}

/**
 * @brief 进某个单场景（switch 做具体动作）。
 * @return 0 成功；#BICYCLE_DEMO_SCENE_ERR_ENTER/-UNKNOWN。
 * @note 调用方已把 `s_scene/s_scene_active` 设好；失败由调用方回滚（leave）。
 * @note `ride`/`data` 走 GPX 回放 + 跟线导航（nav 在 = `helm_in_session()` 成立，
 *       页面锁能直接翻到骑行页）；不额外开录，时间列显示 0 是预期。
 */
static int demo_scene_enter(int idx)
{
    int rc = 0;

    switch ((demo_scene_t)idx) {
    case DEMO_SCENE_STANDBY:
        demo_scene_clear_ride();
        rc = demo_scene_page("standby");
        break;

    case DEMO_SCENE_RIDE:
        if (!demo_enter_gpx_nav()) {
            rc = BICYCLE_DEMO_SCENE_ERR_ENTER;
            break;
        }
        bicycle_runtime_update_sensors(145, true, 85, true, 220, true);
        rc = demo_scene_page("ride");
        break;

    case DEMO_SCENE_DASH:
        if (!demo_enter_gpx_nav()) {
            rc = BICYCLE_DEMO_SCENE_ERR_ENTER;
            break;
        }
        bicycle_runtime_update_sensors(145, true, 85, true, 220, true);
        rc = demo_scene_page("dash");
        break;

    case DEMO_SCENE_DATA:
        if (!demo_enter_gpx_nav()) {
            rc = BICYCLE_DEMO_SCENE_ERR_ENTER;
            break;
        }
        bicycle_runtime_update_sensors(145, true, 85, true, 220, true);
        rc = demo_scene_page("data");
        break;

    case DEMO_SCENE_MAP:
        demo_scene_clear_ride();
        rc = demo_scene_page("map");
        break;

    case DEMO_SCENE_CLIMB:
        if (!demo_enter_gpx_nav()) {
            rc = BICYCLE_DEMO_SCENE_ERR_ENTER;
            break;
        }
        rc = demo_scene_page("climb");
        break;

    case DEMO_SCENE_NAV:
        if (!demo_enter_gpx_nav()) {
            rc = BICYCLE_DEMO_SCENE_ERR_ENTER;
            break;
        }
        rc = demo_scene_page("map");
        if (rc == 0) {
            helm_shell_demo_banner(0);
        }
        break;

    case DEMO_SCENE_NAV_STRAIGHT:
    case DEMO_SCENE_NAV_LEFT:
    case DEMO_SCENE_NAV_RIGHT:
    case DEMO_SCENE_NAV_UTURN:
    case DEMO_SCENE_NAV_WARN:
    case DEMO_SCENE_NAV_REPLAN:
    case DEMO_SCENE_NAV_ARRIVED:
        /* 纯横幅演示：不跑真导航，靠 `helm_shell_demo_banner()` 强制出状态。 */
        demo_scene_clear_ride();
        rc = demo_scene_page("map");
        if (rc == 0) {
            helm_shell_demo_banner(demo_scene_banner_mode(idx));
        }
        break;

    case DEMO_SCENE_ALERT_SPEED:
        /* 回放速度默认 VMAP_NAV_SIM_SPEED_KPH（通常 >= 60），高于 30 km/h 告警阈值；
         * 心率/功率压在阈值下，避免一次亮两行。 */
        if (!demo_enter_gpx_nav()) {
            rc = BICYCLE_DEMO_SCENE_ERR_ENTER;
            break;
        }
        bicycle_runtime_update_sensors(120, true, 80, true, 150, true);
        rc = demo_scene_page("ride");
        break;

    case DEMO_SCENE_ALERT_HR:
        demo_scene_clear_ride();
        rc = demo_scene_page("ride");
        if (rc == 0) {
            /* 功率压到 150 W（< HELM_ALERT_PWR_W=250）：这一场只点亮心率行，
             * 否则两行同时告警，看不出是哪个场景。 */
            bicycle_runtime_update_sensors(190, true, 90, true, 150, true);
            s_scene_alert_hr = true;
            s_scene_hr_ms = lv_tick_get();
        }
        break;

    case DEMO_SCENE_PAUSED:
        demo_scene_clear_ride();
        rc = demo_scene_page("ride");
        if (rc == 0) {
            /* 暂停条只看 `s_sh.paused`（与是否在录无关），页面锁已翻到骑行页，
             * 这里直接置暂停即可；不要为了暂停条再开一条录制。 */
            helm_shell_set_paused(true);
        }
        break;

    case DEMO_SCENE_SAVE:
        rc = demo_scene_page("ride");
        if (rc == 0) {
            helm_shell_demo_save(true);
        }
        break;

    case DEMO_SCENE_NOTIFY:
        /* 保持当前页：只投通知，不动页面锁。 */
        if (myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_NOTIFY_SHOW,
                "演示通知|这是一条测试通知") != 0) {
            rc = BICYCLE_DEMO_SCENE_ERR_ENTER;
        }
        break;

    case DEMO_SCENE_TOAST:
        if (myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_BOTTOM_SHOW,
                "演示提示：底部 toast") != 0) {
            rc = BICYCLE_DEMO_SCENE_ERR_ENTER;
        }
        break;

    case DEMO_SCENE_THEME_CLASSIC:
    case DEMO_SCENE_THEME_NIGHT:
        if (!lvgl_page_theme_set_by_name(idx == DEMO_SCENE_THEME_NIGHT
                ? "night" : "classic")) {
            rc = BICYCLE_DEMO_SCENE_ERR_ENTER;
            break;
        }
        rc = demo_scene_page("standby");
        break;

    case DEMO_SCENE_GPX:
        if (!demo_enter_gpx_nav()) {
            rc = BICYCLE_DEMO_SCENE_ERR_ENTER;
            break;
        }
        rc = demo_scene_page("map");
        break;

    case DEMO_SCENE_PLAN:
        if (!demo_enter_plan_nav()) {
            rc = BICYCLE_DEMO_SCENE_ERR_ENTER;
            break;
        }
        rc = demo_scene_page("map");
        break;

    case DEMO_SCENE_NATION:
        demo_enter_nation();
        rc = demo_scene_page("map");
        break;

    case DEMO_SCENE_IDLE:
        if (myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_IDLE_ENTER, NULL) != 0) {
            rc = BICYCLE_DEMO_SCENE_ERR_ENTER;
        }
        break;

    case DEMO_SCENE_WAKE:
        if (myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_IDLE_WAKE, NULL) != 0) {
            rc = BICYCLE_DEMO_SCENE_ERR_ENTER;
            break;
        }
        rc = demo_scene_page("standby");
        break;

    case DEMO_SCENE_PAUSE_RUN:
        /* 真实暂停/继续：进场景先骑，tick 里按"中键"再继续。
         * 页面锁在 `demo_enter_pause_demo()` 里已锁到骑行页（要看得见速度归 0）。 */
        if (!demo_enter_pause_demo()) {
            rc = BICYCLE_DEMO_SCENE_ERR_ENTER;
        }
        break;

    case DEMO_SCENE_REROUTE_RUN:
        /* 真实城区偏航重规划：规划 → 骑 → 偏航 → reroute，全程由 tick 推进。 */
        if (!demo_enter_reroute_demo()) {
            rc = BICYCLE_DEMO_SCENE_ERR_ENTER;
            break;
        }
        rc = demo_scene_page("map");
        break;

    default:
        return BICYCLE_DEMO_SCENE_ERR_UNKNOWN;
    }

    if (rc != 0) {
        return rc;
    }

    /* 静止时钟场景的覆盖层不能被演示横幅盖住。 */
    if (idx != DEMO_SCENE_IDLE) {
        demo_banner_show(k_scenes[idx].desc);
    }

    demo_publish();
    return 0;
}

/**
 * @brief 退出当前单场景，收掉它起的所有东西。
 * @note 顺序很关键：先停 nav/回放/自行车记录，**最后**才清页面锁；反过来的话
 *       清完锁的那一拍会把还没停的导航状态重新刷出来。
 */
static void demo_scene_leave(void)
{
    if (!s_scene_active) {
        return;
    }

    /* 静止覆盖层先唤醒：后面的页面/横幅操作都在正常主界面上做。 */
    if (s_scene == DEMO_SCENE_IDLE) {
        (void)myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_IDLE_WAKE, NULL);
    }

    helm_shell_demo_save(false);
    helm_shell_set_paused(false);
    helm_shell_demo_banner(0);

    demo_scene_clear_ride();

    helm_shell_demo_page_clear();

    s_scene = -1;
    s_scene_active = false;
    s_scene_alert_hr = false;

    demo_publish();
    syslog(LOG_INFO, "demo: scene leave\n");
}

/** @brief 场景名/下标解析；-1 = 不认识。纯数字按表下标，其余大小写不敏感。 */
static int demo_scene_find(const char * name)
{
    unsigned i;

    if (name == NULL || name[0] == '\0') {
        return -1;
    }

    if (name[0] >= '0' && name[0] <= '9') {
        char * end = NULL;
        unsigned long v = strtoul(name, &end, 10);

        if (end == NULL || *end != '\0' || v >= (unsigned long)DEMO_SCENE_COUNT) {
            return -1;
        }

        return (int)v;
    }

    for (i = 0; i < (unsigned)DEMO_SCENE_COUNT; i++) {
        if (strcasecmp(name, k_scenes[i].name) == 0) {
            return (int)i;
        }
    }

    return -1;
}

unsigned bicycle_demo_scene_count(void)
{
    return (unsigned)DEMO_SCENE_COUNT;
}

const char * bicycle_demo_scene_name(unsigned index)
{
    if (index >= (unsigned)DEMO_SCENE_COUNT) {
        return NULL;
    }

    return k_scenes[index].name;
}

const char * bicycle_demo_scene_desc(unsigned index)
{
    if (index >= (unsigned)DEMO_SCENE_COUNT) {
        return NULL;
    }

    return k_scenes[index].desc;
}

const char * bicycle_demo_scene_current(void)
{
    if (!s_scene_active || s_scene < 0 || s_scene >= (int)DEMO_SCENE_COUNT) {
        return NULL;
    }

    return k_scenes[s_scene].name;
}

const char * bicycle_demo_scene_current_desc(void)
{
    if (!s_scene_active || s_scene < 0 || s_scene >= (int)DEMO_SCENE_COUNT) {
        return NULL;
    }

    return k_scenes[s_scene].desc;
}

bool bicycle_demo_scene_active(void)
{
    return s_scene_active;
}

int bicycle_demo_scene_run(const char * name)
{
    int idx = demo_scene_find(name);
    int rc;

    if (idx < 0) {
        syslog(LOG_WARNING, "demo: unknown scene '%s'\n", name != NULL ? name : "");
        return BICYCLE_DEMO_SCENE_ERR_UNKNOWN;
    }

    /* 与循环演示（全量/新增模式）互斥：先停循环（会恢复它翻过的主题）。 */
    if (s_on || s_mode_active) {
        bicycle_demo_stop();
    }

    if (s_scene_active) {
        demo_scene_leave();
    }

    /* 第一次进场景时记住原主题；后续换场景不要覆盖，`demo off` 才恢复。 */
    if (!s_theme_hold) {
        demo_theme_save();
    }

    s_scene = idx;
    s_scene_active = true;

    rc = demo_scene_enter(idx);
    if (rc != 0) {
        /* 进入失败：回滚到"不在场景"，并把可能已翻掉的主题还原。 */
        demo_scene_leave();
        demo_theme_restore();
        demo_publish();
        syslog(LOG_WARNING, "demo: scene %s failed (%d)\n", k_scenes[idx].name, rc);
        return BICYCLE_DEMO_SCENE_ERR_ENTER;
    }

    syslog(LOG_NOTICE, "demo: scene %s\n", k_scenes[idx].name);
    return 0;
}

int bicycle_demo_scene_step(int dir)
{
    const int n = (int)DEMO_SCENE_COUNT;
    int idx;

    if (s_scene_active && s_scene >= 0 && s_scene < n) {
        idx = (dir < 0) ? ((s_scene + n - 1) % n) : ((s_scene + 1) % n);
    } else {
        idx = (dir < 0) ? (n - 1) : 0;
    }

    return bicycle_demo_scene_run(k_scenes[idx].name);
}

/* ============================ 开关 ============================ */

int bicycle_demo_start_mode(bicycle_demo_mode_t mode)
{
    const bicycle_runtime_t * rt;
    bool wait_map = false;
    int rc;

    if ((unsigned)mode >= (unsigned)BICYCLE_DEMO_MODE_COUNT) {
        return BICYCLE_DEMO_ERR_MODE;
    }

    /* 全量四步沿用旧入口（它自己会停单场景/新增模式）。 */
    if (mode == BICYCLE_DEMO_MODE_FULL) {
        return bicycle_demo_start();
    }

    if (lvgl_page_map() == NULL) {
        /* 冷机 3~4 s 敲命令时地图页可能还没建好。random 是展会/拍摄最常用的
         * 冷机入口，先记下模式、等 UI 主循环里地图可用再真正进入；其余模式
         * 仍然直接报"地图没准备好"（它们依赖的导航/回放更重，别悄悄排队）。 */
        if (mode != BICYCLE_DEMO_MODE_RANDOM) {
            return BICYCLE_DEMO_ERR_NO_MAP;
        }

        wait_map = true;
    }

    /* 与全量/单场景/上一个模式互斥：先全部收干净（会恢复主题）。 */
    if (s_mode_active) {
        demo_mode_leave();
    }

    if (s_scene_active) {
        demo_scene_leave();
    }

    if (s_on) {
        bicycle_demo_stop();
    }

    /* 停掉演示自己的骑行后再判：正在录的真实骑行不能被顶掉。 */
    rt = bicycle_runtime_get();
    if ((rt != NULL && rt->recording) || bicycle_ride_gpx_active()) {
        return BICYCLE_DEMO_ERR_RIDING;
    }

    /* 从单场景切过来时 `s_theme_hold` 已经存着进场景之前的主题，别覆盖。 */
    if (!s_theme_hold) {
        demo_theme_save();
    }

    s_mode = mode;
    s_mode_active = true;
    s_mode_enter_pending = false;
    s_mode_enter_wait_ms = 0;

    if (wait_map) {
        /* 先把状态槽写成 random，命令侧不用等地图；真正进入由 poll 补上。 */
        s_mode_enter_pending = true;
        s_mode_enter_wait_ms = lv_tick_get();
        if (s_mode_enter_wait_ms == 0u) {
            s_mode_enter_wait_ms = 1u;
        }

        demo_publish();
        syslog(LOG_INFO, "demo: random waits for map page\n");
        return BICYCLE_DEMO_OK;
    }

    rc = demo_mode_enter(mode);
    if (rc != BICYCLE_DEMO_OK) {
        /* 进入失败：回到"没有演示"，并把可能已经翻掉的主题还原。 */
        demo_mode_leave();
        syslog(LOG_WARNING, "demo: mode %s failed (%d)\n",
            k_modes[mode].name, rc);
        return rc;
    }

    syslog(LOG_NOTICE, "demo: mode %s\n", k_modes[mode].name);
    return BICYCLE_DEMO_OK;
}

int bicycle_demo_start(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();

    /* 新增循环模式与四步循环互斥：先停掉它（会恢复主题/收骑行）。 */
    if (s_mode_active) {
        bicycle_demo_stop();
    }

    /* 单场景与四步循环互斥：先从场景退出（它会收掉回放/骑行/页面锁）。 */
    if (s_scene_active) {
        demo_scene_leave();
    }

    if (s_on) {
        return BICYCLE_DEMO_OK;
    }

    if (lvgl_page_map() == NULL) {
        return BICYCLE_DEMO_ERR_NO_MAP;
    }

    /* 真实骑行中不开：演示会改里程/轨迹/主题，用户这一趟就废了。 */
    if ((rt != NULL && rt->recording) || bicycle_ride_gpx_active()) {
        return BICYCLE_DEMO_ERR_RIDING;
    }

    /* 从单场景切回来时 `s_theme_hold` 已经存着**进场景之前**的主题，别覆盖。 */
    if (!s_theme_hold) {
        demo_theme_save();
    }
    s_cycle = 0;
    s_gpx[0] = '\0';
    s_on = true;

    demo_goto(STEP_GPX_NAV);
    if (s_gpx[0] == '\0') {
        bicycle_demo_stop();
        return BICYCLE_DEMO_ERR_NAV;
    }

    /* 开演示的唯一入口在菜单里，而演示要看地图 ⇒ 让菜单让位
     * （与"骑行记录 → 导航"同一个姿势：起完导航再收页）。 */
    if (lvgl_page_current_id() == BICYCLE_PM_ID_MENU) {
        (void)lv_pm_close_page_msg(NULL);
    }

    syslog(LOG_NOTICE, "demo: start (gpx=%s)\n", s_gpx);
    return BICYCLE_DEMO_OK;
}

void bicycle_demo_stop(void)
{
    map_page_t * page;

    if (!s_on && !s_scene_active && !s_mode_active) {
        return;
    }

    /* 先退新增循环模式（停 hop/route/gpx、收骑行、清页面锁/横幅/主题）。 */
    if (s_mode_active) {
        demo_mode_leave();

        /* 新模式自己已经做完收尾；没有别的循环在跑就不重复。 */
        if (!s_on && !s_scene_active) {
            return;
        }
    }

    /* 再退单场景（停 nav/回放/骑行、清页面锁）；四步循环两种模式都兼容。 */
    if (s_scene_active) {
        demo_scene_leave();
    }

    if (!s_on) {
        /* 只有单场景在跑：收横幅、恢复原主题（场景里可能翻过）。 */
        demo_banner_clear();
        demo_theme_restore();
        demo_publish();
        syslog(LOG_NOTICE, "demo: scene stop\n");
        return;
    }

    s_on = false;

    /* 跳点在任何一步都可能还开着（比如刚进全国跳点就被关）。 */
    bicycle_gnss_hop_stop();

    /* 页面巡览与演示修饰（回放冻结 / 偏航偏移）、暂停态一律收干净。 */
    demo_tour_stop();
    demo_sim_reset();
    if (helm_shell_paused()) {
        helm_shell_set_paused(false);
    }

    page = lvgl_page_map();
    if (page != NULL) {
        if (s_demo_ride || s_step <= STEP_PLAN_NAV) {
            /* 演示骑行不落盘：每次演示都留一条 TRK 会把骑行记录灌满。 */
            map_page_end_ride(page);
        }

        map_page_nav_stop(page);
        (void)map_page_gnss_sim(page, NULL, false, 0.0f, 0.0f);
    } else {
        bicycle_gpx_sim_stop();
        bicycle_route_sim_stop();
    }
    s_demo_ride = false;

    /* 暂停步会把页面锁在骑行页；停止时解锁，恢复正常轮转。 */
    helm_shell_demo_page_clear();

    demo_banner_clear();
    demo_theme_restore();
    demo_publish();
    syslog(LOG_NOTICE, "demo: stop (%u cycle(s) done)\n", (unsigned)s_cycle);
}

void bicycle_demo_toggle(void)
{
    if (bicycle_demo_active()) {
        bicycle_demo_stop();
    } else {
        (void)bicycle_demo_start();
    }
}

bool bicycle_demo_active(void)
{
    return s_on || s_scene_active || s_mode_active;
}

const char * bicycle_demo_status(void)
{
    static char buf[48];

    if (s_scene_active && s_scene >= 0 && s_scene < (int)DEMO_SCENE_COUNT) {
        lv_snprintf(buf, sizeof(buf), "场景 %s", k_scenes[s_scene].name);
        return buf;
    }

    if (s_mode_active && (unsigned)s_mode < (unsigned)BICYCLE_DEMO_MODE_COUNT) {
        if (s_mode == BICYCLE_DEMO_MODE_RANDOM) {
            if (s_mode_enter_pending || s_mode_random_pending) {
                return "随机跳转 · 准备";
            }

            lv_snprintf(buf, sizeof(buf), "随机跳转 · %s",
                bicycle_gnss_hop_cur_name());
            return buf;
        }

        return k_modes[s_mode].desc;
    }

    if (!s_on) {
        return "自动演示：导航 / 跳点 / 主题";
    }

    switch (s_step) {
    case STEP_NATION:
        lv_snprintf(buf, sizeof(buf), "%s · %s", k_steps[STEP_NATION].text,
            bicycle_gnss_hop_cur_name());
        return buf;

    case STEP_GPX_NAV:
    case STEP_PLAN_NAV:
    case STEP_THEME:
    default:
        return k_steps[s_step].text;
    }
}

const char * bicycle_demo_error_text(int rc)
{
    switch (rc) {
    case BICYCLE_DEMO_ERR_NO_MAP:
        return "地图还没准备好";
    case BICYCLE_DEMO_ERR_NAV:
        return "起不了导航（先导入 GPX？）";
    case BICYCLE_DEMO_ERR_RIDING:
        return "请先结束当前骑行";
    case BICYCLE_DEMO_ERR_MODE:
        return "未知演示模式";
    default:
        return "无法开始";
    }
}

void bicycle_demo_poll(void)
{
    map_page_t * page;
    uint32_t elapsed;

    /* 单场景：不跑全量循环，只管横幅到点收起 + 各场景自己的节拍。 */
    if (s_scene_active) {
        demo_banner_tick();

        if (s_scene == DEMO_SCENE_PAUSE_RUN) {
            demo_pause_tick();
        } else if (s_scene == DEMO_SCENE_REROUTE_RUN) {
            demo_reroute_tick();
        }

        if (s_scene_alert_hr && lv_tick_elaps(s_scene_hr_ms) >= 250u) {
            s_scene_hr_ms = lv_tick_get();
            /* 功率压到 150 W（< HELM_ALERT_PWR_W=250）：这一场只点亮心率行，
             * 否则两行同时告警，看不出是哪个场景。 */
            bicycle_runtime_update_sensors(190, true, 90, true, 150, true);
        }

        return;
    }

    /* 新增循环模式：random 报点名；linear/minghu 到终点循环重开。 */
    if (s_mode_active) {
        if (s_mode_enter_pending) {
            /* 冷机 random 早到：地图页可用后再进入；本拍先不跑模式节拍。 */
            demo_mode_enter_tick();
            return;
        }

        demo_mode_poll();
        return;
    }

    if (!s_on) {
        return;
    }

    demo_banner_tick();

    /* 页面巡览：导航/骑行/暂停/主题几步把主界面各页轮一遍。 */
    demo_tour_tick();

    /* 新增两步自己的节拍（暂停/继续、偏航重规划）。 */
    if (s_step == STEP_PAUSE) {
        demo_pause_tick();
    } else if (s_step == STEP_REROUTE) {
        demo_reroute_tick();
    }

    if (s_step == STEP_NATION) {
        demo_nation_announce();       /* 切换前点名（自己带横幅计时） */
    }

    /* 第 ③ 步：**规划完毕才开始计时**（用户要求）。规划的 route worker 是异步的，
     * 期间把本步起点一直往后挪 = 计时不走；规划一落地就开始数那 30 秒。
     * 上限 `BICYCLE_DEMO_PLAN_WAIT_MAX_MS`：规划卡住时别把演示挂在这儿。 */
    if (s_step == STEP_PLAN_NAV) {
        page = lvgl_page_map();
        if (page != NULL && map_page_nav_planning(page)
            && lv_tick_elaps(s_step_ms) < BICYCLE_DEMO_PLAN_WAIT_MAX_MS) {
            s_step_ms = lv_tick_get();
        } else if (page != NULL && !s_plan_logged && map_page_nav_active(page)) {
            /* 规划落地：打一行"路线就绪 + 点数"，配 `[nav] paint: seen=1 seg=…/N` 就是
             * "规划线真的画出来了"的判据（用户点名要"规划轨迹要绘制"）。 */
            uint32_t pts = 0;
            uint32_t total_cm = 0;
            uint32_t now_idx = 0;

            (void)map_page_nav_progress(page, &pts, &total_cm, &now_idx, 0);
            s_plan_logged = true;
            map_page_set_arrow_visible(page, true);
            syslog(LOG_INFO, "demo: step2 route ready pts=%u len=%.2fkm arrow=on\n",
                (unsigned)pts, (double)total_cm / 100000.0);
        }
    }

    elapsed = lv_tick_elaps(s_step_ms);
    if (elapsed < s_step_len_ms) {
        /* 导航段提前收工：到终点了、规划失败了、被手动停了 —— 都没必要把
         * 剩下的时间空耗掉（但给个下限，免得刚起来那两拍规划还没落地就翻页）。 */
        if ((s_step == STEP_GPX_NAV || s_step == STEP_PLAN_NAV)
            && elapsed >= 5000u) {
            page = lvgl_page_map();
            if (page != NULL && !map_page_nav_active(page)
                && !map_page_nav_planning(page)) {
                syslog(LOG_INFO, "demo: nav step %d ended early at %us\n",
                    (int)s_step, (unsigned)(elapsed / 1000u));
            } else {
                return;
            }
        } else {
            return;
        }
    }

    if (s_step == STEP_THEME) {
#if BICYCLE_DEMO_LOOP
        s_cycle++;
        syslog(LOG_INFO, "demo: cycle %u done, back to step1\n", (unsigned)s_cycle);
        demo_goto((demo_step_t)k_demo_next[STEP_THEME]);
#else
        syslog(LOG_NOTICE, "demo: cycle done, stop\n");
        bicycle_demo_stop();
#endif
        return;
    }

    demo_goto((demo_step_t)k_demo_next[s_step]);
}
