import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:sifli_companion/ui/ride/ride_charts.dart';

/// 曲线画笔的冒烟测试。
///
/// 平滑与单调三次曲线是纯几何代码，最怕的是「某种点数下除零 / 路径非法」——
/// 那种错误在真机上只表现为一张空白卡片，很难定位。这里把边界点数都过一遍。
void main() {
  Future<void> paint(WidgetTester tester, List<double> values) async {
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: SizedBox(
            width: 320,
            height: 132,
            child: RideSeriesChart(
              values: values,
              color: const Color(0xFF3DDC84),
              fill: true,
              avg: 20,
            ),
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
  }

  testWidgets('空 / 单点 / 两点 / 大量点都能画出来', (tester) async {
    await paint(tester, const []);
    await paint(tester, const [12]);
    await paint(tester, const [12, 30]);
    await paint(tester, const [12, 30, 12]);
    await paint(tester, [for (var i = 0; i < 600; i++) 20 + (i % 17)]);
  });

  testWidgets('常数序列不产生 NaN 路径', (tester) async {
    await paint(tester, List<double>.filled(200, 21.5));
  });
}
