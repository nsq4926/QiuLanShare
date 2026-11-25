package com.sanqiu.lanshare.core

import org.json.JSONArray
import org.json.JSONObject

/**
 * 协议常量与纯函数。
 *
 * ⚠️ 每一个常量都必须与 Windows 端（`windows/src`）和鸿蒙端（`harmony/.../Proto.ets`）
 * **逐字一致**。协议是三方约定死的，改一边等于同时打断另外两端。
 *
 * 与 ArkTS 版的差异只有一处，且是**变简单**：64 位哈希直接用 Kotlin 的 `Long`
 * 乘法（溢出回绕的位模式与双半字实现等价），不需要手动拆高低位。
 * 但 offset basis 仍然是那个"少一位"的值，**不许修正成标准值**。
 */
object Proto {

    /** 应用标识（客户端识别用，对齐 Windows 端 app 字段） */
    const val LS_APP: String = "LanShare"

    /** UDP 发现报文的 app 标识（对齐 Windows 端 "TMLS"） */
    const val LS_TAG: String = "TMLS"

    /** 协议代次 */
    const val API_VERSION: String = "2"

    /** 平台标识：announce 带给对端，设备列表显示 "android" */
    const val LS_PLATFORM: String = "android"

    /** deviceId 前缀（pc- / hm- / web- / an-） */
    const val DEVICE_ID_PREFIX: String = "an-"

    /** HTTP 监听端口起始值（占用则递进） */
    const val HTTP_PORT_BASE: Int = 8765
    const val HTTP_PORT_TRIES: Int = 3

    /** UDP 发现端口（固定，与 HTTP 端口无关） */
    const val UDP_PORT: Int = 8766

    /** 五态判定（毫秒）。ONLINE_TTL 必须 < STALE_TTL，否则 unreachable 恒假 */
    const val ONLINE_TTL: Long = 30_000
    const val STALE_TTL: Long = 120_000

    /** ips[] 里某个地址的保鲜期 */
    const val IP_FRESH_MS: Long = 300_000

    /** 长轮询最长挂起秒数（服务端 clamp 到 25） */
    const val MAX_WAIT_SEC: Int = 25

    /** announce 广播周期 */
    const val ANNOUNCE_INTERVAL_MS: Long = 3_000

    /** 对端健康探测周期 */
    const val PROBE_INTERVAL_MS: Long = 5_000

    // 五态取值 —— 与 UI 五标签 1:1，客户端不做派生
    const val ST_ONLINE: String = "online"
    const val ST_ONLINE_NO_INBOUND: String = "online_no_inbound"
    const val ST_UNREACHABLE: String = "unreachable"
    const val ST_DISCOVERED: String = "discovered"
    const val ST_OFFLINE: String = "offline"

    // 消息方向
    const val DIR_IN: String = "in"
    const val DIR_OUT: String = "out"

    // 消息种类
    const val KIND_TEXT: String = "text"
    const val KIND_IMAGE: String = "image"
    const val KIND_FILE: String = "file"
    const val KIND_SYSTEM: String = "system"

    // 出站状态：只有两种，没有 pending
    const val STATUS_SENT: String = "sent"
    const val STATUS_FAILED: String = "failed"

    // 失败原因（文案 2，只允许两种）
    const val REASON_UNREACHABLE: String = "对方当前不可达"
    const val REASON_CLOSED: String = "对方已关闭接收"

    /** 上传上限（移动端 64 MB，与鸿蒙一致；Windows 是 512 MB） */
    const val MAX_UPLOAD_BYTES: Long = 64L * 1024 * 1024

    fun nowMs(): Long = System.currentTimeMillis()

    fun utf8Bytes(s: String): ByteArray = s.toByteArray(Charsets.UTF_8)

    // ------------------------------------------------------------ FNV-1a 64

    /**
     * offset basis 是 **1469598103934665603**，不是标准 FNV-1a 的
     * 14695981039346656037（多一位）。
     *
     * Windows 端 `DeviceRegistry.cpp:MakeConvId` 写死的就是前者。
     * **不要"修正"成标准值** —— 改了三端算出的 convId 就不同，
     * 同一对设备会裂成两个会话，症状是"消息偶尔不显示"且不报错。
     */
    private const val FNV_OFFSET: Long = 1469598103934665603L

    /** 1099511628211 = 256 * 2^32 + 435，与 ArkTS 版分解式里的 256/435 同源 */
    private const val FNV_PRIME: Long = 1099511628211L

    fun fnv1a64Hex(bytes: ByteArray): String {
        var h = FNV_OFFSET
        for (b in bytes) {
            h = h xor (b.toLong() and 0xFF)
            h *= FNV_PRIME
        }
        // ⚠️ 必须 toULong()：Long 为负时 toString(16) 会输出 "-..."，
        // 症状是 convId 少高位、两端裂会话。
        return h.toULong().toString(16).padStart(16, '0')
    }

