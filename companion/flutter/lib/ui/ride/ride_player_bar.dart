import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';

/// 回放播放器条：播放/暂停 + 可拖动进度 + 当前位置里程。
///
/// 常见骑行 App 的做法 —— 地图下方一条细进度条，拖动即定位到那一刻，
/// 比「只能从头播一遍」有用得多。本组件只负责画和回调，状态都在页面里。
class RidePlayerBar extends StatelessWidget {
  /// 创建播放器条。
  const RidePlayerBar({
    super.key,
    required this.playing,
    required this.progress,
    required this.label,
    required this.onToggle,
    required this.onSeek,
    required this.onSeekEnd,
    this.enabled = true,
  });

  /// 是否正在走（false 时按钮显示播放图标）。
  final bool playing;

  /// 0..1 的回放进度。
  final double progress;

  /// 游标处的里程文案，如 `1.24 km`；空串则不占位。
  final String label;

  final VoidCallback onToggle;

  /// 拖动中连续回调（0..1）。
  final ValueChanged<double> onSeek;

  /// 松手回调（0..1）。
  final ValueChanged<double> onSeekEnd;

  /// 轨迹不足两点时禁用。
  final bool enabled;

  @override
  Widget build(BuildContext context) {
    final done = progress >= 0.999;
    return Container(
      height: 46,
      decoration: BoxDecoration(
        // 不叠毛玻璃：地图上方已经有两块 BackdropFilter，第三条会明显加重复合。
        color: const Color(0xE00C0C0E),
        borderRadius: BorderRadius.circular(14),
      ),
      child: Row(
        children: [
          IconButton(
            tooltip: LocaleKeys.ridePlayPause.tr,
            onPressed: enabled ? onToggle : null,
            iconSize: 22,
            visualDensity: VisualDensity.compact,
            padding: const EdgeInsets.only(left: 6, right: 2),
            icon: Icon(
              done ? Icons.replay_rounded : (playing ? Icons.pause : Icons.play_arrow),
              color: enabled ? Colors.white : const Color(0x66FFFFFF),
            ),
          ),
          Expanded(
            child: SliderTheme(
              data: SliderThemeData(
                trackHeight: 3,
                activeTrackColor: AppTheme.accent,
                inactiveTrackColor: const Color(0x33FFFFFF),
                thumbColor: Colors.white,
                overlayColor: AppTheme.accent.withValues(alpha: 0.18),
                thumbShape: const RoundSliderThumbShape(enabledThumbRadius: 6.5),
                overlayShape: const RoundSliderOverlayShape(overlayRadius: 14),
                trackShape: const RoundedRectSliderTrackShape(),
              ),
              child: Slider(
                value: progress.clamp(0.0, 1.0),
                onChanged: enabled ? onSeek : null,
                onChangeEnd: enabled ? onSeekEnd : null,
              ),
            ),
          ),
          if (label.isNotEmpty)
            Padding(
              padding: const EdgeInsets.only(right: 12),
              child: Text(
                label,
                style: const TextStyle(
                  color: Color(0xCCFFFFFF),
                  fontSize: 11,
                  fontWeight: FontWeight.w600,
                  fontFeatures: [FontFeature.tabularFigures()],
                ),
              ),
            ),
        ],
      ),
    );
  }
}
