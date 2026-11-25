package com.sanqiu.lanshare.api

import com.sanqiu.lanshare.core.LsHub
import com.sanqiu.lanshare.core.LsIp
import com.sanqiu.lanshare.core.LsMsg
import com.sanqiu.lanshare.core.Proto
import com.sanqiu.lanshare.core.sanitizeName
import com.sanqiu.lanshare.net.HttpRequest
import com.sanqiu.lanshare.net.HttpResponse
import com.sanqiu.lanshare.net.PeerClient
import com.sanqiu.lanshare.net.PeerResult
import com.sanqiu.lanshare.net.TransferItem
import com.sanqiu.lanshare.net.Transfers
import org.json.JSONObject

/**
 * HTTP 接口实现。
 *
 * 契约以 Windows 端（`windows/src/HttpServer.cpp`）为唯一权威源，这里只做同构复刻：
 * 字段名、状态码、错误串逐字对齐。
 *
 * 拼 JSON 的规矩（沿用两端）：**每段字面量只写到"键名+冒号"**，值一律用
 * `Proto.jsonStr`（自带引号），逗号交给下一段开头 —— 这样不会出现"多一个逗号/引号"，
 * 而这类错误的症状是对端静默丢弃，本端毫无察觉。
 */
/**
 * 按位置读一段字节（流式发送用）。
 * 返回 null = 读不到（调用方必须响亮地报错，不能当成"空片"继续）。
 */
fun interface ChunkReader {
    fun readAt(offset: Long, len: Int): ByteArray?
}


/** 把一个本地文件包装成按位置读取器（收齐后的 .part 走它入库） */
private class FileChunkReader(private val f: java.io.File) : ChunkReader {
    private var ins: java.io.InputStream? = null
    private var pos = 0L

    private fun close() {
        runCatching { ins?.close() }
        ins = null
    }

    override fun readAt(offset: Long, len: Int): ByteArray? {
        if (ins == null || offset != pos) {
            close()
            val stream = try { f.inputStream() } catch (e: Exception) { return null }
            var skipped = 0L
            try {
                while (skipped < offset) {
                    val k = stream.skip(offset - skipped)
                    if (k <= 0) { stream.close(); return null }
                    skipped += k
                }
            } catch (e: Exception) {
                runCatching { stream.close() }
                return null
            }
            ins = stream
            pos = offset
        }
        val buf = ByteArray(len)
        var read = 0
        try {
            while (read < len) {
                val k = ins!!.read(buf, read, len - read)
                if (k < 0) break
                read += k
            }
        } catch (e: Exception) {
            close()
            return null
        }
        if (read <= 0) { close(); return null }
        pos += read
        return if (read == len) buf else buf.copyOf(read)
    }
}
class LsApi(val hub: LsHub, val peer: PeerClient) {

    /** 实际监听端口（端口递进后的真值） */
    @Volatile var port: Int = 0

    /** 最近一次连上来的对端设备名（`/api/hello` 回显） */
    @Volatile var peerName: String = ""

    @Volatile var historyDays: Long = 0

    /** 数据变化通知（UI 订阅用） */
    @Volatile var onChange: (() -> Unit)? = null

    /** 入站消息到达（通知栏用）：convId / 对端 deviceId / 对端名 / 摘要 */
    @Volatile var onInbound: ((String, String, String, String) -> Unit)? = null

    /** 入库后的统一出口：界面刷新 + 系统通知 */
    private fun notifyInbound(m: LsMsg) {
        notifyChanged()
        val preview = when (m.kind) {
            Proto.KIND_IMAGE -> "[图片]"
            Proto.KIND_FILE -> "[文件]"
            else -> if (m.text.length > 60) m.text.substring(0, 60) else m.text
        }
        onInbound?.invoke(m.convId, m.from, m.dev, preview)
    }

    /** `/` 的网页内容；为 null 时返回一段极简页（不假装有完整 UI） */
    @Volatile var htmlProvider: ((Int) -> String)? = null

    /**
     * 最近一次拼出非法 JSON 的内容。
     *
     * 拼串错误的症状是"对端静默丢弃"，所以这里每次响应前验一次合法性，
     * 失败留痕（可被单测断言），把沉默的失败变成响亮的失败。
     */
    @Volatile var lastBadJson: String = ""

    /** 附件出口（相册/下载）。NodeService 注入；未注入时只落私有目录。 */
    @Volatile var exporter: com.sanqiu.lanshare.net.AttachmentExporter? = null

    /** 最近一次导出失败原因（界面显示，不吞掉） */
    @Volatile var lastExportError: String = ""

    fun notifyChanged() {
        onChange?.invoke()
    }

    private fun jsonBody(text: String): HttpResponse {
        try {
            JSONObject(text)
        } catch (e: Exception) {
            lastBadJson = text
        }
        return HttpResponse.json(text)
    }

    // ------------------------------------------------------------ 路由

    /**
     * 串行化闸门 —— 直接复用 `hub.gate`，让**界面线程读消息时用的是同一把锁**。
     *
     * 长轮询不持有这把锁（否则一个 wait=25 的请求会把所有请求堵 25 秒），
     * 它只在内部查询时短暂进入（synchronized 可重入，wait=0 时已在锁内也不会死锁）。
     */
    private val gate: Any get() = hub.gate

    fun handle(req: HttpRequest): HttpResponse = try {
        when {
            // 长轮询不持锁：一个 wait=25 会把所有请求堵 25 秒
            req.path == "/api/messages" && req.qNum("wait", 0) > 0 -> handleMessages(req)
            // 添加设备要先探活对端，**不能持 gate**：
            // 若用户填的是本机地址（自环），本进程的新请求要同一把锁，
            // 而当前请求正等它返回 —— 互等直到探测超时，最后误报 502 connect_fail。
            req.path == "/api/devices/add" -> handleDevicesAdd(req)
            else -> synchronized(gate) { dispatch(req) }
        }
    } catch (e: Exception) {
        HttpResponse.status(500, "internal error: ${e.javaClass.simpleName}")
    }

    private fun dispatch(req: HttpRequest): HttpResponse = when (req.path) {
        "/", "/index.html" -> HttpResponse(
            contentType = "text/html; charset=utf-8",
            text = htmlProvider?.invoke(port) ?: minimalHtml(),
        )
        "/api/ping" -> jsonBody("{\"ok\":true}")
        "/api/hello" -> handleHello(req)
        "/api/devices", "/api/discover" -> handleDevices()
        "/api/conversations" -> handleConversations()
        "/api/messages" -> handleMessages(req)
        "/api/message" -> handleMessage(req)
        "/api/upload" -> handleUpload(req)
        "/api/upload/status" -> handleUploadStatus(req)
        "/api/file" -> handleFile(req)
        "/api/messages/read" -> handleMessagesRead(req)
        "/api/devices/add" -> handleDevicesAdd(req)
        "/api/devices/remove" -> handleDevicesRemove(req)
        "/api/devices/rename" -> handleDevicesRename(req)
        "/api/history/clear" -> handleHistoryClear(req)
        "/api/history/settings" -> handleHistorySettings(req)
        "/api/transfer" -> jsonBody(transfersJson())
        "/api/firewall" -> handleFirewall()
        else -> HttpResponse.status(404, "not found")
    }

