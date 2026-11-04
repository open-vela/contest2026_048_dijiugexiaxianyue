/**
 * @file bicycle_demo.h
 * @brief 自行车 UI — 演示模式（展会/拍摄：一遍跑完各路"看点"）。
 *
 * 一遍 = 七步，跑完回到第一步循环，直到手动关掉：
 *
 * | # | 屏幕中间横幅 | 干什么 |
 * |---|--------------|--------|
 * | 1 | 模拟 GPX 导航   | 回放一条本机 GPX 当定位，再开**这段路的 GPX 跟线导航** |
 * | 2 | 导航结束        | 把上一条导航/回放/演示骑行收干净 |
 * | 3 | 模拟坐标点导航  | 同一条 GPX 回放（把定位挪回轨迹起点），再**规划到固定坐标点**并模拟行驶 |
 * | 4 | 暂停 / 继续     | 走真实**中键长按**路径暂停（录制暂停 + 假 GPS 冻结、速度归 0），再走真实继续路径 |
 * | 5 | 城区偏航重规划  | 北京城区坐标点导航 + 故意绕路的 `北京.gpx` 当假 GPS ⇒ 真判定偏航 ⇒ 真 reroute 画新线 |
 * | 6 | 全国地图解析    | 结束导航，在全国几个风景点上跳（每个 10 秒） |
 * | 7 | 主题切换        | 切到另一套主题（日光 ↔ 夜间） |
 *
 * @section 横幅
 * 提示挂在 **LVGL top layer**（`lv_layer_top()`）上：盖住地图页/菜单/导航横幅，
 * 且**只在演示期间存在**，关掉就删。它**不是**通知横幅（`lv_pm_notify_show`）——
 * 演示期间不该被"来电话/收通知"那类东西挤掉位置。
 * 每次露脸 `BICYCLE_DEMO_BAN_MS`（默认 2 秒）就自己收掉：横幅是"报户口"用的，
 * 不该一直压着地图。
 *
 * @section 怎么用
 * · 菜单：设置 → 系统 → **演示模式**（开关）
 * · 控制台：`bicycle_nsh demo on|off|status`
 * · 循环模式：`bicycle_nsh demo start random|linear|minghu|full`（与全量循环并列）
 * · 单场景：`bicycle_nsh demo run <名字>` 直接进某个 UI 状态（`demo list` 看全表），
 *   `demo next|prev` 按场景表顺序逐个切；`demo off` 退出并恢复主题。
 *
 * 单场景与全量循环互斥：进单场景会先停全量循环，开全量循环会先退单场景。
 *
 * @note 演示只碰"当前这一次会话"：回放/跳点都是合成定位，不写 GNSS；
 *       演示骑行结束时不落盘（会丢弃未保存的 GPX）；主题在停止时还原成开演示前那套。
 */

