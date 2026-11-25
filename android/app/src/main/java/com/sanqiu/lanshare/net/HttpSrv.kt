package com.sanqiu.lanshare.net

import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.Executors
import java.util.concurrent.Semaphore
import java.util.concurrent.atomic.AtomicInteger

/**
 * 自实现的 HTTP/1.1 子集服务端。
 *
 * 与 Windows（`windows/src/HttpServer.cpp`）和鸿蒙（`common/HttpSrv.ets`）**同契约**：
 *  - 只支持 `GET` / `POST`，**不支持 chunked**，每响应后关连接；
 *  - 头部上限 64 KB，普通体 4 MB，上传 512 MB（移动端由上层再限 64 MB）；
 *  - 并发上限 64，超出直接回 `503`。
 *
 * 与鸿蒙版的差别只有一处，且是**变少**：Android 的 `InputStream` 是阻塞语义，
 * 读满 `Content-Length` 才返回，所以鸿蒙那套"自己维护字节队列处理半包/粘包"
 * 在这里可以整段删掉 —— 但**协议行为不能变**，否则三端会分叉。
 */
class HttpRequest(
    val method: String,
    val path: String,
    val query: Map<String, String>,
    /** 键统一小写 */
    val headers: Map<String, String>,
    val body: ByteArray,
) {
    fun header(name: String): String = headers[name.lowercase()] ?: ""

    /** 是否 v2 请求（带 `X-Device-Id`）—— 分流唯一依据 */
    fun isV2(): Boolean = header("x-device-id").isNotEmpty()

    fun bodyText(): String = if (body.isEmpty()) "" else String(body, Charsets.UTF_8)

    fun q(name: String, def: String = ""): String = query[name] ?: def

    fun qNum(name: String, def: Long = 0L): Long = q(name, "").trim().toLongOrNull() ?: def
}

class HttpResponse(
    val status: Int = 200,
    val contentType: String = "application/json; charset=utf-8",
    val text: String = "",
    val bytes: ByteArray? = null,
    val extraHeaders: Map<String, String> = emptyMap(),
) {
    companion object {
        fun json(body: String): HttpResponse = HttpResponse(text = body)

        fun raw(body: ByteArray, contentType: String): HttpResponse =
            HttpResponse(contentType = contentType, bytes = body)

        fun status(code: Int, message: String): HttpResponse =
            HttpResponse(status = code, text = "{\"ok\":false,\"error\":${quote(message)}}")

        private fun quote(s: String): String =
            "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"") + "\""
    }
}