    // ------------------------------------------------------------ /api/hello

    private fun handleHello(req: HttpRequest): HttpResponse {
        // 任何带 X-Device-Id 的请求都意味着"对端成功连上了我"
        val caller = req.header("x-device-id")
        if (caller.isNotEmpty() && caller != hub.self.deviceId) {
            // **不能**在这里新建设备条目 —— 网页端带的是随机 deviceId，
            // 每开一个标签都建条目的话设备列表会被"网页客户端"刷屏
            hub.markReachable(caller, Proto.nowMs())
            if (hub.peers.containsKey(caller)) peerName = hub.displayName(caller)
        }
        val ip = hub.net.primaryIp()
        val body = "{\"app\":${Proto.jsonStr(Proto.LS_APP)}" +
            ",\"name\":${Proto.jsonStr(hub.self.name)}" +
            ",\"ip\":${Proto.jsonStr(ip)}" +
            ",\"port\":$port" +
            ",\"version\":\"1.1\"" +
            ",\"device\":\"android\"" +
            ",\"deviceId\":${Proto.jsonStr(hub.self.deviceId)}" +
            ",\"mac\":${Proto.jsonStr(hub.self.mac)}" +
            ",\"apiVersion\":${Proto.jsonStr(Proto.API_VERSION)}" +
            ",\"resumable\":true" +
            ",\"platform\":${Proto.jsonStr(Proto.LS_PLATFORM)}" +
            ",\"acceptsInbound\":${if (hub.acceptsInbound) "true" else "false"}" +
            ",\"filteredNicCount\":${hub.self.filteredNicCount}" +
            ",\"onlineTtl\":${Proto.ONLINE_TTL}" +
            ",\"staleTtl\":${Proto.STALE_TTL}" +
            ",\"epoch\":${hub.self.epoch}" +
            ",\"instance\":\"default\"" +
            ",\"peer\":${Proto.jsonStr(peerName)}" +
            ",\"historyDays\":$historyDays" +
            ",\"saveDir\":${Proto.jsonStr(hub.store.getSaveDir())}}"
        return jsonBody(body)
    }

    // ------------------------------------------------------------ 只读列表

    private fun handleDevices(): HttpResponse {
        val now = Proto.nowMs()
        val rows = hub.deviceRows(now)
        val body = "{\"ok\":true,\"now\":$now" +
            ",\"total\":${rows.size}" +
            ",\"truncated\":false" +
            ",\"devices\":[${rows.joinToString(",")}]}"
        return jsonBody(body)
    }

    private fun handleConversations(): HttpResponse {
        val rows = hub.conversationRows()
        return jsonBody("{\"ok\":true,\"conversations\":[${rows.joinToString(",")}]}")
    }

    // ------------------------------------------------------------ /api/messages

    private fun handleMessages(req: HttpRequest): HttpResponse {
        val legacy = !req.isV2()
        // legacy 请求带 peer 会被完全忽略
        val peerKey = if (legacy) "" else req.q("peer")
        val since = req.qNum("since", 0)
        val sinceEpoch = req.qNum("sinceEpoch", -1)
        val sinceSeq = req.qNum("sinceSeq", -1)
        var wait = req.qNum("wait", 0)
        if (wait < 0) wait = 0
        if (wait > Proto.MAX_WAIT_SEC) wait = Proto.MAX_WAIT_SEC.toLong()

        val dev = req.q("dev")
        if (dev.isNotEmpty()) peerName = dev

        var msgs = synchronized(gate) {
            hub.queryMessages(peerKey, sinceEpoch, sinceSeq, since, legacy)
        }
        // 长轮询：只有"一条都没有"时才挂起，避免已有时延后返回
        if (msgs.isEmpty() && wait > 0) {
            val deadline = Proto.nowMs() + wait * 1000
            while (Proto.nowMs() < deadline) {
                try {
                    Thread.sleep(400)
                } catch (e: InterruptedException) {
                    Thread.currentThread().interrupt()
                    break
                }
                msgs = synchronized(gate) {
                    hub.queryMessages(peerKey, sinceEpoch, sinceSeq, since, legacy)
                }
                if (msgs.isNotEmpty()) break
            }
        }

        var lastId = since
        var lastEpoch = if (sinceEpoch < 0) 0L else sinceEpoch
        var lastSeq = if (sinceSeq < 0) 0L else sinceSeq
        val parts = StringBuilder()
        for (m in msgs) {
            if (parts.isNotEmpty()) parts.append(',')
            parts.append(m.toJson())
            if (m.id > lastId) lastId = m.id
        }
        if (peerKey.isNotEmpty()) {
            val wm = hub.peerWatermark(peerKey)
            lastEpoch = wm[0]
            lastSeq = wm[1]
        }
        return jsonBody(
            "{\"ok\":true,\"lastId\":$lastId" +
                ",\"lastEpoch\":$lastEpoch" +
                ",\"lastSeq\":$lastSeq" +
                ",\"messages\":[$parts]}"
        )
    }

    // ------------------------------------------------------------ /api/message

    private fun handleMessage(req: HttpRequest): HttpResponse {
        if (req.method != "POST") return HttpResponse.status(405, "use POST")
        val obj = Proto.parseObj(req.bodyText())
        val text = Proto.jStr(obj, "text")
        if (text.isEmpty()) return HttpResponse.status(400, "text required")

        var dir = Proto.jStr(obj, "dir", Proto.DIR_OUT)
        if (dir != Proto.DIR_IN && dir != Proto.DIR_OUT) dir = Proto.DIR_OUT
        var dev = Proto.jStr(obj, "dev", req.q("dev"))
        val msgIdIn = Proto.jStr(obj, "msgId", req.q("msgId"))
        var peerId = Proto.jStr(obj, "peer", req.q("peer"))
        if (!req.isV2() && peerId.isEmpty()) peerId = hub.legacyKey(dev)
        if (dev.isEmpty()) dev = if (dir == Proto.DIR_OUT) hub.self.name else peerName

        if (dir == Proto.DIR_OUT) {
            val m = sendOutboundText(peerId, text, msgIdIn)
            return jsonBody(
                "{\"ok\":true,\"id\":${m.id}" +
                    ",\"msgId\":${Proto.jsonStr(m.msgId)}" +
                    ",\"status\":${Proto.jsonStr(m.status)}" +
                    ",\"reason\":${Proto.jsonStr(m.reason)}" +
                    ",\"detail\":${Proto.jsonStr(m.detail)}}"
            )
        }

        val m = buildInbound(dev, peerId, text, msgIdIn, Proto.KIND_TEXT, "", "", 0)
        if (m.id > 0) {
            hub.appendMessage(m)
            notifyInbound(m)
        }
        return jsonBody("{\"ok\":true,\"id\":${m.id},\"msgId\":${Proto.jsonStr(m.msgId)}}")
    }

