/**
 * @file vmap_view.h
 * @brief retained RGB565 地图视图：瓦片泵、跟车平移、导航/REC 叠层。
 *
 * 地图叠层（底 → 顶），跟车平移时整幅一起挪，避免各层错位：
 *   1. 地图瓦片（RGB565 画布最底层）
 *   2. 导航线（盖进画布）
 *   3. 道路/地名标签（盖进画布，不再用 LVGL overlay）
 *   4. REC 轨迹线 + 公里圆标（盖进画布）
 *   5. REC 公里数字（盖进画布，最顶的栅格）
 * LVGL 定位箭头在画布之上；Helm 状态栏/仪表在地图页之上。
 */

#ifndef VMAP_VIEW_H
#define VMAP_VIEW_H

#include "lvgl/lvgl.h"
#include "vmap_config.h"
#include "vmap_render.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vmap_view vmap_view_t;

/**
 * @brief 开机地图中心：合法上次骑行终点，否则北京天安门。
 */
void vmap_boot_lonlat(double *lon, double *lat);
/**
 * @brief 上次定位是否可作开机中心（有限、非原点、在全国图范围内）。
 */
bool vmap_boot_ll_ok(double lon, double lat);

/**
 * @brief 创建带 overscan 的 RGB565 大画布。
 * @param parent 地图视口（通常是 `map_area`）。
 * @param view_w 可视宽。
 * @param view_h 可视高。
 * @param margin 四周 overscan（像素）。
 * @return 视图；失败为 NULL。
 * @details 缓冲区先 `memset` 0；canvas 隐藏，直到第一帧 `render_finish`
 *          才显示，避免把未初始化的 PSRAM 送到 LCD。
 */
vmap_view_t * vmap_view_create(lv_obj_t * parent, int32_t view_w, int32_t view_h,
    int32_t margin);
/**
 * @brief 销毁视图：释放画布、瓦片缓存与箭头绑定。
 */
void vmap_view_destroy(vmap_view_t * view);

/* ══════════════════════════════════════════════════════════════════════════
 * 组件化 API（运行时可配置：窗口尺寸 / 中心点 / 中心图标 / 平移 / 缩放）
 *
 * 这组入口把"地图"从页面内部的渲染器变成可独立配置的控件：所有几何参数
 * 都可以在 create 之后改，不需要重建视图，也不需要重启渲染泵。
 * 旧入口（set_view / set_scale / set_center / set_arrow_image ...）保持可编译、
 * 调用点不用改。语义变化与新增项见证据包：
 *   WP-VMAPAPI/R1（组件化第一版）、**WP-VMAPC/R1**（自足组件：set_viewport 连父
 *   容器一起改、亚像素拖动、zoom_at 锚点缩放、换图后保持朝向）。
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 运行时改变可视窗口尺寸（设备像素）——**组件自足**，调用方只调这一次。
 *
 * 内部会重建 RGB565 画布（`w + 2*margin` × `h + 2*margin`）、航向朝上用的旋转
 * 缓冲 R、LVGL canvas 绑定、标签收集器视口几何与脏区记账；**父容器**（create 时
 * 传入的那个 object）的尺寸也一并跟着改（见 `vmap_view_set_parent_autosize`），
 * 因此应用层不需要再去 `lv_obj_set_size()` 一次。箭头层若就是父容器，图标会被
 * 重新居中。
 *
 * 中心经纬度、zoom、scale、follow 状态保持不变；已渲染过的视图会自动发起一次
 * 整幅重绘。任何一步分配失败都不改变现视图（先分配后提交）。
 *
 * @param w 新可视宽（> 0）。
 * @param h 新可视高（> 0）。
 * @return true 成功；false 参数非法或缓冲分配失败（此时保持原尺寸不变）。
 */
