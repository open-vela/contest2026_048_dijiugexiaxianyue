/**
 * @file myvendor_epic_blit.c
 * @brief EPIC 2D 搬运（拷贝 / 纯色填充）的板级封装。
 *
 * 为什么要有这一层 —— 都是被真实编译错误逼出来的：
 *   1. **芯片层头文件在 NSH 应用目标里不可见**：`bf0_hal_epic.h` 会拉进
 *      `register.h` / `bf0_hal_def.h`，而应用目标（如 `bicycle`）的 include
 *      路径里没有 `chips` 那一层（只有 `board` 目标有）⇒ 应用侧拿不到
 *      `EPIC_LayerConfigTypeDef` 这类类型；
 *   2. **LVGL 适配层的头文件同理**（`lv_sifli_epic_cfg.h` 也拉 `bf0_hal.h`）。
 *      所以这里按树里的既有做法（`ctl_main.c` / `sys_main.c` 用芯片层 API 时
 *      的处理）用**本地 extern** 声明那几个函数；签名照抄
 *      `apps/graphics/lvgl/lvgl/src/draw/sifli/epic/lv_sifli_epic_cfg.h`。
 *      ⚠ 那几个签名变了这里不会报错（没有编译期检查），改的时候一起改。
 *
 * 硬件约定都在这一层收敛（应用侧只认 RGB565 + 矩形）：
 *   - EPIC 是**独占硬件**，全树只有 `lv_sifli_epic_cfg.c` 里那一个 static
 *     `epic_handle`（`lv_epic_get_handle()`）。自己再建句柄会和 LVGL 自己的
 *     绘制路径抢同一堆寄存器，所以一律复用它；
 *   - 走 `lv_epic_blend()` / `lv_epic_fill()` 这两个包装：它们在起动前会
 *     等硬件空闲、卡住就复位、把 State 置回 READY，并 clean 一遍要读的区间；
 *   - 这两个包装底下是**同步**的（NuttX 上 `HAL_EPIC_BlendStartEx` /
 *     `HAL_EPIC_FillStart` 是 polling 版，内部 `EPIC_WaitDone()`），
 *     **返回时硬件必定已经停** —— 调用方不需要再等。
 */

#include <nuttx/config.h>
#include <nuttx/cache.h>
#include <nuttx/clock.h>

#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#include "bf0_hal_epic.h"
#include "myvendor_epic_blit.h"
#include <syslog.h>

/* 本地 extern：声明在 LVGL 侧，见文件头注释。 */
EPIC_HandleTypeDef * lv_epic_get_handle(void);
HAL_StatusTypeDef lv_epic_fill(EPIC_LayerConfigTypeDef * input_layers,
  uint8_t input_layer_cnt, EPIC_LayerConfigTypeDef * output_layer);
HAL_StatusTypeDef lv_epic_blend(EPIC_LayerConfigTypeDef * input_layers,
  uint8_t input_layer_cnt, EPIC_LayerConfigTypeDef * output_layer);
void lv_epic_wait(void);

/** @brief 行跨度换算成 EPIC 要的"像素数"（本产品画布固定 RGB565 = 2 B/px）。 */
#define MYVENDOR_EPIC_PX_PER_ROW(stride)   ((uint16_t)((stride) / sizeof(uint16_t)))

/*
 * RGB565 → RGB888 的**位复制**展开（EPIC 的填充色分量是 RGB888）。
 *
 * 位复制在"截断"与"四舍五入"两种硬件做法下都能精确回读原来的 5/6 位：
 *   r5=31 → r8=(31<<3)|(31>>2)=255 → 255>>3=31；四舍五入 31*31/255=31 ✓
 *   g6=63 → g8=(63<<2)|(63>>4)=255 → 255>>2=63；四舍五入 63*63/255=63 ✓
 * 而直接 `r5<<3`（低位补 0）在四舍五入的做法下会掉到 30，所以不用它。
 * ⚠ LVGL 的 `lv_color_to_epic_color()` 是把 5/6 位**原样**塞进 8 位字段的，
 * 与本文件约定不同；谁对由 vmap_blit.c 的开机自检（fill 用例）实测。
 */
static uint8_t epic_expand5(uint16_t v)
{
  return (uint8_t)((v << 3) | (v >> 2));
}