    // ------------------------------------------------------------ /api/upload

    private fun handleUpload(req: HttpRequest): HttpResponse {
        if (req.method != "POST") return HttpResponse.status(405, "use POST")
        if (req.body.isEmpty()) return HttpResponse.status(400, "empty body")
        if (req.body.size > MAX_UPLOAD) return HttpResponse.status(413, "file too large")
        // 分片/续传：同一个 xid 的每一片都走这里，攒够 total 才落成一条消息
        val xid = req.q("xid")
        if (xid.isNotEmpty()) return handleUploadChunk(req, xid)

        val name = req.q("name", "file")
        val mime = sanitizeMime(req.q("mime", "application/octet-stream"))
        var dir = req.q("dir", Proto.DIR_IN)
        if (dir != Proto.DIR_OUT) dir = Proto.DIR_IN
        val dev = req.q("dev")
        val msgIdIn = req.q("msgId")
        var peerId = req.q("peer")
        if (!req.isV2() && peerId.isEmpty()) peerId = hub.legacyKey(dev)

        if (dir == Proto.DIR_OUT) {
            val m = sendOutboundFile(peerId, name, mime, msgIdIn, req.body)
            return jsonBody(
                "{\"ok\":true,\"failed\":${if (m.status == Proto.STATUS_FAILED) "true" else "false"}" +
                    ",\"reason\":${Proto.jsonStr(m.reason)}" +
                    ",\"message\":${m.toJson()}}"
            )
        }

        // 幂等：已存在同 msgId 时不重复保存、不重复导出（去重靠 msgId，不是靠文件内容）
        val alreadyThere = msgIdIn.isNotEmpty() && findByMsgId(msgIdIn) != null
        // 整份上传也留一条传输记录：每个会话的传输列表要能回看，不能只有分片上传才记录
        val trItem = Transfers.beginIn(Transfers.xid(), peerId, name, req.body.size.toLong())
        Transfers.setProgress(trItem, req.body.size.toLong())
        val saved = saveIncomingFile(peerId, name, mime, req.body)
        if (!saved.ok) {
            Transfers.finish(trItem, false, "cannot save file")
            return HttpResponse.status(500, "cannot save file")
        }
        Transfers.finish(trItem, true)
        val isImage = isImageMime(mime)
        if (!alreadyThere && ((isImage && hub.settings.autoSaveImage) || (!isImage && hub.settings.autoSaveFile))) {
            exportAttachment(saved, isImage)
        }
        val kind = if (isImage) Proto.KIND_IMAGE else Proto.KIND_FILE
        val m = buildInbound(dev, peerId, "", msgIdIn, kind, saved.fileId, saved.fileName, saved.size)
        m.fileRel = saved.fileRel
        m.mime = mime
        if (m.id > 0) {
            hub.appendMessage(m)
            notifyInbound(m)
        }
        return jsonBody("{\"ok\":true,\"message\":${m.toJson()}}")
    }


    // ------------------------------------------------------------ 分片 / 续传

    /** xid → .part 绝对路径（进程内；重启后客户端从 0 重传） */
    private val partPaths = HashMap<String, String>()

    private fun partOf(xid: String, peerKey: String, dev: String): String {
        partPaths[xid]?.let { return it }
        val dir = hub.store.peerDir(peerKey, hub.displayName(peerKey))
        dir.mkdirs()
        // `.~` 开头：不会被当成收到的文件列出（同名去重也会跳过）
        val p = java.io.File(dir, ".~" + sanitizeName(xid) + ".part")
        partPaths[xid] = p.absolutePath
        return p.absolutePath
    }

    private fun handleUploadStatus(req: HttpRequest): HttpResponse {
        val xid = req.q("xid")
        if (xid.isEmpty()) return HttpResponse.status(400, "xid required")
        var received = 0L
        partPaths[xid]?.let { received = java.io.File(it).length() }
        return jsonBody("{\"ok\":true,\"received\":$received}")
    }

    /**
     * 收一片。
     *
     * 三条校验缺一条就会出现"看起来传完了、打开是坏的"：
     *  1. total 必须 >0 且不超上限；
     *  2. offset 必须等于已收长度，不等回 409 让客户端重新对齐；
     *  3. 收满 total 才入库（中途入库会在聊天里留下半截文件）。
     */
    private fun handleUploadChunk(req: HttpRequest, xid: String): HttpResponse {
        val total = req.qNum("total", 0)
        val offset = req.qNum("offset", 0)
        if (total <= 0 || total > MAX_UPLOAD) return HttpResponse.status(413, "file too large")
        if (offset < 0 || offset > total) return HttpResponse.status(400, "bad offset")
        if (req.body.size > total - offset) return HttpResponse.status(400, "chunk overflow")

        val name = req.q("name", "file")
        val mime = sanitizeMime(req.q("mime", "application/octet-stream"))
        val dev = req.q("dev")
        val msgIdIn = req.q("msgId")
        var peerId = req.q("peer")
        if (!req.isV2() && peerId.isEmpty()) peerId = hub.legacyKey(dev)

        // 幂等：这一单其实早就收完了（客户端没收到 200 又重发最后一片）
        if (msgIdIn.isNotEmpty()) {
            findByMsgId(msgIdIn)?.let { dup ->
                return jsonBody(
                    "{\"ok\":true,\"received\":${dup.fileSize},\"message\":${dup.toJson()}}"
                )
            }
        }

        val path = partOf(xid, peerId, dev)
        val f = java.io.File(path)
        var got = f.length()
        if (offset == 0L) {
            // 截断/创建（append=false 一步做完，不做"先查存在再建"）
            try {
                java.io.FileOutputStream(f, false).use { }
            } catch (e: Exception) {
                return HttpResponse.status(500, "cannot create part")
            }
            got = 0
        } else if (got != offset) {
            // 409 + 真实进度：客户端据此从 got 继续，而不是整份重来
            return jsonBody("{\"ok\":false,\"error\":\"offset_mismatch\",\"received\":$got}")
        }

        val item = Transfers.beginIn(xid, peerId, name, total)
        try {
            if (req.body.isNotEmpty()) {
                java.io.FileOutputStream(f, true).use { it.write(req.body) }
            }
        } catch (e: Exception) {
            Transfers.finish(item, false, "write_failed")
            return HttpResponse.status(500, "cannot write chunk")
        }
        got += req.body.size
        Transfers.setProgress(item, got)
        if (got < total) {
            return jsonBody("{\"ok\":true,\"partial\":true,\"received\":$got}")
        }
        if (got > total) {
            Transfers.finish(item, false, "overflow")
            return HttpResponse.status(400, "chunk overflow")
        }

        // 收齐：**流式**入库（1 GB 的文件不能整份读回内存）
        val saved = saveIncomingFileStream(peerId, name, mime, got, FileChunkReader(f))
        f.delete()
        partPaths.remove(xid)
        if (!saved.ok) {
            Transfers.finish(item, false, "cannot save file")
            return HttpResponse.status(500, "cannot save file")
        }
        val isImage = isImageMime(mime)
        if ((isImage && hub.settings.autoSaveImage) || (!isImage && hub.settings.autoSaveFile)) {
            exportAttachment(saved, isImage)
        }
        val m = buildInbound(dev, peerId, "", msgIdIn,
            if (isImage) Proto.KIND_IMAGE else Proto.KIND_FILE, saved.fileId, saved.fileName, saved.size)
        m.fileRel = saved.fileRel
        m.mime = mime
        if (m.id > 0) {
            hub.appendMessage(m)
            notifyInbound(m)
        }
        Transfers.finish(item, true)
        return jsonBody("{\"ok\":true,\"received\":$total,\"message\":${m.toJson()}}")
    }