bool vmap_view_set_viewport(vmap_view_t * view, int32_t w, int32_t h);
/**
 * @brief 是否让 `set_viewport()` 顺带改父容器尺寸（默认：create 时父容器尺寸
 *        与视口一致就自动开，否则关）。
 * @note 父容器是页面布局的一部分时（例如地图只占页面的一块），把它关掉，
 *       由页面自己决定容器几何；此时 `set_viewport()` 只动组件内部。
 */
void vmap_view_set_parent_autosize(vmap_view_t * view, bool enable);
/** @brief 父容器是否随 `set_viewport()` 改尺寸。 */
bool vmap_view_get_parent_autosize(const vmap_view_t * view);
/** @brief 当前可视窗口尺寸（不含 overscan margin）。 */
void vmap_view_get_viewport(const vmap_view_t * view, int32_t * w, int32_t * h);
/** @brief 当前 overscan 余量（画布每边比视口多出的像素）。 */
int32_t vmap_view_get_margin(const vmap_view_t * view);
/** @brief 当前跟车/拖动平移量（设备像素，画布相对视口的位移）。 */
void vmap_view_get_pan(const vmap_view_t * view, int32_t * dx, int32_t * dy);

/**
 * @brief 直接定位：把地图中心设到 (lon, lat) 并清掉跟车偏移。
 *
 * 与 `vmap_view_set_center()`（跟车：软平移 / 越界重锚）不同，这是"跳转"语义：
 * 不保留旧像素、不滚动，下一次渲染整幅重画。已渲染过的视图会立即开一帧。
 */
void vmap_view_center_on(vmap_view_t * view, double lon, double lat);

/**
 * @brief 设置中心图标（视口正中那个定位/箭头图标）。
 * @param src NULL = 不画（隐藏箭头对象）；非 NULL = lv_img 图源
 *            （`lv_image_dsc_t*`、图片路径或 `LV_SYMBOL_*`）。
 * @note 兼容旧入口 `vmap_view_set_arrow_image()`；箭头层未绑定时先记账，
 *       `vmap_view_bind_arrow_layer()` 时补建。
 */
void vmap_view_set_center_icon(vmap_view_t * view, const void * src);
/** @brief 当前中心图标图源；NULL = 不画。 */
const void * vmap_view_get_center_icon(const vmap_view_t * view);

/**
 * @brief 开始一次拖动：打断跟车（箭头不再钉在视口中心）。
 *
 * 打断后中心点保持不动，直到显式 `vmap_view_set_follow_mode(view, true)`
 * 或 `vmap_view_center_on()`；**不会**在拖动结束后自动弹回。
 */
void vmap_view_pan_begin(vmap_view_t * view);
/**
 * @brief 像素级平移：把地图内容移动 (dx, dy) 像素（屏幕坐标，右/下为正）。
 *
 * **亚像素累积**：请求量以 double 记账（`pan_rx/pan_ry`），只有真正上屏的那一步
 * 才取整；因此 `pan_px(+200,0)` 的地物位移就是 200 px（不再因为"1 map px =
 * 1.69 screen px"的取整变成 199），连续拖 100 次也不会累积误差。
 *
 * 内部换算成中心点位移（`中心 -= (dx,dy)/scale`）后走跟车同一条路径：
 * 未越 overscan 只挪画布；越界自动重锚并分片重画——重锚时按**当前屏上整数位移**
 * 滚动已有像素，余下的小数部分留在 `pan_rx` 里，不会把 1 px 误差吃进新原点。
 * 因此拖动不会引入新的定时器，也不会整屏清空。
 */
void vmap_view_pan_px(vmap_view_t * view, int32_t dx, int32_t dy);
/**
 * @brief 结束拖动：保留当前中心与平移，不做任何回弹。
 */
void vmap_view_pan_end(vmap_view_t * view);
/** @brief 是否处于跟车（箭头钉在视口中心）状态。 */
bool vmap_view_is_following(const vmap_view_t * view);
/**
 * @brief 底层 LVGL canvas 对象（retained RGB565）。
 */
lv_obj_t * vmap_view_get_obj(const vmap_view_t * view);

