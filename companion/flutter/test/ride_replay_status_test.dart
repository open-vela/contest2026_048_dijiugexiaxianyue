import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

/// 回放状态机的前提：`forward(from: 0)` 从完成态重置时，会**同步**经过
/// `dismissed`。
///
/// 详情页的 statusListener 因此只处理 `completed`：如果把 `dismissed` 也当
/// 「播完」，重播的第一步就会把刚建立的播放态清掉 —— 地图照常重播，但底栏
/// 退回汇总口径、`_onPlayTick` 直接提前返回（Codex 审查抓到的 P1）。
/// 这条测试锁住这个易踩的 API 语义，改状态机前先看它。
void main() {
  testWidgets('forward(from: 0) 从完成态重置时会同步报 dismissed', (tester) async {
    final controller = AnimationController(
      vsync: const TestVSync(),
      duration: const Duration(milliseconds: 100),
    );
    addTearDown(controller.dispose);

    final seen = <AnimationStatus>[];
    controller.addStatusListener(seen.add);

    controller.value = 1.0; // 播完
    expect(controller.status, AnimationStatus.completed);
    seen.clear();

    controller.forward(from: 0); // 重播
    expect(seen, contains(AnimationStatus.dismissed));
    expect(seen, contains(AnimationStatus.forward));

    await tester.pumpAndSettle();
    expect(seen.last, AnimationStatus.completed);
  });
}