static uint8_t epic_expand6(uint16_t v)
{
  return (uint8_t)((v << 2) | (v >> 4));
}

static uint8_t * epic_at(void * buf, uint32_t stride, int32_t x, int32_t y)
{
  return (uint8_t *)buf + (size_t)y * stride + (size_t)x * sizeof(uint16_t);
}

bool myvendor_epic_ready(void)
{
  return lv_epic_get_handle() != NULL;
}

/**
 * @brief  EPIC 写完目的矩形后，把**被写到的那些行**从 CPU 缓存里作废。
 * @param  rect_tl 目的矩形左上角像素指针（本层约定：`out.data` 就是它）。
 * @param  stride   目的缓冲的行跨度（字节）。
 * @param  w, h     矩形宽高（像素）。
 *
 * @details 为什么必须做（2026-09-27，PSRAM 写回审计的 G1）：EPIC 是**绕过 CPU 缓存**
 *          直接写内存的 master。CPU 只要在那些像素上有缓存行（写过或读过就会留下），
 *          读回来就是**旧像素**。目的缓冲在 PSRAM（画布 0x60E00000）时这条一定成立 ——
 *          PSRAM 是 cacheable，且产品默认已是写回（`CONFIG_MYVENDOR_PSRAM_CACHE_WB=y`）。
 *
 * @warning ① **只作废矩形内的行**，不要图省事整层作废 —— 矩形外的像素 CPU 可能正握着刚
 *          写的数据（写回模式下是脏行），一并作废就是**丢数据**；
 * @warning ② 只在缓冲位于 PSRAM 时做（判据同芯片的 `IS_DCACHED_RAM`：地址 ≥ 0x60000000；
 *          SRAM 不在缓存范围内，做了纯属浪费）；
 * @warning ③ 依赖"包装返回时硬件必定已停"这个既有约定（见文件头：NuttX 上的
 *          `HAL_EPIC_*Start*` 是 polling 版）—— 换成 `_IT` 版就必须挪到回调里。
 */
static void epic_invalidate_rect_rows(const void * rect_tl, uint32_t stride,
  int32_t w, int32_t h)
{
  int32_t i;

  if ((uintptr_t)rect_tl < 0x60000000u || w <= 0 || h <= 0)
    {
      return;
    }

  for (i = 0; i < h; i++)
    {
      uintptr_t row = (uintptr_t)rect_tl + (uintptr_t)i * stride;

      up_invalidate_dcache(row, row + (uintptr_t)w * sizeof(uint16_t));
    }
}

/* EPIC 等待超时 ⇒ 本次当作**失败**返回：上层 vmap_blit 的 CPU 兜底会把这一块
 * 重做一遍，所以既不会卡死、也不会在半搬完的画布上留错像素（用户 2026-09-26）。 */
extern volatile uint32_t g_epic_wait_timeouts;

static bool epic_result(uint32_t wt_before, bool hal_ok, const char * what)
{
  if (g_epic_wait_timeouts != wt_before)
    {
      syslog(LOG_WARNING,
             "[vblit] EPIC 等待超时（%s；累计 %u 次）⇒ 本次退回 CPU 重做\n",
             what, (unsigned)g_epic_wait_timeouts);
      return false;
    }

  return hal_ok;
}

