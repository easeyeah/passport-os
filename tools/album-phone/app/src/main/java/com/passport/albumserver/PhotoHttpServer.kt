package com.passport.albumserver

import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.URLDecoder
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * 面向 AI Passport 相册 App 的极简 HTTP 静态服务,只提供三类资源:
 *   /manifest.json      照片清单
 *   /<name>.jpg         240x320 原图(<=18KB)
 *   /thumbs/<name>.jpg  60x70 缩略图
 * 外加人类可读的 "/" 状态页(手机浏览器可打开预览)。每个连接一个工作线程,
 * 固定 Connection: close(设备端 http 客户端就是短连接)。
 */
class PhotoHttpServer(
    private val port: Int,
    private val webRoot: File,
    private val beforeServe: () -> Unit,
    private val statusHtml: () -> String,
) {
    private val running = AtomicBoolean(false)
    private var serverSocket: ServerSocket? = null
    private val workers = Executors.newCachedThreadPool()

    fun start() {
        if (!running.compareAndSet(false, true)) return
        thread(name = "album-http") {
            try {
                serverSocket = ServerSocket(port, 16, InetAddress.getByName("0.0.0.0"))
                while (running.get()) {
                    val sock = serverSocket!!.accept()
                    workers.execute { handle(sock) }
                }
            } catch (_: Exception) {
                // stop() 关闭 socket 会让 accept 抛异常,属正常退出路径
            }
        }
    }

    fun stop() {
        running.set(false)
        try {
            serverSocket?.close()
        } catch (_: Exception) {}
        workers.shutdownNow()
    }

    private fun handle(sock: Socket) {
        try {
            sock.soTimeout = 15000
            val input = sock.getInputStream().buffered()
            val requestLine = input.readHttpLine() ?: return
            // 逐行丢弃请求头,直到空行(GET 无请求体,不需要再读)
            while (true) {
                val line = input.readHttpLine() ?: break
                if (line.isEmpty()) break
            }
            val parts = requestLine.trim().split(" ")
            if (parts.size < 2) return
            val method = parts[0].uppercase()
            val headOnly = method == "HEAD"
            val out = sock.getOutputStream()
            if (method != "GET" && !headOnly) {
                respond(out, 405, "Method Not Allowed", "method not allowed".toByteArray(), "text/plain", false, false)
                return
            }
            var path = parts[1]
            val q = path.indexOf('?')
            if (q >= 0) path = path.substring(0, q)
            path = URLDecoder.decode(path, "UTF-8")
            android.util.Log.d("AlbumHttp", "req $method $path")
            if (path == "/" || path.isEmpty()) {
                respond(out, 200, "OK", statusHtml().toByteArray(Charsets.UTF_8), "text/html; charset=utf-8", false, headOnly)
                return
            }
            try {
                beforeServe() // 没变化时只是一圈对账,毫秒级;失败不拦服务(与 Mac 版一致)
            } catch (e: Exception) {
                android.util.Log.w("AlbumHttp", "beforeServe failed", e)
            }
            serve(out, path, headOnly)
        } catch (_: Exception) {
            // 客户端提前断开等,忽略
        } finally {
            try {
                sock.close()
            } catch (_: Exception) {}
        }
    }

    private fun serve(out: OutputStream, rawPath: String, headOnly: Boolean) {
        val root = webRoot.canonicalFile
        val file = File(root, rawPath.trimStart('/')).canonicalFile
        // 路径越狱防护:规范化后必须仍落在 webRoot 内
        if (!file.path.startsWith(root.path + File.separator) || !file.isFile) {
            respond(out, 404, "Not Found", "not found".toByteArray(), "text/plain", false, headOnly)
            return
        }
        val type = when (file.extension.lowercase()) {
            "json" -> "application/json"
            "jpg", "jpeg" -> "image/jpeg"
            "html" -> "text/html; charset=utf-8"
            else -> "application/octet-stream"
        }
        respond(out, 200, "OK", file.readBytes(), type, true, headOnly)
    }

    private fun respond(out: OutputStream, code: Int, reason: String, body: ByteArray, type: String, cacheable: Boolean, headOnly: Boolean) {
        val head = buildString {
            append("HTTP/1.1 ").append(code).append(' ').append(reason).append("\r\n")
            append("Content-Type: ").append(type).append("\r\n")
            append("Content-Length: ").append(body.size).append("\r\n")
            if (cacheable) append("Cache-Control: max-age=3600\r\n")
            append("Connection: close\r\n\r\n")
        }
        out.write(head.toByteArray(Charsets.ISO_8859_1))
        if (!headOnly) out.write(body)
        out.flush()
    }

    /** 读一行 HTTP 头;返回 null 表示连接已关。 */
    private fun InputStream.readHttpLine(): String? {
        val sb = StringBuilder()
        while (true) {
            val b = read()
            if (b == -1) return if (sb.isEmpty()) null else sb.toString()
            if (b == '\n'.code) break
            if (b != '\r'.code) sb.append(b.toChar())
        }
        return sb.toString()
    }
}