#ifndef BICYCLE_DEMO_H
#define BICYCLE_DEMO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name 四步各自的料（改这几个就够了）
 * @{
 */
/** @brief 第 1、3 步：每条导航演多久（毫秒；提前到达/结束会提前进下一步）。 */
#define BICYCLE_DEMO_NAV_MS        30000u
/** @brief ①→② 之间「导航结束」那一步停多久（毫秒）—— 用来把上一个导航彻底清掉。 */
#define BICYCLE_DEMO_NAV_END_MS    2000u
/**
 * @brief 第 3 步等"规划完毕"的上限（毫秒）。
 * @note 用户 2026-09-29："第二段最好在规划完毕后开始计算时间" —— 规划是异步的
 *       （route worker），几百毫秒到几秒不等；这期间计时**不走**（把起点一直往后挪），
 *       规划落地后才开始数那 `BICYCLE_DEMO_NAV_MS`。给个上限免得规划卡住把演示挂住。
 */
#define BICYCLE_DEMO_PLAN_WAIT_MAX_MS 15000u
/** @brief 第 3 步：每个点停多久（毫秒；用户定的 10 秒 = 10000）。 */
#define BICYCLE_DEMO_HOP_PERIOD_MS 10000u
/** @brief 第 4 步：换完主题停多久（毫秒）。 */
#define BICYCLE_DEMO_THEME_MS      15000u
/** @brief 横幅每次露脸多久（毫秒）——到点就自己收掉，别一直压着地图。 */
#define BICYCLE_DEMO_BAN_MS        2000u
/**
 * @brief 第 3 步**切换前**提前多久报下一个点的名字（毫秒）。
 * @note 用户要求"每个点切换前需要告诉是什么位置"：跳点是在 2 km 窗口里看地图，
 *       不点名观众不知道跳到哪了 —— 提前报，地图再跳。
 */
#define BICYCLE_DEMO_ANNOUNCE_MS   1500u
/** @brief 跑完第 4 步回到第 1 步继续（0 = 一遍就停）。展会演示要一直循环。 */
#define BICYCLE_DEMO_LOOP          1

/** @brief 第 1、2 步回放的那条轨迹（App 从 `import/` 导进来的骑行文件）。 */
#define BICYCLE_DEMO_GPX     "/mnt/lfs/mtp/import/20260529户外骑行.gpx"
/** @brief 第 2 步"坐标点导航"的目的地。 */
#define BICYCLE_DEMO_PLAN_LON 118.3762928
#define BICYCLE_DEMO_PLAN_LAT 32.2458731

/* ── 新增两步：暂停/继续、城区偏航重规划（2026-11-03，用户点名要补） ── */
/** @brief 暂停步：先正常骑多久再按"中键"（毫秒）。 */
#define BICYCLE_DEMO_PAUSE_AT_MS    6000u
/**
 * @brief 暂停步：暂停状态停多久再继续（毫秒）；期间回放冻结、速度 0。
 * @note 必须 > 4 s：`bicycle_runtime` 的 SOG 丢弃保持 `RT_SOG_HOLD_MS` = 4000 ms，
 *       速度要 4 s 后才开始往下走（再加 ~1.6 s 降到 0）。留 10 s 才看得到"0 km/h"
 *       这一段；太短的话横幅刚弹出来就继续了，观众看不出差别。
 */
#define BICYCLE_DEMO_PAUSE_HOLD_MS  10000u
/** @brief 暂停步总时长（毫秒）。 */
#define BICYCLE_DEMO_PAUSE_MS       (BICYCLE_DEMO_PAUSE_AT_MS \
    + BICYCLE_DEMO_PAUSE_HOLD_MS + 4000u)
/** @brief 偏航步总时长（毫秒）。 */
#define BICYCLE_DEMO_REROUTE_MS     34000u
/**
 * @brief 偏航步：坐标点导航就绪后，多久还没判出偏航就收工（毫秒）。
 * @note GPX 的绕路段在 ~300 m 处，45 km/h 大约 20 s 才骑到，再加 3 s 偏航确认
 *       ⇒ 实测 reroute 在骑起来 ~20 s 才触发。给 26 s 兜底（别再用 18 s，
 *       那样会在触发前一拍放弃；2026-11-03 上板实测）。
 */
#define BICYCLE_DEMO_REROUTE_WAIT_MS 26000u
/**
 * @brief 偏航步：假 GPS 回放起来后，等地图跟到目标城区再提交规划（毫秒）。
 * @note **必须等**：`map_page_nav_plan()` 用当前显示区域的路由图；地图还在上一站
 *       （滁州）时规划北京会拿错图 —— 实测点被吸到 869 km 外、`both ends off-road`。
 */
#define BICYCLE_DEMO_REROUTE_SETTLE_MS 6000u
/**
 * @brief 偏航步：假 GPS 回放的城区轨迹（用户 2026-11-03 用 gpx.studio 生成）。
 * @note 点故意稀疏、在城市里绕了一段路 ⇒ 和规划路线不一致，自然触发偏航。
 *       文件同时放在 `mkfs/lfs/mtp/import/`，随 LFS 镜像烧录。
 */
#define BICYCLE_DEMO_REROUTE_GPX "/mnt/lfs/mtp/import/北京.gpx"
/** @brief 偏航步：坐标点导航起点（北京，用户 2026-11-03 指定）。 */
#define BICYCLE_DEMO_REROUTE_FROM_LON 116.350961
#define BICYCLE_DEMO_REROUTE_FROM_LAT 39.919940
/** @brief 偏航步：坐标点导航终点。 */
#define BICYCLE_DEMO_REROUTE_TO_LON   116.358281
#define BICYCLE_DEMO_REROUTE_TO_LAT   39.916015
/** @} */

/**
 * @name 三种循环演示的料（与全量四步并列，由 `bicycle_demo_start_mode()` 进）
 * @{
 */