bool myvendor_epic_copy(uint16_t * dst_buf, uint32_t dst_stride, int32_t dx,
  int32_t dy, const uint16_t * src_buf, uint32_t src_stride, int32_t sx,
  int32_t sy, int32_t w, int32_t h)
{
  const uint32_t wt_before = g_epic_wait_timeouts;
  EPIC_LayerConfigTypeDef in;
  EPIC_LayerConfigTypeDef out;

  if (lv_epic_get_handle() == NULL)
    {
      return false;
    }

  /* HAL 明确要求这两个偏移非负（`EPIC_ConfigLayer()` 里判负即 HAL_ERROR）。 */
  if (sx < 0 || sy < 0 || dx < 0 || dy < 0)
    {
      return false;
    }

  /* 坐标/尺寸寄存器是 16 位，且上层另有溢出检查（EPIC_COORDINATES_MAX）。 */
  if (sx + w > EPIC_COORDINATES_MAX || sy + h > EPIC_COORDINATES_MAX
      || dx + w > EPIC_COORDINATES_MAX || dy + h > EPIC_COORDINATES_MAX)
    {
      return false;
    }

  /*
   * ⚠ 这里的摆法是**踩过坑**的，改动前先看这段：
   *
   * EPIC 的混合语义是"**图层内容留在它自己的画布位置**"：对画布点 (X,Y)，
   * 它读 `in.data[X - in.x_offset]` 写到 `out.data[X - out.x_offset]`
   * （见 `EPIC_ConfigOutputLayer()` 里 `offset = (y0 - output->y_offset) *
   * output->total_width + (x0 - output->x_offset)` 那一段）。
   * 也就是说：**平移必须体现在 data 指针上，不能体现在 x_offset/y_offset 上**
   * —— 后者会让内容停在原地（那就成了"原地混合"，不是搬运）。
   * 实测（2026-09-25 开机自检）：把位移放进 offset 时，九个方向**全部**与 CPU
   * 不一致（EPIC 只在两个矩形的交集里写，落点还差了一个位移）。
   *
   * 所以：
   *   - 两个 `data` 各自指向自己矩形的左上角像素（位移在这里）；
   *   - 两个 `x_offset/y_offset` **取同一个值**（用目的矩形的坐标），
   *     这样图层矩形与输出矩形完全重合 ⇒ 交集 = 整个矩形，
   *     EPIC 才会对每一列都读一次源；否则不重合的那部分会被 `continue` 掉
   *     （`EPIC_ConfigBlendEx()` 里"输入与输出不相交就跳过该层"）。
   *   - `total_width` = 各自的行跨度（像素），矩形尺寸另给。
   *   - 单输入层 + 不透明 alpha（`LayerConfigInit` 置 255）⇒ 逐位拷贝。
   */
  HAL_EPIC_LayerConfigInit(&in);
  in.data = epic_at((void *)src_buf, src_stride, sx, sy);
  in.color_mode = EPIC_COLOR_RGB565;
  in.width = (uint16_t)w;
  in.height = (uint16_t)h;
  in.x_offset = (int16_t)dx;
  in.y_offset = (int16_t)dy;
  in.total_width = MYVENDOR_EPIC_PX_PER_ROW(src_stride);

  HAL_EPIC_LayerConfigInit(&out);
  out.data = epic_at((void *)dst_buf, dst_stride, dx, dy);
  out.color_mode = EPIC_COLOR_RGB565;
  out.width = (uint16_t)w;
  out.height = (uint16_t)h;
  out.x_offset = (int16_t)dx;
  out.y_offset = (int16_t)dy;
  out.total_width = MYVENDOR_EPIC_PX_PER_ROW(dst_stride);

  bool ok = epic_result(wt_before, (lv_epic_blend(&in, 1, &out)) == HAL_OK, "copy");

  if (ok)
    {
      /* 目的侧作废（见 epic_invalidate_rect_rows 的说明）；out.data 就是矩形左上角
       * —— 这条摆放约定在 2026-09-25 的开机自检里钉过（见上面那段 ⚠ 注释）。 */
      epic_invalidate_rect_rows(out.data, dst_stride, w, h);
    }

  return ok;
}

bool myvendor_epic_fill(uint16_t * dst_buf, uint32_t dst_stride, int32_t x,
  int32_t y, int32_t w, int32_t h, uint16_t color565)
{
  const uint32_t wt_before = g_epic_wait_timeouts;
  EPIC_LayerConfigTypeDef out;

  if (lv_epic_get_handle() == NULL)
    {
      return false;
    }

  if (x < 0 || y < 0)
    {
      return false;
    }

  if (x + w > EPIC_COORDINATES_MAX || y + h > EPIC_COORDINATES_MAX)
    {
      return false;
    }

  HAL_EPIC_LayerConfigInit(&out);
  out.data = epic_at((void *)dst_buf, dst_stride, x, y);
  out.color_mode = EPIC_COLOR_RGB565;
  out.width = (uint16_t)w;
  out.height = (uint16_t)h;
  out.x_offset = (int16_t)x;
  out.y_offset = (int16_t)y;
  out.total_width = MYVENDOR_EPIC_PX_PER_ROW(dst_stride);
  out.color_en = true;
  out.color_r = epic_expand5((uint16_t)((color565 >> 11) & 0x1fu));
  out.color_g = epic_expand6((uint16_t)((color565 >> 5) & 0x3fu));
  out.color_b = epic_expand5((uint16_t)(color565 & 0x1fu));

  /* 输入层数 = 0 ⇒ `lv_epic_fill()` 走 HAL_EPIC_fill(NULL, 0, &out) == HAL_OK; 这条路径 */
  bool ok = epic_result(wt_before, (lv_epic_fill(NULL, 0, &out)) == HAL_OK, "fill");

  if (ok)
    {
      epic_invalidate_rect_rows(out.data, dst_stride, w, h);
    }

  return ok;
}

