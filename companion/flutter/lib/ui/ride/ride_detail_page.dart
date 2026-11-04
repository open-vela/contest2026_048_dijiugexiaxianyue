import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:get/get.dart';
import 'package:latlong2/latlong.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';
import 'package:share_plus/share_plus.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/map/osm_tiles.dart';
import 'package:sifli_companion/ride/gpx_library_store.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/ride/ride_record_store.dart';
import 'package:sifli_companion/ui/ride/import_upload.dart';
import 'package:sifli_companion/ui/ride/ride_charts.dart';
import 'package:sifli_companion/ui/ride/ride_glance.dart';
import 'package:sifli_companion/ui/ride/ride_metric_cards.dart';
import 'package:sifli_companion/ui/ride/ride_player_bar.dart';
import 'package:sifli_companion/ui/ride/ride_poster.dart';
import 'package:sifli_companion/ui/ride/ride_track_style.dart';
import 'package:sifli_companion/ui/shell/shell_controller.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';

/// 一条本机骑行记录的详情：地图为主，底部条上滑把地图挤到半屏看数据。
class RideDetailPage extends StatefulWidget {
  /// 创建详情页。
  const RideDetailPage({super.key});

  @override
  State<RideDetailPage> createState() => _RideDetailPageState();
}

class _RideDetailPageState extends State<RideDetailPage>
    with TickerProviderStateMixin {
  final _shotKey = GlobalKey();
  final _map = MapController();
  final _shareMap = MapController();
  RideRecord? _rec;
  GpxSummary? _sum;
  List<LatLng> _pts = const [];
  List<LatLng> _disp = const [];
  bool _sharing = false;

  /// GPX 读不出来 / 点太少。以前直接 return，页面只剩一张空地图和一堆横杠。
  bool _loadFailed = false;

  late final AnimationController _sheet;

  /// 轨迹回放：0..1 = 走完整条。进场自动播一次，最长 [_playMaxSec] 秒。
  late final AnimationController _play;

  /// 回放是否在跑。游标静止时底栏回落到汇总口径，靠它切换。
  bool _playing = false;

  /// 播放被暂停（用户按暂停，或抽屉展开时自动暂停）。暂停时 `_playing` 仍为
  /// true —— 回放会继续播，只是停在当前帧。
  bool _paused = false;

  /// 本次暂停是抽屉展开引起的：收起时自动续播，用户手动暂停的则不动。
  bool _pausedBySheet = false;

  /// 正在拖进度条，以及拖之前是不是在播（松手要不要续播靠它）。
  bool _seeking = false;
  bool _seekWasPlaying = false;

  /// 回放中的瞬时读数。用 ValueNotifier 单独推给底栏那四格 ——
  /// 整页 60 fps 重建会把地图、抽屉和毛玻璃一起带上。
  final ValueNotifier<RideGlanceLive?> _liveHud = ValueNotifier(null);

  /// 播放器条的进度与游标里程，同样单独推，避免整页重画。
  final ValueNotifier<double> _progress = ValueNotifier(0);

  /// 图表游标：**轨迹点位置**的 0..1（不是时间比例）。图表的横轴是等距点，
  /// 用时间比例去定位会在缺测/变速时和曲线错开。
  final ValueNotifier<double> _cursorFrac = ValueNotifier(0);
  final ValueNotifier<String> _seekLabel = ValueNotifier('');
  int _hudTick = 0;

  /// 回放压缩上限：再长的骑行也在 10 秒内播完。
  static const _playMaxSec = 10.0;

  /// 每个点的累计骑行秒数（由「距离 / 速度」还原的真实节奏）。
  List<double> _rideT = const [];
  double _rideTotal = 0;

  /// 每个点的累计里程（km），播放器条的游标读数用它。
  List<double> _rideD = const [];

  /// 整条轨迹的色标。加载时算一次，回放只做切片（逐帧重算平滑会掉帧）。
  List<Color> _stops = const [];

  /// 公里牌位置。同样是整条轨迹量级的计算，加载时算一次。
  List<({LatLng ll, int km, double at})> _kmMarks = const [];

  /// 初始相机按 padding 缓存：`CameraFit.bounds().fit()` 要投影整条轨迹，
  /// 抽屉每一帧拖动 / 回放每一帧重建都会重新调它，而它只在首帧有用。
  final Map<EdgeInsets, ({LatLng center, double zoom})> _camCache = {};

  @override
  void initState() {
    super.initState();
    final name = Get.arguments as String?;
    if (name != null) {
      // 先看骑行记录；没有就去 GPX 库找 —— 那边（导入的、地图上画好的）文件
      // 不在这张表里，以前点开是空白页。
      _rec = RideRecordStore().find(name) ?? _fromLibrary(name);
    }
    _loadPts();
    _sheet = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 280),
    )..addStatusListener((status) {
        // 只在「收回 peek」时重算取景。以前 completed（拉满）也会调，
        // 那一帧地图按抽屉挤出来的高度做 fit —— 拉满时高度接近 0，
        // 整条轨迹被塞进一个零高度视口，缩到 minZoom（全国地图）。
        if (status == AnimationStatus.dismissed) _fitLive();
      });
    _sheet.addListener(_onSheetTick);
    _play = AnimationController(vsync: this)
      ..addListener(_onPlayTick)
      ..addStatusListener((status) {
        // 只认 completed。dismissed 不是「播完」，而是 `forward(from: 0)`
        // 重置进度时同步触发的下界回调 —— 重播时若也在这里收尾，会把刚
        // 建立的播放态和 HUD 一起清掉，重播就只剩地图在动。
        if (status != AnimationStatus.completed) return;
        _finishPlayback();
      });
  }

  /// 抽屉拉起来时暂停回放，收回去续播。
  ///
  /// 抽屉过半屏后地图被压到上半屏、底栏读数也被遮住，这时候让动画继续跑
  /// 等于白烧帧，而且用户扯回来时已经不剩多少了。暂停点取 0.9 是留一点提前量：
  /// 底栏在 t ≥ 0.98 才消失，别让用户看着读数停住才发现。
  void _onSheetTick() {
    if (!mounted) return;
    final expanded = _sheet.value >= 0.9;
    if (expanded && !_paused && _playing) {
      _play.stop();
      _paused = true;
      _pausedBySheet = true;
      setState(() {});
    } else if (!expanded && _pausedBySheet) {
      _pausedBySheet = false;
      _paused = false;
      if (_playing) _play.forward();
      setState(() {});
    }
  }

  /// 回放每帧都会回调；底栏读数按 6 帧一次推（≈10 Hz）就够看了。
  ///
  /// 数字在 60 Hz 下大部分帧都是同一个字符串，重建 `Text` 只会白跑一遍排版。
  void _onPlayTick() {
    if (!_playing) return;
    if (++_hudTick % 6 != 0) return;
    final cut = _playCut;
    _liveHud.value = _liveAt(cut);
    _progress.value = _play.value;
    _cursorFrac.value = _cutFraction(cut);
    _seekLabel.value = _distanceLabel(cut);
  }

  /// 小数下标 → 0..1 的「点在轨迹上的位置」。图表横轴与游标都用它。
  double _cutFraction(double cut) {
    final n = _disp.length;
    if (n < 2) return 0;
    return (cut / (n - 1)).clamp(0.0, 1.0);
  }

  /// 回放收尾：只在「真的播完」或「拖到最末端」时执行，幂等。
  ///
  /// 以前这段逻辑只挂在 `completed` 回调里，而把进度直接设成 1 时控制器
  /// 未必再发一次状态变化 —— 于是按钮变回了「重播」，底栏却还停在实时口径。
  void _finishPlayback() {
    _liveHud.value = null;
    _progress.value = 1;
    _cursorFrac.value = 1;
    _seekLabel.value = _distanceLabel(
      _disp.isEmpty ? 0 : _disp.length - 1.0,
    );
    _paused = false;
    if (mounted && _playing) setState(() => _playing = false);
  }

  /// 游标处的里程文案（`1.24 km`）。没有里程信息时给空串。
  String _distanceLabel(double cut) {
    final d = _rideD;
    if (d.length < 2) return '';
    final i = cut.round().clamp(0, d.length - 1);
    final km = d[i];
    if (!(km > 0)) return '';
    return '${km.toStringAsFixed(km < 10 ? 2 : 1)} km';
  }

  /// 游标 [cut] 处的瞬时读数。逐点数组与 `_pts` 等长（解析时对齐）。
  RideGlanceLive _liveAt(double cut) {
    final n = _pts.length;
    if (n == 0) {
      return (speedKmh: null, cadenceRpm: null, powerW: null, hrBpm: null);
    }
    final i = cut.round().clamp(0, n - 1);
    final sum = _sum;
    final speeds = sum?.pointSpeedsKmh ?? const <double>[];
    final cad = sum?.pointCadenceRpm ?? const <int?>[];
    final pw = sum?.pointPowerW ?? const <int?>[];
    final hr = sum?.pointHrBpm ?? const <int?>[];
    return (
      speedKmh: i < speeds.length ? speeds[i] : null,
      cadenceRpm: i < cad.length ? cad[i] : null,
      powerW: i < pw.length ? pw[i] : null,
      hrBpm: i < hr.length ? hr[i] : null,
    );
  }

  /// 在 GPX 库里按文件名找一条（找不到返回 null）。
  static RideRecord? _fromLibrary(String fileName) {
    for (final rec in GpxLibraryStore().items) {
      if (rec.fileName == fileName) return rec;
    }
    return null;
  }

  @override
  void dispose() {
    _map.dispose();
    _shareMap.dispose();
    _sheet.dispose();
    _play.dispose();
    _liveHud.dispose();
    _progress.dispose();
    _cursorFrac.dispose();
    _seekLabel.dispose();
    super.dispose();
  }

  Future<void> _loadPts() async {
    final rec = _rec;
    if (rec == null) return;
    final f = File(rec.localPath);
    if (!await f.exists()) {
      if (mounted) setState(() => _loadFailed = true);
      return;
    }
    final xml = utf8.decode(await f.readAsBytes(), allowMalformed: true);
    final sum = GpxUtil.parse(xml);
    if (sum.points.length < 2) {
      if (mounted) setState(() => _loadFailed = true);
      return;
    }
    final speeds = sum.pointSpeedsKmh;
    // 回放时间轴：按「距离 / 速度」累加，快的地方画得快。
    final rideT = GpxUtil.cumulativeRideSeconds(sum.points, speeds);
    final total = rideT.isEmpty ? 0.0 : rideT.last;
    final rideD = _cumulativeKm(sum.points);
    if (!mounted) return;
    _camCache.clear();
    _liveHud.value = null;
    setState(() {
      _sum = sum;
      _pts = sum.points;
      _disp = HelmOsmTiles.toDisplayAll(sum.points);
      _rideT = rideT;
      _rideTotal = total;
      _rideD = rideD;
      _stops = RideTrackStyle.bandStops(
        speeds,
        fallbackKmh: sum.avgSpeedKmh ?? 18,
      );
      _kmMarks = GpxUtil.kmMarkers(sum.points);
    });
    _startPlayback();
  }

  /// 每个点的累计里程（km）。播放器条的游标读数用它；
  /// 点列是等距采样的说法不成立（停车时连续多个点位置相同），所以必须真算。
  static List<double> _cumulativeKm(List<LatLng> pts) {
    final out = List<double>.filled(pts.length, 0);
    if (pts.length < 2) return out;
    // roundResult 默认 true：它会把每段结果四舍五入到整数。1 Hz 采样的相邻点
    // 通常只差几米，按公里取整后全是 0，累加到最后一个点也还是 0
    // （实测 label 一直为空就是这么来的）。必须关掉。
    const dist = Distance(roundResult: false);
    var acc = 0.0;
    for (var i = 1; i < pts.length; i++) {
      acc += dist.as(LengthUnit.Kilometer, pts[i - 1], pts[i]);
      out[i] = acc;
    }
    return out;
  }

  /// 进场自动回放一次。
  ///
  /// 把整段骑行时长等比压进 [_playMaxSec] 秒内播完：等比压缩保留了各段的快慢差别，
  /// 所以「以运动的速度绘制」这件事在压缩后依然成立。本来就比上限短的按实际时长播。
  void _startPlayback() {
    if (_disp.length < 2) return;
    final secs = _rideTotal > 0 ? _rideTotal : _playMaxSec;
    _play.duration = Duration(
      milliseconds: (secs.clamp(0.6, _playMaxSec) * 1000).round(),
    );
    _hudTick = 0;
    _playing = true;
    _paused = false;
    _pausedBySheet = false;
    _liveHud.value = _liveAt(0);
    _progress.value = 0;
    _cursorFrac.value = 0;
    _seekLabel.value = _distanceLabel(0);
    _play.forward(from: 0);
  }

  /// 重播：底栏读数跟着游标从头走。
  ///
  /// 先 `forward` 再置播放态：`forward(from: 0)` 会把进度拉回下界，状态机
  /// 在这一步同步经过 dismissed，播放态放在它之后建立才不会被重置动作影响。
  void _restartPlayback() {
    if (_disp.length < 2) return;
    _hudTick = 0;
    _play.forward(from: 0);
    setState(() {
      _playing = true;
      _paused = false;
      _pausedBySheet = false;
    });
    _liveHud.value = _liveAt(0);
    _progress.value = 0;
    _cursorFrac.value = 0;
    _seekLabel.value = _distanceLabel(0);
  }

  /// 播放 / 暂停 / 播完了再播一次 —— 播放器条上那一个按钮的全部语义。
  void _togglePlay() {
    if (_disp.length < 2) return;
    if (!_playing) {
      _restartPlayback();
      return;
    }
    if (_paused) {
      setState(() {
        _paused = false;
        _pausedBySheet = false;
      });
      _play.forward();
      return;
    }
    _play.stop();
    setState(() => _paused = true);
  }

  /// 拖动进度条：先停住，跟手改进度，读数随手更新。
  void _onSeek(double value) {
    if (_disp.length < 2) return;
    final v = value.clamp(0.0, 1.0);
    if (!_seeking) {
      // 记住「拖之前是不是在播」：暂停状态下只是拖一下位置，松手不该自己播起来。
      _seeking = true;
      _seekWasPlaying = _playing && !_paused;
    }
    _play.stop();
    _pausedBySheet = false;
    if (!_playing) {
      setState(() => _playing = true);
    }
    if (!_paused) setState(() => _paused = true);
    _play.value = v;
    _progress.value = v;
    final cut = _playCut;
    _cursorFrac.value = _cutFraction(cut);
    _seekLabel.value = _distanceLabel(cut);
    if (v >= 0.999) {
      _finishPlayback();
    } else {
      _liveHud.value = _liveAt(cut);
    }
  }

  /// 松手：原来在播就接着播；本来是暂停 / 已播完就停在新位置等用户按播放。
  void _onSeekEnd(double value) {
    if (_disp.length < 2) return;
    final v = value.clamp(0.0, 1.0);
    final wasPlaying = _seekWasPlaying;
    _seeking = false;
    _play.value = v;
    _progress.value = v;
    if (v >= 0.999) {
      // 直接给到末尾：不依赖再次触发 completed，显式收尾（幂等）。
      _finishPlayback();
      return;
    }
    if (wasPlaying) {
      setState(() {
        _playing = true;
        _paused = false;
        _pausedBySheet = false;
      });
      _liveHud.value = _liveAt(_playCut);
      _play.forward();
      return;
    }
    // 停在新位置：地图停在这儿，读数也停在这儿，等播放键。
    _liveHud.value = _liveAt(_playCut);
  }

  /// 当前画到的小数下标（4.5 表示在第 4、5 点之间）。
  double get _playCut {
    if (_disp.length < 2) return 0;
    // 没有速度信息 → 没有时间轴，按点数均匀播。
    if (_rideT.length != _disp.length || _rideTotal <= 0) {
      return _play.value * (_disp.length - 1);
    }
    final sec = _play.value * _rideTotal;
    // 播完就整条。二进制查找找的是「第一个 >= sec 的下标」，如果尾部有一串
    // 零位移点（原地停留，累计时间相同）会停在那串的第一个上，线就画不到底、
    // 游标也不消失。
    if (sec >= _rideTotal) return _disp.length - 1;
    var lo = 0;
    var hi = _rideT.length - 1;
    while (lo < hi) {
      final mid = (lo + hi) >> 1;
      if (_rideT[mid] < sec) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    final i = lo < 1 ? 1 : lo;
    final t0 = _rideT[i - 1];
    final t1 = _rideT[i];
    final f = t1 - t0 <= 0 ? 1.0 : ((sec - t0) / (t1 - t0)).clamp(0.0, 1.0);
    return (i - 1) + f;
  }

  static const _mapPad = EdgeInsets.fromLTRB(40, 48, 40, 40);
  static const _mapPadPeek = EdgeInsets.fromLTRB(36, 80, 36, 100);
  static const _peekBody = 56.0;

  void _fitLive() {
    if (!mounted || _disp.length < 2) return;
    final fit = _fitOf(
      _disp,
      padding: _sheet.value > 0.5 ? _mapPad : _mapPadPeek,
    );
    if (fit == null) return;
    try {
      _map.fitCamera(fit);
    } catch (_) {}
  }

  void _onSheetDragUpdate(double dy, double bodyH) {
    // 抽屉可以一路拉到顶（bodyH），不再是半屏。
    final travel = bodyH - _peekBody;
    if (travel <= 8) return;
    if (_sheet.isAnimating) _sheet.stop();
    _sheet.value = (_sheet.value - dy / travel).clamp(0.0, 1.0);
  }

  void _onSheetDragEnd(double velocity) {
    final next = velocity < -350
        ? 1.0
        : velocity > 350
            ? 0.0
            : _sheet.value >= 0.45
                ? 1.0
                : 0.0;
    _sheet.animateTo(next, curve: Curves.easeOutCubic);
  }

  CameraFit? _fitOf(List<LatLng> pts, {EdgeInsets padding = _mapPad}) {
    if (pts.length < 2) return null;
    return CameraFit.bounds(
      bounds: LatLngBounds.fromPoints(pts),
      padding: padding,
      maxZoom: 16,
      minZoom: 3,
      forceIntegerZoomLevel: true,
    );
  }

  /// 用控件真实尺寸算出第一帧相机，避免先对着起点再跳到整条轨迹。
  ///
  /// 结果按 [padding] 缓存：只在 FlutterMap 首次 build 时生效，之后控件重建、
  /// 抽屉拖动都不会用到它，重算等于每帧白跑一遍整条轨迹的投影。
  ({LatLng center, double zoom})? _cameraFor(
    List<LatLng> pts,
    Size size, {
    EdgeInsets padding = _mapPad,
  }) {
    final cached = _camCache[padding];
    if (cached != null) return cached;
    final fit = _fitOf(pts, padding: padding);
    if (fit == null || size.width < 8 || size.height < 8) return null;
    try {
      final fitted = fit.fit(
        MapCamera(
          crs: const Epsg3857(),
          center: pts.first,
          zoom: 10,
          rotation: 0,
          nonRotatedSize: size,
          minZoom: 3,
          maxZoom: 18,
        ),
      );
      final cam = (center: fitted.center, zoom: fitted.zoom);
      _camCache[padding] = cam;
      return cam;
    } catch (_) {
      return null;
    }
  }

  void _fitTrack(MapController map) {
    final fit = _fitOf(_disp, padding: _mapPadPeek);
    if (fit == null) return;
    try {
      map.fitCamera(fit);
    } catch (_) {}
  }

  Future<void> _shareGpx() async {
    final rec = _rec;
    if (rec == null) return;
    await SharePlus.instance.share(
      ShareParams(
        files: [
          XFile(
            rec.localPath,
            mimeType: 'application/gpx+xml',
            name: '${rec.title}.gpx',
          ),
        ],
        text: rec.title,
      ),
    );
  }

  Future<void> _rename() async {
    final rec = _rec;
    if (rec == null) return;
    final name = await HelmPromptDialog.show(
      context,
      title: LocaleKeys.rideRename.tr,
      cancel: LocaleKeys.cancel.tr,
      confirm: LocaleKeys.confirm.tr,
      label: LocaleKeys.navRoutesName.tr,
      hint: LocaleKeys.rideRenameHint.tr,
      initial: rec.title,
    );
    if (name == null || name.isEmpty) return;
    final updated = await RideRecordStore().setDisplayName(rec.fileName, name);
    if (Get.isRegistered<ShellController>()) {
      Get.find<ShellController>().bumpRides();
    }
    if (!mounted) return;
    setState(() {
      if (updated != null) _rec = updated;
    });
  }

  Future<void> _uploadImport() async {
    final rec = _rec;
    if (rec == null) return;
    await ImportUpload.push(
      context: context,
      localPath: rec.localPath,
      defaultName: rec.title,
    );
  }

  Future<void> _shareImage() async {
    final rec = _rec;
    if (rec == null || _sharing) return;
    setState(() => _sharing = true);
    try {
      await WidgetsBinding.instance.endOfFrame;
      if (!mounted) return;
      if (_pts.length >= 2) {
        _fitTrack(_shareMap);
        await Future<void>.delayed(const Duration(milliseconds: 700));
      }
      if (!mounted) return;
      final boundary =
          _shotKey.currentContext?.findRenderObject() as RenderRepaintBoundary?;
      if (boundary == null) {
        throw StateError('map not ready');
      }
      final dpr = MediaQuery.devicePixelRatioOf(context).clamp(2.0, 3.0);
      final image = await boundary.toImage(pixelRatio: dpr);
      final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
      if (bytes == null) {
        throw StateError('empty png');
      }
      final dir = await getTemporaryDirectory();
      final stem = rec.fileName.toLowerCase().endsWith('.gpx')
          ? rec.fileName.substring(0, rec.fileName.length - 4)
          : rec.fileName;
      final file = File(p.join(dir.path, 'helm_$stem.png'));
      await file.writeAsBytes(bytes.buffer.asUint8List(), flush: true);
      final caption = RidePoster.shareCaption(rec, _sum);
      await SharePlus.instance.share(
        ShareParams(
          files: [
            XFile(file.path, mimeType: 'image/png', name: '${rec.title}.png'),
          ],
          text: caption,
        ),
      );
    } catch (e, st) {
      if (!mounted) return;
      AppTheme.fail(LocaleKeys.rideShareImage.tr, 'RideShare', e, st);
    } finally {
      if (mounted) setState(() => _sharing = false);
    }
  }

  Future<void> _delete() async {
    final rec = _rec;
    if (rec == null) return;
    final ok = await Get.dialog<bool>(
      AlertDialog(
        title: Text(LocaleKeys.rideDelete.tr),
        content: Text(
          LocaleKeys.deleteConfirm.trParams({'name': rec.title}),
        ),
        actions: [
          TextButton(
            onPressed: () => Get.back(result: false),
            child: Text(LocaleKeys.cancel.tr),
          ),
          TextButton(
            onPressed: () => Get.back(result: true),
            child: Text(LocaleKeys.confirm.tr),
          ),
        ],
      ),
    );
    if (ok != true) return;
    await RideRecordStore().remove(rec.fileName);
    if (Get.isRegistered<ShellController>()) {
      Get.find<ShellController>().bumpRides();
    }
    if (mounted) Get.back();
  }

  @override
  Widget build(BuildContext context) {
    final rec = _rec;
    return Scaffold(
      appBar: AppBar(
        title: Text(rec?.title ?? LocaleKeys.rideDetailTitle.tr),
        actions: [
          IconButton(
            tooltip: LocaleKeys.rideRename.tr,
            onPressed: rec == null || _sharing ? null : _rename,
            icon: const Icon(Icons.drive_file_rename_outline),
          ),
          IconButton(
            tooltip: LocaleKeys.rideShareImage.tr,
            onPressed: rec == null || _sharing ? null : _shareImage,
            icon: const Icon(Icons.ios_share),
          ),
          PopupMenuButton<String>(
            enabled: rec != null && !_sharing,
            onSelected: (v) {
              if (v == 'import') _uploadImport();
              if (v == 'gpx') _shareGpx();
              if (v == 'del') _delete();
            },
            itemBuilder: (context) => [
              PopupMenuItem(
                value: 'import',
                child: Text(LocaleKeys.rideUploadImport.tr),
              ),
              PopupMenuItem(
                value: 'gpx',
                child: Text(LocaleKeys.rideShareGpx.tr),
              ),
              PopupMenuItem(
                value: 'del',
                child: Text(LocaleKeys.rideDelete.tr),
              ),
            ],
          ),
        ],
      ),
      body: rec == null
          ? Center(child: Text(LocaleKeys.rideEmpty.tr))
          : LayoutBuilder(
              builder: (context, box) {
                final h = box.maxHeight;
                final bottom = MediaQuery.paddingOf(context).bottom;
                // 地图和 HUD 固定满屏、**不参与抽屉动画**：以前它们和抽屉在
                // 同一个 Column 里，拖动时每一帧都要按新高度重排整张地图
                // （瓦片、折线、标记全跟着重算），所以拉起来很卡。
                // 现在抽屉是一块整屏高的面板，靠位移滑入滑出，只改绘制偏移。
                final mapLayer = Stack(
                  fit: StackFit.expand,
                  children: [
                    _mapView(_map, fitPad: _mapPadPeek, playable: true),
                    // 这里不能再套 IgnorePointer：播放器条和 HUD 都要能接触摸，
                    // 空白处（Spacer）本来就不吃指针，会自然透给地图。
                    RideGlanceOverlay(
                      rec: rec,
                      summary: _sum,
                      live: _liveHud,
                      player: _disp.length < 2 ? null : _playerBar(),
                      bottomInset: _peekBody + bottom,
                      onDragUpdate: (dy) => _onSheetDragUpdate(dy, h),
                      onDragEnd: _onSheetDragEnd,
                    ),
                  ],
                );
                return Stack(
                  children: [
                    Positioned.fill(child: mapLayer),
                    // 抽屉升起时压暗地图：一层纯色（不是 Opacity 包整屏，
                    // 那会在拖动时每帧做一次 saveLayer，正是卡顿来源之一）。
                    // 透明度跟着抽屉走到 100%：拉到顶时背景全黑，抽屉顶部的
                    // 圆角外面才不会是「半透的地图」。
                    Positioned.fill(
                      child: IgnorePointer(
                        child: AnimatedBuilder(
                          animation: _sheet,
                          builder: (context, _) => ColoredBox(
                            color: Color.fromRGBO(
                              0,
                              0,
                              0,
                              _sheet.value.clamp(0.0, 1.0),
                            ),
                          ),
                        ),
                      ),
                    ),
                    Positioned.fill(
                      child: AnimatedBuilder(
                        animation: _sheet,
                        builder: (context, _) {
                          final t = _sheet.value;
                          final peekH = _peekBody + bottom * (1 - t);
                          final sheetH = peekH + (h - peekH) * t;
                          // 面板本身始终是整屏高，往下平移 (h - sheetH)：
                          // 露出来的正好是它顶部的那一段，内部列表不用重排。
                          return Transform.translate(
                            offset: Offset(0, h - sheetH),
                            child: _sheetWidget(
                              rec: rec,
                              h: h,
                              expanded: t > 0.04,
                            ),
                          );
                        },
                      ),
                    ),
                    if (_sharing)
                      Positioned.fill(
                        child: RepaintBoundary(
                          key: _shotKey,
                          child: RidePoster(
                            rec: rec,
                            summary: _sum,
                            map: _mapView(_shareMap, fitPad: _mapPadPeek),
                          ),
                        ),
                      ),
                    if (_sharing)
                      const Positioned.fill(
                        child: ColoredBox(
                          color: Color(0x88000000),
                          child: Center(child: CircularProgressIndicator()),
                        ),
                      ),
                  ],
                );
              },
            ),
    );
  }

  /// 抽屉的开/合（点标题行）。
  void _toggleSheet() {
    if (_sheet.value < 0.5) {
      _sheet.animateTo(1, curve: Curves.easeOutCubic);
    } else {
      _sheet.animateTo(0, curve: Curves.easeOutCubic);
    }
  }

  /// 抽屉内容。拖动时每帧都会走一次 builder，但**内容本身**只在
  /// summary / 展开状态 / 分享状态变化时才需要重建 —— 缓存同一个 widget
  /// 实例，Flutter 的 updateChild 会直接跳过整棵子树（四张卡里各有一遍
  /// O(n) 的取数与分区统计，不该每帧跑）。
  Widget? _sheetCache;
  Object? _sheetCacheKey;

  Widget _sheetWidget({
    required RideRecord rec,
    required double h,
    required bool expanded,
  }) {
    final key = Object.hash(rec, _sum, _loadFailed, _sharing, expanded, h);
    if (_sheetCache == null || _sheetCacheKey != key) {
      _sheetCacheKey = key;
      _sheetCache = _RideDataSheet(
        rec: rec,
        summary: _sum,
        cursor: _cursorFrac,
        loadFailed: _loadFailed,
        expanded: expanded,
        sharing: _sharing,
        onShare: _shareImage,
        onDelete: _delete,
        onDragUpdate: (dy) => _onSheetDragUpdate(dy, h),
        onDragEnd: _onSheetDragEnd,
        onHeaderTap: _toggleSheet,
      );
    }
    return _sheetCache!;
  }

  /// 播放器条：播放/暂停 + 可拖动进度 + 游标里程。
  ///
  /// 进度和里程各用一个 notifier：回放时这行要跟着走，但没必要让整页重建。
  Widget _playerBar() {
    return ValueListenableBuilder<double>(
      valueListenable: _progress,
      builder: (context, progress, _) => ValueListenableBuilder<String>(
        valueListenable: _seekLabel,
        builder: (context, label, _) => RidePlayerBar(
          playing: _playing && !_paused,
          progress: progress,
          label: label,
          onToggle: _togglePlay,
          onSeek: _onSeek,
          onSeekEnd: _onSeekEnd,
        ),
      ),
    );
  }

  /// [playable] 为 true 时地图跟着回放进度重建（只有详情页的主地图要）。
  Widget _mapView(
    MapController controller, {
    EdgeInsets fitPad = _mapPad,
    bool playable = false,
  }) {
    if (_disp.isEmpty) {
      return const ColoredBox(
        color: Color(0xFFEDE8DF),
        child: Center(
          child: Icon(Icons.map_outlined, color: AppTheme.muted, size: 48),
        ),
      );
    }
    Widget build({required double? cut}) => LayoutBuilder(
      builder: (context, box) {
        final mapSize = Size(box.maxWidth, box.maxHeight);
        if (mapSize.width < 8 || mapSize.height < 8) {
          return const ColoredBox(color: Color(0xFFEDE8DF));
        }
        final cam = _cameraFor(_disp, mapSize, padding: fitPad);
        // 单独一层：抽屉拖动 / 底栏刷新不会把地图的栅格一起标脏。
        return RepaintBoundary(
          child: FlutterMap(
            mapController: controller,
            options: MapOptions(
              initialCenter: cam?.center ?? _disp.first,
              initialZoom: cam?.zoom ?? 14,
              minZoom: 3,
              maxZoom: 18,
              interactionOptions: const InteractionOptions(
                flags: InteractiveFlag.all & ~InteractiveFlag.rotate,
              ),
              backgroundColor: const Color(0xFFEDE8DF),
            ),
            children: [
              HelmOsmTiles.layer(
                tileDisplay: const TileDisplay.instantaneous(),
              ),
              ...rideTrackLayers(
                display: _disp,
                summary: _sum,
                wgs: _pts,
                stops: _stops,
                cut: cut,
                kmMarks: _kmMarks,
              ),
            ],
          ),
        );
      },
    );

    // 分享封面用完整轨迹，不参与回放。
    if (!playable) return build(cut: null);
    // 只让地图这一层跟着回放重建：整页 60 fps 重建会把面板和榜单一起带上。
    return ListenableBuilder(
      listenable: _play,
      builder: (context, _) => build(cut: _playCut),
    );
  }
}

