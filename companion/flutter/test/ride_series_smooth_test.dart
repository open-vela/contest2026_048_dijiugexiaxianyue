import 'dart:math' as math;

import 'package:flutter_test/flutter_test.dart';
import 'package:sifli_companion/ride/ride_series_smooth.dart';

/// 平滑链路的锁：对每个函数都锁住「不制造新问题」的底线。
///
/// 这类数值代码在真机上出错只会表现为「曲线看着怪」，很难复现，
/// 所以把去尖峰、端点、降采样边界都写成可直接跑的断言。
void main() {
  group('边界输入', () {
    test('空序列返回空，不抛异常', () {
      expect(RideSeriesSmooth.despike(const <double>[]), isEmpty);
      expect(RideSeriesSmooth.smooth(const <double>[]), isEmpty);
      expect(RideSeriesSmooth.downsample(const <double>[], 160), isEmpty);
      expect(RideSeriesSmooth.prepare(const <double>[]), isEmpty);
    });

    test('单点原样返回，且是新的 list', () {
      final src = <double>[42];
      final outs = <List<double>>[
        RideSeriesSmooth.despike(src),
        RideSeriesSmooth.smooth(src),
        RideSeriesSmooth.downsample(src, 160),
        RideSeriesSmooth.prepare(src),
      ];
      for (final out in outs) {
        expect(out, <double>[42]);
        expect(identical(out, src), isFalse);
      }
    });

    test('两点不平滑，避免把仅有的差异抹平', () {
      final src = <double>[0, 10];
      expect(RideSeriesSmooth.despike(src), src);
      expect(RideSeriesSmooth.smooth(src), src);
      expect(RideSeriesSmooth.prepare(src), src);
    });
  });

  group('常数序列', () {
    test('各环节都不引入漂移（< 1e-9）', () {
      const c = 7.3;
      final src = List<double>.filled(64, c);
      final outs = <List<double>>[
        RideSeriesSmooth.despike(src),
        RideSeriesSmooth.smooth(src),
        RideSeriesSmooth.downsample(src, 16),
        RideSeriesSmooth.prepare(src),
      ];
      for (final out in outs) {
        expect(out, isNotEmpty);
        for (final v in out) {
          expect(v, closeTo(c, 1e-9));
        }
      }
    });
  });

  group('despike：去孤立尖峰', () {
    test('平坦 20 中间一个 90：峰值至少砍半，平坦段不动', () {
      final src = List<double>.filled(21, 20);
      src[10] = 90;
      final out = RideSeriesSmooth.despike(src);
      expect(out[10], lessThan(45));
      expect(out[10], closeTo(20, 1e-9));
      for (var i = 0; i < out.length; i++) {
        if (i == 10) continue;
        expect(out[i], closeTo(20, 0.5));
      }
    });

    test('均匀斜坡不把趋势当尖峰削掉', () {
      final ramp = [for (var i = 0; i < 40; i++) i.toDouble()];
      final out = RideSeriesSmooth.despike(ramp);
      for (var i = 0; i < ramp.length; i++) {
        expect(out[i], closeTo(ramp[i], 1e-9));
      }
    });

    test('连续两秒的 0（滑行 / 红灯）不能被补成邻居的值', () {
      // 平坦段 MAD=0 ⇒ 阈值 0，任何偏离都会被判成尖峰。判据必须给「掉到 0」
      // 开一个口子，否则 [80,80,80,0,0,80,80,80] 会被抹平成 80 ——
      // 用户看到的是「一路没停」。
      const src = <double>[80, 80, 80, 0, 0, 80, 80, 80];
      final out = RideSeriesSmooth.despike(src);
      expect(out, src);
    });

    test('单点尖峰要摘掉', () {
      final src = List<double>.filled(9, 20);
      src[4] = 90;
      final out = RideSeriesSmooth.despike(src);
      expect(out[4], closeTo(20, 1e-9));
    });

    test('成对跳点也要摘掉（不能靠「是否孤立」判断）', () {
      // 两个相邻跳点互为邻居，按孤立判据谁都不会被替换，3 遍平均 + 降采样后
      // 仍会在 20 km/h 的曲线上留下 ~40 km/h 的假峰（Codex 第二轮抓到的回归）。
      final src = List<double>.filled(318, 20);
      src[159] = 75;
      src[160] = 75;
      final out = RideSeriesSmooth.despike(src);
      expect(out[159], closeTo(20, 1e-9));
      expect(out[160], closeTo(20, 1e-9));
    });

    test('被正值夹住的孤立 0 是掉点，要按尖峰摘掉', () {
      // 只保护「成片的 0」：孤立的 0 保留下来会在平坦曲线上做出 18% 的假谷，
      // 而纵轴自适应会把这个谷放大到整个卡片高度。
      final src = List<double>.filled(9, 20);
      src[4] = 0;
      final out = RideSeriesSmooth.despike(src);
      expect(out[4], closeTo(20, 1e-9));
    });
  });

  group('smooth：加权滑动平均', () {
    test('端点用缩短窗口而不是补零', () {
      // 补零时首点会掉到 100*3/9≈33.3，这里必须还是 100。
      final flat = RideSeriesSmooth.smooth(List<double>.filled(9, 100));
      for (final v in flat) {
        expect(v, closeTo(100, 1e-9));
      }

      // 斜坡首尾贴着真实端点：首点 51.33，末点 86.67（一遍）。
      final ramp = [for (var i = 0; i < 20; i++) 50 + i * 2.0];
      final out = RideSeriesSmooth.smooth(ramp, passes: 1);
      expect(out.first, closeTo(51.3333, 0.01));
      expect(out.last, closeTo(86.6667, 0.01));
    });

    test('斜坡平滑后仍单调不减', () {
      final ramp = [for (var i = 0; i < 60; i++) 10 + i * 0.5];
      final out = RideSeriesSmooth.smooth(ramp);
      for (var i = 1; i < out.length; i++) {
        expect(out[i], greaterThanOrEqualTo(out[i - 1]));
      }
    });
  });

  group('downsample：分桶平均', () {
    test('首尾样本原值保留，长度不超过上限', () {
      final src = [for (var i = 0; i < 1000; i++) i * 0.1];
      src[0] = -5;
      src[999] = 500; // 末端尖峰也要原样留住，否则回放看着像断尾
      final out = RideSeriesSmooth.downsample(src, 100);
      expect(out.length, lessThanOrEqualTo(100));
      expect(out.length, 100);
      expect(out.first, -5);
      expect(out.last, 500);
    });

    test('中间按桶取均值', () {
      final src = <double>[0, 10, 20, 30, 40, 50];
      // 首尾各占一个单点桶，中间 4 个样本分 2 桶：
      // [10,20]→15，[30,40]→35。
      final out = RideSeriesSmooth.downsample(src, 4);
      expect(out, <double>[0, 15, 35, 50]);
    });

    test('maxN 为 2 时至少两点，且就是首尾', () {
      final src = [for (var i = 0; i < 10; i++) i.toDouble()];
      expect(RideSeriesSmooth.downsample(src, 2), <double>[0, 9]);
    });

    test('短序列不放大', () {
      final src = <double>[1, 2, 3];
      expect(RideSeriesSmooth.downsample(src, 160), src);
      expect(RideSeriesSmooth.downsample(src, 3), src);
    });
  });

  group('prepare：整条链路', () {
    test('1000 点降到默认 160 点以内', () {
      final big = [for (var i = 0; i < 1000; i++) i.toDouble()];
      final out = RideSeriesSmooth.prepare(big);
      expect(out.length, lessThanOrEqualTo(160));
      expect(out.length, 160);
    });

    test('短于 maxN 时不放大', () {
      final src = [for (var i = 0; i < 20; i++) i * 1.5];
      expect(RideSeriesSmooth.prepare(src).length, 20);
    });

    test('短序列也不能突破 maxN', () {
      // 提前返回的分支曾经绕过上限：prepare([1,2], maxN: 1) 会返回两个点。
      expect(RideSeriesSmooth.prepare(const [1, 2], maxN: 1).length, 1);
      expect(RideSeriesSmooth.prepare(const [7], maxN: 0), isEmpty);
      expect(RideSeriesSmooth.prepare(const [1, 2, 3], maxN: 2).length, 2);
    });

    test('成对跳点走完整链路后不留假峰', () {
      final src = List<double>.filled(318, 20);
      src[159] = 75;
      src[160] = 75;
      final out = RideSeriesSmooth.prepare(src);
      expect(out.reduce(math.max), closeTo(20, 0.5));
    });

    test('滑行的 0 走完整链路后仍是低谷（不会被填平）', () {
      final src = List<double>.filled(120, 80);
      src[60] = 0;
      src[61] = 0;
      final out = RideSeriesSmooth.prepare(src);
      expect(out.reduce(math.min), lessThan(60));
    });

    test('孤立单点掉零走完整链路后不留假谷', () {
      // Codex 第三轮的反例：20 km/h 的平坦序列里一个 0，若保留，
      // 3 遍平均 + 降采样后是 16.34，纵轴自适应后能占满整个卡片。
      final src = List<double>.filled(318, 20);
      src[159] = 0;
      final out = RideSeriesSmooth.prepare(src);
      expect(out.reduce(math.min), closeTo(20, 0.5));
      expect(out.reduce(math.max), closeTo(20, 0.5));
    });

    test('斜坡仍单调不减（Hampel 不削趋势）', () {
      final ramp = [for (var i = 0; i < 60; i++) 10 + i * 0.7];
      final out = RideSeriesSmooth.prepare(ramp);
      for (var i = 1; i < out.length; i++) {
        expect(out[i], greaterThanOrEqualTo(out[i - 1]));
      }
    });

    test('不越界外插：min/max 不超出原始范围', () {
      final wave = [for (var i = 0; i < 500; i++) 30 + 12 * math.sin(i / 9)];
      final out = RideSeriesSmooth.prepare(wave);
      final lo = wave.reduce(math.min);
      final hi = wave.reduce(math.max);
      expect(out.reduce(math.min), greaterThanOrEqualTo(lo - 1e-9));
      expect(out.reduce(math.max), lessThanOrEqualTo(hi + 1e-9));
    });
  });
}
