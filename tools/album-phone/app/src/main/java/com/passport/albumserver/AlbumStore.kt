package com.passport.albumserver

import android.content.ContentUris
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Build
import android.provider.MediaStore
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.net.Inet4Address
import java.net.NetworkInterface
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 相册数据引擎:Mac 版 album_server.py 的 Android 移植。
 * 照片源从 src/ 目录换成系统媒体库(MediaStore),其余行为一一对应:
 *   - 按 (mtime,size) 增量同步,只转换新照片/变过的照片;
 *   - 240x320 中心裁切原图,质量 80->25 递减压到 <=18KB(设备端缓冲档位);
 *   - 60x70 缩略图(240x320 -> 60x80 -> 裁掉上下各 5 行),网格用;
 *   - 输出名清洗成纯 ASCII,重名加序号(与设备端 URL 约定配套);
 *   - 删照片自动清产物,manifest 内容变化才重写。
 */
object AlbumStore {
    const val W = 240
    const val H = 320
    // 设备拉图按实际大小精确分配堆(2026-11-03 晚固件:Content-Length+64B),
    // 但碎片堆的"最大连续块"会缩水(实测最差 7424B)。照片压 ≤6.5KB,
    // 6.7KB 的分配请求在最差状态下也拿得到块;超限逐级降分辨率(0.875×)
    const val MAX_BYTES = 6656

    // ---------- 运行状态(UI 与通知轮询读取) ----------
    @Volatile var photoCount = 0
        private set
    @Volatile var phase = "未启动"
    @Volatile var lastError: String? = null
    /** 清单超出设备上限被裁剪时的说明(UI 显示用)。 */
    @Volatile var cappedInfo: String? = null
    /** 状态行变化时通知(服务层刷新通知栏),由 AlbumService 设置。 */
    @Volatile var onPhaseChanged: (() -> Unit)? = null

    private lateinit var appContext: Context
    var cameraOnly = true
        private set
    private val syncLock = Any()
    private var lastSyncAt = 0L
    private val syncing = AtomicBoolean(false)
    private val sigs = HashMap<String, String>() // 输出名 -> "mtime:size"
    private var lastNames: List<String>? = null

    private data class Source(val id: Long, val displayName: String, val modified: Long, val size: Long)
    private data class Named(val src: Source, val name: String)

    fun webRoot(context: Context): File =
        File(context.filesDir, "web").apply { mkdirs() }

    fun loadPrefs(context: Context) {
        cameraOnly = context.getSharedPreferences("album", Context.MODE_PRIVATE)
            .getBoolean("camera_only", true)
    }

    fun setCameraOnly(context: Context, value: Boolean) {
        cameraOnly = value
        context.getSharedPreferences("album", Context.MODE_PRIVATE).edit()
            .putBoolean("camera_only", value).apply()
        syncAsync()
    }

    fun init(context: Context) {
        appContext = context.applicationContext
        synchronized(syncLock) {
            loadState(webRoot(appContext))
            lastSyncAt = 0L // 强制启动后先做一次完整增量
        }
        syncAsync()
    }

    /** HTTP 取文件前的增量检查,限频 2s(与 Mac 版"每次请求前对账"同构)。
     *  整理进行中直接放行:先供应旧清单,绝不让设备端请求排队等转换。 */
    fun syncIfStale() {
        if (syncing.get()) return
        if (System.currentTimeMillis() - lastSyncAt < 2000) return
        if (!syncing.compareAndSet(false, true)) return
        try {
            synchronized(syncLock) { runSync() }
        } finally {
            syncing.set(false)
        }
    }

    /** 后台全量增量同步(启动/切照片范围时调);进行中则跳过。 */
    fun syncAsync() {
        if (!syncing.compareAndSet(false, true)) return
        Thread({
            try {
                synchronized(syncLock) { runSync() }
            } finally {
                syncing.set(false)
            }
        }, "album-sync").start()
    }

    // ---------- 同步主流程 ----------