class _RideDataSheet extends StatelessWidget {
  const _RideDataSheet({
    required this.rec,
    required this.summary,
    required this.cursor,
    required this.loadFailed,
    required this.expanded,
    required this.sharing,
    required this.onShare,
    required this.onDelete,
    required this.onDragUpdate,
    required this.onDragEnd,
    required this.onHeaderTap,
  });

  final RideRecord rec;
  final GpxSummary? summary;
  final ValueListenable<double>? cursor;
  final bool loadFailed;
  final bool expanded;
  final bool sharing;
  final VoidCallback onShare;
  final VoidCallback onDelete;
  final ValueChanged<double> onDragUpdate;
  final ValueChanged<double> onDragEnd;
  final VoidCallback onHeaderTap;

  String get _time {
    if (summary?.duration != null) {
      return GpxUtil.formatHms(summary!.duration!);
    }
    final t = summary?.startTime ??
        (rec.mtime > 0
            ? DateTime.fromMillisecondsSinceEpoch(rec.mtime * 1000)
            : null);
    return t == null ? '' : GpxUtil.formatStamp(t);
  }

  String get _km {
    final km = summary?.distanceKm ?? rec.distanceKm ?? 0;
    if (km < 0.001) return '';
    if (km < 1) {
      return LocaleKeys.rideDistM.trParams({
        'm': (km * 1000).round().toString(),
      });
    }
    return LocaleKeys.rideDistKm.trParams({
      'km': km < 10 ? km.toStringAsFixed(2) : km.toStringAsFixed(1),
    });
  }

