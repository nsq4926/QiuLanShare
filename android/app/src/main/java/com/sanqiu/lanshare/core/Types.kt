package com.sanqiu.lanshare.core

import org.json.JSONObject

/**
 * 数据模型。
 *
 * 字段名必须与 Windows 端（`windows/src`）落盘的 JSON **完全一致** ——
 * 三端读的是同一套协议字段，改名即破坏互通。
 *
 * 这里刻意**手写 toJson 拼串**而不是 `JSONObject.toString()`：
 * 后者键顺序不保证，落盘字节与另两端不同，`check-protocol-parity.py`
 * 这类按文本比对的检查会失效，而且排查时 diff 出满屏噪声。
 */

/** 对端的一个候选地址 */
data class LsIp(
    var ip: String = "",
    var lastOkAt: Long = 0L,
) {
    fun toJson(): String = "{\"ip\":${Proto.jsonStr(ip)},\"lastOkAt\":$lastOkAt}"

    companion object {
        fun fromJson(o: JSONObject): LsIp = LsIp(
            ip = Proto.jStr(o, "ip"),
            lastOkAt = Proto.jNum(o, "lastOkAt"),
        )
    }
}

/** 本机身份段 */
data class LsSelf(
    var deviceId: String = "",
    var name: String = "",
    var mac: String = "",
    var ips: MutableList<String> = mutableListOf(),
    var filteredNicCount: Long = 0L,
    var keptNicCount: Long = 0L,
    var epoch: Long = 1L,
    var seq: Long = 0L,
) {
    fun toJson(): String {
        val arr = ips.joinToString(",") { Proto.jsonStr(it) }
        return "{\"deviceId\":${Proto.jsonStr(deviceId)}" +
            ",\"name\":${Proto.jsonStr(name)}" +
            ",\"mac\":${Proto.jsonStr(mac)}" +
            ",\"ips\":[$arr]" +
            ",\"filteredNicCount\":$filteredNicCount" +
            ",\"keptNicCount\":$keptNicCount" +
            ",\"epoch\":$epoch" +
            ",\"seq\":$seq}"
    }

    companion object {
        fun fromJson(o: JSONObject): LsSelf {
            val s = LsSelf()
            s.deviceId = Proto.jStr(o, "deviceId")
            s.name = Proto.jStr(o, "name")
            s.mac = Proto.jStr(o, "mac")
            val raw = Proto.jArr(o, "ips")
            val list = mutableListOf<String>()
            for (i in 0 until raw.length()) {
                val v = raw.opt(i)
                if (v is String) list += v
            }
            s.ips = list
            s.filteredNicCount = Proto.jNum(o, "filteredNicCount")
            s.keptNicCount = Proto.jNum(o, "keptNicCount")
            s.epoch = Proto.jNum(o, "epoch", 1L)
            s.seq = Proto.jNum(o, "seq")
            return s
        }
    }
}