/**
 * @brief 设置地图目录（网格 `.vpk` 根，如 `/mnt/fat/map`）。
 */
void vmap_view_set_tile_dir(vmap_view_t * view, const char * dir);
/**
 * @brief 设置中心与缩放。不打开 .vpk；render_pump 第一拍再 ensure_region。
 */
void vmap_view_set_view(vmap_view_t * view, double lon, double lat, int zoom);
/**
 * @brief 打开当前经纬度所在 cell 的 region pack。
 * @return 已就绪或打开成功为 true。
 */
bool vmap_view_ensure_region(vmap_view_t * view, double lon, double lat);
struct vmap_tile_cache;
/** @brief 解析瓦片 LRU / region slot 缓存。 */
struct vmap_tile_cache * vmap_view_tile_cache(vmap_view_t * view);
/** @brief 当前瓦片 zoom（整数级）。 */
int vmap_view_get_zoom(const vmap_view_t * view);
/** @brief 当前显示倍率（相对瓦片像素）。 */
double vmap_view_get_scale(const vmap_view_t * view);
/**
 * @brief 当前跟车中心（经纬度）。
 */
void vmap_view_get_center(const vmap_view_t * view, double * lon, double * lat);
/**
 * @brief 改显示倍率；必要时触发重绘。
 */
void vmap_view_set_scale(vmap_view_t * view, double scale);
/**
 * @brief **锚点缩放**：以视口内 (sx, sy) 为锚，把倍率乘上 @p factor。
 *
 * 缩放后锚点下的那块地理内容仍停在 (sx, sy)（误差 ≤1 px，受屏幕整数量化限制），
 * 也就是说手指/光标按住的那一点不会跑。瓦片层级不变（仍是 z14），只改连续倍率。
 *
 * @param sx 锚点 x（视口坐标，0..view_w-1）。
 * @param sy 锚点 y（视口坐标，0..view_h-1）。
 * @param factor >1 放大，<1 缩小；结果夹在 0.25–8.0。
 * @return true 已应用（会立即重画一帧）；false 参数非法或缩放前后没有可测变化。
 * @note 锚点不在视口正中时**会打断跟随**（跟随要求车辆钉在中心，两者矛盾）；
 *       锚点就在中心（±1 px）则保持跟随，等价于普通中心缩放。
 * @note 与 `vmap_view_geo_to_viewport()` 同一坐标系（不含 `y_bias`）。
 */
bool vmap_view_zoom_at(vmap_view_t * view, int32_t sx, int32_t sy, double factor);
/**
 * @brief 视口像素 → 经纬度（`vmap_view_geo_to_viewport()` 的逆）。
 * @return true 已换算；false 参数非法/视图未初始化。
 * @note 不含 `y_bias`（与 `geo_to_viewport` 对称）。
 */
bool vmap_view_viewport_to_geo(const vmap_view_t * view, int32_t x, int32_t y,
    double * lon, double * lat);
/**
 * @brief 显示偏置：把画布内容与箭头一起沿 y 平移多少像素（导航横幅盖住顶部时用）。
 *
 * **只作用于显示位置**，不进跟车/rebase 的数学（pan_* 与投影原点一律不变）。
 * 取值为「需要让出来的那块高度的一半」：通栏导航横幅即 HELM_NAV_BAN_H/2。
 */
void vmap_view_set_y_bias(vmap_view_t * view, int32_t bias);
/**
 * @brief 路名标签字体。
 */
void vmap_view_set_font(vmap_view_t * view, const lv_font_t * font);

/**
 * @brief 航向朝上：地图跟随骑行方向旋转、定位箭头固定指上。
 * @param enable true 开启。旋转由 LVGL 交给 EPIC 的 transform 通路完成（画布仍按
 *               正北向上渲染，不重光栅化、不加中间缓冲）。
 * @note 该模式下**不烘焙路名标签**（文字会随地图转成斜字）；导航线/轨迹/公里标照常。
 *       创建后立即调用是安全的：首帧之前只是记录状态，不会提前开帧。
 */