  /// 把只含有效样本的序列铺回「每个轨迹点一个值」的等距横轴。
  ///
  /// 图表的 x 轴是等距的，游标按轨迹点位置定位；直接把过滤后的样本按下标
  /// 铺满整张图，缺测的时段会被压缩 —— 游标就会和曲线错开（心率只在后半程
  /// 有数据时最明显）。缺测段沿用上一个已知值，是「没有新数据」的常见画法。
  static List<double> _denseSeries(List<RideSeriesPoint> series, int n) {
    if (series.isEmpty) return const [];
    if (n < 2) return [for (final p in series) p.value];
    // 旧数据 / 手工构造的序列没有下标：保持原样（此时无法对齐）。
    if (series.every((p) => p.index < 0)) {
      return [for (final p in series) p.value];
    }
    final out = List<double>.filled(n, double.nan);
    for (final p in series) {
      final i = p.index;
      if (i < 0 || i >= n) continue;
      out[i] = p.value;
    }
    var last = double.nan;
    for (var i = 0; i < n; i++) {
      if (out[i].isNaN) {
        if (!last.isNaN) out[i] = last;
      } else {
        last = out[i];
      }
    }
    var first = double.nan;
    for (var i = 0; i < n; i++) {
      if (!out[i].isNaN) {
        first = out[i];
        break;
      }
    }
    if (first.isNaN) return [for (final p in series) p.value];
    for (var i = 0; i < n; i++) {
      if (out[i].isNaN) {
        out[i] = first;
      } else {
        break;
      }
    }
    return out;
  }