/** 对端设备条目 */
data class LsPeer(
    var deviceId: String = "",
    var name: String = "",
    var alias: String = "",
    var platform: String = "windows",
    var mac: String = "",
    var macHistory: MutableList<String> = mutableListOf(),
    var ips: MutableList<LsIp> = mutableListOf(),
    var port: Int = 0,
    var role: String = "peer",
    var state: String = "offline",
    var acceptsInbound: Boolean = true,
    var ephemeral: Boolean = false,
    var lastSeen: Long = 0L,
    var lastReachableAt: Long = 0L,
    var firstSeen: Long = 0L,
    var apiVersion: String = "",
    /** 对端支持分片/续传（announce 或 hello 带 resumable:true） */
    var resumable: Boolean = false,
    var filteredNicCount: Long = 0L,
    var paired: Boolean = false,
    var convId: String = "",
) {
    fun toJson(): String {
        val macs = macHistory.joinToString(",") { Proto.jsonStr(it) }
        val ipJson = ips.joinToString(",") { it.toJson() }
        return "{\"deviceId\":${Proto.jsonStr(deviceId)}" +
            ",\"name\":${Proto.jsonStr(name)}" +
            ",\"alias\":${Proto.jsonStr(alias)}" +
            ",\"platform\":${Proto.jsonStr(platform)}" +
            ",\"mac\":${Proto.jsonStr(mac)}" +
            ",\"macHistory\":[$macs]" +
            ",\"ips\":[$ipJson]" +
            ",\"port\":$port" +
            ",\"role\":${Proto.jsonStr(role)}" +
            ",\"state\":${Proto.jsonStr(state)}" +
            ",\"acceptsInbound\":${if (acceptsInbound) "true" else "false"}" +
            ",\"ephemeral\":${if (ephemeral) "true" else "false"}" +
            ",\"lastSeen\":$lastSeen" +
            ",\"lastReachableAt\":$lastReachableAt" +
            ",\"firstSeen\":$firstSeen" +
            ",\"apiVersion\":${Proto.jsonStr(apiVersion)}" +
            ",\"resumable\":${if (resumable) "true" else "false"}" +
            ",\"filteredNicCount\":$filteredNicCount" +
            ",\"paired\":${if (paired) "true" else "false"}" +
            ",\"convId\":${Proto.jsonStr(convId)}}"
    }

    companion object {
        fun fromJson(o: JSONObject): LsPeer {
            val p = LsPeer()
            p.deviceId = Proto.jStr(o, "deviceId")
            p.name = Proto.jStr(o, "name")
            p.alias = Proto.jStr(o, "alias")
            p.platform = Proto.jStr(o, "platform", "windows")
            p.mac = Proto.jStr(o, "mac")
            val macRaw = Proto.jArr(o, "macHistory")
            val macs = mutableListOf<String>()
            for (i in 0 until macRaw.length()) {
                val v = macRaw.opt(i)
                if (v is String) macs += v
            }
            p.macHistory = macs
            val ipRaw = Proto.jArr(o, "ips")
            val ipList = mutableListOf<LsIp>()
            for (i in 0 until ipRaw.length()) {
                val v = ipRaw.opt(i)
                if (v is JSONObject) ipList += LsIp.fromJson(v)
            }
            p.ips = ipList
            p.port = Proto.jNum(o, "port").toInt()
            p.role = Proto.jStr(o, "role", "peer")
            p.state = Proto.jStr(o, "state", "offline")
            p.acceptsInbound = Proto.jBool(o, "acceptsInbound", true)
            p.ephemeral = Proto.jBool(o, "ephemeral")
            p.lastSeen = Proto.jNum(o, "lastSeen")
            // 冷启动一律视为"从未可达"，持久化值只供排障
            p.lastReachableAt = 0L
            p.firstSeen = Proto.jNum(o, "firstSeen")
            p.apiVersion = Proto.jStr(o, "apiVersion")
            p.resumable = Proto.jBool(o, "resumable")
            p.filteredNicCount = Proto.jNum(o, "filteredNicCount")
            p.paired = Proto.jBool(o, "paired")
            p.convId = Proto.jStr(o, "convId")
            return p
        }
    }
}

/** 消息（与 Windows 端落盘行同构） */
data class LsMsg(
    var id: Long = 0L,
    var dir: String = Proto.DIR_IN,
    var kind: String = Proto.KIND_TEXT,
    var text: String = "",
    var fileId: String = "",
    var fileName: String = "",
    var fileRel: String = "",
    var mime: String = "",
    var dev: String = "",
    var fileSize: Long = 0L,
    var time: Long = 0L,
    var msgId: String = "",
    var convId: String = "",
    var from: String = "",
    var to: String = "",
    var epoch: Long = 0L,
    var seq: Long = 0L,
    var peerTs: Long = 0L,
    var recvTs: Long = 0L,
    var status: String = "",
    /** 失败原因（仅出站失败时有值，**不落盘**） */
    var reason: String = "",
    /** 失败诊断串（**不落盘**） */
    var detail: String = "",
    var unread: Boolean = false,
) {
    fun toJson(): String = "{\"id\":$id" +
        ",\"dir\":${Proto.jsonStr(dir)}" +
        ",\"kind\":${Proto.jsonStr(kind)}" +
        ",\"text\":${Proto.jsonStr(text)}" +
        ",\"fileId\":${Proto.jsonStr(fileId)}" +
        ",\"fileName\":${Proto.jsonStr(fileName)}" +
        ",\"fileRel\":${Proto.jsonStr(fileRel)}" +
        ",\"mime\":${Proto.jsonStr(mime)}" +
        ",\"dev\":${Proto.jsonStr(dev)}" +
        ",\"fileSize\":$fileSize" +
        ",\"time\":$time" +
        ",\"msgId\":${Proto.jsonStr(msgId)}" +
        ",\"convId\":${Proto.jsonStr(convId)}" +
        ",\"from\":${Proto.jsonStr(from)}" +
        ",\"to\":${Proto.jsonStr(to)}" +
        ",\"epoch\":$epoch" +
        ",\"seq\":$seq" +
        ",\"peerTs\":$peerTs" +
        ",\"recvTs\":$recvTs" +
        ",\"status\":${Proto.jsonStr(status)}" +
        ",\"unread\":${if (unread) "true" else "false"}}"

    /** 是否计入未读（口径与 Windows 端一致） */
    fun isUnreadCountable(): Boolean =
        unread && dir == Proto.DIR_IN && kind != Proto.KIND_SYSTEM

    companion object {
        fun fromJson(o: JSONObject): LsMsg {
            val m = LsMsg()
            m.id = Proto.jNum(o, "id")
            m.dir = Proto.jStr(o, "dir", Proto.DIR_IN)
            if (m.dir != Proto.DIR_OUT) m.dir = Proto.DIR_IN
            m.kind = Proto.jStr(o, "kind", Proto.KIND_TEXT)
            m.text = Proto.jStr(o, "text")
            m.fileId = Proto.jStr(o, "fileId")
            m.fileName = Proto.jStr(o, "fileName")
            m.fileRel = Proto.jStr(o, "fileRel")
            m.mime = Proto.jStr(o, "mime")
            m.dev = Proto.jStr(o, "dev")
            m.fileSize = Proto.jNum(o, "fileSize")
            m.time = Proto.jNum(o, "time")
            m.msgId = Proto.jStr(o, "msgId")
            m.convId = Proto.jStr(o, "convId")
            m.from = Proto.jStr(o, "from")
            m.to = Proto.jStr(o, "to")
            m.epoch = Proto.jNum(o, "epoch")
            m.seq = Proto.jNum(o, "seq")
            m.peerTs = Proto.jNum(o, "peerTs")
            m.recvTs = Proto.jNum(o, "recvTs")
            m.status = Proto.jStr(o, "status")
            // reason/detail 只在内存里（失败当场的提示用），不落盘
            m.reason = ""
            m.detail = ""
            // 历史行一律视为已读，避免重启后未读爆表
            m.unread = false
            return m
        }
    }
}

