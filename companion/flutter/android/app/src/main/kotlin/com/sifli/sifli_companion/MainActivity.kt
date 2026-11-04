package com.sifli.sifli_companion

import android.content.Intent
import android.os.Build
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {

    companion object {
        private const val CHANNEL = "com.sifli.sifli_companion/companion_fgs"
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)

        CompanionNotifPlugin.register(flutterEngine, this)

        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL)
            .setMethodCallHandler { call, result ->
                when (call.method) {
                    "start" -> {
                        val text = call.argument<String>("text")
                        startCompanionService(CompanionForegroundService.ACTION_START, text)
                        result.success(true)
                    }

                    "update" -> {
                        val text = call.argument<String>("text")
                        startCompanionService(CompanionForegroundService.ACTION_UPDATE, text)
                        result.success(true)
                    }

                    "stop" -> {
                        startCompanionService(CompanionForegroundService.ACTION_STOP, null)
                        result.success(true)
                    }

                    "isRunning" -> result.success(CompanionForegroundService.isRunning)

                    else -> result.notImplemented()
                }
            }
    }

    private fun startCompanionService(action: String, text: String?) {
        val intent = Intent(this, CompanionForegroundService::class.java).apply {
            this.action = action
            text?.let { putExtra(CompanionForegroundService.EXTRA_TEXT, it) }
        }

        // startForegroundService 要求服务在 5 秒内调用 startForeground()，
        // 停止走普通 startService 即可，避免 Android 12+ 的后台启动限制。
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O &&
            action != CompanionForegroundService.ACTION_STOP
        ) {
            startForegroundService(intent)
        } else {
            startService(intent)
        }
    }
}
