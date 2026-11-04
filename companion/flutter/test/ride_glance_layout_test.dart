import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:sifli_companion/ride/ride_record_store.dart';
import 'package:sifli_companion/ui/ride/ride_glance.dart';

/// 回放页地图浮层在矮窗口/大字号下不许溢出。
///
/// 横屏、分屏、系统字体放大都会把可用的高度压得很小；浮层里有顶栏、
/// 播放器和四格数据条三段，按「扣掉抽屉让位后的剩余高度」逐段降级。
/// 这里把边界高度都过一遍 —— RenderFlex 溢出在真机上只是一条黄黑条纹，
/// 很容易漏过去。
void main() {
  final rec = RideRecord(
    fileName: 'a.gpx',
    localPath: '/tmp/a.gpx',
    size: 1,
    mtime: 1790000000,
    distanceKm: 1,
  );

  testWidgets('各种高度都不溢出', (tester) async {
    // 临界点：140/220 只能放底栏；284/320 刚好跨过「顶栏出现」的边界
    // （普通底栏 64 + 播放器 54 + 顶栏 52 = 170 预算）。
    // 临界点：inset=104 时 168~171 正好跨过「顶栏出现」的边界
    // （普通底栏 65.3 + 播放器 54 + 顶栏 52 ≈ 171.3 预算）。
    for (final h in [
      140.0,
      180.0,
      220.0,
      260.0,
      284.0,
      294.0,
      295.0,
      296.0,
      297.0,
      298.0,
      320.0,
      400.0,
      700.0,
    ]) {
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SizedBox(
              width: 360,
              height: h,
              child: Stack(
                children: [
                  const ColoredBox(color: Colors.black),
                  RideGlanceOverlay(
                    rec: rec,
                    bottomInset: 104,
                    player: const SizedBox(height: 46),
                    onDragUpdate: (_) {},
                    onDragEnd: (_) {},
                  ),
                ],
              ),
            ),
          ),
        ),
      );
      await tester.pump();
      expect(tester.takeException(), isNull, reason: 'height=$h');
      // 顶栏（另一块 RideGlancePanel）只在普通态出现：
      // inset=104 时 avail 168~171 都是紧凑态，172 起才显示顶栏。
      final avail = h - 104 - 22;
      // 0 = 什么都不画（极矮）；1 = 只有数据条；2 = 数据条 + 顶栏。
      final expected = avail >= 172 ? 2 : (avail >= 54 ? 1 : 0);
      expect(
        find.byType(RideGlancePanel).evaluate().length,
        expected,
        reason: 'height=$h 浮层块数不对',
      );
    }
  });

  testWidgets('分享封面在任何高度都保留顶栏（标题/品牌）', (tester) async {
    for (final h in [140.0, 141.0, 200.0, 320.0, 900.0]) {
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SizedBox(
              width: 360,
              height: h,
              child: RideGlanceOverlay(rec: rec, forShare: true),
            ),
          ),
        ),
      );
      await tester.pump();
      expect(tester.takeException(), isNull, reason: 'share height=$h');
      expect(
        find.text(rec.title),
        findsOneWidget,
        reason: '分享封面必须显示标题 height=$h',
      );
    }
  });

  testWidgets('大字号下也不溢出', (tester) async {
    await tester.pumpWidget(
      MaterialApp(
        home: MediaQuery(
          data: const MediaQueryData(textScaler: TextScaler.linear(1.6)),
          child: Scaffold(
            body: SizedBox(
              width: 360,
              height: 260,
              child: RideGlanceOverlay(
                rec: rec,
                bottomInset: 104,
                player: const SizedBox(height: 46),
                onDragUpdate: (_) {},
                onDragEnd: (_) {},
              ),
            ),
          ),
        ),
      ),
    );
    await tester.pump();
    expect(tester.takeException(), isNull);
  });
}
