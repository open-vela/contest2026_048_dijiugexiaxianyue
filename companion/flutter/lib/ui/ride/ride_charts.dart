import 'dart:math' as math;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/ride/ride_series_smooth.dart';
import 'package:sifli_companion/ui/ride/ride_track_style.dart';

/// 折线 / 面积图，虚线标均值。
///
/// 画之前先过一遍 [RideSeriesSmooth.prepare]：GNSS 抖出来的孤立尖峰在这里被
/// 摘掉，线本身再用单调三次曲线连起来 —— 直角折线在 130 px 高的卡片里看着
/// 像振动，不像趋势。
class RideSeriesChart extends StatefulWidget {
  const RideSeriesChart({
    super.key,
    required this.values,
    required this.color,
    this.fill = false,
    this.avg,
    this.cursor,
    this.smooth = true,
    this.animate = true,
    this.maxPoints = 160,
  });

  final List<double> values;
  final Color color;
  final bool fill;
  final double? avg;

  /// 回放游标，0..1；null 表示没在回放、不画游标。
  ///
  /// 只叠加竖线和交点圆点，曲线本身与 [avg] 虚线都不受影响 —— 没有回放时
  /// 的渲染必须和加这个参数之前完全一样。
  final double? cursor;

  /// 是否做去尖峰 + 平滑。原始序列（如速度条）不要走这里。
  final bool smooth;

  /// 进场时从左往右画出来。卡片在抽屉展开时才首次 build，正好播一次。
  final bool animate;

  /// 平滑后保留的点数上限。
  final int maxPoints;

  @override
  State<RideSeriesChart> createState() => _RideSeriesChartState();

  /// 按位置取样降采样（保留尖峰）。平滑序列不要用它。
  static List<double> downsample(List<double> src, int maxN) {
    if (src.length <= maxN) return src;
    final out = <double>[];
    for (var i = 0; i < maxN; i++) {
      final idx = (i * (src.length - 1) / (maxN - 1)).round();
      out.add(src[idx]);
    }
    return out;
  }
}

class _RideSeriesChartState extends State<RideSeriesChart> {
  List<double> _src = const [];
  List<double> _prepared = const [];
  bool _smooth = true;
  int _maxPoints = 160;
  bool _ready = false;

  @override
  void initState() {
    super.initState();
    _sync();
  }

  @override
  void didUpdateWidget(covariant RideSeriesChart old) {
    super.didUpdateWidget(old);
    _sync();
  }

  /// 抽屉拖动时整张卡片会逐帧重建，而平滑是 O(n·w) 的整条序列计算 ——
  /// 每帧重算会把拖拽帧预算吃掉。这里按**内容**判断是否需要重算：
  /// 卡片每次 build 都会新建一个 values 列表，比身份（identical）永远不等。
  /// 一次线性比较的成本可以忽略，换来的是平滑只在数据变化时跑一次。
  void _sync() {
    final v = widget.values;
    if (_ready &&
        _smooth == widget.smooth &&
        _maxPoints == widget.maxPoints &&
        listEquals(v, _src)) {
      return;
    }
    _smooth = widget.smooth;
    _maxPoints = widget.maxPoints;
    _src = List<double>.of(v);
    _prepared = widget.smooth
        ? RideSeriesSmooth.prepare(v, maxN: widget.maxPoints)
        : RideSeriesChart.downsample(v, 180);
    _ready = true;
  }

  @override
  Widget build(BuildContext context) {
    Widget paint(double reveal) => CustomPaint(
      painter: _SeriesPainter(
        values: _prepared,
        color: widget.color,
        fill: widget.fill,
        avg: widget.avg,
        cursor: widget.cursor,
        reveal: reveal,
      ),
      child: const SizedBox.expand(),
    );
    if (!widget.animate) return paint(1);
    return TweenAnimationBuilder<double>(
      tween: Tween<double>(begin: 0, end: 1),
      duration: const Duration(milliseconds: 760),
      curve: Curves.easeOutCubic,
      builder: (context, t, _) => paint(t),
    );
  }
}

