package com.passport.albumserver

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast

/**
 * 一个大按钮 + 一块地址牌:启动后把要填到设备里的 http://…:8098 直接显示在
 * 屏幕上,使用步骤写在界面上。零第三方依赖,布局全部代码生成。
 */
class MainActivity : Activity() {
    private lateinit var statusView: TextView
    private lateinit var urlView: TextView
    private lateinit var toggle: Button
    private lateinit var scopeSwitch: Switch
    private val handler = Handler(Looper.getMainLooper())
    private var pendingStart = false

    private val ticker = object : Runnable {
        override fun run() {
            refresh()
            handler.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        AlbumStore.loadPrefs(this)
        setContentView(buildUi())
        scopeSwitch.isChecked = AlbumStore.cameraOnly
        scopeSwitch.setOnCheckedChangeListener { _, checked ->
            AlbumStore.setCameraOnly(this, checked)
        }
        toggle.setOnClickListener { onToggle() }
    }

    override fun onResume() {
        super.onResume()
        handler.post(ticker)
    }

    override fun onPause() {
        super.onPause()
        handler.removeCallbacks(ticker)
    }

    private fun buildUi(): android.view.View {
        val pad = dp(20)
        val scroll = ScrollView(this)
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, dp(28), pad, pad)
        }
        scroll.addView(col)

        col.addView(TextView(this).apply {
            text = "AI Passport 相册服务"
            textSize = 24f
            gravity = Gravity.CENTER
            setPadding(0, 0, 0, dp(20))
        })

        statusView = TextView(this).apply {
            textSize = 16f
            setPadding(0, 0, 0, dp(12))
        }
        col.addView(statusView)

        urlView = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            textSize = 15f
            setTextColor(0xFF1B5E20.toInt())
            setBackgroundColor(0xFFF2F2EE.toInt())
            setPadding(dp(10), dp(10), dp(10), dp(10))
            setTextIsSelectable(true)
        }
        col.addView(urlView)

        col.addView(TextView(this).apply {
            text = "\n照片范围"
            textSize = 15f
            setPadding(0, dp(14), 0, dp(4))
        })
        scopeSwitch = Switch(this).apply { text = "只收录相机拍摄的照片(推荐)" }
        col.addView(scopeSwitch)

        toggle = Button(this).apply {
            textSize = 18f
            setPadding(0, dp(14), 0, dp(14))
        }
        col.addView(toggle, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        col.addView(TextView(this).apply {
            text = """
                
                使用步骤:
                1. 手机开热点:把「AP 频段」选成 2.4GHz(设备只认 2.4GHz),
                   并关闭「无人连接时自动关闭热点」;
                2. 点「启动服务」;
                3. 设备的 WiFi 设置里连接这个手机热点;
                4. 打开设备设置网页(平时往设备传照片用的那个),
                   把相册源地址改成上面绿框里的 http://…:8098;
                5. 设备上打开「相册」。手机随手拍的新照片会自动出现,
                   删掉的照片也会自动消失。
                
                提示:相册画面走的是设备与本机的点对点传输,不耗流量套餐;
                通知栏里有「停止」按钮,看完照片建议随手停掉省电。
            """.trimIndent()
            textSize = 14f
            setTextColor(0xFF666666.toInt())
            setLineSpacing(dp(2).toFloat(), 1f)
        })
        return scroll
    }

    private fun refresh() {
        val running = AlbumService.running
        toggle.text = if (running) "停止服务" else "启动服务"
        statusView.text = buildString {
            append(if (running) "状态:${AlbumStore.phase}" else "状态:未启动")
            AlbumStore.cappedInfo?.let { append("\n$it") }
        }
        val sb = StringBuilder()
        if (running) {
            sb.append("把下面地址填到设备相册源设置:\n")
            val ips = AlbumStore.localAddresses()
            if (ips.isEmpty()) {
                sb.append("(还没拿到网络地址,请确认热点已开启…)\n")
            }
            for (ip in ips) {
                sb.append("http://").append(ip).append(":").append(AlbumService.PORT)
                if (ip.startsWith("192.168.") && ip.endsWith(".1")) sb.append("   ← 一般填这个")
                sb.append('\n')
            }
        } else {
            sb.append("启动后,要填到设备里的地址会显示在这里。")
        }
        if (running && AlbumStore.lastError != null) sb.append("\n最近问题:").append(AlbumStore.lastError)
        urlView.text = sb.toString()
    }

    private fun onToggle() {
        if (AlbumService.running) {
            val i = Intent(this, AlbumService::class.java).setAction(AlbumService.ACTION_STOP)
            startService(i)
            return
        }
        if (!hasPhotoPermission()) {
            pendingStart = true
            requestPhotoPermissions()
            return
        }
        startServer()
    }

    private fun hasPhotoPermission(): Boolean {
        return if (Build.VERSION.SDK_INT >= 33) {
            checkSelfPermission(Manifest.permission.READ_MEDIA_IMAGES) == PackageManager.PERMISSION_GRANTED ||
                checkSelfPermission(Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED) == PackageManager.PERMISSION_GRANTED
        } else {
            checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
        }
    }

    private fun requestPhotoPermissions() {
        val perms = ArrayList<String>()
        if (Build.VERSION.SDK_INT >= 33) {
            perms.add(Manifest.permission.READ_MEDIA_IMAGES)
            perms.add(Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED)
            perms.add(Manifest.permission.POST_NOTIFICATIONS)
        } else {
            perms.add(Manifest.permission.READ_EXTERNAL_STORAGE)
        }
        requestPermissions(perms.toTypedArray(), 1)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != 1) return
        if (pendingStart) {
            pendingStart = false
            if (hasPhotoPermission()) {
                startServer()
            } else {
                Toast.makeText(this, "没有照片权限,无法提供相册服务", Toast.LENGTH_LONG).show()
            }
        }
    }

    private fun startServer() {
        val i = Intent(this, AlbumService::class.java)
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(i) else startService(i)
        Toast.makeText(this, "正在启动…", Toast.LENGTH_SHORT).show()
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()
}