bool myvendor_epic_busy(void)
{
  EPIC_HandleTypeDef *epic = lv_epic_get_handle();

  if (epic == NULL || epic->Instance == NULL)
    {
      return false;
    }

  /* 只看 IA_BUSY：HAL 的 State 在 IRQ 没人收时可能比硬件"晚"停下
     （LVGL 适配层 `lv_epic_is_hardware_active()` 的注释就是这么说的）。 */
  return (epic->Instance->STATUS & EPIC_STATUS_IA_BUSY_Msk) != 0U;
}

void myvendor_epic_wait(void)
{
  /*
   * 这条路上其实没什么可等的（包装底下是同步 HAL）。留着它是为了守住
   * "返回前等到完成"这个约定，日后换成 _IT 版或别的引擎时不会漏。
   */
  lv_epic_wait();
}

/** @brief 旋转的实际实现（公开入口只负责计时探针，测完删）。 */
static bool myvendor_epic_rotate_impl(const uint16_t * src_buf,
  uint32_t src_stride, int32_t src_w, int32_t src_h, uint16_t * dst_buf,
  uint32_t dst_stride, int32_t dst_w, int32_t dst_h, int32_t angle10,
  int32_t pivot_x, int32_t pivot_y, int32_t dst_cx, int32_t dst_cy);

/**
 * @brief RGB565 整层旋转：把 `src` 绕 (pivot_x, pivot_y) 转 `angle10`（0.1°）后，
 *        让这个 pivot 落在 `dst` 的 (dst_cx, dst_cy)。
 *
 * @details 摆法照抄 LVGL 适配层（`lv_epic_setup_layers` + `lv_draw_sifli_epic_img`）：
 *          bg = 目的整层，fg = 源整层 + `transform_cfg`，两者在**同一个坐标空间**里，
 *          fg 的 `x_offset/y_offset` 决定"源的左上角摆在哪" ⇒ 让 pivot 落在
 *          (dst_cx,dst_cy) 就是 `offset = dst_c - pivot`。pivot 按 HAL 注释是
 *          "base on VideoLayer's origin"，即**源缓冲的像素坐标**。
 *
 * @note 同步（包装底下是 polling 版 HAL），返回时硬件已停。
 * @note 源先 clean、目的按行 invalidate（PSRAM 写回缓存的前提）。
 * @return true = 已执行；false = 两种：① 参数越界/硬件不可用，**什么都没做**；
 *         ② **执行超时**（`EPIC_WaitDone` 超时）—— 这一种 R 可能已被写了**一部分**，
 *         调用方必须把目的缓冲当成"脏的"，重新烘一次整幅，不能沿用上一帧。
 */