    /** 给网页 / 调试看的传输列表（与界面同源） */
    private fun transfersJson(): String {
        val parts = Transfers.snapshot().map { t ->
            "{\"name\":${Proto.jsonStr(t.name)}" +
                ",\"dir\":${Proto.jsonStr(t.dir)}" +
                ",\"total\":${t.total},\"received\":${t.done}" +
                ",\"speed\":${t.speed.toLong()}" +
                ",\"status\":${Proto.jsonStr(t.status)}" +
                ",\"peer\":${Proto.jsonStr(t.peerId)}}"
        }
        return "{\"ok\":true,\"items\":[${parts.joinToString(",")}]}"
    }

    // ------------------------------------------------------------ /api/file

    private fun handleFile(req: HttpRequest): HttpResponse {
        val id = req.q("id")
        if (id.isEmpty()) return HttpResponse.status(400, "id required")
        val path = findFilePath(id)
        if (path == null) return HttpResponse.status(404, "file not found")
        val bytes = hub.store.readBytes(path) ?: return HttpResponse.status(500, "cannot open file")
        val inline = req.q("inline") == "1"
        val name = findFileName(id)
        return HttpResponse(
            status = 200,
            contentType = findFileMime(id),
            bytes = bytes,
            extraHeaders = mapOf(
                "Content-Disposition" to
                    (if (inline) "inline" else "attachment") + "; filename*=UTF-8''" + Proto.pctEncode(name)
            ),
        )
    }

    // ------------------------------------------------------------ 未读 / 设备 / 历史

    private fun handleMessagesRead(req: HttpRequest): HttpResponse {
        if (req.method != "POST") return HttpResponse.status(405, "use POST")
        val obj = Proto.parseObj(req.bodyText())
        var key = Proto.jStr(obj, "convId")
        if (key.isEmpty()) key = Proto.jStr(obj, "peer", req.q("peer"))
        if (key.isEmpty()) return HttpResponse.status(400, "convId or peer required")
        val cleared = hub.markRead(key)
        notifyChanged()
        return jsonBody("{\"ok\":true,\"cleared\":$cleared}")
    }

    private fun handleDevicesAdd(req: HttpRequest): HttpResponse {
        if (req.method != "POST") return HttpResponse.status(405, "use POST")
        val obj = Proto.parseObj(req.bodyText())
        val rawIp = Proto.jStr(obj, "ip", req.q("ip"))
        val rawPort = Proto.jStr(obj, "port", req.q("port"))
        val parsed = splitHostPort(rawIp, rawPort)
        val ip = parsed.first
        val portNum = parsed.second.toIntOrNull() ?: Proto.HTTP_PORT_BASE
        if (ip.isEmpty()) return HttpResponse.status(400, "ip_required")

        // 先做**本地判断**：目标是本机地址就直接拒绝。
        // 不要指望"自己连自己"能成功 —— 真机实测：连本机 8765 能建立 TCP 连接，
        // 但对端迟迟不回（3 秒读超时），最终被误报成 502 connect_fail。
        // 用户手动输入自己的 IP 时，得到"这是本机"才是对的。
        if (ip == "127.0.0.1" || ip == "localhost" || ip == "0.0.0.0" ||
            hub.self.ips.contains(ip) || ip == hub.net.primaryIp()
        ) {
            return HttpResponse.status(400, "self")
        }

        // 探活在锁外做（见 handle 里的说明）
        val deviceId = peer.probeHello(ip, portNum)
        if (deviceId.isEmpty()) return HttpResponse.status(502, "connect_fail")
        if (deviceId == hub.self.deviceId) return HttpResponse.status(400, "self")

        synchronized(gate) {
            val p = hub.upsertPeer(deviceId)
            p.port = portNum
            p.lastSeen = Proto.nowMs()
            hub.addPeerIp(p, ip, Proto.nowMs())
            hub.markReachable(deviceId, Proto.nowMs())
            hub.saveDevices()
        }
        notifyChanged()
        return jsonBody("{\"ok\":true,\"deviceId\":${Proto.jsonStr(deviceId)}}")
    }

    private fun handleDevicesRemove(req: HttpRequest): HttpResponse {
        if (req.method != "POST") return HttpResponse.status(405, "use POST")
        val obj = Proto.parseObj(req.bodyText())
        val deviceId = Proto.jStr(obj, "deviceId", req.q("deviceId"))
        if (deviceId.isEmpty()) return HttpResponse.status(400, "deviceId required")

        val display = hub.displayName(deviceId)
        val dir = hub.store.peerDir(deviceId, display)
        val dry = req.q("dry") == "1"
        val keepFiles = req.q("files") == "0"
        val stat = hub.store.statDir(dir)

        if (dry) {
            return jsonBody(
                "{\"ok\":true,\"dry\":true,\"path\":${Proto.jsonStr(dir.absolutePath)}" +
                    ",\"exists\":${if (dir.exists()) "true" else "false"}" +
                    ",\"fileCount\":${stat[0]}" +
                    ",\"sizeBytes\":${stat[1]}}"
            )
        }

        var removedFiles = 0L
        if (!keepFiles && dir.exists()) {
            hub.store.removeDirRecursive(dir)
            removedFiles = stat[0]
        }
        // 设备都删了，它的传输记录也没有意义了
        Transfers.dropPeer(deviceId)

        // 还活着的设备删掉后要压制自动发现一小会儿，否则下一轮 announce 立刻加回来。
        // **只压自动发现**：消息入站与手动添加照旧 —— 删除不是拉黑。
        var suppressedMs = 0L
        var peerRemoved = false
        hub.peers[deviceId]?.let { p ->
            val st = Proto.stateOf(hub.reachableAt(deviceId), p.acceptsInbound, p.lastSeen, Proto.nowMs())
            if (st != Proto.ST_OFFLINE) {
                suppressedMs = 60_000
                hub.suppressDiscovery(deviceId, suppressedMs)
            }
            hub.peers.remove(deviceId)
            peerRemoved = true
        }
        hub.saveDevices()
        notifyChanged()
        return jsonBody(
            "{\"ok\":true,\"removedFiles\":$removedFiles" +
                ",\"peerRemoved\":${if (peerRemoved) "true" else "false"}" +
                ",\"discoverySuppressedMs\":$suppressedMs}"
        )
    }