    /**
     * 流式 FNV-1a 64 位（与 [fnv1a64Hex] **同一组常量**）。
     * 大文件不能整份读进内存再算哈希：1 GB 的 byte[] 会让手机直接 OOM。
     */
    class Fnv1a64 {
        private var h: Long = FNV_OFFSET

        fun update(buf: ByteArray, off: Int = 0, len: Int = buf.size) {
            var hh = h
            val end = off + len
            for (i in off until end) {
                hh = hh xor (buf[i].toLong() and 0xFF)
                hh *= FNV_PRIME
            }
            h = hh
        }

        fun hex(): String = h.toULong().toString(16).padStart(16, '0')
    }

    /**
     * convId 算法自检。
     *
     * 三个向量是按 Windows 端那个 offset basis 算出来的，任何一位改动都会红。
     * 返回空列表 = 通过。**自检失败只记日志，不阻断启动**（体检不是门禁）。
     */
    fun convIdSelfCheck(): List<String> {
        val problems = mutableListOf<String>()
        val expectEmpty = "14650fb0739d0383"
        val expectA = "44bd8ad473cd9906"
        val expectFoobar = "88fad7c0a8ff07f2"
        if (fnv1a64Hex(utf8Bytes("")) != expectEmpty) {
            problems += "fnv1a64(\"\") != $expectEmpty"
        }
        if (fnv1a64Hex(utf8Bytes("a")) != expectA) {
            problems += "fnv1a64(\"a\") != $expectA"
        }
        if (fnv1a64Hex(utf8Bytes("foobar")) != expectFoobar) {
            problems += "fnv1a64(\"foobar\") != $expectFoobar"
        }
        if (makeConvId("pc-1", "hm-2") != makeConvId("hm-2", "pc-1")) {
            problems += "makeConvId 不对称"
        }
        return problems
    }

    /**
     * 会话 id：两方 deviceId 排序后哈希，**不含 IP**。分隔符 0x1F。
     */
    fun makeConvId(a: String, b: String): String {
        val lo = if (a <= b) a else b
        val hi = if (a <= b) b else a
        return "c-" + fnv1a64Hex(utf8Bytes(lo + '\u001f' + hi))
    }

    /** 消息 id：发送方 deviceId + 代次 + 本端全局序号 */
    fun makeMsgId(deviceId: String, epoch: Long, seq: Long): String =
        "$deviceId:$epoch:$seq"

    /** legacy 会话键（唯一产出点，幂等） */
    fun makeLegacyKey(dev: String): String = when {
        dev.startsWith("legacy:") -> dev
        dev.isEmpty() -> "legacy:unknown"
        else -> "legacy:$dev"
    }

    /** 五态判定（与 Windows 端逐字一致） */
    fun stateOf(reachableAt: Long, acceptsInbound: Boolean, lastSeen: Long, now: Long): String {
        if (reachableAt > 0 && (now - reachableAt) <= ONLINE_TTL) {
            return if (acceptsInbound) ST_ONLINE else ST_ONLINE_NO_INBOUND
        }
        if ((now - lastSeen) <= STALE_TTL) {
            return if (reachableAt > 0) ST_UNREACHABLE else ST_DISCOVERED
        }
        return ST_OFFLINE
    }

    /** 状态排序优先级（越小越靠前） */
    fun stateRank(state: String): Int = when (state) {
        ST_ONLINE -> 0
        ST_ONLINE_NO_INBOUND -> 1
        ST_UNREACHABLE -> 2
        ST_DISCOVERED -> 3
        else -> 4
    }

    /** 五态 → UI 文案（照抄文案 8，三端逐字一致） */
    fun stateLabel(state: String): String = when (state) {
        ST_ONLINE -> "在线"
        ST_ONLINE_NO_INBOUND -> "在线 · 不接收"
        ST_UNREACHABLE -> "暂时连不上"
        ST_DISCOVERED -> "已发现"
        else -> "离线"
    }

    // ------------------------------------------------------------ JSON 安全取值

    /**
     * 复刻 ArkTS 的 `jStr` 容错语义，**不要换成 JSONObject.getString**：
     * 后者类型不符直接抛异常，与"缺参不报错、按默认值走"的全局口径相反。
     */
    fun jStr(o: JSONObject?, k: String, def: String = ""): String {
        val raw = o?.opt(k) ?: return def
        if (raw === JSONObject.NULL) return def
        return when (raw) {
            is String -> raw
            is Boolean -> if (raw) "true" else "false"
            is Double -> if (raw == Math.floor(raw) && !raw.isInfinite()) {
                raw.toLong().toString()
            } else {
                raw.toString()
            }
            is Number -> raw.toString()
            else -> def
        }
    }

