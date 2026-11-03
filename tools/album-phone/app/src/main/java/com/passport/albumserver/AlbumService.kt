package com.passport.albumserver

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.PowerManager

/**
 * 前台服务 = "一键启动"的载体:通知栏常驻(系统不会杀),持有唤醒锁
 * (熄屏后系统不深度休眠,设备随时能连进来),内部跑 PhotoHttpServer。
 */
class AlbumService : Service() {
    companion object {
        const val ACTION_STOP = "com.passport.albumserver.STOP"
        private const val CHANNEL_ID = "album_server"
        private const val NOTIF_ID = 1
        const val PORT = 8098

        @Volatile var running = false
            private set
        @Volatile private var live: AlbumService? = null
    }

    private var server: PhotoHttpServer? = null
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        live = this
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        startAsForeground(buildNotification())
        if (server == null) {
            running = true
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "passport:album").apply {
                setReferenceCounted(false)
                acquire()
            }
            AlbumStore.onPhaseChanged = { refreshNotification() }
            AlbumStore.init(applicationContext)
            server = PhotoHttpServer(
                port = PORT,
                webRoot = AlbumStore.webRoot(applicationContext),
                beforeServe = { AlbumStore.syncIfStale() },
                statusHtml = { statusPage() },
            )
            server?.start()
        }
        return START_STICKY
    }

    override fun onDestroy() {
        server?.stop()
        server = null
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
        running = false
        AlbumStore.onPhaseChanged = null
        AlbumStore.phase = "未启动"
        live = null
        super.onDestroy()
    }

    private fun startAsForeground(n: Notification) {
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        } else {
            startForeground(NOTIF_ID, n)
        }
    }

    private fun channel(): NotificationChannel? {
        if (Build.VERSION.SDK_INT < 26) return null
        val mgr = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        var ch = mgr.getNotificationChannel(CHANNEL_ID)
        if (ch == null) {
            ch = NotificationChannel(CHANNEL_ID, "相册服务", NotificationManager.IMPORTANCE_LOW)
            mgr.createNotificationChannel(ch)
        }
        return ch
    }

    private fun buildNotification(): Notification {
        val launch = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val stop = PendingIntent.getService(
            this, 1, Intent(this, AlbumService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val builder = if (Build.VERSION.SDK_INT >= 26) {
            Notification.Builder(this, channel()!!.id)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
        }
        return builder
            .setSmallIcon(R.drawable.ic_stat)
            .setContentTitle("相册服务")
            .setContentText(AlbumStore.phase)
            .setContentIntent(launch)
            .setOngoing(true)
            .addAction(0, "停止", stop)
            .build()
    }

    private fun refreshNotification() {
        val mgr = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        mgr.notify(NOTIF_ID, buildNotification())
    }

    /** 手机浏览器打开 http://<本机>:8098/ 看到的状态页。 */
    private fun statusPage(): String {
        val sb = StringBuilder()
        sb.append("<html><head><meta charset='utf-8'>")
        sb.append("<meta name='viewport' content='width=device-width,initial-scale=1'>")
        sb.append("<title>相册服务</title></head>")
        sb.append("<body style='font-family:sans-serif;max-width:36em;margin:auto'>")
        sb.append("<h2>相册服务运行中</h2>")
        sb.append("<p>状态:${AlbumStore.phase}</p>")
        AlbumStore.lastError?.let {
            sb.append("<p style='color:#c00'>最近问题:$it</p>")
        }
        sb.append("<p>把下面地址填到设备「相册源」设置(一般填 192.168 开头、结尾是 .1 的那个):</p><pre>")
        for (ip in AlbumStore.localAddresses()) sb.append("http://").append(ip).append(":").append(PORT).append('\n')
        sb.append("</pre><p>本机预览:<a href='/manifest.json'>照片清单</a></p>")
        sb.append("</body></html>")
        return sb.toString()
    }
}