    private fun handleDevicesRename(req: HttpRequest): HttpResponse {
        if (req.method != "POST") return HttpResponse.status(405, "use POST")
        val obj = Proto.parseObj(req.bodyText())
        var deviceId = Proto.jStr(obj, "deviceId", req.q("deviceId"))
        val alias = Proto.jStr(obj, "alias", req.q("alias"))
        if (deviceId.isEmpty()) return HttpResponse.status(400, "deviceId required")
        if (!deviceId.startsWith("legacy:") && !deviceId.startsWith("unknown:") &&
            !hub.peers.containsKey(deviceId)
        ) {
            deviceId = hub.legacyKey(deviceId)
        }
        hub.setAlias(deviceId, alias)
        notifyChanged()
        return jsonBody("{\"ok\":true,\"deviceId\":${Proto.jsonStr(deviceId)}}")
    }

    private fun handleHistoryClear(req: HttpRequest): HttpResponse {
        val convId = req.q("convId")
        if (convId.isNotEmpty()) {
            // 先记下这条会话的对端，再清（清完 convs 里可能就查不到了）。
            // 传输列表与消息同生命周期：清会话 = 连它的传输记录一起清。
            val peerOfConv = synchronized(hub.gate) { hub.convs[convId]?.peerDeviceId ?: "" }
            val removed = hub.clearConversation(convId)
            Transfers.dropPeer(peerOfConv)
            notifyChanged()
            return jsonBody("{\"ok\":true,\"removed\":$removed}")
        }
        val dev = req.q("dev")
        if (dev.isNotEmpty()) {
            val key = hub.legacyKey(dev)
            val conv = Proto.makeConvId(hub.self.deviceId, key)
            val removed = hub.clearConversation(conv)
            Transfers.dropPeer(key)
            notifyChanged()
            return jsonBody("{\"ok\":true,\"removed\":$removed}")
        }
        hub.clearAll()
        Transfers.dropAll()
        notifyChanged()
        return jsonBody("{\"ok\":true}")
    }

    private fun handleHistorySettings(req: HttpRequest): HttpResponse {
        val days = req.qNum("days", -1)
        if (days >= 0) {
            historyDays = days
            hub.settings.retentionMode =
                if (days == 0L) com.sanqiu.lanshare.core.LsSettings.RET_NEVER
                else com.sanqiu.lanshare.core.LsSettings.RET_DAYS
            hub.settings.retentionDays = if (days == 0L) 30 else days
            hub.saveSettings()
        }
        return jsonBody("{\"ok\":true,\"days\":$historyDays}")
    }

    private fun handleFirewall(): HttpResponse =
        // Android 没有 Windows 防火墙这回事：如实报告"未被拦截"，别编规则名骗人
        jsonBody(
            "{\"ok\":true,\"blocked\":false,\"isAdmin\":false" +
                ",\"ruleExists\":true,\"selfProbeOk\":true,\"port\":$port" +
                ",\"lanIp\":${Proto.jsonStr(hub.net.primaryIp())}" +
                ",\"reason\":\"Android 系统不拦截局域网入站连接\"" +
                ",\"shouldPrompt\":false,\"tcpRule\":\"\",\"udpRule\":\"\"" +
                ",\"tcpPorts\":\"\",\"udpPort\":0}"
        )

    // ------------------------------------------------------------ 出站投递

    /** 本端发文字：分配身份 → 真投递 → 一次入库（失败也入库，标"未发送"） */
    fun sendOutboundText(peerKey: String, text: String, msgIdIn: String): LsMsg {
        val target = resolveTarget(peerKey)
        val msg = LsMsg(
            kind = Proto.KIND_TEXT,
            text = text,
            dir = Proto.DIR_OUT,
            dev = hub.self.name,
            time = Proto.nowMs(),
            from = hub.self.deviceId,
            to = target,
        )
        msg.peerTs = msg.time
        msg.recvTs = msg.time

        var reuse: LsMsg? = null
        if (msgIdIn.isNotEmpty()) {
            reuse = findByMsgId(msgIdIn)
            if (reuse != null) {
                // 重发：沿用同一个 msgId，**必须真的再投一次**
                // （旧实现只做幂等返回、不投递，症状就是"重发功能失效"）
                val r = deliverText(target, text, msgIdIn)
                reuse.status = if (r.ok) Proto.STATUS_SENT else Proto.STATUS_FAILED
                reuse.reason = r.reason
                reuse.detail = r.detail
                hub.rewriteHistory()
                notifyChanged()
                return reuse
            }
            msg.msgId = msgIdIn
            parseMsgIdTail(msgIdIn, msg)
        } else {
            msg.msgId = hub.newMsgId()
            msg.epoch = hub.self.epoch
            msg.seq = hub.self.seq
        }
        msg.convId = Proto.makeConvId(msg.from, msg.to)

        val r = deliverText(target, text, msg.msgId)
        msg.status = if (r.ok) Proto.STATUS_SENT else Proto.STATUS_FAILED
        msg.reason = r.reason
        msg.detail = r.detail
        hub.appendMessage(msg)
        notifyChanged()
        return msg
    }

    /** 本端发文件 */
    fun sendOutboundFile(
        peerKey: String,
        fileName: String,
        mime: String,
        msgIdIn: String,
        bytes: ByteArray,
    ): LsMsg {
        val total = bytes.size.toLong()
        val reader = ChunkReader { off, len ->
            if (off < 0 || off >= total) null
            else bytes.copyOfRange(off.toInt(), minOf(off + len, total).toInt())
        }
        return sendOutboundCore(peerKey, fileName, mime, msgIdIn, total, reader, reader)
    }