bool myvendor_epic_rotate(const uint16_t * src_buf, uint32_t src_stride,
  int32_t src_w, int32_t src_h, uint16_t * dst_buf, uint32_t dst_stride,
  int32_t dst_w, int32_t dst_h, int32_t angle10, int32_t pivot_x,
  int32_t pivot_y, int32_t dst_cx, int32_t dst_cy)
{
  if (!myvendor_epic_ready() || src_buf == NULL || dst_buf == NULL)
    {
      return false;
    }

  /* 【临时探针】旋转耗时统计（[vrot]，LOG_DEBUG，`ctl log all` 可见）。测完删。 */
  {
    static struct
    {
      uint32_t n;
      uint32_t sum;
      uint32_t max;
      uint32_t win;
    } pf;
    uint32_t t0 = clock_systime_ticks();
    bool r = myvendor_epic_rotate_impl(src_buf, src_stride, src_w, src_h,
      dst_buf, dst_stride, dst_w, dst_h, angle10, pivot_x, pivot_y, dst_cx,
      dst_cy);
    uint32_t ms = (uint32_t)(clock_systime_ticks() - t0);
    uint32_t win;

    if (pf.win == 0u)
      {
        pf.win = clock_systime_ticks();
      }
    if (r)
      {
        pf.n++;
        pf.sum += ms;
        if (ms > pf.max)
          {
            pf.max = ms;
          }
      }
    win = (uint32_t)(clock_systime_ticks() - pf.win);
    if (win >= 2000u)
      {
        if (pf.n > 0u)
          {
            syslog(LOG_DEBUG, "[vrot] bakes n=%u avg=%ums max=%ums win=%ums",
              (unsigned)pf.n, (unsigned)(pf.sum / pf.n), (unsigned)pf.max,
              (unsigned)win);
          }
        pf.n = 0u;
        pf.sum = 0u;
        pf.max = 0u;
        pf.win = clock_systime_ticks();
      }
    return r;
  }
}

static bool myvendor_epic_rotate_impl(const uint16_t * src_buf,
  uint32_t src_stride, int32_t src_w, int32_t src_h, uint16_t * dst_buf,
  uint32_t dst_stride, int32_t dst_w, int32_t dst_h, int32_t angle10,
  int32_t pivot_x, int32_t pivot_y, int32_t dst_cx, int32_t dst_cy)
{
  EPIC_LayerConfigTypeDef layers[2];
  EPIC_LayerConfigTypeDef out;
  bool ok;
  const uint32_t wt_before = g_epic_wait_timeouts;

  if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0
      || angle10 < 0 || angle10 >= 3600)
    {
      return false;
    }

  /* 坐标寄存器 10 bit（EPIC_COORDINATES_MAX = 1010），越界 HAL 会断言。 */
  if (src_w > EPIC_COORDINATES_MAX || src_h > EPIC_COORDINATES_MAX
      || dst_w > EPIC_COORDINATES_MAX || dst_h > EPIC_COORDINATES_MAX)
    {
      return false;
    }

  /* EPIC 绕过 CPU 缓存直读 PSRAM：先把源写回去。 */
  lv_epic_clean_invalidate_cache_range(src_buf,
    (size_t)src_stride * (size_t)src_h);

  HAL_EPIC_LayerConfigInit(&layers[0]);          /* 背景 = 目的整层 */
  layers[0].data = (uint8_t *)dst_buf;
  layers[0].color_mode = EPIC_COLOR_RGB565;
  layers[0].width = (uint16_t)dst_w;
  layers[0].height = (uint16_t)dst_h;
  layers[0].x_offset = 0;
  layers[0].y_offset = 0;
  layers[0].total_width = MYVENDOR_EPIC_PX_PER_ROW(dst_stride);

  HAL_EPIC_LayerConfigInit(&layers[1]);          /* 前景 = 源整层 + 变换 */
  layers[1].data = (uint8_t *)src_buf;
  layers[1].color_mode = EPIC_COLOR_RGB565;
  layers[1].width = (uint16_t)src_w;
  layers[1].height = (uint16_t)src_h;
  layers[1].x_offset = (int16_t)(dst_cx - pivot_x);
  layers[1].y_offset = (int16_t)(dst_cy - pivot_y);
  layers[1].total_width = MYVENDOR_EPIC_PX_PER_ROW(src_stride);
  layers[1].transform_cfg.angle = (uint16_t)angle10;
  layers[1].transform_cfg.pivot_x = (int16_t)pivot_x;
  layers[1].transform_cfg.pivot_y = (int16_t)pivot_y;
  layers[1].transform_cfg.scale_x = EPIC_INPUT_SCALE_NONE;
  layers[1].transform_cfg.scale_y = EPIC_INPUT_SCALE_NONE;

  memcpy(&out, &layers[0], sizeof(out));

  /*
   * 超时也算失败：HAL 的 `EPIC_WaitDone()` 超时只把 `g_epic_wait_timeouts` 加一、
   * 仍返回 HAL_OK。照 HAL_OK 收下会把"没转完的像素"当成功 —— 调用方
   * （`vmap_rot_full`）据此认为 R 已更新，屏上就会停在半张旧图。
   * 本文件其它搬运路径（copy/fill）一直用 `epic_result()` 兜这件事，旋转也接上。
   */
  ok = epic_result(wt_before, lv_epic_blend(layers, 2, &out) == HAL_OK,
    "rotate");

  if (ok)
    {
      epic_invalidate_rect_rows(dst_buf, dst_stride, dst_w, dst_h);
    }

  return ok;
}

