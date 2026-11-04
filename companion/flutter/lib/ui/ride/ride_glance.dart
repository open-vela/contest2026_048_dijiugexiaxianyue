import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/ride/ride_record_store.dart';

/// 回放游标处的瞬时读数。
typedef RideGlanceLive = ({
  double? speedKmh,
  int? cadenceRpm,
  int? powerW,
  int? hrBpm,
});

/// 地图 / 分享图上的毛玻璃条：顶栏说明，底栏四格数据。
class RideGlanceOverlay extends StatelessWidget {
  /// [forShare] 时顶栏带品牌和标题，底栏改为距离 / 时长等分享向数据。
  ///
  /// [live] 非空时，回放中的底栏改为「当前速度 / 踏频 / 功率 / 心率」，
  /// 跟着游标走；值为 `null`（没在回放）时回到底栏的汇总口径。
  /// 只重建底栏那四格 —— 顶栏和毛玻璃底板不参与每秒十几次的刷新。
  ///
  /// [player] 是回放播放器条，放在底栏上方；[forShare] 时不传。
  ///
  /// [onDragUpdate]/[onDragEnd] 非空时，顶部说明条和底部四格都变成抽屉的
  /// 拖拽把手 —— 地图上这两块浮层最顺手，不用非去够那条 4 px 的横杠。
  const RideGlanceOverlay({
    super.key,
    required this.rec,
    this.summary,
    this.forShare = false,
    this.live,
    this.player,
    this.bottomInset = 0,
    this.onDragUpdate,
    this.onDragEnd,
  });

  final RideRecord rec;
  final GpxSummary? summary;
  final bool forShare;
  final ValueListenable<RideGlanceLive?>? live;
  final Widget? player;

  /// 底部让位高度（收起状态的抽屉高度）。地图满屏铺在抽屉下面，
  /// 不让位的话数据条和播放器会被抽屉盖住。
  final double bottomInset;

  final ValueChanged<double>? onDragUpdate;
  final ValueChanged<double>? onDragEnd;

  @override
  Widget build(BuildContext context) {
    final km = summary?.distanceKm ?? rec.distanceKm ?? 0;
    final when = summary?.startTime ??
        (rec.mtime > 0
            ? DateTime.fromMillisecondsSinceEpoch(rec.mtime * 1000)
            : null);
    final stamp = when == null ? rec.title : GpxUtil.formatStamp(when);
    final avg = summary?.avgSpeedKmh;
    // 横屏 / 分屏时地图只有两三百像素高：顶栏（品牌 + 时间）是装饰性的，
    // 优先让给播放器和数据条，否则 Column 会挤到溢出。
    return LayoutBuilder(
      builder: (context, box) {
        // 按「扣掉底部让位和上下内边距之后还剩多少」决定放哪几块。
        // 固定阈值不行：安全区 48 时，body 220 的窗口减去让位(56+48)只剩 94，
        // 播放器(46+8)一放进去就溢出。
        //
        // 注意两种底栏高度不同：紧凑态（dense）上下各 8 → 36+16=52；
        // 普通态上下各 14 → 36+28=64。预算必须先定「用哪种」，再据此判断。
        final scale =
            MediaQuery.textScalerOf(context).scale(1.0).clamp(1.0, 2.0);
        final avail = box.maxHeight - bottomInset - 22;
        // 文字按主题行高算：数值 16×1.1 + 4 间距 + 标签 11×1.43（M3 bodyMedium
        // 的 height:1.43，底栏标签没有显式 height，会继承）。
        final cellH = (16 * 1.1 + 4 + 11 * 1.43) * scale;
        final footerBase = cellH > 36 ? cellH : 36; // 36 是分隔线的高度
        final denseFooterH = footerBase + 16; // 上下各 8
        final normalFooterH = footerBase + 28; // 上下各 14
        const playerH = 54.0; // 46 + 8 间隔
        // 顶栏两行（分享封面的标题 + 副标题）比应用内那行大，
        // 分开算：16×1.15 + 2 + 12×1.43 ≈ 37.6，左侧色条 34。
        final headTextH = (forShare
                ? 16 * 1.15 + 2 + 12 * 1.43
                : 15 * 1.43) *
            scale;
        final headMinH = forShare ? 34.0 : 28.0;
        final headerH = (headTextH > headMinH ? headTextH : headMinH) + 24;
        // 分享封面是整屏渲染，永远按普通态（顶栏 + 品牌 + 标题）画，
        // 顶栏强制显示，所以底栏要额外把它的高度算进去。
        final headerNeed = forShare ? headerH : 0.0;
        final normal =
            forShare || avail >= normalFooterH + playerH + headerH;
        final footerH = normal ? normalFooterH : denseFooterH;
        final showFooter = avail >= headerNeed + footerH;
        final showPlayer = player != null && avail >= footerH + playerH;
        final showHeader = normal;
        final dense = !normal;
        return Padding(
          padding: EdgeInsets.fromLTRB(12, 12, 12, 10 + bottomInset),
          child: Column(
            children: [
              if (showHeader) ...[
                _draggable(_header(stamp)),
              ],
              const Spacer(),
              if (showPlayer) ...[
                player!,
                const SizedBox(height: 8),
              ],
              if (showFooter) _draggable(_footer(km, avg, dense: dense)),
            ],
          ),
        );
      },
    );
  }