class HttpServer(
    private val handler: (HttpRequest) -> HttpResponse,
    private val maxConn: Int = 64,
) {
    @Volatile private var serverSocket: ServerSocket? = null
    private var acceptThread: Thread? = null
    private val permits = Semaphore(maxConn)
    private val pool = Executors.newCachedThreadPool()
    private val activeCount = AtomicInteger(0)

    /** 实际监听端口（端口递进后的真值，对端必须以它为准） */
    @Volatile var port: Int = 0
        private set

    @Volatile var lastError: String = ""
        private set

    val isRunning: Boolean get() = serverSocket?.isClosed == false

    /**
     * 绑定并开始 accept。
     *
     * 端口递进与 Windows 一致：从 [portStart] 起试 [tries] 个。
     * 全部占用则抛异常 —— **不允许静默监听在随机端口上**（对端 announce
     * 会带错端口，症状是"能发现、连不上"）。
     */
    fun start(host: String = "0.0.0.0", portStart: Int = 8765, tries: Int = 3): Int {
        var ss: ServerSocket? = null
        var lastException: Exception? = null
        for (i in 0 until tries) {
            val p = portStart + i
            try {
                val candidate = ServerSocket()
                candidate.reuseAddress = true
                candidate.bind(InetSocketAddress(host, p), 32)
                ss = candidate
                port = candidate.localPort
                break
            } catch (e: Exception) {
                lastException = e
            }
        }
        if (ss == null) {
            val msg = "端口 ${portStart}..${portStart + tries - 1} 全部不可用: " +
                "${lastException?.javaClass?.simpleName}: ${lastException?.message}"
            lastError = msg
            throw IllegalStateException(msg, lastException)
        }
        serverSocket = ss
        lastError = ""
        val t = Thread({ acceptLoop(ss) }, "lanshare-http-accept")
        t.isDaemon = true
        acceptThread = t
        t.start()
        return port
    }

    fun stop() {
        runCatching { serverSocket?.close() }
        acceptThread?.interrupt()
        pool.shutdownNow()
    }

    private fun acceptLoop(ss: ServerSocket) {
        while (!ss.isClosed) {
            val client = try {
                ss.accept()
            } catch (e: Exception) {
                if (ss.isClosed) break
                lastError = "accept failed: ${e.javaClass.simpleName}: ${e.message}"
                continue
            }
            if (!permits.tryAcquire()) {
                // 超并发：如实回 503，不排队、不丢弃（丢弃会变成"偶发连不上"）
                runCatching { writeResponse(client, HttpResponse.status(503, "too many connections")) }
                runCatching { client.close() }
                continue
            }
            activeCount.incrementAndGet()
            pool.execute {
                try {
                    handle(client)
                } finally {
                    activeCount.decrementAndGet()
                    permits.release()
                    runCatching { client.close() }
                }
            }
        }
    }

    private fun handle(client: Socket) {
        try {
            // 关 Nagle：本项目"一连接一请求"，响应头和 body 分两次写，
            // 开着 Nagle 会被推迟到对端 ACK（40ms 级），分片上传时每片都吃一次。
            runCatching { client.tcpNoDelay = true }
            client.soTimeout = READ_TIMEOUT_MS
            val input = BufferedInputStream(client.getInputStream())
            val head = readHead(input) ?: return
            val parsed = parseHead(head) ?: run {
                writeResponse(client, HttpResponse.status(400, "bad request"))
                return
            }

            val contentLength = parsed.headers["content-length"]?.trim()?.toLongOrNull() ?: 0L
            if (contentLength > MAX_UPLOAD_BYTES) {
                writeResponse(client, HttpResponse.status(413, "payload too large"))
                return
            }
            if (contentLength > MAX_BODY_BYTES && !parsed.path.startsWith("/api/upload")) {
                writeResponse(client, HttpResponse.status(413, "body too large"))
                return
            }
            val body = if (contentLength > 0) readExactly(input, contentLength.toInt()) else ByteArray(0)

            val req = HttpRequest(
                method = parsed.method,
                path = parsed.path,
                query = parsed.query,
                headers = parsed.headers,
                body = body,
            )
            val resp = try {
                handler(req)
            } catch (e: Exception) {
                HttpResponse.status(500, "internal error: ${e.javaClass.simpleName}")
            }
            writeResponse(client, resp)
        } catch (e: Exception) {
            // 失败要响亮：读超时/断连是常态，但异常类型要留痕
            lastError = "${e.javaClass.simpleName}: ${e.message}"
        }
    }

    private data class Head(
        val method: String,
        val path: String,
        val query: Map<String, String>,
        val headers: Map<String, String>,
    )

    /** 读请求头（到空行为止）。返回 null 表示连接已断/无内容。 */
    private fun readHead(input: InputStream): String? {
        val buf = ByteArrayOutputStream(512)
        var prev3 = 0
        var prev2 = 0
        var prev1 = 0
        var total = 0
        while (total < MAX_HEAD_BYTES) {
            val b = input.read()
            if (b < 0) return if (total == 0) null else buf.toString("UTF-8")
            buf.write(b)
            total++
            // 结束标志：\r\n\r\n
            if (prev3 == '\r'.code && prev2 == '\n'.code && prev1 == '\r'.code && b == '\n'.code) {
                return buf.toString("UTF-8")
            }
            prev3 = prev2
            prev2 = prev1
            prev1 = b
        }
        return buf.toString("UTF-8")
    }

    private fun parseHead(head: String): Head? {
        val lines = head.split("\r\n")
        if (lines.isEmpty()) return null
        val requestLine = lines[0].trim()
        if (requestLine.isEmpty()) return null
        val parts = requestLine.split(' ')
        if (parts.size < 2) return null
        val method = parts[0].uppercase()
        val target = parts[1]
        val qIndex = target.indexOf('?')
        val rawPath = if (qIndex >= 0) target.substring(0, qIndex) else target
        val rawQuery = if (qIndex >= 0) target.substring(qIndex + 1) else ""

        val headers = mutableMapOf<String, String>()
        for (i in 1 until lines.size) {
            val line = lines[i]
            if (line.isEmpty()) break
            val c = line.indexOf(':')
            if (c <= 0) continue
            headers[line.substring(0, c).trim().lowercase()] = line.substring(c + 1).trim()
        }
        return Head(
            method = method,
            path = decodePath(rawPath),
            query = parseQuery(rawQuery),
            headers = headers,
        )
    }

    private fun decodePath(p: String): String = try {
        java.net.URLDecoder.decode(p, "UTF-8")
    } catch (e: Exception) {
        p
    }

    private fun parseQuery(q: String): Map<String, String> {
        if (q.isEmpty()) return emptyMap()
        val m = mutableMapOf<String, String>()
        for (pair in q.split('&')) {
            if (pair.isEmpty()) continue
            val eq = pair.indexOf('=')
            if (eq >= 0) {
                m[decodePath(pair.substring(0, eq))] = decodePath(pair.substring(eq + 1))
            } else {
                m[decodePath(pair)] = ""
            }
        }
        return m
    }

    private fun readExactly(input: InputStream, length: Int): ByteArray {
        val out = ByteArray(length)
        var read = 0
        while (read < length) {
            val n = input.read(out, read, length - read)
            if (n < 0) break
            read += n
        }
        return if (read == length) out else out.copyOf(read)
    }

    private fun writeResponse(client: Socket, resp: HttpResponse) {
        val bodyBytes = resp.bytes ?: resp.text.toByteArray(Charsets.UTF_8)
        val header = buildString {
            append("HTTP/1.1 ").append(resp.status).append(' ').append(statusText(resp.status)).append("\r\n")
            append("Content-Type: ").append(resp.contentType).append("\r\n")
            append("Content-Length: ").append(bodyBytes.size).append("\r\n")
            append("Cache-Control: no-store\r\n")
            for ((k, v) in resp.extraHeaders) append(k).append(": ").append(v).append("\r\n")
            append("Connection: close\r\n\r\n")
        }
        val out: OutputStream = client.getOutputStream()
        out.write(header.toByteArray(Charsets.UTF_8))
        if (bodyBytes.isNotEmpty()) out.write(bodyBytes)
        out.flush()
    }

    private fun statusText(code: Int): String = when (code) {
        200 -> "OK"
        400 -> "Bad Request"
        404 -> "Not Found"
        405 -> "Method Not Allowed"
        413 -> "Payload Too Large"
        500 -> "Internal Server Error"
        502 -> "Bad Gateway"
        503 -> "Service Unavailable"
        else -> "OK"
    }

    companion object {
        const val MAX_HEAD_BYTES = 64 * 1024
        const val MAX_BODY_BYTES = 4L * 1024 * 1024
        const val MAX_UPLOAD_BYTES = 512L * 1024 * 1024
        private const val READ_TIMEOUT_MS = 15_000
    }
}
