import 'package:flutter_test/flutter_test.dart';
import 'package:latlong2/latlong.dart';
import 'package:sifli_companion/ride/gpx_util.dart';

void main() {
  test('parse trkpt and distance', () {
    const xml = '''
<gpx><name>demo</name>
<trkpt lat="32.0" lon="118.0"></trkpt>
<trkpt lat="32.01" lon="118.0"></trkpt>
</gpx>
''';
    final sum = GpxUtil.parse(xml);
    expect(sum.name, 'demo');
    expect(sum.points.length, 2);
    expect(sum.distanceKm, greaterThan(0));
    expect(sum.startTime, isNull);
  });

  test('write route then parse rtept and metadata time', () {
    final now = DateTime.utc(2026, 9, 7, 11, 16);
    final xml = GpxUtil.writeRoute(
      name: '环城',
      points: const [LatLng(32.0, 118.0), LatLng(32.1, 118.1)],
      timeUtc: now,
    );
    final sum = GpxUtil.parse(xml);
    expect(sum.name, '环城');
    expect(sum.points.length, 2);
    expect(sum.startTime, now);
    expect(xml.contains('<time>2026-09-07T11:16:00Z</time>'), isTrue);
  });

  test('write track stamps each point', () {
    final end = DateTime.utc(2026, 9, 7, 12, 0, 2);
    final xml = GpxUtil.writeTrack(
      name: 'ride',
      points: const [
        LatLng(32.0, 118.0),
        LatLng(32.01, 118.0),
        LatLng(32.02, 118.0),
      ],
      endUtc: end,
    );
    expect(xml.contains('<time>2026-09-07T12:00:00Z</time>'), isTrue);
    expect(xml.contains('<time>2026-09-07T12:00:01Z</time>'), isTrue);
    expect(xml.contains('<time>2026-09-07T12:00:02Z</time>'), isTrue);
    final sum = GpxUtil.parse(xml);
    expect(sum.startTime, DateTime.utc(2026, 9, 7, 12, 0, 0));
    expect(sum.endTime, end);
  });

  test('ensureTimestamps fills missing times from phone clock', () {
    const xml = '''
<gpx>
<trkseg>
<trkpt lat="32.0" lon="118.0"><ele>10</ele></trkpt>
<trkpt lat="32.01" lon="118.0"><ele>11</ele></trkpt>
</trkseg>
</gpx>
''';
    final now = DateTime.utc(2026, 9, 7, 8, 0, 1);
    final stamped = GpxUtil.ensureTimestamps(xml, nowUtc: now);
    expect(stamped.contains('<ele>10</ele>'), isTrue);
    expect(stamped.contains('<time>2026-09-07T08:00:00Z</time>'), isTrue);
    expect(stamped.contains('<time>2026-09-07T08:00:01Z</time>'), isTrue);
    final sum = GpxUtil.parse(stamped);
    expect(sum.endTime, now);
  });

  test('ensureTimestamps keeps GNSS times, ignores epoch FS times in GPX', () {
    const good = '''
<gpx>
<trkpt lat="32.0" lon="118.0"><time>2026-06-10T19:25:15Z</time></trkpt>
</gpx>
''';
    expect(GpxUtil.ensureTimestamps(good), good);

    const epoch = '''
<gpx>
<trkpt lat="32.0" lon="118.0"><time>1970-01-01T00:00:00Z</time></trkpt>
<trkpt lat="32.01" lon="118.0"><time>1970-01-01T00:00:01Z</time></trkpt>
</gpx>
''';
    final now = DateTime.utc(2026, 9, 7, 10, 0, 1);
    final stamped = GpxUtil.ensureTimestamps(epoch, nowUtc: now);
    expect(stamped.contains('1970-'), isFalse);
    expect(stamped.contains('<time>2026-09-07T10:00:00Z</time>'), isTrue);
    expect(stamped.contains('<time>2026-09-07T10:00:01Z</time>'), isTrue);
  });

  test('safeFileName keeps gpx and length', () {
    expect(GpxUtil.safeFileName('环 城.gpx').endsWith('.gpx'), isTrue);
    expect(GpxUtil.safeFileName('a' * 80).length, lessThanOrEqualTo(31));
  });

  test('safeTsvFileName keeps tsv', () {
    expect(GpxUtil.safeTsvFileName('环城'), '环城.tsv');
  });

  test('parse computes duration speed and climb', () {
    const xml = '''
<gpx>
<trkpt lat="32.0" lon="118.0"><ele>10</ele><time>2026-09-08T08:00:00Z</time></trkpt>
<trkpt lat="32.00015" lon="118.0"><ele>12</ele><time>2026-09-08T08:00:02Z</time></trkpt>
<trkpt lat="32.01" lon="118.0"><ele>25</ele><time>2026-09-08T09:00:00Z</time></trkpt>
</gpx>
''';
    final sum = GpxUtil.parse(xml);
    expect(sum.duration, const Duration(hours: 1));
    expect(sum.elevGainM, closeTo(15, 0.1));
    expect(sum.maxEleM, closeTo(25, 0.1));
    expect(sum.avgSpeedKmh, greaterThan(1));
    expect(sum.maxSpeedKmh, greaterThan(20));
    expect(GpxUtil.formatElapsed(sum.duration!), '1:00:00');
  });

  test('parse gpxtpx heart rate and kcal estimate', () {
    const xml = '''
<gpx>
<trkpt lat="32.0" lon="118.0">
  <time>2026-09-08T08:00:00Z</time>
  <extensions><gpxtpx:TrackPointExtension>
    <gpxtpx:hr>114</gpxtpx:hr>
  </gpxtpx:TrackPointExtension></extensions>
</trkpt>
<trkpt lat="32.00015" lon="118.0">
  <time>2026-09-08T08:00:02Z</time>
  <extensions><gpxtpx:hr>128</gpxtpx:hr></extensions>
</trkpt>
</gpx>
''';
    final sum = GpxUtil.parse(xml);
    expect(sum.hasHr, isTrue);
    expect(sum.avgHrBpm, 121);
    expect(sum.maxHrBpm, 128);
    expect(sum.hrPoints.length, 2);
    expect(GpxUtil.formatHms(const Duration(minutes: 4, seconds: 16)), '00:04:16');
    expect(
      GpxUtil.estimateKcal(
        avgHrBpm: 114,
        duration: const Duration(minutes: 4, seconds: 16),
      ),
      15,
    );
  });

  test('setName updates or inserts metadata name', () {
    const xml = '''
<gpx>
  <metadata><name>old</name></metadata>
  <trk><name>old</name></trk>
</gpx>
''';
    final next = GpxUtil.setName(xml, '环城');
    expect(next.contains('<name>环城</name>'), isTrue);
    expect(next.contains('<name>old</name>'), isFalse);

    const bare = '<gpx version="1.1"></gpx>';
    final added = GpxUtil.setName(bare, 'demo');
    expect(added.contains('<metadata><name>demo</name></metadata>'), isTrue);
  });

  test('kmMarkers every kilometre along the track', () {
    const xml = '''
<gpx>
<trkpt lat="32.0" lon="118.0"></trkpt>
<trkpt lat="32.009" lon="118.0"></trkpt>
<trkpt lat="32.018" lon="118.0"></trkpt>
</gpx>
''';
    final sum = GpxUtil.parse(xml);
    expect(sum.distanceKm, greaterThan(1.5));
    final marks = GpxUtil.kmMarkers(sum.points);
    expect(marks, isNotEmpty);
    expect(marks.first.km, 1);
    expect(marks.length, greaterThanOrEqualTo(1));
  });

  test('previewTrack keeps start and end', () {
    final pts = [
      for (var i = 0; i < 200; i++) LatLng(32 + i * 0.001, 118 + i * 0.001),
    ];
    final preview = GpxUtil.previewTrack(pts, max: 48);
    expect(preview.length, lessThanOrEqualTo(49));
    expect(preview.first, pts.first);
    expect(preview.last, pts.last);
  });

  test('parse firmware cadence power and device speed', () {
    const xml = '''
<gpx>
<trkpt lat="32.0" lon="118.0">
  <ele>12.5</ele>
  <time>2026-10-01T08:00:00Z</time>
  <extensions>
      <gpxtpx:TrackPointExtension xmlns:gpxtpx="http://www.garmin.com/xmlschemas/TrackPointExtension/v2">
        <gpxtpx:hr>120</gpxtpx:hr>
        <gpxtpx:cad>85</gpxtpx:cad>
        <gpxtpx:speed>6.50</gpxtpx:speed>
      </gpxtpx:TrackPointExtension>
      <gpxpx:PowerInWatts xmlns:gpxpx="http://www.garmin.com/xmlschemas/PowerExtension/v1">210</gpxpx:PowerInWatts>
  </extensions>
</trkpt>
<trkpt lat="32.0001" lon="118.0">
  <time>2026-10-01T08:00:02Z</time>
  <extensions><gpxtpx:cad>90</gpxtpx:cad><gpxtpx:speed>6.50</gpxtpx:speed>
    <gpxpx:PowerInWatts>0</gpxpx:PowerInWatts></extensions>
</trkpt>
<trkpt lat="32.0002" lon="118.0">
  <time>2026-10-01T08:00:04Z</time>
</trkpt>
</gpx>
''';
    final sum = GpxUtil.parse(xml);
    expect(sum.points.length, 3);
    expect(sum.hasCadence, isTrue);
    expect(sum.hasPower, isTrue);
    expect(sum.avgCadenceRpm, 88);
    expect(sum.maxCadenceRpm, 90);
    expect(sum.avgPowerW, 105);
    expect(sum.maxPowerW, 210);
    expect(sum.cadPoints.length, 2);
    expect(sum.powerPoints.length, 2);
    expect(sum.pointCadenceRpm.length, sum.points.length);
    expect(sum.pointPowerW.length, sum.points.length);
    expect(sum.pointCadenceRpm, [85, 90, null]);
    expect(sum.pointPowerW, [210, 0, null]);
    expect(sum.pointSpeedsKmh[0], closeTo(23.4, 0.01));
    expect(sum.pointSpeedsKmh[1], closeTo(23.4, 0.01));
  });

  test('parse bare power element from third-party gpx', () {
    const xml = '''
<gpx>
<trkpt lat="32.0" lon="118.0">
  <time>2026-10-01T08:00:00Z</time>
  <extensions><power>210</power></extensions>
</trkpt>
<trkpt lat="32.0001" lon="118.0">
  <time>2026-10-01T08:00:02Z</time>
  <extensions><power>250</power></extensions>
</trkpt>
</gpx>
''';
    final sum = GpxUtil.parse(xml);
    expect(sum.hasPower, isTrue);
    expect(sum.avgPowerW, 230);
    expect(sum.maxPowerW, 250);
    expect(sum.pointPowerW, [210, 250]);
  });

  test('drop out-of-range cadence power and speed', () {
    const xml = '''
<gpx>
<trkpt lat="32.0" lon="118.0">
  <time>2026-10-01T08:00:00Z</time>
  <extensions><gpxtpx:cad>300</gpxtpx:cad>
    <gpxpx:PowerInWatts>9999</gpxpx:PowerInWatts>
    <gpxtpx:speed>-1</gpxtpx:speed></extensions>
</trkpt>
<trkpt lat="32.0001" lon="118.0">
  <time>2026-10-01T08:00:02Z</time>
  <extensions><gpxtpx:speed>60</gpxtpx:speed></extensions>
</trkpt>
</gpx>
''';
    final sum = GpxUtil.parse(xml);
    expect(sum.hasCadence, isFalse);
    expect(sum.hasPower, isFalse);
    expect(sum.avgCadenceRpm, isNull);
    expect(sum.maxCadenceRpm, isNull);
    expect(sum.avgPowerW, isNull);
    expect(sum.maxPowerW, isNull);
    expect(sum.pointCadenceRpm, [null, null]);
    expect(sum.pointPowerW, [null, null]);
    // 设备速度被丢弃，第 2 点回落到距离 / 时间差（约 20 km/h）。
    expect(sum.pointSpeedsKmh.first, 0);
    expect(sum.pointSpeedsKmh.every((v) => v < 50), isTrue);
    expect(sum.maxSpeedKmh, lessThan(50));
  });

  test('device speed wins over distance time fallback', () {
    const xml = '''
<gpx>
<trkpt lat="32.0" lon="118.0">
  <time>2026-10-01T08:00:00Z</time>
  <extensions><gpxtpx:speed>10.0</gpxtpx:speed></extensions>
</trkpt>
<trkpt lat="32.0001" lon="118.0">
  <time>2026-10-01T08:00:00.500Z</time>
  <extensions><gpxtpx:speed>10.0</gpxtpx:speed></extensions>
</trkpt>
<trkpt lat="32.0002" lon="118.0">
  <time>2026-10-01T08:00:01Z</time>
  <extensions><gpxtpx:speed>10.0</gpxtpx:speed></extensions>
</trkpt>
</gpx>
''';
    final sum = GpxUtil.parse(xml);
    expect(sum.pointSpeedsKmh.length, 3);
    for (final v in sum.pointSpeedsKmh) {
      expect(v, closeTo(36, 0.01));
    }
    expect(sum.maxSpeedKmh, closeTo(36, 0.01));
    // 0.5 s 的点间距不在 0.8~60 s 窗口里，设备速度仍要进序列。
    expect(sum.speedPoints.length, 3);
  });

  test('gpx without extensions has no cadence or power', () {
    const xml = '''
<gpx>
<trkpt lat="32.0" lon="118.0"><time>2026-10-01T08:00:00Z</time></trkpt>
<trkpt lat="32.0001" lon="118.0"><time>2026-10-01T08:00:02Z</time></trkpt>
</gpx>
''';
    final sum = GpxUtil.parse(xml);
    expect(sum.hasCadence, isFalse);
    expect(sum.hasPower, isFalse);
    expect(sum.cadPoints, isEmpty);
    expect(sum.powerPoints, isEmpty);
    expect(sum.pointCadenceRpm.length, sum.points.length);
    expect(sum.pointPowerW.length, sum.points.length);
    expect(sum.pointCadenceRpm.every((v) => v == null), isTrue);
    expect(sum.pointPowerW.every((v) => v == null), isTrue);

    final empty = GpxUtil.parse('<gpx></gpx>');
    expect(empty.hasCadence, isFalse);
    expect(empty.hasPower, isFalse);
    expect(empty.pointCadenceRpm, isEmpty);
    expect(empty.pointPowerW, isEmpty);
  });

  test('all-zero cadence still counts as cadence data', () {
    // 传感器在报、人一路滑行：均值给不出来，但这仍然是有效数据 ——
    // 按「平均值算得出来」判断会把整张卡片藏掉，用户会以为踏频丢了。
    const xml = '''
<gpx>
<trkpt lat="32.0" lon="118.0"><extensions><gpxtpx:cad>0</gpxtpx:cad></extensions></trkpt>
<trkpt lat="32.0001" lon="118.0"><extensions><gpxtpx:cad>0</gpxtpx:cad></extensions></trkpt>
</gpx>
''';
    final sum = GpxUtil.parse(xml);
    expect(sum.hasCadence, isTrue);
    expect(sum.avgCadenceRpm, isNull);
    expect(sum.maxCadenceRpm, 0);
    expect(sum.cadPoints.length, 2);
  });

  test('sensor tags carrying their own xmlns are parsed', () {
    // 有的工具把命名空间声明直接写在字段上，开标签后面还有属性。
    const xml = '''
<gpx>
<trkpt lat="32.0" lon="118.0"><extensions>
  <cad xmlns="http://www.garmin.com/xmlschemas/TrackPointExtension/v2">88</cad>
  <speed xmlns:gpxtpx="http://www.garmin.com/xmlschemas/TrackPointExtension/v2">6.50</speed>
  <power xmlns="http://www.garmin.com/xmlschemas/PowerExtension/v1">210</power>
</extensions></trkpt>
</gpx>
''';
    final sum = GpxUtil.parse(xml);
    expect(sum.pointCadenceRpm.first, 88);
    expect(sum.pointPowerW.first, 210);
    expect(sum.pointSpeedsKmh.first, closeTo(23.4, 0.01));
  });
}
