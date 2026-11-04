/*
 * WP-16c — 子矩形旋转的"源行区间"穷举验证（宿主，纯计算）。
 *
 * 背景：`myvendor_epic_rotate_area()` 为了不整幅 clean 画布，只把"这块目的像素
 * 会采到的源行"写回 cache。若这个行区间算小了，EPIC 会读到陈旧像素 ⇒ 花屏。
 *
 * 这里用 double 复现 HAL 的反投影（`bf0_hal_epic.c` 的 EPIC_GetRotatedArea 调用序列）：
 *
 *     源 = pivot + R(-θ) · (目的 − dst_c)          θ = angle10 * 0.1°（顺时针，y 向下）
 *     实现里 R(-θ) = [ c  s ; -s  c ]（等价于 HAL 用 3600-angle 配 [cos -sin; sin cos]）
 *
 * 然后对随机 clip 内的**每个目的像素**算出它采样的源像素（最近邻取 floor 与 round
 * 两种都查），确认都落在实现使用的"四角 AABB（两种 sin 符号取并集）外扩 2 px"之内。
 *
 * 编译：cc -O2 -Wall -Wextra -o /tmp/area_diff area_diff.c -lm
 * 运行：/tmp/area_diff
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SRC_W 496
#define SRC_H 472
#define DST_W 288
#define DST_H 264
#ifndef MARGIN
#define MARGIN 2      /* 实现里的外扩量；用 -DMARGIN=n 可测"最少要几 px" */
#endif

/*
 * 采样角相对"算区间用的角"的扰动，单位 0.001°。
 *
 * HAL 用的是 Q15 查表 sin + 定点移位，实现用的是 double。两者对同一个角最多差
 * 约 3e-5（±0.0017°），在 400 px 力臂上不到 0.02 px。这里用一个远大于它的扰动
 * 来把"定点误差"量化成像素：若 MARGIN=2 在 ANGLE_JITTER=10（0.1°）下仍 PASS，
 * 定点误差就不是问题。
 */
#ifndef ANGLE_JITTER
#define ANGLE_JITTER 0      /* 单位 0.001°；HAL 定点误差 ≈ 0.0017°，取 2 即已覆盖 */
#endif

/*
 * HAL_FIXED=1：采样用 HAL 真正下发的那对系数。完整复现三条：
 *   ① `EPIC_TrigoSin()` 的 0..90 度 Q1.15 查表 + 象限展开（`bf0_hal_epic.c:178-236`）；
 *   ② `EPIC_TrigoSinCosP1()` 的 0.1° 线性插值、整数除法向零截断（`:246-289`）；
 *   ③ `abs_sinma >> (15-12)`（`:2186-2196`、`:2667-2672`）—— 取幅值再右移 ⇒ Q12，
 *      符号单独由旋向寄存器表达。于是 sin²+cos² ≠ 1，且有"负值按幅值截断"的非对称性，
 *      这是"ideal matrix + 角度扰动"模拟不出来的。
 */
#ifndef HAL_FIXED
#define HAL_FIXED 0
#endif

/* 实现里用的行区间（与 services/myvendor_epic_blit.c 同式，含两种 sin 符号的并集）。 */
static void impl_rows(int32_t angle10, int32_t pivot_y, int32_t dst_cx,
  int32_t dst_cy, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
  int32_t * sy0, int32_t * sy1)
{
  const double th = (double)angle10 * (0.1 * M_PI / 180.0);
  const double c = cos(th);
  const double s = sin(th);
  const int32_t u[4] = { x0, x1, x0, x1 };
  const int32_t v[4] = { y0, y0, y1, y1 };
  int32_t lo = SRC_H - 1;
  int32_t hi = 0;
  int32_t i;

  for (i = 0; i < 4; i++) {
    const double dx = (double)(u[i] - dst_cx);
    const double dy = (double)(v[i] - dst_cy);
    const int32_t ya = (int32_t)lround((double)pivot_y - s * dx + c * dy);
    const int32_t yb = (int32_t)lround((double)pivot_y + s * dx + c * dy);
    const int32_t y_lo = (ya < yb) ? ya : yb;
    const int32_t y_hi = (ya < yb) ? yb : ya;

    (void)MARGIN;
    if (y_lo < lo) {
      lo = y_lo;
    }
    if (y_hi > hi) {
      hi = y_hi;
    }
  }

  lo -= MARGIN;
  hi += MARGIN;
  if (lo < 0) {
    lo = 0;
  }
  if (hi >= SRC_H) {
    hi = SRC_H - 1;
  }
  if (hi < lo) {
    lo = 0;
    hi = SRC_H - 1;
  }
  *sy0 = lo;
  *sy1 = hi;
}

