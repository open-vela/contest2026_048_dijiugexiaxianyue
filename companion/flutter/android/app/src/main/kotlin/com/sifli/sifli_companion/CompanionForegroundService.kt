package com.sifli.sifli_companion

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log

/**
 * 保活前台服务：骑行时手机在兜里、屏幕黑着，进程一旦被回收 BLE 连接即断开。
 *
 * 服务本身不碰蓝牙。它只负责让进程（以及其中的 Flutter 引擎与 Dart 侧的
 * CompanionClient）保持存活，蓝牙与定位逻辑仍在 Dart 层。
 *
 * 规格见固件仓库 vendor/my_vendor/docs/ble/companion_impl_plan.md。
 */
class CompanionForegroundService : Service() {

    companion object {
        private const val TAG = "CompanionFgs"

        private const val CHANNEL_ID = "companion_link"
        private const val NOTIFICATION_ID = 1001

        const val ACTION_START = "com.sifli.sifli_companion.action.START"
        const val ACTION_STOP = "com.sifli.sifli_companion.action.STOP"
        const val ACTION_UPDATE = "com.sifli.sifli_companion.action.UPDATE"

        const val EXTRA_TEXT = "text"

        /** 供 Dart 侧查询，避免重复启动。 */
        @Volatile
        var isRunning: Boolean = false
            private set
    }

    private var contentText: String = "正在保持与码表的连接"

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopSelfAndForeground()
                return START_NOT_STICKY
            }

            ACTION_UPDATE -> {
                intent.getStringExtra(EXTRA_TEXT)?.let { contentText = it }
                if (isRunning) {
                    notificationManager()?.notify(NOTIFICATION_ID, buildNotification())
                    return START_STICKY
                }
                // 还没起来就当作一次启动处理。
            }

            else -> {
                intent?.getStringExtra(EXTRA_TEXT)?.let { contentText = it }
            }
        }

        startInForeground()
        return START_STICKY
    }

    private fun startInForeground() {
        val notification = buildNotification()

        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(NOTIFICATION_ID, notification, resolveServiceType())
            } else {
                startForeground(NOTIFICATION_ID, notification)
            }
            isRunning = true
        } catch (e: Exception) {
            // 典型原因：以 location 类型启动但定位权限被拒（Android 会抛
            // SecurityException），或 Android 12+ 的后台启动限制。
            Log.e(TAG, "startForeground 失败: ${e.message}", e)
            isRunning = false
            stopSelf()
        }
    }

    /**
     * 按**实际已授予的权限**挑选前台服务类型。
     *
     * 以 [ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION] 启动但没有定位权限时，
     * 系统会直接抛异常，所以这里不能照搬 manifest 里声明的全集。
     */
    private fun resolveServiceType(): Int {
        var types = 0

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && hasLocationPermission()) {
            types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && hasBluetoothPermission()) {
            types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
        }

        // 一个类型都拿不到时退回 connectedDevice：仅保活、不取定位。
        if (types == 0 && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            types = ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
        }

        return types
    }

    private fun hasLocationPermission(): Boolean =
        checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED ||
            checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED

    private fun hasBluetoothPermission(): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return true
        return checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) ==
            PackageManager.PERMISSION_GRANTED
    }

    private fun buildNotification(): Notification {
        val launchIntent = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }

        val pendingFlags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        } else {
            PendingIntent.FLAG_UPDATE_CURRENT
        }

        val contentIntent =
            PendingIntent.getActivity(this, 0, launchIntent, pendingFlags)

        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL_ID)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
        }

        return builder
            .setContentTitle("Helm One")
            .setContentText(contentText)
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentIntent(contentIntent)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .build()
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return

        val channel = NotificationChannel(
            CHANNEL_ID,
            "码表连接",
            // LOW: 常驻通知，不要出声也不要横幅打扰骑行。
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = "保持与码表的蓝牙连接"
            setShowBadge(false)
        }

        notificationManager()?.createNotificationChannel(channel)
    }

    private fun notificationManager(): NotificationManager? =
        getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager

    private fun stopSelfAndForeground() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } else {
            @Suppress("DEPRECATION")
            stopForeground(true)
        }
        isRunning = false
        stopSelf()
    }

    override fun onDestroy() {
        isRunning = false
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