  /// 速度。没有逐点速度时用均速画一条平线，至少让卡片有内容。
  Widget _speedCard() {
    final sum = summary;
    final values = _denseSeries(
      sum?.speedPoints ?? const <RideSeriesPoint>[],
      sum?.points.length ?? 0,
    );
    final avg = sum?.avgSpeedKmh;
    return RideMetricCard(
      icon: Icons.speed,
      color: const Color(0xFF3DDC84),
      title: '${LocaleKeys.rideSpeedCard.tr} (km/h)',
      values: values.isEmpty && avg != null ? [avg, avg] : values,
      avg: avg,
      max: sum?.maxSpeedKmh,
      avgLabel: LocaleKeys.rideMetricAvgSpeed.tr,
      maxLabel: LocaleKeys.rideMetricMaxSpeed.tr,
      decimals: 1,
      cursor: cursor,
    );
  }

  /// 心率。没有数据也留着卡片，写清「本次无心率」，整块消失会让人以为漏了。
  Widget _hrCard() {
    final sum = summary;
    final has = sum?.hasHr ?? false;
    var zones = const <Duration>[];
    var maxMs = 1;
    if (has) {
      zones = GpxUtil.hrZoneDurations(sum!.hrPoints, total: sum.duration);
      maxMs = zones.fold<int>(
        0,
        (m, d) => d.inMilliseconds > m ? d.inMilliseconds : m,
      );
    }
    return RideMetricCard(
      icon: has ? Icons.favorite : Icons.favorite_border,
      color: has ? const Color(0xFFFF5C4D) : AppTheme.muted,
      title: '${LocaleKeys.rideHrCard.tr} (BPM)',
      values: _denseSeries(
        sum?.hrPoints ?? const <RideSeriesPoint>[],
        sum?.points.length ?? 0,
      ),
      avg: sum?.avgHrBpm?.toDouble(),
      max: sum?.maxHrBpm?.toDouble(),
      avgLabel: LocaleKeys.rideMetricAvgHr.tr,
      maxLabel: LocaleKeys.rideMetricMaxHr.tr,
      cursor: cursor,
      note: LocaleKeys.rideNoHr.tr,
      extra: has ? RideHrZones(zones: zones, maxMs: maxMs) : null,
    );
  }

