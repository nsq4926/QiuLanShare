package com.sanqiu.lanshare.net

import com.sanqiu.lanshare.core.Proto
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL

/** 出站投递结果 */
data class PeerResult(
    val ok: Boolean = false,
    /** 失败原因：只允许两种（不可达 / 对方已关闭接收） */
    val reason: String = "",
    val detail: String = "",
    val body: String = "",
    val status: Int = 0,
    val ipUsed: String = "",
)

/**
 * 出站客户端：向对端发消息 / 发文件 / 探活。
 *
 * 口径与 Windows `PeerClient`、鸿蒙 `Peer.ets` 一致：
 *  - 每个请求都带 `X-Device-Id: <自己的 deviceId>` —— **带这个头才算 v2**，
 *    不带会被对端按 legacy 通道处理，两个世界的消息互相看不到。
 *  - 服务器之间投递时 body 的 `dir` 恒为 `"in"`（对端视角：这是它收到的一条）。
 *  - `peer` 字段带的是**发送方自己的 deviceId**，对端据此知道"对面是谁"。
 *  - 失败只有两种原因，不猜第三种。
 */
class PeerClient {

    private var selfId: String = ""
    private var selfName: String = ""

    fun setSelf(id: String, name: String) {
        selfId = id
        selfName = name
    }

    /** 探测对端可达性；返回对端 deviceId（不匹配则空串） */
    fun probeHello(ip: String, port: Int, connectTimeoutMs: Int = 2500, readTimeoutMs: Int = 3000): String {
        val url = "http://$ip:$port/api/hello"
        val r = doRequest("GET", url, null, null, connectTimeoutMs, readTimeoutMs)
        if (!r.ok || r.body.isEmpty()) return ""
        // 必须核对 deviceId：端口可能被别的程序占着，连上不等于连的是我们对端
        return Protocol.peekString(r.body, "deviceId")
    }

    /** 发文字 */
    fun sendText(ip: String, port: Int, text: String, msgId: String): PeerResult {
        val url = "http://$ip:$port/api/message"
        // 每段字面量只写到"键名+冒号"，值用 jsonStr，逗号交给下一段
        var body = "{\"text\":${Proto.jsonStr(text)}" +
            ",\"dir\":\"in\"" +
            ",\"dev\":${Proto.jsonStr(selfName)}" +
            ",\"peer\":${Proto.jsonStr(selfId)}"
        if (msgId.isNotEmpty()) body += ",\"msgId\":${Proto.jsonStr(msgId)}"
        body += "}"
        return doRequest("POST", url, body.toByteArray(Charsets.UTF_8), JSON_CT, 6000, 15000)
    }

    /** 发文件（body 是裸字节，不走 multipart） */
    fun upload(
        ip: String,
        port: Int,
        fileName: String,
        mime: String,
        msgId: String,
        bytes: ByteArray,
    ): PeerResult {
        var path = "/api/upload?name=${Proto.urlEncode(fileName)}" +
            "&mime=${Proto.urlEncode(mime.ifEmpty { "application/octet-stream" })}" +
            "&dir=in&dev=${Proto.urlEncode(selfName)}" +
            "&peer=${Proto.urlEncode(selfId)}"
        if (msgId.isNotEmpty()) path += "&msgId=${Proto.urlEncode(msgId)}"
        return doRequest("POST", "http://$ip:$port$path", bytes, OCTET_CT, 8000, 180000)
    }

    /**
     * 发一片（断点续传）。
     *
     * 服务端按 xid 把片追加进同一个 .part，并核对 offset 是否等于已收长度；
     * 不等就回 409，调用方再问 /api/upload/status 从真实进度继续。
     */
    fun uploadChunk(
        ip: String,
        port: Int,
        fileName: String,
        mime: String,
        msgId: String,
        xid: String,
        offset: Long,
        total: Long,
        bytes: ByteArray,
    ): PeerResult {
        var path = "/api/upload?name=${Proto.urlEncode(fileName)}" +
            "&mime=${Proto.urlEncode(mime.ifEmpty { "application/octet-stream" })}" +
            "&dir=in&dev=${Proto.urlEncode(selfName)}" +
            "&peer=${Proto.urlEncode(selfId)}" +
            "&xid=${Proto.urlEncode(xid)}" +
            "&offset=$offset&total=$total"
        if (msgId.isNotEmpty()) path += "&msgId=${Proto.urlEncode(msgId)}"
        return doRequest("POST", "http://$ip:$port$path", bytes, OCTET_CT, 6000, 12000)
    }

