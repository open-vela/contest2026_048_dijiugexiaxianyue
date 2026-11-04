import 'dart:io';
import 'dart:typed_data';

import 'package:path/path.dart' as p;
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/cache/app_cache_store.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/gnss/eph_brdc.dart';
import 'package:sifli_companion/gnss/mga_ubx.dart';
import 'package:sifli_companion/location/location_manager.dart';
import 'package:sifli_companion/log/app_log.dart';

/// 从网上拉广播星历、生成 UBX-MGA，写到码表 `eph/mga_<utc>.ubx`。
class EphService {
  /// 创建服务。
  EphService({EphBrdc? brdc}) : _brdc = brdc ?? EphBrdc();

  static const _tag = 'Eph';

  final EphBrdc _brdc;

  /// 保证 `eph/` 存在。目录已有则忽略。
  Future<void> ensureDir(CompanionClient client) async {
    try {
      await client.fsMkdir(CompanionFs.ephDir);
    } on CompanionFsException catch (e) {
      if (e.status != 17) rethrow;
    }
  }

  /// 列出码表上的星历文件（`mga_*.ubx`，兼容旧名 `mga.ubx` / `dump.ubx`）。
  Future<List<CompanionFsEntry>> listOnDevice(CompanionClient client) async {
    try {
      final out = <CompanionFsEntry>[];
      var cursor = 0;
      while (true) {
        final page = await client.fsList(CompanionFs.ephDir, cursor: cursor);
        if (page.isEmpty) break;
        out.addAll(page.where(_isEphName));
        cursor += page.length;
        if (page.length < 4) break;
      }
      out.sort((a, b) {
        final ua = MgaUbx.parseFileNameUtc(a.name) ?? a.mtime;
        final ub = MgaUbx.parseFileNameUtc(b.name) ?? b.mtime;
        return ub.compareTo(ua);
      });
      return out;
    } on CompanionFsException catch (e) {
      if (e.status == 2) return [];
      rethrow;
    }
  }

  /// 当天的 BRDC 转换结果缓存多久：星历有效 2–4 h，缓存窗口取一半。
  ///
  /// 定期同步（每 2 小时）如果每次重下几 MB 的 RINEX，流量和电都白花；
  /// 缓存 2 小时既避免重下，又不会把快过期的星历喂给模组。
  static const cacheMaxAge = Duration(hours: 2);

  /// 下载 BRDC → 生成 MGA（**含时间/位置辅助**）→ 上传。码表写完会热加载，不必重启。
  ///
  /// 文件里先放两帧辅助数据、再放星历帧：
  /// `MGA-INI-TIME_UTC` → `MGA-INI-POS_LLH`（有定位才放）→ 各系统的 EPH。
  /// 顺序和 u-blox 的要求一致（时间在前，否则模组不用其余辅助数据），而且固件的注入
  /// 路径只过滤 DBD 帧、其余原样转发，所以这两帧不用改固件就能进模组。
  ///
  /// 时间/位置都是**粗辅助**：时间必给（还带如实的 tAcc），位置取不到就不发那一帧，
  /// 绝不编造坐标。
  Future<EphSyncResult> syncToDevice(
    CompanionClient client, {
    void Function(EphSyncStage stage)? onStage,
    void Function(int done, int total)? onProgress,
  }) async {
    onStage?.call(EphSyncStage.download);
    // 位置和星历并行取：抓位置可能花几秒，不该加在下发的关键路径上。
    final assistF = _assistHead();
    final eph = await _fetchBrdc();
    final assist = await assistF;
    final bytes = Uint8List(assist.length + eph.length)
      ..setAll(0, assist)
      ..setAll(assist.length, eph);

    onStage?.call(EphSyncStage.convert);
    final parsed = MgaUbx.parse(bytes);
    if (!parsed.ok) {
      throw EphException(parsed);
    }
    onStage?.call(EphSyncStage.upload);
    final path = await uploadToDevice(
      client,
      bytes,
      onProgress: onProgress,
    );
    return EphSyncResult(path: path, bytes: bytes, parsed: parsed);
  }

  /// 时间 + （有定位时的）位置辅助帧。
  Future<Uint8List> _assistHead() async {
    final out = BytesBuilder(copy: false);
    out.add(MgaUbx.iniTimeUtc(DateTime.now().toUtc()));
    try {
      // 先要系统缓存里那份（瞬时）；没有再花最多 6 秒做一次粗定位。
      final pos = await LocationManager().getLastKnownPosition() ??
          await LocationManager().getCoarsePosition();
      if (pos == null) {
        await AppLog.i(_tag, '没有定位，位置辅助跳过');
      } else {
        final acc = pos.accuracy > 0 ? pos.accuracy : 5000.0;
        out.add(
          MgaUbx.iniPosLlh(
            lat: pos.latitude,
            lon: pos.longitude,
            altM: pos.altitude,
            posAccM: acc,
          ),
        );
        await AppLog.i(_tag, '位置辅助 ±${acc.round()} m');
      }
    } catch (e) {
      await AppLog.w(_tag, '取位置失败，位置辅助跳过: $e');
    }
    return out.takeBytes();
  }