  /// 功率。只有真的录到才出现 —— 整块消失不会误导，因为心率那格已经说明了
  /// “这次没戴传感器”这条规则。
  Widget _powerCard() {
    final sum = summary;
    return RideMetricCard(
      icon: Icons.bolt,
      color: const Color(0xFFFFA733),
      title: '${LocaleKeys.ridePowerCard.tr} (W)',
      values: _denseSeries(
        sum?.powerPoints ?? const <RideSeriesPoint>[],
        sum?.points.length ?? 0,
      ),
      avg: sum?.avgPowerW,
      max: sum?.maxPowerW,
      avgLabel: LocaleKeys.rideMetricAvgPower.tr,
      maxLabel: LocaleKeys.rideMetricMaxPower.tr,
      cursor: cursor,
    );
  }

  /// 踏频。
  Widget _cadenceCard() {
    final sum = summary;
    return RideMetricCard(
      icon: Icons.loop,
      color: const Color(0xFF34D399),
      title: '${LocaleKeys.rideCadenceCard.tr} (RPM)',
      values: _denseSeries(
        sum?.cadPoints ?? const <RideSeriesPoint>[],
        sum?.points.length ?? 0,
      ),
      avg: sum?.avgCadenceRpm?.toDouble(),
      max: sum?.maxCadenceRpm?.toDouble(),
      avgLabel: LocaleKeys.rideMetricAvgCadence.tr,
      maxLabel: LocaleKeys.rideMetricMaxCadence.tr,
      cursor: cursor,
    );
  }