class _SeriesPainter extends CustomPainter {
  _SeriesPainter({
    required this.values,
    required this.color,
    required this.fill,
    required this.avg,
    this.cursor,
    this.reveal = 1,
  });

  final List<double> values;
  final Color color;
  final bool fill;
  final double? avg;

  /// 回放游标，0..1；null 不画。
  final double? cursor;

  /// 0..1：只画到这一段宽度（进场揭示）。1 = 整条。
  final double reveal;

  @override
  void paint(Canvas canvas, Size size) {
    if (values.isEmpty || size.width < 8 || size.height < 8) return;
    const pad = EdgeInsets.fromLTRB(4, 10, 4, 6);
    final w = size.width - pad.left - pad.right;
    final h = size.height - pad.top - pad.bottom;
    if (w <= 0 || h <= 0) return;

    var minV = values.reduce(math.min);
    var maxV = values.reduce(math.max);
    if (avg != null) {
      minV = math.min(minV, avg!);
      maxV = math.max(maxV, avg!);
    }
    if (maxV - minV < 0.2) {
      minV -= 1;
      maxV += 1;
    }
    final span = maxV - minV;

    Offset pt(int i, double v) {
      final x = pad.left +
          (values.length == 1 ? w / 2 : w * i / (values.length - 1));
      final y = pad.top + h * (1 - (v - minV) / span);
      return Offset(x, y);
    }

    final pts = [for (var i = 0; i < values.length; i++) pt(i, values[i])];
    final line = monotonePath(pts);

    final r = reveal.clamp(0.0, 1.0);
    final revealed = r < 1;
    if (revealed) {
      canvas.save();
      canvas.clipRect(Rect.fromLTWH(0, 0, size.width * r, size.height));
    }

    // 只有一个采样点时路径退化成 moveTo，描边和填充都画不出东西 ——
    // 画个点，别让「整趟只有一个 0 踏频采样」的卡片变成空白。
    if (pts.length == 1) {
      canvas.drawCircle(pts.first, 2.8, Paint()..color = color);
    }

    if (fill) {
      final area = Path.from(line)
        ..lineTo(pts.last.dx, pad.top + h)
        ..lineTo(pts.first.dx, pad.top + h)
        ..close();
      canvas.drawPath(
        area,
        Paint()
          ..shader = LinearGradient(
            begin: Alignment.topCenter,
            end: Alignment.bottomCenter,
            colors: [
              color.withValues(alpha: 0.42),
              color.withValues(alpha: 0.02),
            ],
          ).createShader(Offset.zero & size),
      );
    }

    canvas.drawPath(
      line,
      Paint()
        ..color = color
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2.2
        ..strokeJoin = StrokeJoin.round
        ..strokeCap = StrokeCap.round,
    );

    final a = avg;
    if (a != null) {
      final y = pt(0, a).dy;
      _dash(
        canvas,
        Offset(pad.left, y),
        Offset(pad.left + w, y),
        Paint()
          ..color = color.withValues(alpha: 0.7)
          ..strokeWidth = 1.2,
      );
    }

    final c = cursor;
    if (c != null) {
      // 游标 x 是整条时间轴的 0..1，不是采样下标：回放按里程/时间推进，
      // 只能落在等距的图宽上。
      final cx = pad.left + w * c.clamp(0.0, 1.0);
      canvas.drawLine(
        Offset(cx, pad.top),
        Offset(cx, pad.top + h),
        Paint()
          ..color = color.withValues(alpha: 0.55)
          ..strokeWidth = 1,
      );

      // 交点 y 用相邻两点的折线插值。曲线本身是单调三次 Bézier，但解它的
      // 反函数在极值附近不稳；游标每帧只动几个像素，折线插值看不出来。
      final dot = values.length == 1 ? pts.first : _dotAt(cx, pts);
      // 先画 2 px 白边再盖实心点：深色卡片上单靠颜色，3 px 的点会被线吃掉。
      canvas.drawCircle(
        dot,
        4,
        Paint()
          ..color = Colors.white
          ..style = PaintingStyle.stroke
          ..strokeWidth = 2,
      );
      canvas.drawCircle(dot, 3, Paint()..color = color);
    }

    if (revealed) canvas.restore();
  }