/** @brief random：全国安全点之间跳一次的周期（毫秒）。 */
#define BICYCLE_DEMO_RANDOM_PERIOD_MS  8000u
/**
 * @brief random：进入后先让地图页画稳，再交给跳点引擎（毫秒）。
 *
 * @note 冷机（开机 3~4 s 就 `demo start random`）时，地图页首帧的瓦片还在加载，
 *       紧接着跳到上千公里外又会整帧重画；两段叠在同一个 UI 拍里会超过看门狗
 *       10 s 的 UI 心跳门限（实测 11.5 s ⇒ WDT 复位）。先只显示地图页（横幅报
 *       “准备”），等这么久再起跳，让首帧先落地。热机时同样等这一会儿，
 *       用户看到的是“地图先出来、再开始跳”，不会觉得卡住。
 */
#define BICYCLE_DEMO_RANDOM_WARMUP_MS  6000u
/** @brief linear：向东直线巡航的长度（km）。 */
#define BICYCLE_DEMO_LINEAR_KM         30.0
/** @brief linear：巡航速度（km/h）—— 想看得慢一点就改这里。
 *
 * 2026-10-28 用户定：演示模式按 **40 km/h** 跑（原来 120 只是为了"看起来快"，
 * 但那既不像骑行、也让 rebase 帧密到 8 s 一次，跟真实使用差太远）。
 * 30 km 的直线因此约 45 min 一圈。 */
#define BICYCLE_DEMO_LINEAR_KPH        40.0f
/** @brief minghu：滁州明湖中心经度。 */
#define BICYCLE_DEMO_MINGHU_LON        118.329
/** @brief minghu：滁州明湖中心纬度。 */
#define BICYCLE_DEMO_MINGHU_LAT        32.212
/** @brief minghu：环线半径（米）。 */
#define BICYCLE_DEMO_MINGHU_RADIUS_M   1600.0
/** @brief minghu：绕圈速度（km/h）。 */
#define BICYCLE_DEMO_MINGHU_KPH        30.0f
/** @brief linear/minghu 到终点后停顿多久再重开（毫秒）。 */
#define BICYCLE_DEMO_LOOP_PAUSE_MS     2000u
/** @} */

/** @brief `bicycle_demo_start()` 的返回值。 */
enum {
    BICYCLE_DEMO_OK = 0,
    /** @brief 地图页还没建好（开机没走完 / 工厂模式）。 */
    BICYCLE_DEMO_ERR_NO_MAP = -1,
    /** @brief 导航起不来（轨迹打不开、路线规划没起来）。 */
    BICYCLE_DEMO_ERR_NAV = -2,
    /** @brief 正在记录一次真实骑行：演示会把它搅掉，拒绝。 */
    BICYCLE_DEMO_ERR_RIDING = -3,
    /** @brief 模式编号非法（越界）。 */
    BICYCLE_DEMO_ERR_MODE = -4,
};

/**
 * @name 循环演示模式
 * @{
 */
/**
 * @brief 循环演示的三种新增模式 + 现有四步全量。
 * @note `BICYCLE_DEMO_MODE_FULL` 就是 `bicycle_demo_start()` 那套四步循环；
 *       其余三种由 `bicycle_demo_start_mode()` 进入，互斥，可随时 `bicycle_demo_stop()`。
 */
typedef enum {
    BICYCLE_DEMO_MODE_FULL = 0,   /* 现有四步全量 */
    BICYCLE_DEMO_MODE_RANDOM,     /* 全国安全点随机跳转 */
    BICYCLE_DEMO_MODE_LINEAR,     /* 30 km 向东直线巡航 */
    BICYCLE_DEMO_MODE_MINGHU,     /* 绕滁州明湖环线 */
    BICYCLE_DEMO_MODE_COUNT
} bicycle_demo_mode_t;

/**
 * @brief 进入一个循环演示模式。
 * @param mode 模式编号；FULL 直接走 `bicycle_demo_start()`。
 * @return #BICYCLE_DEMO_OK；负值为 #BICYCLE_DEMO_ERR_*。
 * @note 会先停掉正在跑的全量/单场景/上一个模式。菜单不在这里收，由调用方收页。
 */
int bicycle_demo_start_mode(bicycle_demo_mode_t mode);
/**
 * @brief 模式名解析（"full"/"random"/"linear"/"minghu"，大小写不敏感）。
 * @return 模式编号；不认识为 -1。
 */