void vmap_view_set_heading_up(vmap_view_t * view, bool enable);
/** @brief 航向朝上是否开启。 */
bool vmap_view_heading_up(const vmap_view_t * view);
/**
 * @brief 更新航向（0–360，正北 0，顺时针）与当前速度。
 * @note 只在 `heading_up` 时生效。首帧航向无条件接受（否则停车时箭头指上、地图却是
 *       正北，两者矛盾）；之后低于 `VMAP_ROT_MIN_KPH` 时保持上次朝向（GPS course
 *       在低速下是噪声）。内部做圆周低通，且只有相对已应用角度变化超过
 *       `VMAP_ROT_STEP_10` 时才重设旋转。
 */
void vmap_view_set_heading(vmap_view_t * view, float heading_deg, float speed_kph);

/**
 * @brief 航向朝上时是否也烘焙路名标签。
 * @param enable true 画（文字会随地图转成斜字，仅用于 A/B 与观感对比）。
 * @note 标签烘在正北画布 C 里，所以切换会触发一次整幅重画。
 */
void vmap_view_set_rot_labels(vmap_view_t * view, bool enable);
/**
 * @brief 开始一帧栅格化。跟车重锚时平移已有像素、只补新边，避免整屏清空。
 *        由 render_pump 分瓦片、限时切片画完。
 * @note MTP quiesce 时直接返回。boot_deferred 期间不要在主题回调里调用，
 *       留给 Startup warmup 的 `set_view` + `render`。
 */
void vmap_view_render(vmap_view_t * view);

/**
 * @brief 按当前瓦片原点重画，不改 follow pan、不清空画布。
 *
 * 用于擦掉已经画进 canvas 的轨迹，避免开/停 REC 时地图跳回中心。
 */
void vmap_view_redraw_keep_pan(vmap_view_t * view);

/**
 * @brief 在已有底图上按叠层盖导航线，再盖 REC 线/数字。
 * @details 不含标签：进度刷新若重描全部路名会把字描胖。完整叠层只在瓦片结束帧做。
 */
void vmap_view_stamp_nav_then_track(vmap_view_t * view);

/**
 * @brief 一次只装/画若干瓦片，并受时间预算限制。phase==0 时视为空闲。
 * @return 本帧已画完或空闲则为 true。
 */
bool vmap_view_render_pump(vmap_view_t * view, unsigned max_tiles);

/**
 * @brief 是否正在分片绘制一帧。
 */
bool vmap_view_render_busy(const vmap_view_t * view);

/**
 * @brief 跟车：改中心经纬度。未越 overscan 只平移 canvas；越界才 rebase。
 * @note 开机 `map_ui_seen==false` 时页面层禁止调用，以免提前开出半帧。
 */
void vmap_view_set_center(vmap_view_t * view, double lon, double lat);
/**
 * @brief 绑定定位箭头所在 LVGL 层（通常是 `map_area`）。
 */
void vmap_view_bind_arrow_layer(vmap_view_t * view, lv_obj_t * layer);
/**
 * @brief 设置箭头图源；跟随模式创建后立刻居中。
 */
void vmap_view_set_arrow_image(vmap_view_t * view, const void * src);
/**
 * @brief 跟随模式：箭头钉在视口中心，平移画布。
 */
void vmap_view_set_follow_mode(vmap_view_t * view, bool enable);
/**
 * @brief 按经纬度与航向摆箭头。
 */
void vmap_view_update_arrow(vmap_view_t * view, double lon, double lat, float course_deg);
/** @brief 显示或隐藏定位箭头（轨迹回放时关掉）。 */
void vmap_view_set_arrow_visible(vmap_view_t * view, bool visible);
/**
 * @brief 把箭头提到 map_area 最前（chrome 之下）。
 */
void vmap_view_bring_arrow_to_front(vmap_view_t * view);