    private fun runSync() {
        try {
            lastError = null
            val web = webRoot(appContext)
            val thumbs = File(web, "thumbs").apply { mkdirs() }

            val sources = queryPhotos()
            Log.d(TAG, "runSync query=${sources.size} cameraOnly=$cameraOnly")
            val seen = HashMap<String, Int>()

            // 设备清单池按档位 16K/12K/8K/4K 堆分配(2026-11-03 固件升级后),
            // 富余档 16K≈收录 600 名;预算 14.5K 确保落在最大档内,堆紧张降档
            // 时设备端按前缀截断(收录多少算多少)
            val named = ArrayList<Named>(sources.size)
            for (src in sources) named.add(Named(src, assignName(src.displayName, seen)))
            var used = 48 // {"rev":...,"photos":[...]} 外壳 + 余量
            var cut = 0
            while (cut < named.size) {
                val cost = named[cut].name.length + 3 // 引号对 + 逗号
                if (used + cost > 14500) break
                used += cost
                cut++
            }
            cappedInfo =
                if (cut < named.size) "手机共 ${named.size} 张,受设备清单上限限制,收录最新 $cut 张"
                else null
            Log.d(TAG, "runSync manifest cut=$cut/${named.size} ~${used}B")

            val keep = HashSet<String>()
            val entries = ArrayList<String>(cut)
            var done = 0
            for (i in 0 until cut) {
                val n = named[i]
                done++
                phase = "整理照片中 $done/$cut"
                onPhaseChanged?.invoke()
                val outName = n.name
                // sig 带 v4 前缀:黑底画布补齐后强制重编码(只影响输出的那批)
                val sig = "4/${n.src.modified}:${n.src.size}"
                val target = File(web, outName)
                val thumb = File(thumbs, outName)
                if (sigs[outName] == sig && target.isFile && thumb.isFile) {
                    keep.add(outName)
                    entries.add(outName)
                    continue
                }
                // 转换成功才收录进清单(与 Mac 版"skip 后不进 entries"一致);坏图只跳过不中断
                try {
                    val bmp = loadBitmap(n.src.id)
                    if (bmp == null) {
                        lastError = "无法解码 ${n.src.displayName}"
                        continue
                    }
                    writeConverted(bmp, target, thumb)
                    bmp.recycle()
                    sigs[outName] = sig
                    keep.add(outName)
                    entries.add(outName)
                } catch (e: Exception) {
                    lastError = "转换 ${n.src.displayName} 出错: ${e.message}"
                }
            }

            // src(媒体库)里已删除的照片,清掉产物
            val iter = sigs.entries.iterator()
            while (iter.hasNext()) {
                val e = iter.next()
                if (e.key !in keep) {
                    File(web, e.key).delete()
                    File(thumbs, e.key).delete()
                    iter.remove()
                }
            }

            // 清单:顺序 = 最新在前;内容变化且非空才重写(空清单沿用旧文件,与 Mac 版一致)
            if (entries.isNotEmpty() && entries != lastNames) {
                val obj = JSONObject()
                obj.put("rev", System.currentTimeMillis() / 1000)
                obj.put("photos", JSONArray(entries))
                atomicWrite(File(web, "manifest.json"), obj.toString())
            }
            val stateObj = JSONObject()
            for ((k, v) in sigs) stateObj.put(k, v)
            atomicWrite(File(web, ".sync_state.json"), stateObj.toString())
            lastNames = entries

            photoCount = entries.size
            phase = if (entries.isEmpty()) "没有可展示的照片" else "运行中 · ${entries.size} 张照片"
            lastSyncAt = System.currentTimeMillis()
            Log.d(TAG, "runSync done entries=${entries.size}")
        } catch (e: Exception) {
            lastError = e.message ?: e.javaClass.simpleName
            phase = "同步出错"
            Log.w(TAG, "runSync failed", e)
        }
        onPhaseChanged?.invoke()
    }

    private const val TAG = "AlbumSync"