/**
 * @brief 旋转自检：用"像素里编码自己坐标"的图案转一次，抽查若干目的点。
 *
 * @details 判据是**坐标**而不是颜色：源 32x32 每像素写 (x<<11)|(y<<5)，
 *          转 30° 后对抽查点做逆变换求源点，读回来的坐标必须落在 ±2 px 内
 *          （EPIC 最近邻 + 亚像素）。失败时把第一处"期望 vs 实际"打出来 ——
 *          角度正负、pivot 语义错了都能从那一行反推。
 */
bool myvendor_epic_rotate_selftest(void)
{
  enum
  {
    SRC_W = 32, SRC_H = 32, DST_W = 48, DST_H = 48, PIV = 16, ANGLE = 300,
    PROBES = 10
  };
  static uint16_t src[SRC_W * SRC_H];
  static uint16_t dst[DST_W * DST_H];
  static const int16_t probe[PROBES][2] =
  {
    { 24, 24 }, { 24, 12 }, { 24, 36 }, { 12, 24 }, { 36, 24 },
    { 16, 16 }, { 32, 32 }, { 32, 16 }, { 16, 32 }, { 24, 6 }
  };
  const int32_t cx = DST_W / 2;
  const int32_t cy = DST_H / 2;
  const double th = (double)ANGLE * 0.1 * M_PI / 180.0;
  int32_t x;
  int32_t y;
  int32_t i;
  int32_t fails = 0;

  for (y = 0; y < SRC_H; y++)
    {
      for (x = 0; x < SRC_W; x++)
        {
          src[y * SRC_W + x] = (uint16_t)(((uint16_t)x << 11)
            | ((uint16_t)y << 5));
        }
    }
  memset(dst, 0, sizeof(dst));

  if (!myvendor_epic_rotate(src, SRC_W * sizeof(uint16_t), SRC_W, SRC_H,
          dst, DST_W * sizeof(uint16_t), DST_W, DST_H, ANGLE, PIV, PIV,
          cx, cy))
    {
      syslog(LOG_WARNING, "[vrot] selftest SKIP: epic rotate unavailable");
      return false;
    }

  for (i = 0; i < PROBES; i++)
    {
      /* 目的 (u,v) → 源：s = piv + R(-θ)·((u,v) - c)，θ 顺时针（y 向下）。 */
      const double du = (double)(probe[i][0] - cx);
      const double dv = (double)(probe[i][1] - cy);
      const double sx = (double)PIV + (cos(th) * du + sin(th) * dv);
      const double sy = (double)PIV + (-sin(th) * du + cos(th) * dv);
      const int32_t rx = (int32_t)lround(sx);
      const int32_t ry = (int32_t)lround(sy);
      uint16_t got;
      int32_t gx;
      int32_t gy;

      if (rx < 0 || ry < 0 || rx >= SRC_W || ry >= SRC_H)
        {
          continue;                /* 该点落在源的空白区 */
        }

      got = dst[probe[i][1] * DST_W + probe[i][0]];
      gx = (int32_t)((got >> 11) & 0x1F);
      gy = (int32_t)((got >> 5) & 0x3F);

      if (abs((int)(gx - rx)) > 2 || abs((int)(gy - ry)) > 2)
        {
          if (fails == 0)
            {
              syslog(LOG_WARNING,
                "[vrot] selftest fail dst(%d,%d): expect src(%d,%d) got (%d,%d) raw=0x%04x",
                (int)probe[i][0], (int)probe[i][1], (int)rx, (int)ry,
                (int)gx, (int)gy, (unsigned)got);
            }
          fails++;
        }
    }

  syslog((fails == 0) ? LOG_NOTICE : LOG_WARNING,
    "[vrot] selftest %s: %d probes, %d fail",
    (fails == 0) ? "PASS" : "FAIL", (int)PROBES, (int)fails);

  return fails == 0;
}