  /// 绘图区内横坐标 [x] 处曲线上的点（按相邻采样点线性插值）。
  static Offset _dotAt(double x, List<Offset> pts) {
    final n = pts.length;
    if (n < 2) return pts.first;
    final first = pts.first.dx;
    final last = pts.last.dx;
    if (last - first <= 0) return pts.first;
    final t = ((x - first) / (last - first) * (n - 1))
        .clamp(0.0, (n - 1).toDouble());
    final i = t.floor().clamp(0, n - 2);
    final f = t - i;
    final y = pts[i].dy + (pts[i + 1].dy - pts[i].dy) * f;
    return Offset(x, y);
  }

  /// 单调三次曲线（Fritsch–Carlson）：相邻两点之间用 Bézier 连接，切线在
  /// 局部极值处置 0，所以平滑之后**不会在极值附近过冲** —— 普通 Catmull-Rom
  /// 会把尖峰画成一个超出数据范围的鼓包，读数就成了假的。
  static Path monotonePath(List<Offset> p) {
    final path = Path();
    if (p.isEmpty) return path;
    path.moveTo(p.first.dx, p.first.dy);
    final n = p.length;
    if (n == 1) return path;
    if (n == 2) {
      path.lineTo(p[1].dx, p[1].dy);
      return path;
    }
    final d = List<double>.filled(n - 1, 0);
    for (var i = 0; i < n - 1; i++) {
      final dx = p[i + 1].dx - p[i].dx;
      d[i] = dx.abs() < 1e-6 ? 0 : (p[i + 1].dy - p[i].dy) / dx;
    }
    final m = List<double>.filled(n, 0);
    for (var i = 1; i < n - 1; i++) {
      // 极值点切线取 0：曲线在峰顶是平的，不会冲过头。
      m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) / 2;
    }
    m[0] = d.first;
    m[n - 1] = d.last;
    for (var i = 0; i < n - 1; i++) {
      if (d[i] == 0) {
        m[i] = 0;
        m[i + 1] = 0;
        continue;
      }
      final a = m[i] / d[i];
      final b = m[i + 1] / d[i];
      final s = a * a + b * b;
      if (s > 9) {
        final t = 3 / math.sqrt(s);
        m[i] = t * a * d[i];
        m[i + 1] = t * b * d[i];
      }
    }
    for (var i = 0; i < n - 1; i++) {
      final dx = p[i + 1].dx - p[i].dx;
      path.cubicTo(
        p[i].dx + dx / 3,
        p[i].dy + m[i] * dx / 3,
        p[i + 1].dx - dx / 3,
        p[i + 1].dy - m[i + 1] * dx / 3,
        p[i + 1].dx,
        p[i + 1].dy,
      );
    }
    return path;
  }

  static void _dash(Canvas canvas, Offset a, Offset b, Paint paint) {
    const dash = 5.0;
    const gap = 4.0;
    final len = (b - a).distance;
    if (len < 1) return;
    final dir = (b - a) / len;
    var t = 0.0;
    while (t < len) {
      final end = math.min(t + dash, len);
      canvas.drawLine(a + dir * t, a + dir * end, paint);
      t += dash + gap;
    }
  }

  @override
  bool shouldRepaint(covariant _SeriesPainter old) =>
      old.values != values ||
      old.color != color ||
      old.fill != fill ||
      old.avg != avg ||
      old.cursor != cursor ||
      old.reveal != reveal;
}

/// 柱状图：一格一根柱（周 = 逐日 7 根，月 = 逐日 28–31 根），虚线标均值。
///
/// 取值、标签由调用方按区间算好传进来 —— 图表只管画，不碰业务。
class RideBarChart extends StatelessWidget {
  /// 创建柱状图。
  const RideBarChart({
    super.key,
    required this.values,
    required this.labels,
    required this.color,
    this.avg,
    this.avgText,
    this.highlight = -1,
  });