int bicycle_demo_mode_from_name(const char * name);
/** @brief 模式名（状态槽用）；越界返回 NULL。 */
const char * bicycle_demo_mode_name(bicycle_demo_mode_t mode);
/** @brief 模式的中文说明（菜单/状态用）；越界返回 NULL。 */
const char * bicycle_demo_mode_desc(bicycle_demo_mode_t mode);
/** @brief 模式表长度（含 FULL）。 */
unsigned bicycle_demo_mode_count(void);
/** @brief 当前循环演示模式；不在循环演示（关着/单场景）时返回 -1。 */
bicycle_demo_mode_t bicycle_demo_current_mode(void);
/** @} */

/**
 * @brief 开演示模式（四步全量）。
 * @return #BICYCLE_DEMO_OK（含"本来就在跑"）；负值为上面的失败原因。
 * @note 菜单开着时会顺手把菜单收起（演示要看地图，与"骑行记录 → 导航"同一个姿势）。
 * @note 正在跑某个新增循环模式时，会先停掉它再进四步全量。
 */
int bicycle_demo_start(void);
/**
 * @brief 关演示模式：停回放/跳点/路线模拟、收掉演示骑行（未保存的 GPX 丢弃）、
 *        删掉演示横幅、把主题与地图缩放恢复成开演示之前的样子。
 * @note 全量四步、三种新增循环模式、单场景都能用这一个入口停掉。
 */
void bicycle_demo_stop(void);
/** @brief 已开则停、未开则开（菜单那一行用）。 */
void bicycle_demo_toggle(void);
/** @brief 是否在演示中。 */
bool bicycle_demo_active(void);

/**
 * @brief 当前这一步的横幅文案（菜单副标题 / `bicycle_nsh demo status` 用）。
 * @return 静态缓冲；未开时给的是这一行"没开"时该显示的说明文案。
 */
const char * bicycle_demo_status(void);
/**
 * @brief 失败原因文案（通知横幅）。
 */
const char * bicycle_demo_error_text(int rc);

/**
 * @brief UI 主循环每圈调用（循环顶，与 `bicycle_ui_ctl_poll()` 同处）。
 * @note 未开演示时是空操作；开着时只在某一步到点时推进一步。
 */
void bicycle_demo_poll(void);

/**
 * @name 单场景模式
 * 终端用 `bicycle_nsh demo run <名字>` 直接展示某个 UI 状态（展会/拍照单点取景）。
 * 场景表顺序固定（next/prev 按它走），名字大小写不敏感，纯数字按索引。
 * @note 场景表是静态只读的，NSH 任务也会读（`demo list`），不要在运行期改。
 * @{
 */
/** @brief 场景名不认识 / 纯数字越界。 */
#define BICYCLE_DEMO_SCENE_ERR_UNKNOWN (-1)
/** @brief 场景进入失败（没有地图 / GPX，页面锁建不起来等）。 */
#define BICYCLE_DEMO_SCENE_ERR_ENTER   (-2)

/** @brief 场景表长度。 */
unsigned bicycle_demo_scene_count(void);
/** @brief 表内第 index 个场景名；越界返回 NULL。 */
const char * bicycle_demo_scene_name(unsigned index);
/** @brief 表内第 index 个场景的中文说明（列表/横幅用）；越界返回 NULL。 */
const char * bicycle_demo_scene_desc(unsigned index);
/** @brief 当前正在跑的单场景名；不在单场景时返回 NULL。 */
const char * bicycle_demo_scene_current(void);
/** @brief 当前单场景的中文说明；不在单场景时返回 NULL。 */
const char * bicycle_demo_scene_current_desc(void);
/** @brief 是否处于单场景模式。 */
bool bicycle_demo_scene_active(void);

/**
 * @brief 进入一个单场景（会先停四步循环/上一个场景）。
 * @param name 场景名（大小写不敏感）或纯十进制下标。
 * @return 0 成功；#BICYCLE_DEMO_SCENE_ERR_UNKNOWN 名字不认识；
 *         #BICYCLE_DEMO_SCENE_ERR_ENTER 进入失败（已回滚到无场景）。
 */
int bicycle_demo_scene_run(const char * name);
/**
 * @brief 按场景表切到下一个/上一个（首尾循环）。
 * @param dir +1 下一个；-1 上一个；0 按下一个处理。
 * @return 同 bicycle_demo_scene_run()。
 */
int bicycle_demo_scene_step(int dir);
/** @} */

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_DEMO_H */