    /**
     * 流式发送：按片从 [reader] 读（picker 的 URI / 文件句柄），**不再把整个文件读进内存**。
     * [copyReader] 是另一份独立读取器，用于发完之后在本机留一份副本。
     */
    fun sendOutboundFileStream(
        peerKey: String,
        fileName: String,
        mime: String,
        total: Long,
        reader: ChunkReader,
        copyReader: ChunkReader,
    ): LsMsg = sendOutboundCore(peerKey, fileName, mime, "", total, reader, copyReader)

    private fun sendOutboundCore(
        peerKey: String,
        fileName: String,
        mime: String,
        msgIdIn: String,
        total: Long,
        reader: ChunkReader,
        copyReader: ChunkReader,
    ): LsMsg {
        val target = resolveTarget(peerKey)
        val msg = LsMsg(
            kind = if (isImageMime(mime)) Proto.KIND_IMAGE else Proto.KIND_FILE,
            text = "",
            dir = Proto.DIR_OUT,
            dev = hub.self.name,
            time = Proto.nowMs(),
            from = hub.self.deviceId,
            to = target,
            fileName = fileName,
            mime = mime,
            fileSize = total,
        )
        msg.peerTs = msg.time
        msg.recvTs = msg.time

        if (msgIdIn.isNotEmpty()) {
            val reuse = findByMsgId(msgIdIn)
            if (reuse != null) {
                val xidReuse = Transfers.xid()
                val itemReuse = Transfers.beginOut(target, fileName, total, xidReuse, msgIdIn)
                val r = deliverFile(target, fileName, mime, msgIdIn, total, reader, xidReuse, itemReuse)
                reuse.status = if (r.ok) Proto.STATUS_SENT else Proto.STATUS_FAILED
                reuse.reason = r.reason
                reuse.detail = r.detail
                hub.rewriteHistory()
                notifyChanged()
                return reuse
            }
            msg.msgId = msgIdIn
            parseMsgIdTail(msgIdIn, msg)
        } else {
            msg.msgId = hub.newMsgId()
            msg.epoch = hub.self.epoch
            msg.seq = hub.self.seq
        }
        msg.convId = Proto.makeConvId(msg.from, msg.to)

        // 顺序很重要：**先登记传输、立刻开始发**，本地副本最后再落。
        // 以前先整份拷贝到应用目录再上传，用户点了"发送"要等读文件 + 写副本才看到
        // 传输列表和会话气泡 —— 这就是"发送图片文件时界面延迟很大"的来源。
        val xid = Transfers.xid()
        val item = Transfers.beginOut(target, fileName, total, xid, msg.msgId)
        notifyChanged()

        val r = deliverFile(target, fileName, mime, msg.msgId, total, reader, xid, item)
        msg.status = if (r.ok) Proto.STATUS_SENT else Proto.STATUS_FAILED
        msg.reason = r.reason
        msg.detail = r.detail

        // 本地也留一份：用户要能看到自己发出去的东西，重启后还得点得开
        val saved = saveIncomingFileStream(target, fileName, mime, total, copyReader)
        if (saved.ok) {
            msg.fileId = saved.fileId
            msg.fileName = saved.fileName
            msg.fileRel = saved.fileRel
        }

        hub.appendMessage(msg)
        notifyChanged()
        return msg
    }

    private data class SendOutcome(
        val ok: Boolean = false,
        val reason: String = "",
        val detail: String = "",
        val ipUsed: String = "",
    )

    private fun deliverText(target: String, text: String, msgId: String): SendOutcome {
        val p = hub.findPeer(target)
            ?: return SendOutcome(reason = Proto.REASON_UNREACHABLE)
        if (!p.acceptsInbound) return SendOutcome(reason = Proto.REASON_CLOSED)
        val ips = hub.sendCandidates(p)
        if (ips.isEmpty()) return SendOutcome(reason = Proto.REASON_UNREACHABLE)

        val portNum = if (p.port > 0) p.port else Proto.HTTP_PORT_BASE
        // 取前两个候选各试一次：同一 deviceId 内的地址切换，不是队列、不落 pending
        var detail = ""
        for (i in 0 until minOf(ips.size, 2)) {
            val r: PeerResult = peer.sendText(ips[i].ip, portNum, text, msgId)
            if (r.ok) {
                hub.touchPeer(target, ips[i].ip, portNum)
                return SendOutcome(ok = true, ipUsed = ips[i].ip)
            }
            detail = "try$i:${r.detail}/${ips[i].ip}"
        }
        return SendOutcome(reason = Proto.REASON_UNREACHABLE, detail = detail)
    }

    private fun deliverFile(
        target: String,
        fileName: String,
        mime: String,
        msgId: String,
        total: Long,
        reader: ChunkReader,
        xid: String,
        item: TransferItem,
    ): SendOutcome {
        val p = hub.findPeer(target)
        if (p == null) {
            // 已经登记了传输项：提前返回必须把它标失败，否则一条卡在"传输中"的幽灵行永远不消失
            Transfers.finish(item, false, Proto.REASON_UNREACHABLE)
            return SendOutcome(reason = Proto.REASON_UNREACHABLE)
        }
        if (!p.acceptsInbound) {
            Transfers.finish(item, false, Proto.REASON_CLOSED)
            return SendOutcome(reason = Proto.REASON_CLOSED)
        }
        val ips = hub.sendCandidates(p)
        if (ips.isEmpty()) {
            Transfers.finish(item, false, Proto.REASON_UNREACHABLE)
            return SendOutcome(reason = Proto.REASON_UNREACHABLE)
        }

        val portNum = if (p.port > 0) p.port else Proto.HTTP_PORT_BASE
        var detail = ""
        for (i in 0 until minOf(ips.size, 2)) {
            val r = if (p.resumable) {
                uploadChunked(ips[i].ip, portNum, fileName, mime, msgId, total, reader, xid, item)
            } else {
                // 老对端不会拼片：只能整份发（>256 MB 时明确拒绝，不偷偷读进内存）
                if (total > 256L * 1024 * 1024) {
                    return SendOutcome(reason = "对方版本太旧，不支持大文件分片")
                }
                val all = ByteArray(total.toInt())
                var off = 0
                while (off < total) {
                    val len = minOf(Transfers.CHUNK.toLong(), total - off).toInt()
                    val part = reader.readAt(off.toLong(), len) ?: return SendOutcome(reason = "读取文件失败")
                    System.arraycopy(part, 0, all, off, part.size)
                    off += part.size
                }
                val one = peer.upload(ips[i].ip, portNum, fileName, mime, msgId, all)
                if (one.ok) Transfers.setProgress(item, total)
                one
            }
            if (r.ok) {
                Transfers.finish(item, true)
                hub.touchPeer(target, ips[i].ip, portNum)
                return SendOutcome(ok = true, ipUsed = ips[i].ip)
            }
            detail = "try$i:${r.detail}/${ips[i].ip}"
        }
        // 取消不是"失败"：传输项已经是 cancelled，这里只把消息原因写清楚
        if (Transfers.isCancelled(xid)) {
            return SendOutcome(reason = "已取消", detail = detail)
        }
        Transfers.finish(item, false, Proto.REASON_UNREACHABLE)
        return SendOutcome(reason = Proto.REASON_UNREACHABLE, detail = detail)
    }