    /** 问服务端这个 xid 已收多少字节（-1 = 问不到） */
    fun uploadStatus(ip: String, port: Int, xid: String): Long {
        val url = "http://$ip:$port/api/upload/status?xid=${Proto.urlEncode(xid)}"
        val r = doRequest("GET", url, null, null, 2500, 3000)
        if (!r.ok) return -1
        val at = r.body.indexOf("\"received\":")
        if (at < 0) return -1
        var i = at + 11
        val sb = StringBuilder()
        while (i < r.body.length && r.body[i].isDigit()) {
            sb.append(r.body[i])
            i++
        }
        return sb.toString().toLongOrNull() ?: -1
    }

    private fun doRequest(
        method: String,
        url: String,
        body: ByteArray?,
        contentType: String?,
        connectTimeout: Int,
        readTimeout: Int,
    ): PeerResult {
        var conn: HttpURLConnection? = null
        return try {
            conn = (URL(url).openConnection() as HttpURLConnection).apply {
                requestMethod = method
                // 必须 this. 限定：参数名与属性同名，不加就变成给 val 参数赋值
                this.connectTimeout = connectTimeout
                this.readTimeout = readTimeout
                instanceFollowRedirects = false
                setRequestProperty("X-Device-Id", selfId)
                setRequestProperty("Content-Type", contentType ?: JSON_CT)
                if (body != null) {
                    doOutput = true
                    setFixedLengthStreamingMode(body.size)
                }
            }
            if (body != null) {
                conn.outputStream.use { it.write(body) }
            }
            val status = conn.responseCode
            val stream = if (status >= 400) conn.errorStream else conn.inputStream
            val text = stream?.use { readAll(it) } ?: ""
            if (status == 200) {
                PeerResult(ok = true, body = text, status = status, ipUsed = "")
            } else {
                PeerResult(
                    ok = false,
                    reason = Proto.REASON_UNREACHABLE,
                    detail = "http$status",
                    body = text,
                    status = status,
                )
            }
        } catch (e: Exception) {
            PeerResult(
                ok = false,
                reason = Proto.REASON_UNREACHABLE,
                detail = "error${e.javaClass.simpleName}",
            )
        } finally {
            runCatching { conn?.disconnect() }
        }
    }

    private fun readAll(input: java.io.InputStream): String {
        val bos = ByteArrayOutputStream(1024)
        val buf = ByteArray(4096)
        while (true) {
            val n = input.read(buf)
            if (n < 0) break
            bos.write(buf, 0, n)
        }
        return bos.toString("UTF-8")
    }

    companion object {
        private const val JSON_CT = "application/json"
        private const val OCTET_CT = "application/octet-stream"
    }
}

/** 轻量 JSON 取值（探活只需要一个字段，不做完整解析也能容忍截断） */
internal object Protocol {
    fun peekString(text: String, key: String): String {
        val needle = "\"$key\":\""
        val at = text.indexOf(needle)
        if (at < 0) return ""
        val start = at + needle.length
        val sb = StringBuilder()
        var i = start
        while (i < text.length) {
            val c = text[i]
            if (c == '\\') {
                i++
                if (i < text.length) {
                    sb.append(
                        when (text[i]) {
                            'n' -> '\n'
                            'r' -> '\r'
                            't' -> '\t'
                            'b' -> '\b'
                            'f' -> '\u000C'
                            else -> text[i]
                        }
                    )
                }
                i++
                continue
            }
            if (c == '"') break
            sb.append(c)
            i++
        }
        return sb.toString()
    }
}