/** 会话元信息（落盘 lanshare_convs.json） */
data class LsConvInfo(
    var peerDeviceId: String = "",
    var lastKnownName: String = "",
    var rev: Long = 0L,
) {
    fun toJson(): String = "{\"peerDeviceId\":${Proto.jsonStr(peerDeviceId)}" +
        ",\"lastKnownName\":${Proto.jsonStr(lastKnownName)}" +
        ",\"rev\":$rev}"

    companion object {
        fun fromJson(o: JSONObject): LsConvInfo = LsConvInfo(
            peerDeviceId = Proto.jStr(o, "peerDeviceId"),
            lastKnownName = Proto.jStr(o, "lastKnownName"),
            rev = Proto.jNum(o, "rev"),
        )
    }
}

/** 会话列表条目（`/api/conversations` 的一行） */
data class LsConv(
    var convId: String = "",
    var deviceId: String = "",
    var displayName: String = "",
    var lastTs: Long = 0L,
    var lastMessageId: Long = 0L,
    var count: Long = 0L,
    var unread: Long = 0L,
    var legacy: Boolean = false,
    var histRev: Long = 0L,
)

/** 设备列表条目（UI 用） */
data class LsDeviceItem(
    var deviceId: String = "",
    var displayName: String = "",
    var platform: String = "",
    var ip: String = "",
    var mac: String = "",
    var state: String = "",
    var unread: Long = 0L,
    var acceptsInbound: Boolean = true,
    /** 对端自报的设备名（列表里跟在别名后的括号内容） */
    var realName: String = "",
    /** 该会话最后一条消息的时间；0 = 还没聊过。设备列表按它置顶 */
    var lastTs: Long = 0L,
    /** 最后一条消息摘要（第二行用，文件消息显示文件名） */
    var preview: String = "",
)

/** 传输进度条目 */
data class LsTransfer(
    var id: Long = 0L,
    var name: String = "",
    var dir: String = Proto.DIR_IN,
    var total: Long = 0L,
    var received: Long = 0L,
    var speed: Long = 0L,
)

/** 设备表文件（lanshare_devices.json） */
class LsDeviceFile {
    var version: Long = 1L
    var self: LsSelf = LsSelf()
    var peers: MutableList<LsPeer> = mutableListOf()
    var aliases: MutableMap<String, String> = linkedMapOf()

    fun toJson(): String {
        val ps = peers.joinToString(",") { it.toJson() }
        val as_ = aliases.entries.joinToString(",") {
            Proto.jsonStr(it.key) + ":" + Proto.jsonStr(it.value)
        }
        return "{\"version\":$version" +
            ",\"self\":${self.toJson()}" +
            ",\"peers\":[$ps]" +
            ",\"aliases\":{$as_}}"
    }

    companion object {
        fun fromJson(text: String): LsDeviceFile {
            val f = LsDeviceFile()
            val o = Proto.parseObj(text)
            f.version = Proto.jNum(o, "version", 1L)
            val selfRaw = o.opt("self")
            if (selfRaw is JSONObject) {
                f.self = LsSelf.fromJson(selfRaw)
            }
            val peerRaw = Proto.jArr(o, "peers")
            val peers = mutableListOf<LsPeer>()
            for (i in 0 until peerRaw.length()) {
                val v = peerRaw.opt(i)
                if (v is JSONObject) {
                    val p = LsPeer.fromJson(v)
                    if (p.deviceId.isNotEmpty()) peers += p
                }
            }
            f.peers = peers
            val aliasRaw = o.opt("aliases")
            if (aliasRaw is JSONObject) {
                val keys = aliasRaw.keys()
                while (keys.hasNext()) {
                    val k = keys.next()
                    val v = aliasRaw.opt(k)
                    if (v is String) f.aliases[k] = v
                }
            }
            return f
        }
    }
}