  @override
  Widget build(BuildContext context) {
    final time = _time;
    final km = _km;
    return Material(
      color: const Color(0xFF141416),
      elevation: 0,
      shadowColor: Colors.transparent,
      surfaceTintColor: Colors.transparent,
      borderRadius: const BorderRadius.vertical(top: Radius.circular(20)),
      clipBehavior: Clip.antiAlias,
      child: Column(
        children: [
          GestureDetector(
            behavior: HitTestBehavior.opaque,
            onTap: onHeaderTap,
            onVerticalDragUpdate: (d) => onDragUpdate(d.delta.dy),
            onVerticalDragEnd: (d) => onDragEnd(d.primaryVelocity ?? 0),
            child: Padding(
              padding: EdgeInsets.fromLTRB(
                16,
                8,
                16,
                expanded ? 12 : 12 + MediaQuery.paddingOf(context).bottom,
              ),
              child: Column(
                children: [
                  Container(
                    width: 36,
                    height: 4,
                    decoration: BoxDecoration(
                      color: const Color(0xFF5C5C5E),
                      borderRadius: BorderRadius.circular(2),
                    ),
                  ),
                  const SizedBox(height: 10),
                  Text(
                    [
                      if (time.isNotEmpty) time,
                      if (km.isNotEmpty) km,
                    ].join('  ·  '),
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: const TextStyle(
                      color: Colors.white,
                      fontSize: 16,
                      fontWeight: FontWeight.w700,
                      height: 1.2,
                    ),
                  ),
                ],
              ),
            ),
          ),
          if (expanded)
            Expanded(
              child: NotificationListener<ScrollNotification>(
                // 列表已经到顶还在往下拉：把这段「没处可滚」的位移交给抽屉。
                // Android 的 Clamping physics 在这种情况会发 OverscrollNotification，
                // 拿它就能实现「在内容区直接下拉收起抽屉」，不用把整页改成
                // DraggableScrollableSheet。到顶时 overscroll 为负（手指向下）。
                onNotification: (n) {
                  if (n is OverscrollNotification) {
                    if (n.overscroll != 0 && n.metrics.pixels <= 0) {
                      onDragUpdate(-n.overscroll);
                    }
                  } else if (n is ScrollEndNotification) {
                    // 速度可能是 0（慢慢拖住再松手）或 null（惯性阶段结束时），
                    // 都要按当前位置吸附，否则抽屉会停在半截。
                    onDragEnd(n.dragDetails?.primaryVelocity ?? 0);
                  }
                  return false;
                },
                child: ListView(
                // 全局是 BouncingScrollPhysics（到顶会被拉成负偏移），那种物理
                // 下「到顶再下拉」不会发 overscroll 通知，抽屉就收不回来。
                // 这一处单独用 clamping：到顶的额外位移被拒收 → 变成通知 → 交给抽屉。
                physics: const ClampingScrollPhysics(),
                padding: const EdgeInsets.fromLTRB(12, 0, 12, 28),
                children: [
                  if (loadFailed) ...[
                    _Reveal(
                      index: 0,
                      child: HelmCard(
                        padding: const EdgeInsets.fromLTRB(16, 14, 16, 14),
                        child: Text(
                          LocaleKeys.rideLoadFailed.tr,
                          style: const TextStyle(
                            color: AppTheme.muted,
                            fontSize: 13,
                            height: 1.4,
                          ),
                        ),
                      ),
                    ),
                    const SizedBox(height: 12),
                  ],
                  _Reveal(
                    index: 0,
                    child: _SummaryCard(rec: rec, summary: summary),
                  ),
                  const SizedBox(height: 12),
                  // 顺序按用户指定：速度（含里程摘要）→ 心率 → 功率 → 踏频。
                  // 一页一页切太碎，全部纵向排开、滚着看。
                  _Reveal(index: 1, child: _speedCard()),
                  const SizedBox(height: 12),
                  _Reveal(index: 2, child: _hrCard()),
                  if (summary?.hasPower ?? false) ...[
                    const SizedBox(height: 12),
                    _Reveal(index: 3, child: _powerCard()),
                  ],
                  if (summary?.hasCadence ?? false) ...[
                    const SizedBox(height: 12),
                    _Reveal(index: 4, child: _cadenceCard()),
                  ],
                  const SizedBox(height: 16),
                  _Reveal(
                    index: 5,
                    child: Row(
                      children: [
                        Expanded(
                          child: FilledButton.icon(
                            onPressed: sharing ? null : onShare,
                            icon: const Icon(Icons.image_outlined, size: 18),
                            label: Text(
                              sharing
                                  ? LocaleKeys.rideSharePreparing.tr
                                  : LocaleKeys.rideShareImage.tr,
                            ),
                          ),
                        ),
                        const SizedBox(width: 10),
                        Expanded(
                          child: FilledButton.icon(
                            onPressed: sharing ? null : onDelete,
                            style: FilledButton.styleFrom(
                              backgroundColor: const Color(0xFF3A1C1C),
                              foregroundColor: const Color(0xFFEF4444),
                            ),
                            icon: const Icon(Icons.delete_outline, size: 18),
                            label: Text(LocaleKeys.rideDelete.tr),
                          ),
                        ),
                      ],
                    ),
                  ),
                  const SizedBox(height: 20),
                  Center(
                    child: Text(
                      LocaleKeys.appName.tr,
                      style: const TextStyle(
                        color: AppTheme.muted,
                        fontSize: 13,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                  ),
                ],
              ),
            ),
              ),
        ],
      ),
    );
  }
}

/// 抽屉里的卡片错峰入场：展开时逐张上浮淡入。
///
/// 用 `Interval` 在一条 Tween 里做出延迟，不额外起 Timer / Controller；
/// 抽屉收起时 ListView 整体销毁，再展开自然重播一次。
class _Reveal extends StatelessWidget {
  const _Reveal({required this.index, required this.child});

  final int index;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    return TweenAnimationBuilder<double>(
      tween: Tween<double>(begin: 0, end: 1),
      duration: const Duration(milliseconds: 560),
      curve: Interval(
        (index * 0.08).clamp(0.0, 0.5),
        1,
        curve: Curves.easeOutCubic,
      ),
      builder: (context, t, child) => Opacity(
        opacity: t,
        child: Transform.translate(offset: Offset(0, 12 * (1 - t)), child: child),
      ),
      child: child,
    );
  }
}

class _SummaryCard extends StatelessWidget {
  const _SummaryCard({required this.rec, required this.summary});