    fun jNum(o: JSONObject?, k: String, def: Long = 0): Long {
        val raw = o?.opt(k) ?: return def
        if (raw === JSONObject.NULL) return def
        return when (raw) {
            is Number -> raw.toLong()
            is String -> raw.trim().toLongOrNull() ?: def
            is Boolean -> if (raw) 1 else 0
            else -> def
        }
    }

    fun jBool(o: JSONObject?, k: String, def: Boolean = false): Boolean {
        val raw = o?.opt(k) ?: return def
        if (raw === JSONObject.NULL) return def
        return when (raw) {
            is Boolean -> raw
            is Number -> raw.toDouble() != 0.0
            is String -> raw.lowercase() == "true" || raw == "1"
            else -> def
        }
    }

    fun jArr(o: JSONObject?, k: String): JSONArray {
        val raw = o?.opt(k)
        return raw as? JSONArray ?: JSONArray()
    }

    fun jHas(o: JSONObject?, k: String): Boolean {
        val raw = o?.opt(k) ?: return false
        return raw !== JSONObject.NULL
    }

    /** 解析一段 JSON 文本为对象；失败返回空对象（不抛） */
    fun parseObj(text: String?): JSONObject {
        if (text.isNullOrEmpty()) return JSONObject()
        return try {
            JSONObject(text)
        } catch (e: Exception) {
            JSONObject()
        }
    }

    // ------------------------------------------------------------ URL 编解码

    fun urlEncode(s: String): String = java.net.URLEncoder.encode(s, "UTF-8")

    fun urlDecode(s: String): String = try {
        java.net.URLDecoder.decode(s, "UTF-8")
    } catch (e: Exception) {
        s
    }

    /** 解析 query 串成 Map（值已 URL 解码） */
    fun parseQuery(q: String): MutableMap<String, String> {
        val m = mutableMapOf<String, String>()
        if (q.isEmpty()) return m
        q.split('&').forEach { p ->
            if (p.isEmpty()) return@forEach
            val eq = p.indexOf('=')
            if (eq >= 0) {
                m[urlDecode(p.substring(0, eq))] = urlDecode(p.substring(eq + 1))
            } else {
                m[urlDecode(p)] = ""
            }
        }
        return m
    }

    private const val HEX_UPPER = "0123456789ABCDEF"

    /** 百分号编码（RFC 5987 的 filename* 用，保留 unreserved 字符原样） */
    fun pctEncode(s: String): String {
        val sb = StringBuilder()
        for (ch in s) {
            val c = ch.code
            val unreserved = (c in 0x30..0x39) || (c in 0x41..0x5A) || (c in 0x61..0x7A) ||
                c == 0x2D || c == 0x2E || c == 0x5F || c == 0x7E
            if (unreserved) {
                sb.append(ch)
            } else {
                utf8Bytes(ch.toString()).forEach { b ->
                    val v = b.toInt() and 0xFF
                    sb.append('%').append(HEX_UPPER[v shr 4]).append(HEX_UPPER[v and 0x0F])
                }
            }
        }
        return sb.toString()
    }

    /**
     * JSON 字符串转义（与 Windows 端 JsonEscape 同口径：非 ASCII 原样透传）。
     *
     * ⚠️ **不导出**给业务层直接用 —— 拼 JSON 的字符串值一律走 [jsonStr]。
     * 这个函数不带引号，用错地方就会拼出非法 JSON，而非法 JSON 的症状是
     * "对端静默看不见我"，不报错。
     */
    private fun jsonEscape(s: String): String {
        val sb = StringBuilder(s.length + 8)
        for (ch in s) {
            when {
                ch == '"' -> sb.append("\\\"")
                ch == '\\' -> sb.append("\\\\")
                ch == '\n' -> sb.append("\\n")
                ch == '\r' -> sb.append("\\r")
                ch == '\t' -> sb.append("\\t")
                ch.code < 0x20 -> sb.append(' ')
                else -> sb.append(ch)
            }
        }
        return sb.toString()
    }

    /** 生成 JSON 字符串字面量（含引号） */
    fun jsonStr(v: String): String = "\"" + jsonEscape(v) + "\""

    /** 随机 hex（deviceId 后 12 位、网页身份 web-<12hex> 同源口径） */
    fun randomHex(len: Int): String {
        val bytes = ByteArray((len + 1) / 2)
        java.security.SecureRandom().nextBytes(bytes)
        val sb = StringBuilder(len)
        for (b in bytes) {
            val v = b.toInt() and 0xFF
            sb.append(HEX_UPPER[v shr 4].lowercaseChar())
            if (sb.length < len) sb.append(HEX_UPPER[v and 0x0F].lowercaseChar())
        }
        return sb.toString()
    }
}