/* HAL 的 0..90° sin 表（Q1.15），逐字抄自 `bf0_hal_epic.c:178-190`。 */
static const int16_t sin0_90_table[91] = {
  0,     572,   1144,  1715,  2286,  2856,  3425,  3993,  4560,  5126,
  5690,  6252,  6813,  7371,  7927,  8481,  9032,  9580,  10126, 10668,
  11207, 11743, 12275, 12803, 13328, 13848, 14364, 14876, 15383, 15886,
  16383, 16876, 17364, 17846, 18323, 18794, 19260, 19720, 20173, 20621,
  21062, 21497, 21925, 22347, 22762, 23170, 23571, 23964, 24351, 24730,
  25101, 25465, 25821, 26169, 26509, 26841, 27165, 27481, 27788, 28087,
  28377, 28659, 28932, 29196, 29451, 29697, 29934, 30162, 30381, 30591,
  30791, 30982, 31163, 31335, 31498, 31650, 31794, 31927, 32051, 32165,
  32269, 32364, 32448, 32523, 32587, 32642, 32687, 32722, 32747, 32762,
  32767
};

static int16_t hal_trigo_sin(int16_t angle)
{
  while (angle < 0) {
    angle += 360;
  }
  while (angle >= 360) {
    angle -= 360;
  }
  if (angle < 90) {
    return sin0_90_table[angle];
  }
  if (angle < 180) {
    return sin0_90_table[180 - angle];
  }
  if (angle < 270) {
    return (int16_t)(-sin0_90_table[angle - 180]);
  }
  return (int16_t)(-sin0_90_table[360 - angle]);
}

/* 返回 HAL 真正写进 sin/cos 系数寄存器的那对值（Q12，带正确符号）。 */
static void hal_sincos_q12(int32_t angle10, double *c_out, double *s_out)
{
  const int32_t low = angle10 / 10;

  /* HAL 在 angle_degree==0 时**直接关掉旋转**（`bf0_hal_epic.c:2655-2662`），
   * 是恒等映射，不经过系数寄存器。0° 必须单独处理，否则 cos 会变成 4095/4096。 */
  if ((angle10 % 3600) == 0) {
    *c_out = 1.0;
    *s_out = 0.0;
    return;
  }

  const int32_t high = low + 1;
  const int32_t rem = angle10 - low * 10;
  const int32_t s1 = hal_trigo_sin((int16_t)low);
  const int32_t s2 = hal_trigo_sin((int16_t)high);
  const int32_t c1 = hal_trigo_sin((int16_t)(low + 90));
  const int32_t c2 = hal_trigo_sin((int16_t)(high + 90));
  const int32_t sinma = (s1 * (10 - rem) + s2 * rem) / 10;
  const int32_t cosma = (c1 * (10 - rem) + c2 * rem) / 10;
  const int32_t as = (sinma < 0) ? -sinma : sinma;
  const int32_t ac = (cosma < 0) ? -cosma : cosma;

  *s_out = ((sinma < 0) ? -1.0 : 1.0) * (double)(as >> 3) / 4096.0;
  *c_out = ((cosma < 0) ? -1.0 : 1.0) * (double)(ac >> 3) / 4096.0;
}

/* 目的像素 (u,v) 真正采样的源点（double）。 */
static void sample_src(int32_t angle10, int32_t pivot_x, int32_t pivot_y,
  int32_t dst_cx, int32_t dst_cy, int32_t u, int32_t v, double * sx,
  double * sy)
{
  const double th = ((double)angle10 + (double)ANGLE_JITTER / 10.0)
    * (0.1 * M_PI / 180.0);
  double c = cos(th);
  double s = sin(th);
  const double dx = (double)(u - dst_cx);
  const double dy = (double)(v - dst_cy);

#if HAL_FIXED
  hal_sincos_q12(angle10, &c, &s);
#endif

  /* 源 = pivot + [c s; -s c]·(d - dst_c) */
  *sx = (double)pivot_x + c * dx + s * dy;
  *sy = (double)pivot_y - s * dx + c * dy;
}

static uint32_t rng_state = 0x12345678u;

static uint32_t rnd(void)
{
  rng_state = rng_state * 1664525u + 1013904223u;
  return rng_state;
}

static int32_t rnd_range(int32_t lo, int32_t hi)
{
  return lo + (int32_t)(rnd() % (uint32_t)(hi - lo + 1));
}

