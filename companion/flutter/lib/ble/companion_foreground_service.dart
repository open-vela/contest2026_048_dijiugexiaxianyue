import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:sifli_companion/log/app_log.dart';

/// 保活前台服务的 Dart 侧入口（仅 Android 有效）。
///
/// 骑行时手机在兜里、屏幕黑着，进程一旦被系统回收，BLE 连接就断了 —— 文件传输、
/// 通知转发、GNSS 推送会一起失效。因此这是所有后台功能的前提，而不是可选项。
///
/// iOS 不走这条路径：那边靠 Info.plist 的 `bluetooth-central` 后台模式，
/// 本类在 iOS 上所有方法都是空操作。
abstract class CompanionForegroundService {
  static const _channel =
      MethodChannel('com.sifli.sifli_companion/companion_fgs');

  static const _tag = 'CompanionFgs';

  /// 当前平台是否需要（且支持）前台服务。
  static bool get isSupported => !kIsWeb && Platform.isAndroid;

  /// 服务当前是否在跑（常驻通知正在显示）。
  ///
  /// 由本类统一维护：`start()` 成功、`isRunning()` 查询、`stop()` 都会更新它。
  /// 连接状态驱动的文案刷新靠它判断"通知栏此刻是否存在"。
  static bool _up = false;

  /// 抢占式文案。非空时连接状态的自动刷新让位 —— 升级固件这类提示不能被
  /// "正在连接码表…"顶掉。见 [pin] / [updateLinkState]。
  static String? _pinned;

  /// 最近一次连接状态文案，由 [updateLinkState] 记录。
  ///
  /// 用途是让 [start] 也能显示真实状态：状态刷新是事件驱动的，而"启动保活"这件事
  /// 常常发生在状态**已经**就绪之后（例如设备页刚连上就拉服务），那一刻不会再有任何
  /// 状态变化，只按事件刷新就会让通知栏一直停在入参那句占位文案上。
  static String? _lastLinkText;

  /// 申请前台服务通知所需权限。
  ///
  /// Android 13+ 不给 `POST_NOTIFICATIONS` 时，常驻通知不显示，前台服务也就
  /// 起不来。定位与蓝牙权限由 `BleManager.requestPermissions` 负责。
  static Future<bool> requestNotificationPermission() async {
    if (!isSupported) return true;

    final status = await Permission.notification.request();
    return status.isGranted || status.isLimited;
  }

  /// 申请后台定位权限。
  ///
  /// 必须在已拿到前台定位权限之后再调用，否则系统会直接拒绝。Android 11+ 还会
  /// 把用户引导到设置页手动选择「始终允许」。
  static Future<bool> requestBackgroundLocation() async {
    if (!isSupported) return true;

    if (!await Permission.locationWhenInUse.isGranted) {
      await AppLog.w(_tag, '未取得前台定位权限，跳过后台定位申请');
      return false;
    }

    final status = await Permission.locationAlways.request();
    return status.isGranted;
  }

  /// 启动前台服务。[text] 为常驻通知的正文 —— 但只是**兜底**：有抢占文案（[pin]）
  /// 或已知连接状态（[_lastLinkText]）时优先显示它们，这样通知栏一开始讲的就是真话。
  ///
  /// [onStarted] 在服务**确认已启动**之后回调 —— 后台启动时没有任何 UI 路径会去
  /// 连码表，所以连接必须由调用方在这里踢一脚（见 CompanionClient.ensureConnected /
  /// kickReconnect）。不传则行为与以前一致。
  static Future<void> start({
    String text = '正在保持与码表的连接',
    Future<void> Function()? onStarted,
  }) async {
    if (!isSupported) return;

    try {
      await _channel.invokeMethod<bool>(
        'start',
        {'text': _pinned ?? _lastLinkText ?? text},
      );
      _up = true;
      await AppLog.i(_tag, '前台服务已启动');

      /* 后台启动的关键一步：服务起来了但没人连码表（前台服务本身只管保活与通知），
       * 于是通知转发、文件、GNSS 推送全都"活着但不工作"。所以在这里把连接踢起来。 */
      if (onStarted != null) {
        await onStarted();
      }
    } on PlatformException catch (e) {
      await AppLog.e(_tag, '前台服务启动失败: ${e.message}');
    }
  }

  /// 无条件更新常驻通知正文（显式调用方用，例如显示剩余空间）。
  ///
  /// 按连接状态自动刷新的路径请用 [updateLinkState]。
  static Future<void> update(String text) async {
    if (!isSupported) return;
    await _push(text);
  }

  /// 连接状态驱动的正文刷新。
  ///
  /// 与 [update] 的区别是两道让位：有抢占文案（[pin]）时不动，服务没在跑时也不动
  /// —— 后者很关键，平台侧收到 update 而服务未启动时会把它当成一次启动，那样
  /// "状态变了"这种小事就会把前台服务拉起来。文案仍然记下来（见 [_lastLinkText]），
  /// 供之后 [start] 使用。
  static Future<void> updateLinkState(String text) async {
    _lastLinkText = text;
    if (_pinned != null || !_up) return;
    await _push(text);
  }

  /// 用一句高优先级文案接管通知栏，直到 [unpin]。
  ///
  /// 调用方自己确保服务已（或即将）启动并带上同样的 [text]；这里只负责登记
  /// 占用，避免随后被连接状态覆盖。
  static Future<void> pin(String text) async {
    _pinned = text;
    if (_up) await _push(text);
  }

  /// 释放 [pin] 的占用，之后连接状态重新接管通知栏。
  static Future<void> unpin() async {
    _pinned = null;
  }

  /// 停止前台服务。
  static Future<void> stop() async {
    if (!isSupported) return;

    try {
      await _channel.invokeMethod<bool>('stop');
      _up = false;
      _pinned = null;
      await AppLog.i(_tag, '前台服务已停止');
    } on PlatformException catch (e) {
      await AppLog.w(_tag, '前台服务停止失败: ${e.message}');
    }
  }

  /// 查询前台服务是否在运行。
  static Future<bool> isRunning() async {
    if (!isSupported) return false;

    try {
      _up = await _channel.invokeMethod<bool>('isRunning') ?? false;
      return _up;
    } on PlatformException {
      return false;
    }
  }

  static Future<void> _push(String text) async {
    try {
      await _channel.invokeMethod<bool>('update', {'text': text});
    } on PlatformException catch (e) {
      await AppLog.w(_tag, '前台通知更新失败: ${e.message}');
    }
  }
}