    /**
     * 分片发送 + 断点续传。
     *
     * 断线时先问服务端已收多少，从那个位置接着发 —— 大文件在弱网下重传整份等于永远发不完。
     */
    private fun uploadChunked(
        ip: String,
        portNum: Int,
        fileName: String,
        mime: String,
        msgId: String,
        total: Long,
        reader: ChunkReader,
        xid: String,
        item: TransferItem,
    ): PeerResult {
        var offset = 0L
        var failures = 0
        var last = PeerResult()
        while (offset < total) {
            // 用户点了 ×：立刻停，不再发下一片（已经发出去的片由服务端按 xid 留着，重发可续）
            if (Transfers.isCancelled(xid)) {
                return PeerResult(ok = false, reason = "已取消", detail = "cancelled")
            }
            val end = minOf(offset + Transfers.CHUNK, total)
            val slice = reader.readAt(offset, (end - offset).toInt())
                ?: return PeerResult(ok = false, reason = "读取文件失败", detail = "read_failed")
            val r = peer.uploadChunk(ip, portNum, fileName, mime, msgId, xid, offset, total, slice)
            if (r.ok) {
                offset = end
                failures = 0
                Transfers.setProgress(item, offset)
                last = r
                continue
            }
            last = r
            failures++
            if (failures > 6) return last
            val got = peer.uploadStatus(ip, portNum, xid)
            if (got in 0..total && got != offset) {
                offset = got
                failures = 0
                Transfers.setProgress(item, offset)
                continue
            }
            Thread.sleep(300)
        }
        return last
    }

    private fun resolveTarget(peerKey: String): String {
        if (peerKey.isEmpty()) return hub.resolvePeerKey("")
        if (hub.peers.containsKey(peerKey)) return peerKey
        return hub.resolvePeerKey(peerKey)
    }

    private fun findByMsgId(msgId: String): LsMsg? = hub.messages.firstOrNull { it.msgId == msgId }

    /** 从 `<dev>:<epoch>:<seq>` 里取后两段 */
    private fun parseMsgIdTail(msgId: String, msg: LsMsg) {
        val last = msgId.lastIndexOf(':')
        if (last < 0) return
        val prev = msgId.lastIndexOf(':', last - 1)
        if (prev < 0) return
        msgId.substring(prev + 1, last).toLongOrNull()?.let { msg.epoch = it }
        msgId.substring(last + 1).toLongOrNull()?.let { msg.seq = it }
    }

    /** 构造一条入站消息（含幂等去重） */
    fun buildInbound(
        dev: String,
        peerKey: String,
        text: String,
        msgIdIn: String,
        kind: String,
        fileId: String,
        fileName: String,
        size: Long,
    ): LsMsg {
        val msg = LsMsg(
            kind = kind,
            text = text,
            dir = Proto.DIR_IN,
            fileId = fileId,
            fileName = fileName,
            fileSize = size,
            time = Proto.nowMs(),
            unread = true,
            dev = if (dev.isNotEmpty()) dev else peerName,
        )
        msg.peerTs = msg.time
        msg.recvTs = msg.time
        val peer = if (peerKey.isNotEmpty()) peerKey else hub.resolvePeerKey(msg.dev)
        msg.from = peer
        msg.to = hub.self.deviceId

        // 幂等优先：同一个 msgId 只留第一条（重复提交不能变成两条气泡）
        if (msgIdIn.isNotEmpty()) {
            findByMsgId(msgIdIn)?.let { return it }
        }

        // **id 必须在这里分配**（不管有没有 msgId）：调用方按 `id > 0` 判断要不要入库，
        // id 留 0 就等于消息被静默丢弃，症状是"电脑发来的消息手机一条都收不到"，接口还回 ok:true
        msg.id = hub.peekNextId()
        if (msgIdIn.isNotEmpty()) {
            msg.msgId = msgIdIn
            parseMsgIdTail(msgIdIn, msg)
        } else {
            // 老客户端不带 msgId：用全局 id 推导。
            // peer 本身可能已经是 legacy:xxx，无脑再拼一次前缀会得到 legacy:legacy:xxx:0:1 ——
            // 与历史行推导规则不一致，同一条消息在"入库"和"重启恢复"两条路上会得到两个 msgId
            val marker = if (peer.startsWith("legacy:")) "" else "legacy:"
            msg.epoch = 0
            msg.seq = msg.id
            msg.msgId = "$marker$peer:0:${msg.id}"
        }
        msg.convId = Proto.makeConvId(msg.from, msg.to)
        return msg
    }

    // ------------------------------------------------------------ 文件落盘

    private data class SavedFile(
        val ok: Boolean = false,
        val fileId: String = "",
        val fileName: String = "",
        val fileRel: String = "",
        val size: Long = 0,
    )

    /** 流式落盘：1 MB 一块读 reader，边算哈希边写临时文件，最后改名（不整份进内存） */
    private fun saveIncomingFileStream(
        peerKey: String,
        fileName: String,
        mime: String,
        size: Long,
        reader: ChunkReader,
    ): SavedFile {
        val display = hub.displayName(peerKey)
        val dir = hub.store.peerDir(peerKey, display)
        com.sanqiu.lanshare.core.LsStore.ensureDir(dir)

        val tmp = java.io.File(dir, ".~out_" + Proto.nowMs() + ".part")
        val fnv = Proto.Fnv1a64()
        var written = 0L
        try {
            java.io.FileOutputStream(tmp).use { fos ->
                var off = 0L
                while (off < size) {
                    val len = minOf(Transfers.CHUNK.toLong(), size - off).toInt()
                    val buf = reader.readAt(off, len) ?: throw java.io.IOException("read_at_" + off)
                    if (buf.isEmpty()) break
                    fnv.update(buf)
                    fos.write(buf)
                    off += buf.size
                    written += buf.size
                }
            }
        } catch (e: Exception) {
            tmp.delete()
            android.util.Log.w("LanShare", "stream save failed: " + e.message)
            return SavedFile(ok = false)
        }
        if (written != size) {
            tmp.delete()
            return SavedFile(ok = false)
        }
        val hash = fnv.hex()

        // 内容去重：同尺寸 + 同 64 位哈希 → 复用已有文件（读盘也是流式）
        dir.listFiles()?.forEach { f ->
            if (!f.isFile || f.name.startsWith(".")) return@forEach
            if (f.length() != size) return@forEach
            if (hashFile(f) != hash) return@forEach
            tmp.delete()
            return SavedFile(
                ok = true,
                fileId = "f_" + Proto.nowMs() + "_" + f.name.length.toString(16),
                fileName = f.name,
                fileRel = dir.name + "/" + f.name,
                size = size,
            )
        }

        var target = sanitizeName(fileName)
        if (target.isEmpty()) target = "file"
        var finalName = target
        var n = 2
        while (java.io.File(dir, finalName).exists()) {
            finalName = target + " (" + n + ")"
            n++
            if (n > 999) break
        }
        val out = java.io.File(dir, finalName)
        if (!tmp.renameTo(out)) {
            // 跨卷/被占用：退化成流式拷贝（绝不再整份读进内存）
            try {
                tmp.inputStream().use { ins -> out.outputStream().use { ins.copyTo(it, 1024 * 1024) } }
                tmp.delete()
            } catch (e: Exception) {
                tmp.delete()
                return SavedFile(ok = false)
            }
        }
        return SavedFile(
            ok = true,
            fileId = "f_" + Proto.nowMs() + "_" + (size and 0xFFFF).toString(16),
            fileName = finalName,
            fileRel = dir.name + "/" + finalName,
            size = size,
        )
    }

