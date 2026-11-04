import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';

/// 统计区间的选择器：`◀  2026/8  ▶`，右侧「回到当前」只在看历史时出现。
///
/// 周 / 月都用它，免得两处各写一套箭头和禁用逻辑。
class RidePeriodHeader extends StatelessWidget {
  /// 创建区间选择器。
  const RidePeriodHeader({
    super.key,
    required this.label,
    required this.canForward,
    required this.onStep,
    this.canBack = true,
    this.onNow,
  });

  /// 区间名，如 `本周` / `2026/8` / `8/17 – 8/23`。
  final String label;

  /// 能否往后（不允许看未来）。
  final bool canForward;

  /// 能否往前（再往前没有记录了就别让点）。
  final bool canBack;

  /// 往前 / 往后挪一个周期：-1 往前，+1 往后。
  final void Function(int delta) onStep;

  /// 回到当前区间；为 null 表示已经在当前区间（不显示按钮）。
  final VoidCallback? onNow;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        _arrow(Icons.chevron_left, canBack ? () => onStep(-1) : null),
        Expanded(
          child: Text(
            label,
            key: const ValueKey('ridePeriodLabel'),
            textAlign: TextAlign.center,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: const TextStyle(
              color: Colors.white,
              fontSize: 15,
              fontWeight: FontWeight.w700,
            ),
          ),
        ),
        _arrow(Icons.chevron_right, canForward ? () => onStep(1) : null),
        if (onNow != null)
          TextButton(
            onPressed: onNow,
            style: TextButton.styleFrom(
              padding: const EdgeInsets.symmetric(horizontal: 8),
              minimumSize: const Size(0, 32),
              tapTargetSize: MaterialTapTargetSize.shrinkWrap,
            ),
            child: Text(
              LocaleKeys.ridePeriodNow.tr,
              style: const TextStyle(fontSize: 13),
            ),
          )
        else
          const SizedBox(width: 8),
      ],
    );
  }

  Widget _arrow(IconData icon, VoidCallback? onTap) {
    final on = onTap != null;
    return IconButton(
      onPressed: onTap,
      visualDensity: VisualDensity.compact,
      style: IconButton.styleFrom(
        backgroundColor: AppTheme.field,
        foregroundColor: on ? Colors.white : AppTheme.muted,
        disabledBackgroundColor: AppTheme.field.withValues(alpha: 0.45),
        disabledForegroundColor: AppTheme.muted.withValues(alpha: 0.5),
      ),
      icon: Icon(icon, size: 20),
    );
  }
}