  /// 每根柱子的值，与 [labels] 一一对应。
  final List<double> values;

  /// 柱子下方的标签；空串表示这根不标（月的 31 根只标 1/5/10…）。
  final List<String> labels;

  /// 柱色。0 值的柱子画成灰底，与「这天没骑」区分开。
  final Color color;

  /// 虚线位置（均值）。null 或 0 不画。
  final double? avg;

  /// 虚线右端的数值标签（如 `78.1`）。空则不画 —— 光一条线分不清它落在哪一档。
  final String? avgText;

  /// 高亮哪根（今天）；-1 不高亮。
  final int highlight;

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      height: 112,
      width: double.infinity,
      child: CustomPaint(
        painter: _BarPainter(
          values: values,
          labels: labels,
          color: color,
          avg: avg,
          avgText: avgText,
          highlight: highlight,
        ),
        child: const SizedBox.expand(),
      ),
    );
  }
}

class _BarPainter extends CustomPainter {
  _BarPainter({
    required this.values,
    required this.labels,
    required this.color,
    required this.avg,
    required this.avgText,
    required this.highlight,
  });

  final List<double> values;
  final List<String> labels;
  final Color color;
  final double? avg;
  final String? avgText;
  final int highlight;

  static const _emptyBar = Color(0xFF3A3A3C);
  static const _labelColor = Color(0xFF8E8E93);

  @override
  void paint(Canvas canvas, Size size) {
    if (values.isEmpty || size.width < 8 || size.height < 8) return;
    final w = size.width;
    // 有标签才留那一行；迷你图（labels 全空）就贴边画，别浪费高度。
    final hasLabel = labels.any((l) => l.isNotEmpty);
    final padTop = hasLabel ? 10.0 : 3.0;
    final padBottom = hasLabel ? 17.0 : 3.0;
    final h = size.height - padTop - padBottom;
    if (h <= 0) return;

    final maxV = values.fold<double>(0, math.max);
    final scaleMax = math.max(maxV, avg ?? 0) * 1.12;
    final slot = w / values.length;
    final barW = math.min(slot * 0.62, 18.0);

    for (var i = 0; i < values.length; i++) {
      final v = values[i];
      final cx = slot * (i + 0.5);
      final frac = scaleMax <= 0 ? 0.0 : (v / scaleMax).clamp(0.0, 1.0);
      // 0 也留 2 px 灰底：能看出「这一天存在、只是没骑」。
      final barH = frac <= 0 ? 2.0 : math.max(2.0, h * frac);
      final isHi = i == highlight;

      canvas.drawRRect(
        RRect.fromRectAndRadius(
          Rect.fromLTWH(cx - barW / 2, padTop + h - barH, barW, barH),
          Radius.circular(math.min(barW / 2, 3)),
        ),
        Paint()
          ..color = v <= 0
              ? _emptyBar
              : (isHi ? color : color.withValues(alpha: 0.55)),
      );

      final label = i < labels.length ? labels[i] : '';
      if (label.isEmpty) continue;
      final tp = TextPainter(
        text: TextSpan(
          text: label,
          style: TextStyle(
            color: isHi ? Colors.white : _labelColor,
            fontSize: 10,
            fontWeight: isHi ? FontWeight.w700 : FontWeight.w400,
          ),
        ),
        textDirection: TextDirection.ltr,
      )..layout();
      tp.paint(canvas, Offset(cx - tp.width / 2, size.height - padBottom + 4));
    }

    final a = avg;
    if (a != null && a > 0) {
      final y = padTop + h * (1 - (a / scaleMax).clamp(0.0, 1.0));
      final paint = Paint()
        ..color = color.withValues(alpha: 0.85)
        ..strokeWidth = 1.4;
      _SeriesPainter._dash(canvas, Offset(0, y), Offset(w, y), paint);

      // 数值标签贴在线右端上方：只有一条线看不出它落在哪一档。
      final text = avgText;
      if (text != null && text.isNotEmpty) {
        final tp = TextPainter(
          text: TextSpan(
            text: text,
            style: TextStyle(
              color: color,
              fontSize: 9,
              fontWeight: FontWeight.w700,
            ),
          ),
          textDirection: TextDirection.ltr,
        )..layout();
        tp.paint(canvas, Offset(w - tp.width, y - tp.height - 2));
      }
    }
  }