  /// 把浮层包成抽屉的拖拽把手（分享封面不包）。
  Widget _draggable(Widget child) {
    final update = onDragUpdate;
    if (update == null || forShare) return child;
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onVerticalDragUpdate: (d) => update(d.delta.dy),
      onVerticalDragEnd: (d) => onDragEnd?.call(d.primaryVelocity ?? 0),
      child: child,
    );
  }

  Widget _header(String stamp) {
    return RideGlancePanel(
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
              child: Row(
                children: [
                  Container(
                    width: 3,
                    height: forShare ? 34 : 28,
                    margin: const EdgeInsets.only(right: 10),
                    decoration: BoxDecoration(
                      color: AppTheme.accent,
                      borderRadius: BorderRadius.circular(2),
                    ),
                  ),
                  Expanded(
                    child: forShare
                        ? Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              Text(
                                rec.title,
                                maxLines: 1,
                                overflow: TextOverflow.ellipsis,
                                style: const TextStyle(
                                  color: Colors.white,
                                  fontSize: 16,
                                  fontWeight: FontWeight.w800,
                                  height: 1.15,
                                ),
                              ),
                              const SizedBox(height: 2),
                              Text(
                                '${LocaleKeys.appName.tr}  ·  $stamp',
                                maxLines: 1,
                                overflow: TextOverflow.ellipsis,
                                style: TextStyle(
                                  color: Colors.white.withValues(alpha: 0.7),
                                  fontSize: 12,
                                  fontWeight: FontWeight.w500,
                                ),
                              ),
                            ],
                          )
                        : Text(
                            LocaleKeys.rideOutdoor.tr,
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: const TextStyle(
                              color: Colors.white,
                              fontSize: 15,
                              fontWeight: FontWeight.w700,
                            ),
                          ),
                  ),
                  if (!forShare)
                    Text(
                      stamp,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: TextStyle(
                        color: Colors.white.withValues(alpha: 0.72),
                        fontSize: 13,
                        fontWeight: FontWeight.w500,
                      ),
                    ),
                ],
              ),
            ),
    );
  }

  Widget _footer(double km, double? avg, {bool dense = false}) {
    if (live != null && !forShare) {
      return ValueListenableBuilder<RideGlanceLive?>(
        valueListenable: live!,
        builder: (context, sample, _) => _metrics(
          sample == null ? _summaryMetrics(km, avg) : _liveMetrics(sample),
          dense: dense,
        ),
      );
    }
    return _metrics(
      forShare ? _shareMetrics(km) : _summaryMetrics(km, avg),
      dense: dense,
    );
  }

  /// 分享封面：距离 / 时长 / 均速 / 心率（没有心率用千卡兜底）。
  List<({String value, String unit, String label})> _shareMetrics(double km) {
    final kcal = GpxUtil.estimateKcal(
      avgHrBpm: summary?.avgHrBpm,
      duration: summary?.duration,
      distanceKm: km,
    );
    final avg = summary?.avgSpeedKmh;
    final hr = summary?.avgHrBpm;
    final dash = LocaleKeys.rideStatDash.tr;
    return [
      (
        value: km < 0.001
            ? dash
            : km < 1
                ? (km * 1000).round().toString()
                : km < 10
                    ? km.toStringAsFixed(2)
                    : km.toStringAsFixed(1),
        unit: km < 0.001
            ? ''
            : km < 1
                ? 'm'
                : 'km',
        label: LocaleKeys.rideMetricDistance.tr,
      ),
      (
        value: summary?.duration == null
            ? dash
            : GpxUtil.formatHms(summary!.duration!),
        unit: '',
        label: LocaleKeys.rideMetricDuration.tr,
      ),
      (
        value: avg == null ? dash : avg.toStringAsFixed(1),
        unit: avg == null ? '' : 'km/h',
        label: LocaleKeys.rideMetricAvgSpeed.tr,
      ),
      hr != null
          ? (value: '$hr', unit: 'BPM', label: LocaleKeys.rideMetricAvgHr.tr)
          : (
              value: kcal > 0 ? '$kcal' : dash,
              unit: kcal > 0 ? 'kcal' : '',
              label: LocaleKeys.rideMetricKcal.tr,
            ),
    ];
  }

  /// 应用内静态底栏：均速 / 平均心率 / 爬升 / 千卡。
  List<({String value, String unit, String label})> _summaryMetrics(
    double km,
    double? avg,
  ) {
    final kcal = GpxUtil.estimateKcal(
      avgHrBpm: summary?.avgHrBpm,
      duration: summary?.duration,
      distanceKm: km,
    );
    final climb = summary?.elevGainM;
    final hr = summary?.avgHrBpm;
    final dash = LocaleKeys.rideStatDash.tr;
    return [
      (
        value: avg == null ? dash : avg.toStringAsFixed(1),
        unit: avg == null ? '' : 'km/h',
        label: LocaleKeys.rideMetricAvgSpeed.tr,
      ),
      // 应用内那四格固定摆平均心率（没有就给一条横杠）：指标位置随心率有无换人，
      // 会让人以为心率这项不存在。分享封面仍用千卡兜底 —— 封面出现横杠不好看。
      (
        value: hr == null ? dash : '$hr',
        unit: hr == null ? '' : 'BPM',
        label: LocaleKeys.rideMetricAvgHr.tr,
      ),
      (
        value: climb == null
            ? dash
            : climb >= 10
                ? climb.round().toString()
                : climb.toStringAsFixed(1),
        unit: climb == null ? '' : 'm',
        label: LocaleKeys.rideMetricClimb.tr,
      ),
      (
        value: kcal > 0 ? '$kcal' : dash,
        unit: kcal > 0 ? 'kcal' : '',
        label: LocaleKeys.rideMetricKcal.tr,
      ),
    ];
  }

  /// 回放中的底栏：跟着游标走的瞬时读数。
  List<({String value, String unit, String label})> _liveMetrics(
    RideGlanceLive s,
  ) {
    final dash = LocaleKeys.rideStatDash.tr;
    return [
      (
        value: s.speedKmh == null ? dash : s.speedKmh!.toStringAsFixed(1),
        unit: s.speedKmh == null ? '' : 'km/h',
        label: LocaleKeys.rideMetricSpeed.tr,
      ),
      (
        value: s.cadenceRpm == null ? dash : '${s.cadenceRpm}',
        unit: s.cadenceRpm == null ? '' : 'rpm',
        label: LocaleKeys.rideMetricCadence.tr,
      ),
      (
        value: s.powerW == null ? dash : '${s.powerW}',
        unit: s.powerW == null ? '' : 'W',
        label: LocaleKeys.rideMetricPower.tr,
      ),
      (
        value: s.hrBpm == null ? dash : '${s.hrBpm}',
        unit: s.hrBpm == null ? '' : 'BPM',
        label: LocaleKeys.rideMetricHr.tr,
      ),
    ];
  }

  Widget _metrics(
    List<({String value, String unit, String label})> metrics, {
    bool dense = false,
  }) {
    return RideGlancePanel(
      child: Padding(
        padding: EdgeInsets.symmetric(vertical: dense ? 8 : 14),
        child: Row(
          children: [
            for (var i = 0; i < metrics.length; i++) ...[
              if (i > 0)
                Container(width: 1, height: 36, color: const Color(0x33FFFFFF)),
              Expanded(
                child: RideGlanceCell(
                  value: metrics[i].value,
                  unit: metrics[i].unit,
                  label: metrics[i].label,
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }
}

/// 等宽毛玻璃底板。
class RideGlancePanel extends StatelessWidget {
  /// 创建底板。
  const RideGlancePanel({super.key, required this.child});

  final Widget child;

  @override
  Widget build(BuildContext context) {
    return ClipRRect(
      borderRadius: BorderRadius.circular(AppTheme.radius),
      child: BackdropFilter(
        filter: ui.ImageFilter.blur(sigmaX: 18, sigmaY: 18),
        child: DecoratedBox(
          decoration: BoxDecoration(
            color: const Color(0xA80C0C0E),
            borderRadius: BorderRadius.circular(AppTheme.radius),
          ),
          child: child,
        ),
      ),
    );
  }
}

/// 四格里的一格。
class RideGlanceCell extends StatelessWidget {
  /// 创建一格。
  const RideGlanceCell({
    super.key,
    required this.value,
    required this.unit,
    required this.label,
  });

  final String value;
  final String unit;
  final String label;

  @override
  Widget build(BuildContext context) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        Text.rich(
          TextSpan(
            text: value,
            style: const TextStyle(
              color: Colors.white,
              fontSize: 16,
              fontWeight: FontWeight.w800,
              height: 1.1,
            ),
            children: [
              if (unit.isNotEmpty)
                TextSpan(
                  text: ' $unit',
                  style: TextStyle(
                    color: Colors.white.withValues(alpha: 0.55),
                    fontSize: 10,
                    fontWeight: FontWeight.w600,
                  ),
                ),
            ],
          ),
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          textAlign: TextAlign.center,
        ),
        const SizedBox(height: 4),
        Text(
          label,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          textAlign: TextAlign.center,
          style: TextStyle(
            color: Colors.white.withValues(alpha: 0.58),
            fontSize: 11,
            fontWeight: FontWeight.w500,
          ),
        ),
      ],
    );
  }
}
