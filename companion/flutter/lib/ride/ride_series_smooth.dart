import 'dart:math' as math;

/// 骑行回放曲线的平滑工具：抹掉 GNSS 抖动和单采样离群值。
///
/// 放在 ride 层而不是画笔里，是因为这一层全是纯函数、能单测；
/// 画笔只负责画线，不该夹带数据清洗。所有函数都不修改入参。
abstract class RideSeriesSmooth {
  /// 权重核 [1,2,3,2,1]：中心权重略高，两端衰减，削毛刺但不压住平台。
  static const List<double> _kernel = <double>[1, 2, 3, 2, 1];

  /// 核的单侧半径，与 [_kernel] 的长度一一对应。
  static const int _radius = 2;

  /// Hampel 去尖峰：|v - median| > [sigmas] * 1.4826 * MAD 时替换为窗口
  /// median。[halfWindow] 是单侧窗口（窗口 = 2*halfWindow+1）。
  ///
  /// 用中位数而不是均值，尖峰本身就不会把判定基准带偏；1.4826 把 MAD
  /// 折算成正态分布下的标准差，[sigmas] 才能按「几倍标准差」理解。
  /// 所有替换都基于原始序列同时判定，避免前一个修补值污染后一个窗口。
  ///
  /// **连续 ≥2 个 0 才是滑行/停车**：平坦段 MAD=0 ⇒ 阈值 0，任何偏离都会被
  /// 替换，`[80,80,80,0,0,80,80,80]`（两秒滑行）会被抹平成 80，速度图上的
  /// 红绿灯停车同理。所以「成片的 0」保留。
  ///
  /// 但**被正值夹住的孤立 0 是掉点噪声**，必须照常替换：否则 20 km/h 的曲线里
  /// 一个瞬时 0 会在 3 遍平均后变成 16.3，而纵轴自适应只剩 16.3–20 这段，
  /// 一个点就能在 132 px 的卡片里做出 100 px 深的假谷。
  ///
  /// 高值跳点不区分单点/成对：偏离超过本窗口阈值就替换。仍有两种情况会存活 ——
  /// 连续三点以上的高值占据窗口多数，或旁边有成片 0 把 MAD 撑大
  /// （`[80,80,0,0,300,80,80]` 的阈值被抬到 ~356，300 会保留）。
  /// 这两种都不是 1 Hz 采样能产生的 GNSS 跳点形态，按真实数据处理。
  static List<double> despike(
    List<double> src, {
    int halfWindow = 2,
    double sigmas = 3.0,
  }) {
    final n = src.length;
    // 两点之间没有「邻域」可言，强行去尖峰只会把仅有的差异抹平。
    if (n <= 2) return List<double>.of(src);
    final half = math.max(0, halfWindow);
    final out = List<double>.of(src);
    for (var i = 0; i < n; i++) {
      final lo = math.max(0, i - half);
      final hi = math.min(n - 1, i + half);
      final window = src.sublist(lo, hi + 1)..sort();
      final median = _medianSorted(window);
      final devs = <double>[
        for (final v in window) (v - median).abs(),
      ]..sort();
      final limit = sigmas * 1.4826 * _medianSorted(devs);
      if ((src[i] - median).abs() <= limit) continue;
      // 停车 / 滑行：成片（≥2 个采样）的 0 是真实事件，整段保留。
      if (src[i] <= 0 && median > 0) {
        final prevZero = i > 0 && src[i - 1] <= 0;
        final nextZero = i < n - 1 && src[i + 1] <= 0;
        if (prevZero || nextZero) continue;
      }
      out[i] = median;
    }
    return out;
  }

  /// 加权滑动平均（核 [1,2,3,2,1]/9），边界自动缩短窗口，跑 [passes] 遍。
  ///
  /// 边界只取真实存在的邻点并重新归一化权值：补零会把端点朝 0 拽，
  /// 曲线首尾就会凭空多出一段下坠。多跑一遍是为了把单点毛刺摊得更开。
  static List<double> smooth(List<double> src, {int passes = 2}) {
    if (src.length <= 2) return List<double>.of(src);
    var cur = List<double>.of(src);
    for (var p = 0; p < math.max(0, passes); p++) {
      cur = _smoothOnce(cur);
    }
    return cur;
  }

  static List<double> _smoothOnce(List<double> src) {
    final n = src.length;
    final out = List<double>.filled(n, 0);
    for (var i = 0; i < n; i++) {
      var sum = 0.0;
      var weight = 0.0;
      for (var k = 0; k < _kernel.length; k++) {
        final j = i + k - _radius;
        if (j < 0 || j >= n) continue;
        final w = _kernel[k];
        sum += src[j] * w;
        weight += w;
      }
      out[i] = sum / weight;
    }
    return out;
  }

  /// 分桶平均降采样：每桶取均值，首尾样本原值保留，输出长度 ≤ [maxN]。
  ///
  /// 首尾各占一个单点桶：曲线必须从第一个真实样本起、落到最后一个真实
  /// 样本上，否则尾段会被吞掉，看起来像「回放没画完」。中间等分成
  /// maxN-2 个桶做均值，整数除法对齐边界，既不漏样本也不越界。
  static List<double> downsample(List<double> src, int maxN) {
    final n = src.length;
    if (n == 0 || maxN <= 0) return const [];
    if (n <= maxN) return List<double>.of(src);
    // 只有一个输出点时保不住两端，退化为首样本，至少不凭空造值。
    if (maxN == 1) return <double>[src.first];
    final out = List<double>.filled(maxN, 0);
    out[0] = src[0];
    out[maxN - 1] = src[n - 1];
    final buckets = maxN - 2;
    final inner = n - 2;
    for (var b = 0; b < buckets; b++) {
      final lo = 1 + (b * inner) ~/ buckets;
      final hi = 1 + ((b + 1) * inner) ~/ buckets;
      var sum = 0.0;
      for (var i = lo; i < hi; i++) {
        sum += src[i];
      }
      out[b + 1] = sum / (hi - lo);
    }
    return out;
  }

  /// 组合：despike → smooth → downsample。
  ///
  /// 顺序有讲究：先摘掉孤立尖峰，否则平滑会把一个尖峰摊成一圈小鼓包；
  /// 最后再降采样，画笔每帧只需处理 [maxN] 个点（默认 160 够宽屏用）。
  ///
  /// 默认跑 3 遍平均：1 Hz 采样的速度序列做 2 遍时，峰谷之间残留的锯齿在
  /// 130 px 高的卡片里仍然像振动（实测肉眼可辨）；3 遍之后曲线才读得出趋势，
  /// 而真实的加减速段（几十个采样宽）不会被抹掉。
  static List<double> prepare(List<double> src, {int maxN = 160, int passes = 3}) {
    if (src.isEmpty) return const [];
    // 两个点连「平滑」都谈不上，保持原样最不容易出错 —— 但点数上限仍然生效，
    // 否则 `prepare([a, b], maxN: 1)` 会返回两个点，破坏「≤ maxN」的约定。
    if (src.length <= 2) return downsample(List<double>.of(src), maxN);
    return downsample(smooth(despike(src), passes: passes), maxN);
  }

  /// 已排序序列的中位数；偶数长度取中间两个的均值。
  static double _medianSorted(List<double> sorted) {
    final mid = sorted.length ~/ 2;
    if (sorted.length.isOdd) return sorted[mid];
    return (sorted[mid - 1] + sorted[mid]) / 2;
  }
}