  @override
  bool shouldRepaint(covariant _BarPainter old) =>
      old.values != values ||
      old.labels != labels ||
      old.color != color ||
      old.avg != avg ||
      old.avgText != avgText ||
      old.highlight != highlight;
}

/// 速度带：按采样点从绿到红。
class RidePaceStrip extends StatelessWidget {
  const RidePaceStrip({super.key, required this.speeds, this.range});

  final List<double> speeds;

  /// 色带归一化区间。由调用方用**原始**速度序列（和轨迹线同一份）算好传进来，
  /// 上下两条颜色才对得上；为空时自己按这段算。
  final ({double lo, double hi})? range;

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      height: 10,
      width: double.infinity,
      child: CustomPaint(
        painter: _PacePainter(
          speeds: RideSeriesChart.downsample(speeds, 120),
          range: range,
        ),
        child: const SizedBox.expand(),
      ),
    );
  }
}

class _PacePainter extends CustomPainter {
  _PacePainter({required this.speeds, this.range});

  final List<double> speeds;
  final ({double lo, double hi})? range;

  @override
  void paint(Canvas canvas, Size size) {
    final r = RRect.fromRectAndRadius(
      Offset.zero & size,
      const Radius.circular(5),
    );
    canvas.save();
    canvas.clipRRect(r);
    if (speeds.length < 2) {
      canvas.drawRRect(
        r,
        Paint()
          ..shader = const LinearGradient(
            colors: [
              RideTrackStyle.bandSlow,
              RideTrackStyle.bandMid,
              RideTrackStyle.bandFast,
            ],
          ).createShader(Offset.zero & size),
      );
    } else {
      final minV = range?.lo ?? speeds.reduce(math.min);
      final maxV = range?.hi ?? speeds.reduce(math.max);
      final w = size.width / speeds.length;
      for (var i = 0; i < speeds.length; i++) {
        // 与轨迹线共用同一个映射（[RideTrackStyle.speedBandColor]），颜色才对得上。
        canvas.drawRect(
          Rect.fromLTWH(i * w, 0, w + 0.6, size.height),
          Paint()..color = RideTrackStyle.speedBandColor(speeds[i], minV, maxV),
        );
      }
    }
    canvas.restore();
  }

  @override
  bool shouldRepaint(covariant _PacePainter old) =>
      old.speeds != speeds || old.range != range;
}

/// 心率五区横条。
class RideZoneBar extends StatelessWidget {
  const RideZoneBar({
    super.key,
    required this.label,
    required this.color,
    required this.time,
    required this.maxMs,
  });

  final String label;
  final Color color;
  final Duration time;
  final int maxMs;

  @override
  Widget build(BuildContext context) {
    final frac = maxMs <= 0 ? 0.0 : (time.inMilliseconds / maxMs).clamp(0.0, 1.0);
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Row(
        children: [
          SizedBox(
            width: 64,
            child: Text(
              label,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: const TextStyle(color: Color(0xFF8E8E93), fontSize: 12),
            ),
          ),
          Expanded(
            child: ClipRRect(
              borderRadius: BorderRadius.circular(3),
              child: SizedBox(
                height: 8,
                child: ColoredBox(
                  color: const Color(0xFF2C2C2E),
                  child: Align(
                    alignment: Alignment.centerLeft,
                    child: FractionallySizedBox(
                      widthFactor: frac,
                      child: ColoredBox(color: color),
                    ),
                  ),
                ),
              ),
            ),
          ),
          const SizedBox(width: 10),
          SizedBox(
            width: 64,
            child: Text(
              GpxUtil.formatHms(time),
              textAlign: TextAlign.right,
              style: const TextStyle(
                color: Colors.white,
                fontSize: 12,
                fontFeatures: [FontFeature.tabularFigures()],
              ),
            ),
          ),
        ],
      ),
    );
  }
}