  final RideRecord rec;
  final GpxSummary? summary;

  @override
  Widget build(BuildContext context) {
    final km = summary?.distanceKm ?? rec.distanceKm ?? 0;
    final speedVals = summary?.pointSpeedsKmh ?? const <double>[];
    final when = summary?.startTime ??
        (rec.mtime > 0
            ? DateTime.fromMillisecondsSinceEpoch(rec.mtime * 1000)
            : null);
    final kcal = GpxUtil.estimateKcal(
      avgHrBpm: summary?.avgHrBpm,
      duration: summary?.duration,
      distanceKm: km,
    );
    final dist = km < 0.001
        ? '—'
        : km < 1
            ? LocaleKeys.rideDistM.trParams({
                'm': (km * 1000).round().toString(),
              })
            : LocaleKeys.rideDistKm.trParams({
                'km': km < 10 ? km.toStringAsFixed(2) : km.toStringAsFixed(1),
              });

    return HelmCard(
      padding: const EdgeInsets.fromLTRB(18, 16, 18, 16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            '${LocaleKeys.appName.tr}  |  ${LocaleKeys.rideOutdoor.tr}',
            style: const TextStyle(
              color: AppTheme.muted,
              fontSize: 13,
              fontWeight: FontWeight.w500,
            ),
          ),
          const SizedBox(height: 10),
          Text(
            dist,
            style: const TextStyle(
              color: Colors.white,
              fontSize: 40,
              fontWeight: FontWeight.w800,
              height: 1.05,
            ),
          ),
          if (when != null) ...[
            const SizedBox(height: 6),
            Text(
              GpxUtil.formatStamp(when),
              style: const TextStyle(color: AppTheme.muted, fontSize: 13),
            ),
          ],
          const SizedBox(height: 14),
          // 用逐点速度（和轨迹线同一份序列）+ 同一套归一化区间，
          // 「线」和「条」的同一段速度才会是同一个颜色。
          RidePaceStrip(
            speeds: speedVals,
            range: RideTrackStyle.bandRange(speedVals),
          ),
          const SizedBox(height: 6),
          Row(
            children: [
              Text(
                LocaleKeys.rideSlower.tr,
                style: const TextStyle(color: AppTheme.muted, fontSize: 11),
              ),
              const Spacer(),
              Text(
                LocaleKeys.rideFaster.tr,
                style: const TextStyle(color: AppTheme.muted, fontSize: 11),
              ),
            ],
          ),
          const SizedBox(height: 16),
          Row(
            children: [
              Expanded(
                child: _BigStat(
                  value: summary?.duration == null
                      ? '—'
                      : GpxUtil.formatHms(summary!.duration!),
                  label: LocaleKeys.rideMetricMoveTime.tr,
                ),
              ),
              Expanded(
                child: _BigStat(
                  value: kcal > 0 ? '$kcal kcal' : '—',
                  label: LocaleKeys.rideMetricKcal.tr,
                ),
              ),
              Expanded(
                child: _BigStat(
                  value: summary?.avgSpeedKmh == null
                      ? '—'
                      : '${summary!.avgSpeedKmh!.toStringAsFixed(1)} km/h',
                  label: LocaleKeys.rideMetricAvgSpeed.tr,
                ),
              ),
            ],
          ),
          const SizedBox(height: 16),
          _BigStat(
            value: summary?.avgHrBpm == null
                ? '—'
                : '${summary!.avgHrBpm} BPM',
            label: LocaleKeys.rideMetricAvgHr.tr,
          ),
        ],
      ),
    );
  }
}

class _BigStat extends StatelessWidget {
  const _BigStat({required this.value, required this.label});

  final String value;
  final String label;

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          value,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: const TextStyle(
            color: Colors.white,
            fontSize: 20,
            fontWeight: FontWeight.w800,
            height: 1.1,
          ),
        ),
        const SizedBox(height: 4),
        Text(
          label,
          style: const TextStyle(color: AppTheme.muted, fontSize: 12),
        ),
      ],
    );
  }
}