    /** 流式计算文件哈希（1 MB 一块），用于内容去重 */
    private fun hashFile(f: java.io.File): String {
        val fnv = Proto.Fnv1a64()
        try {
            f.inputStream().use { ins ->
                val buf = ByteArray(1024 * 1024)
                while (true) {
                    val n = ins.read(buf)
                    if (n <= 0) break
                    fnv.update(buf, 0, n)
                }
            }
        } catch (e: Exception) {
            android.util.Log.w("LanShare", "hash file failed: " + f.name)
            return ""
        }
        return fnv.hex()
    }

    private fun saveIncomingFile(peerKey: String, fileName: String, mime: String, bytes: ByteArray): SavedFile {
        val display = hub.displayName(peerKey)
        val dir = hub.store.peerDir(peerKey, display)
        com.sanqiu.lanshare.core.LsStore.ensureDir(dir)
        val hash = Proto.fnv1a64Hex(bytes)

        // 内容相同的文件只留一份（尺寸 + 64 位内容哈希）
        dir.listFiles()?.forEach { f ->
            if (!f.isFile) return@forEach
            if (f.length() != bytes.size.toLong()) return@forEach
            val existing = hub.store.readBytes(f) ?: return@forEach
            if (Proto.fnv1a64Hex(existing) != hash) return@forEach
            return SavedFile(
                ok = true,
                fileId = "f_${Proto.nowMs()}_${f.name.length.toString(16)}",
                fileName = f.name,
                fileRel = "${dir.name}/${f.name}",
                size = bytes.size.toLong(),
            )
        }

        var target = sanitizeName(fileName)
        if (target.isEmpty()) target = "file"
        var finalName = target
        var n = 2
        while (java.io.File(dir, finalName).exists()) {
            finalName = "$target ($n)"
            n++
            if (n > 999) break
        }
        if (!hub.store.writeBytes(java.io.File(dir, finalName), bytes)) {
            return SavedFile(ok = false)
        }
        return SavedFile(
            ok = true,
            fileId = "f_${Proto.nowMs()}_${(bytes.size and 0xFFFF).toString(16)}",
            fileName = finalName,
            fileRel = "${dir.name}/$finalName",
            size = bytes.size.toLong(),
        )
    }

    /** 导出到相册/下载。失败只记录，不影响消息入库（私有目录里的那份仍可点开）。 */
    private fun exportAttachment(saved: SavedFile, isImage: Boolean) {
        val ex = exporter ?: return
        val src = java.io.File(hub.store.getSaveDir(), saved.fileRel)
        val err = ex.export(src, isImage, saved.fileName)
        if (err.isNotEmpty()) {
            lastExportError = err
            com.sanqiu.lanshare.net.MediaExporter.noteFailure()
        }
    }

    private fun findFilePath(fileId: String): java.io.File? {
        for (m in hub.messages) {
            if (m.fileId != fileId) continue
            if (m.fileRel.isNotEmpty()) return java.io.File(hub.store.getSaveDir(), m.fileRel)
            if (m.fileName.isNotEmpty()) return java.io.File(hub.store.getSaveDir(), m.fileName)
        }
        return null
    }

    private fun findFileName(fileId: String): String =
        hub.messages.firstOrNull { it.fileId == fileId }?.fileName ?: "file"

    private fun findFileMime(fileId: String): String =
        hub.messages.firstOrNull { it.fileId == fileId && it.mime.isNotEmpty() }?.mime
            ?: "application/octet-stream"

    private fun isImageMime(mime: String): Boolean = mime.startsWith("image/")

    private fun minimalHtml(): String =
        "<!doctype html><meta charset=utf-8><title>LanShare</title>" +
            "<body style=\"font:16px system-ui;padding:24px\">" +
            "<h3>LanShare 服务在运行</h3>" +
            "<p>端口 $port。网页界面尚未内置，可用 <code>/api/devices</code> 查看设备。</p>"

    companion object {
        /** 移动端单次上传上限（手机内存有限；超出 413） */
        const val MAX_UPLOAD = 4096L * 1024 * 1024

        private const val MIME_ALLOWED =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/+-."

        /** mime 只保留 [A-Za-z0-9/+-.]，与 Windows 端口径一致 */
        fun sanitizeMime(mime: String): String {
            val sb = StringBuilder()
            for (c in mime) if (MIME_ALLOWED.contains(c)) sb.append(c)
            return if (sb.isEmpty()) "application/octet-stream" else sb.toString()
        }

        /** 容错解析 `1.2.3.4:8765` / `http://1.2.3.4:8765/` / `1.2.3.4` */
        fun splitHostPort(input: String, portText: String): Pair<String, String> {
            var host = input.trim()
            if (host.startsWith("http://")) host = host.substring(7)
            if (host.startsWith("https://")) host = host.substring(8)
            val slash = host.indexOf('/')
            if (slash >= 0) host = host.substring(0, slash)
            var portNum = portText.trim()
            val colon = host.indexOf(':')
            if (colon >= 0) {
                portNum = host.substring(colon + 1).trim()
                host = host.substring(0, colon).trim()
            }
            if (portNum.isEmpty()) portNum = Proto.HTTP_PORT_BASE.toString()
            val p = portNum.toIntOrNull() ?: Proto.HTTP_PORT_BASE
            val use = if (p <= 0 || p > 65535) Proto.HTTP_PORT_BASE else p
            return host to use.toString()
        }
    }
}