  /// 当天的 BRDC 转换结果；2 小时内的缓存直接用，省一次几 MB 的下载。
  Future<Uint8List> _fetchBrdc() async {
    final now = DateTime.now();
    final day = _dayKey(now.toUtc());
    File? cache;
    try {
      final dir = await AppCacheStore.instance.subdir('eph');
      cache = File(p.join(dir.path, 'brdc_$day.ubx'));
      if (await cache.exists()) {
        final age = now.difference(await cache.lastModified());
        if (age < cacheMaxAge) {
          await AppLog.i(_tag, '用缓存星历（${age.inMinutes} 分钟前下载）');
          return await cache.readAsBytes();
        }
      }
    } catch (e) {
      await AppLog.w(_tag, '读星历缓存失败: $e');
      cache = null;
    }

    final bytes = await _brdc.fetchAndBuild();
    if (cache != null) {
      try {
        await cache.writeAsBytes(bytes, flush: true);
        await _pruneCache(cache);
      } catch (e) {
        await AppLog.w(_tag, '写星历缓存失败: $e');
      }
    }
    return bytes;
  }

  /// 只留当天那份缓存。跨天后旧文件没用了，省得一直堆着 ——
  /// 只删自己写的 `brdc_*.ubx`，目录里别的东西不碰。
  Future<void> _pruneCache(File keep) async {
    final name = p.basename(keep.path);
    await for (final e in keep.parent.list()) {
      if (e is! File) continue;
      final n = p.basename(e.path);
      if (n == name || !n.startsWith('brdc_') || !n.endsWith('.ubx')) continue;
      try {
        await e.delete();
      } catch (_) {}
    }
  }

  static String _dayKey(DateTime utc) =>
      '${utc.year.toString().padLeft(4, '0')}'
      '${utc.month.toString().padLeft(2, '0')}'
      '${utc.day.toString().padLeft(2, '0')}';

  /// 校验后上传。返回设备上的相对路径。
  Future<String> uploadToDevice(
    CompanionClient client,
    Uint8List bytes, {
    int? utcSec,
    void Function(int done, int total)? onProgress,
  }) async {
    final parsed = MgaUbx.parse(bytes);
    if (!parsed.ok) {
      throw EphException(parsed);
    }
    final name = MgaUbx.fileName(utcSec: utcSec);
    final path = CompanionFs.ephPath(name);
    await AppLog.i(
      _tag,
      '上传 $path ${bytes.length} B ${parsed.typeSummary}',
    );
    await ensureDir(client);
    await client.fsUpload(path, bytes, resume: true, onProgress: onProgress);
    return path;
  }

  bool _isEphName(CompanionFsEntry e) {
    if (e.isDir) return false;
    final n = e.name.toLowerCase();
    if (n == 'mga.ubx' || n == 'dump.ubx') return true;
    return n.startsWith('mga_') && n.endsWith('.ubx');
  }
}

/// [EphService.syncToDevice] 的阶段。
enum EphSyncStage {
  /// 拉 IGS BRDC。
  download,

  /// 编 UBX-MGA。
  convert,

  /// BLE 写入码表。
  upload,
}

/// 一次同步的结果。
class EphSyncResult {
  /// 创建结果。
  const EphSyncResult({
    required this.path,
    required this.bytes,
    required this.parsed,
  });

  /// 码表相对路径。
  final String path;

  /// 生成的 UBX。
  final Uint8List bytes;

  /// 解析摘要。
  final MgaUbxFile parsed;
}

/// 文件不是码表能注入的 UBX-MGA。
class EphException implements Exception {
  /// 带上解析结果，方便 UI 展示原因。
  EphException(this.file);

  /// 解析结果。
  final MgaUbxFile file;

  @override
  String toString() {
    if (file.bytes == 0) return '空文件';
    if (file.frames == 0) return '不是 UBX（需要 B5 62 开头）';
    if (file.checksumFail > 0) return 'UBX 校验失败 ${file.checksumFail} 帧';
    if (file.leftover > 0) return '文件尾截断 ${file.leftover} 字节';
    if (file.tooBig > 0) return '单帧 payload 超过 ${MgaUbx.maxPayload}';
    if (file.mgaFrames == 0) return '没有 UBX-MGA 帧';
    if (file.bytes > MgaUbx.maxFileBytes) {
      return '文件过大 ${file.bytes} B';
    }
    return '无效星历文件';
  }
}