int main(void)
{
  int32_t worst_miss = 0;          /* 采样行落在区间外的最大像素数 */
  int32_t worst_case = -1;
  int32_t worst_angle = 0;
  int32_t slack_min = 1000000;
  int32_t checked = 0;
  int32_t cases = 10000;
  int32_t k;

  for (k = 0; k < cases; k++) {
    int32_t angle10;
    int32_t pivot_x;
    int32_t pivot_y;
    int32_t cw;
    int32_t ch;
    int32_t cx;
    int32_t cy;
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
    int32_t sy0;
    int32_t sy1;
    int32_t dcx;
    int32_t dcy;
    int32_t u;
    int32_t v;
    int32_t miss = 0;
    int32_t slack_lo = 1000000;
    int32_t slack_hi = 1000000;

    switch (k % 6) {
      case 0:                       /* 整数倍角 */
        angle10 = (int32_t)(rnd() % 4u) * 900;
        break;
      case 1:                       /* 接近 0/360 */
        angle10 = (int32_t)(rnd() % 12u);
        break;
      case 2:                       /* 接近 90 的整数倍 */
        angle10 = 890 + (int32_t)(rnd() % 21u);
        break;
      default:
        angle10 = (int32_t)(rnd() % 3600u);
        break;
    }

    pivot_x = rnd_range(0, SRC_W - 1);
    pivot_y = rnd_range(0, SRC_H - 1);
    /* 目的中心也随机：实现允许调用方给任意 dst_c（vmap 用 R 中心，别处不一定）。 */
    dcx = rnd_range(0, DST_W - 1);
    dcy = rnd_range(0, DST_H - 1);

    /* clip 尺寸 1..64；含贴边/越界（实现会 clamp，这里按同样规则先 clamp）。 */
    cw = (k % 17 == 0) ? 1 : rnd_range(2, 64);
    ch = (k % 19 == 0) ? 1 : rnd_range(2, 64);
    cx = rnd_range(-20, DST_W - 1);
    cy = rnd_range(-20, DST_H - 1);

    x0 = cx;
    y0 = cy;
    x1 = cx + cw - 1;
    y1 = cy + ch - 1;
    if (x0 < 0) {
      x0 = 0;
    }
    if (y0 < 0) {
      y0 = 0;
    }
    if (x1 > DST_W - 1) {
      x1 = DST_W - 1;
    }
    if (y1 > DST_H - 1) {
      y1 = DST_H - 1;
    }
    if (x1 < x0 || y1 < y0) {
      continue;                     /* 实现里返回"无事可做" */
    }

    impl_rows(angle10, pivot_y, dcx, dcy, x0, y0, x1, y1, &sy0, &sy1);

    for (v = y0; v <= y1; v++) {
      for (u = x0; u <= x1; u++) {
        double sx;
        double sy;
        int32_t yf;
        int32_t yr;
        int32_t dyf;
        int32_t dyr;
        bool in_f;
        bool in_r;

        sample_src(angle10, pivot_x, pivot_y, dcx, dcy, u, v, &sx, &sy);

        /* 最近邻的两种取整都可能被硬件采用；两种都必须在区间内。
         * 采样点落在源缓冲之外时 HAL 会按"前景没覆盖"处理（不读那几个像素），
         * 这类点不参与判据 —— 否则量到的是源边界外的坐标，不是 cache 的问题。 */
        yf = (int32_t)floor(sy);
        yr = (int32_t)lround(sy);
        in_f = (yf >= 0 && yf < SRC_H);
        in_r = (yr >= 0 && yr < SRC_H);
        if (!in_f && !in_r) {
          continue;                 /* 两种取整都在源外：HAL 不读这些像素 */
        }
        dyf = in_f ? ((yf < sy0) ? (sy0 - yf) : ((yf > sy1) ? (yf - sy1) : 0)) : 0;
        dyr = in_r ? ((yr < sy0) ? (sy0 - yr) : ((yr > sy1) ? (yr - sy1) : 0)) : 0;
        if (dyf > miss) {
          miss = dyf;
        }
        if (dyr > miss) {
          miss = dyr;
        }
        if (in_f) {
          if (yf - sy0 < slack_lo) {
            slack_lo = yf - sy0;
          }
          if (sy1 - yf < slack_hi) {
            slack_hi = sy1 - yf;
          }
        }
        checked++;
      }
    }

    if (miss > worst_miss) {
      worst_miss = miss;
      worst_case = k;
      worst_angle = angle10;
    }
    if (slack_lo < slack_min) {
      slack_min = slack_lo;
    }
    if (slack_hi < slack_min) {
      slack_min = slack_hi;
    }
  }

  printf("cases=%d sampled_pixels=%d\n", (int)cases, (int)checked);
  printf("worst_miss_px=%d (case %d, angle10=%d)\n", (int)worst_miss,
    (int)worst_case, (int)worst_angle);
  printf("min_slack_px=%d margin=%d jitter_0.001deg=%d\n", (int)slack_min,
    MARGIN, ANGLE_JITTER);
  printf("verdict=%s\n", (worst_miss == 0) ? "PASS" : "FAIL");
  return (worst_miss == 0) ? 0 : 1;
}