    /** 查媒体库:只收 jpeg/png/webp/bmp/heic;相机优先模式按目录过滤;最新在前。 */
    private fun queryPhotos(): List<Source> {
        val out = ArrayList<Source>()
        val proj = arrayOf(
            MediaStore.Images.Media._ID,
            MediaStore.Images.Media.DISPLAY_NAME,
            MediaStore.Images.Media.DATE_MODIFIED,
            MediaStore.Images.Media.SIZE,
        )
        val mimeIn = "mime_type IN ('image/jpeg','image/png','image/webp'," +
            "'image/bmp','image/x-ms-bmp','image/heic','image/heif')"
        val selection: String
        val args: Array<String>?
        if (cameraOnly) {
            if (Build.VERSION.SDK_INT >= 29) {
                selection = "$mimeIn AND relative_path LIKE ?"
                args = arrayOf("DCIM/Camera%")
            } else {
                selection = "$mimeIn AND bucket_display_name LIKE ?"
                args = arrayOf("Camera%")
            }
        } else {
            selection = mimeIn
            args = null
        }
        appContext.contentResolver.query(
            MediaStore.Images.Media.EXTERNAL_CONTENT_URI,
            proj, selection, args,
            "${MediaStore.Images.Media.DATE_ADDED} DESC, ${MediaStore.Images.Media._ID} DESC",
        )?.use { c ->
            val ci = c.getColumnIndexOrThrow(MediaStore.Images.Media._ID)
            val cn = c.getColumnIndexOrThrow(MediaStore.Images.Media.DISPLAY_NAME)
            val cm = c.getColumnIndexOrThrow(MediaStore.Images.Media.DATE_MODIFIED)
            val cs = c.getColumnIndexOrThrow(MediaStore.Images.Media.SIZE)
            while (c.moveToNext()) {
                val name = c.getString(cn) ?: continue
                out.add(Source(c.getLong(ci), name, c.getLong(cm), c.getLong(cs)))
            }
        }
        return out
    }

    /** 与 Mac 版 safe_name 同规则:非 ASCII/符号一律下划线,前后 trim,重名加 _N。 */
    private fun assignName(displayName: String, seen: MutableMap<String, Int>): String {
        val stem = displayName.substringBeforeLast('.', displayName)
        var safe = buildString {
            for (ch in stem) {
                append(
                    if (ch in 'a'..'z' || ch in 'A'..'Z' || ch in '0'..'9' || ch in "._-") ch else '_'
                )
            }
        }.trim('_')
        if (safe.isEmpty()) safe = "photo"
        val key = safe.lowercase()
        return if (!seen.containsKey(key)) {
            seen[key] = 0
            "$safe.jpg"
        } else {
            seen[key] = seen[key]!! + 1
            val base = if (safe.contains('_')) safe.substringBeforeLast('_') else safe
            "${base}_${seen[key]}.jpg"
        }
    }

