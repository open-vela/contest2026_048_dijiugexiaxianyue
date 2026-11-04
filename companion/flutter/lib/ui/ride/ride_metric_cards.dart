import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ui/ride/ride_charts.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';

/// 数据卡片：标题 + 均值/峰值 + 一条平滑曲线（可选附加块，如心率五区）。
///
/// 四张卡（速度 / 心率 / 功率 / 踏频）共用这一套视觉，只换图标、颜色和文案；
/// 回放游标从详情页透传下来，卡片里的曲线会跟着游标画竖线。
class RideMetricCard extends StatelessWidget {
  const RideMetricCard({
    super.key,
    required this.icon,
    required this.color,
    required this.title,
    required this.values,
    required this.avg,
    required this.max,
    required this.avgLabel,
    required this.maxLabel,
    this.decimals = 0,
    this.cursor,
    this.extra,
    this.note,
  });

  final IconData icon;
  final Color color;
  final String title;
  final List<double> values;

  /// 均值（图上虚线）。null 表示这次没有有效均值。
  final double? avg;
  final double? max;
  final String avgLabel;
  final String maxLabel;

  /// 统计值保留几位小数（速度 1，其余 0）。
  final int decimals;

  /// 回放游标 0..1；null 不画。
  final ValueListenable<double?>? cursor;

  /// 曲线下面的附加块（心率五区条）。
  final Widget? extra;

  /// 没有数据时替代曲线的说明文字。
  final String? note;

  String _fmt(double? v) {
    if (v == null) return '—';
    return v.toStringAsFixed(decimals);
  }

  @override
  Widget build(BuildContext context) {
    final empty = values.isEmpty;
    Widget chart(double? c) => SizedBox(
          height: 132,
          width: double.infinity,
          child: empty
              ? Center(
                  child: Text(
                    note ?? LocaleKeys.rideStatDash.tr,
                    textAlign: TextAlign.center,
                    style: const TextStyle(
                      color: AppTheme.muted,
                      fontSize: 13,
                      height: 1.4,
                    ),
                  ),
                )
              : RideSeriesChart(
                  values: values,
                  color: color,
                  fill: true,
                  avg: avg,
                  cursor: c,
                ),
        );
    final listenable = cursor;

    return HelmCard(
      padding: const EdgeInsets.fromLTRB(18, 16, 18, 16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(icon, size: 18, color: color),
              const SizedBox(width: 6),
              Text(
                title,
                style: const TextStyle(
                  color: Colors.white,
                  fontSize: 16,
                  fontWeight: FontWeight.w700,
                ),
              ),
            ],
          ),
          const SizedBox(height: 12),
          Row(
            children: [
              Expanded(child: _Stat(value: _fmt(avg), label: avgLabel)),
              Expanded(child: _Stat(value: _fmt(max), label: maxLabel)),
            ],
          ),
          const SizedBox(height: 12),
          if (listenable == null)
            chart(null)
          else
            ValueListenableBuilder<double?>(
              valueListenable: listenable,
              builder: (context, c, _) => chart(c),
            ),
          if (extra != null) ...[
            const SizedBox(height: 12),
            extra!,
          ],
        ],
      ),
    );
  }
}

/// 卡里的一个统计：数值大、标签小。
class _Stat extends StatelessWidget {
  const _Stat({required this.value, required this.label});

  final String value;
  final String label;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Text(
          value,
          style: const TextStyle(
            color: Colors.white,
            fontSize: 26,
            fontWeight: FontWeight.w800,
            height: 1.1,
          ),
        ),
        const SizedBox(width: 8),
        Expanded(
          child: Text(
            label,
            maxLines: 2,
            overflow: TextOverflow.ellipsis,
            style: const TextStyle(
              color: AppTheme.muted,
              fontSize: 13,
              fontWeight: FontWeight.w500,
            ),
          ),
        ),
      ],
    );
  }
}

/// 心率五区横条。
class RideHrZones extends StatelessWidget {
  const RideHrZones({super.key, required this.zones, required this.maxMs});

  final List<Duration> zones;
  final int maxMs;

  static const _colors = [
    Color(0xFF5B8DEF),
    Color(0xFF3DDC84),
    Color(0xFFFFD54A),
    Color(0xFFFF8A3D),
    Color(0xFFFF4D4D),
  ];

  @override
  Widget build(BuildContext context) {
    final names = [
      LocaleKeys.rideZoneWarmup.tr,
      LocaleKeys.rideZoneFat.tr,
      LocaleKeys.rideZoneAerobic.tr,
      LocaleKeys.rideZoneAnaerobic.tr,
      LocaleKeys.rideZoneExtreme.tr,
    ];
    return Column(
      children: [
        for (var i = 0; i < 5; i++)
          RideZoneBar(
            label: names[i],
            color: _colors[i],
            time: zones.length > i ? zones[i] : Duration.zero,
            maxMs: maxMs < 1 ? 1 : maxMs,
          ),
      ],
    );
  }
}