/** @brief 丢弃瓦片索引/arena 并重绘（MTP 释放 LFS 后）。 */
void vmap_view_reload_storage(vmap_view_t * view);
/** @brief 关闭已打开的地图分片句柄；保留 PSRAM 瓦片缓存与画布。 */
void vmap_view_release_storage(vmap_view_t * view);

/**
 * @brief 经纬度 → 视口像素（含 follow pan）。
 * @return 在视口（含少量 pad）内为 true。
 */
bool vmap_view_geo_to_viewport(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y);
/**
 * @brief 经纬度 → 视口像素，不做可见性裁剪。
 */
bool vmap_view_geo_to_viewport_raw(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y);
/**
 * @brief 经纬度 → 画布整数像素（相对 canvas 左上）。
 */
bool vmap_view_geo_to_canvas_raw(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y);
/**
 * @brief 经纬度 → 画布浮点像素。
 */
bool vmap_view_geo_to_canvas_f(const vmap_view_t * view, double lon, double lat,
    double * out_x, double * out_y);

/** @brief 缓存画布投影原点，避免轨迹扫描时每点重复算中心墨卡托。
 *  @note `base_x/base_y` 是**浮点** map 像素：重锚时保留亚像素余量，整数屏幕
 *        位移在投影里严格平移（拖动 200 px 就是 200 px）。 */
typedef struct {
    double base_x;
    double base_y;
    int zoom;
    double scale;
    double half_w;
    double half_h;
} vmap_view_proj_t;

/** @brief 从当前视图填投影缓存。 */
void vmap_view_proj_begin(const vmap_view_t * view, vmap_view_proj_t * proj);
/** @brief 用缓存把一点投到画布整数坐标。 */
void vmap_view_proj_point(const vmap_view_proj_t * proj, double lon, double lat,
    int32_t * out_x, int32_t * out_y);
/** @brief 用缓存把一点投到画布浮点坐标。 */
void vmap_view_proj_point_f(const vmap_view_proj_t * proj, double lon, double lat,
    double * out_x, double * out_y);

/**
 * @brief retained 画布宽高（含 overscan）。
 */
void vmap_view_get_buffer_size(const vmap_view_t * view, int32_t * w, int32_t * h);
/** @brief 画布坐标点可安全描边时为 true（避免色带填充伪影）。 */
bool vmap_view_canvas_point_sane(const vmap_view_t * view, int32_t x, int32_t y);
/**
 * @brief 画布点是否落在可视窗口内。
 */
bool vmap_view_canvas_point_visible(const vmap_view_t * view, int32_t x, int32_t y);
/**
 * @brief 画布点是否落在可视窗口外扩 @p pad 像素内。
 */
bool vmap_view_canvas_point_in_pad(const vmap_view_t * view, int32_t x, int32_t y,
    int32_t pad);
/**
 * @brief 取出当前 RGB565 画布描述。
 */
bool vmap_view_get_canvas(const vmap_view_t * view, vmap_canvas_t * canvas);
/**
 * @brief 仅刷新画布局部区域（坐标相对 canvas 左上角）。
 */
void vmap_view_invalidate_canvas_area(const vmap_view_t * view,
    int32_t x1, int32_t y1, int32_t x2, int32_t y2);
/**
 * @brief 地图存储世代（MTP reload 后递增）。
 */
uint32_t vmap_view_map_revision(const vmap_view_t * view);
/**
 * @brief 已完成的渲染帧计数。
 * @return 0 表示从未 `render_finish`；开机 splash 要等非 0 且 `!busy`。
 */
uint32_t vmap_view_render_revision(const vmap_view_t * view);

#if VMAP_ROUTE_ENABLE
struct vmap_route;
/**
 * @brief 把规划路线绑到本视图（导航线叠层）。
 */
void vmap_view_attach_route(vmap_view_t * view, struct vmap_route * route);
#endif

#ifdef __cplusplus
}
#endif

#endif /* VMAP_VIEW_H */