    /** 解码媒体库照片并中心裁切成 240x320(短边贴满、居中,与 Mac 版同构)。 */
    private fun loadBitmap(id: Long): Bitmap? {
        val uri = ContentUris.withAppendedId(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, id)
        val resolver = appContext.contentResolver
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        // bounds 模式的 decodeStream 约定返回 null,只读 outWidth/outHeight,不能用返回值判失败
        resolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, bounds) }
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
        var sample = 1
        while (bounds.outWidth / (sample * 2) >= W && bounds.outHeight / (sample * 2) >= H) sample *= 2
        val opts = BitmapFactory.Options().apply { inSampleSize = sample }
        val src = resolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, opts) }
            ?: return null

        val ratio = W.toFloat() / H
        val cropW: Int
        val cropH: Int
        if (src.width.toFloat() / src.height > ratio) {
            cropH = src.height
            cropW = minOf(src.width, Math.round(src.height * ratio))
        } else {
            cropW = src.width
            cropH = minOf(src.height, Math.round(src.width / ratio))
        }
        if (cropW < 2 || cropH < 2) {
            src.recycle()
            return null
        }
        val x = (src.width - cropW) / 2
        val y = (src.height - cropH) / 2
        val cropped =
            if (x == 0 && y == 0 && cropW == src.width && cropH == src.height) src
            else Bitmap.createBitmap(src, x, y, cropW, cropH)
        val out =
            if (cropped.width == W && cropped.height == H) cropped
            else Bitmap.createScaledBitmap(cropped, W, H, true)
        if (out !== cropped) cropped.recycle()
        if (cropped !== src) src.recycle()
        return out
    }

    /** 原图 80->25 递减压到 MAX_BYTES;高细节照片仍超就逐级降分辨率,
     *  再贴到 240x320 黑底画布(与 Mac 版底色一致)——设备端全屏不清屏,
     *  非 240x320 的图会残留上一张的画面(用户看到的"重叠")。 */
    private fun writeConverted(bmp: Bitmap, target: File, thumb: File) {
        var work = bmp
        var workOwned = false
        var bytes = jpegBytes(work, 80, MAX_BYTES)
        while (bytes.size > MAX_BYTES && work.width > 168) {
            val next = Bitmap.createScaledBitmap(
                work, Math.round(work.width * 0.875f), Math.round(work.height * 0.875f), true
            )
            if (workOwned) work.recycle()
            work = next
            workOwned = true
            bytes = jpegBytes(work, 80, MAX_BYTES)
        }
        if (work.width != W || work.height != H) {
            val canvas = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888)
            val c = android.graphics.Canvas(canvas)
            c.drawColor(android.graphics.Color.rgb(16, 18, 18))
            c.drawBitmap(work, (W - work.width) / 2f, (H - work.height) / 2f, null)
            if (workOwned) work.recycle()
            work = canvas
            workOwned = true
            bytes = jpegBytes(work, 80, MAX_BYTES)
        }
        atomicWriteBytes(target, bytes)
        if (workOwned) work.recycle()

        val tall = Bitmap.createScaledBitmap(bmp, 60, 80, true)
        val small = Bitmap.createBitmap(tall, 0, 5, 60, 70)
        val bos = ByteArrayOutputStream()
        small.compress(Bitmap.CompressFormat.JPEG, 75, bos)
        atomicWriteBytes(thumb, bos.toByteArray())
        small.recycle()
        tall.recycle()
    }

    private fun jpegBytes(bmp: Bitmap, startQuality: Int, maxBytes: Int): ByteArray {
        var q = startQuality
        while (true) {
            val bos = ByteArrayOutputStream()
            bmp.compress(Bitmap.CompressFormat.JPEG, q, bos)
            val bytes = bos.toByteArray()
            if (bytes.size <= maxBytes || q <= 25) return bytes
            q -= 5
        }
    }

    private fun atomicWrite(f: File, content: String) = atomicWriteBytes(f, content.toByteArray(Charsets.UTF_8))

    private fun atomicWriteBytes(f: File, bytes: ByteArray) {
        val tmp = File(f.parentFile, f.name + ".tmp")
        tmp.writeBytes(bytes)
        if (f.exists()) f.delete()
        if (!tmp.renameTo(f)) {
            tmp.delete()
            f.writeBytes(bytes)
        }
    }

    private fun loadState(web: File) {
        sigs.clear()
        lastNames = null
        val f = File(web, ".sync_state.json")
        if (!f.isFile) return
        try {
            val obj = JSONObject(f.readText())
            for (key in obj.keys()) sigs[key] = obj.getString(key)
        } catch (_: Exception) {
            sigs.clear() // 状态坏了就当全量重建,代价只是重转一遍
        }
    }

    /** 本机"能被局域网设备访问"的 IPv4:只收 WiFi/热点网卡(接口名含 wlan 或 ap 开头),
     *  排除 VPN/蜂窝等设备永远够不到的干扰地址;接口名不标准时退回全量。 */
    fun localAddresses(): List<String> {
        val tagged = ArrayList<Pair<String, String>>()
        try {
            val en = NetworkInterface.getNetworkInterfaces() ?: return emptyList()
            while (en.hasMoreElements()) {
                val nif = en.nextElement()
                val addrs = nif.inetAddresses
                while (addrs.hasMoreElements()) {
                    val addr = addrs.nextElement()
                    if (addr !is Inet4Address || addr.isLoopbackAddress || addr.isLinkLocalAddress) continue
                    val host = addr.hostAddress ?: continue
                    tagged.add(host to nif.name)
                }
            }
        } catch (_: Exception) {}
        val wlan = tagged.filter { it.second.contains("wlan") || it.second.startsWith("ap") }
            .map { it.first }
            .distinct()
        val list = (if (wlan.isNotEmpty()) wlan else tagged.map { it.first }.distinct()).toMutableList()
        list.sortWith(
            compareByDescending<String> { it.startsWith("192.168.") }
                .thenByDescending { it.endsWith(".1") }
        )
        return list
    }
}
