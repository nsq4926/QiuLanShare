package com.sanqiu.lanshare.api

import com.sanqiu.lanshare.core.LsHub
import com.sanqiu.lanshare.core.LsStore
import com.sanqiu.lanshare.core.Proto
import com.sanqiu.lanshare.net.HttpServer
import com.sanqiu.lanshare.net.PeerClient
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.nio.file.Files

/**
 * HTTP 路由回归测试。
 *
 * 走**真 HTTP**（起服务 + 连接）而不是直接调 handler —— 这样 Content-Length、
 * 状态码、query 解析、响应头都在验证范围内，和真机上的调用路径一致。
 *
 * 重点盯三件最容易静默出错的事：
 *  1. `dir=in` 的消息必须入库且计入未读；
 *  2. 同 `msgId` 重复提交**不能变成两条**（幂等）；
 *  3. 上传同名文件要加 `(2)`，内容相同要复用（不产生第二份）。
 */
class ApiTest {

    private lateinit var dir: File
    private lateinit var hub: LsHub
    private lateinit var api: LsApi
    private lateinit var server: HttpServer
    private var port = 0

    private val peerId = "an-peer00000009"

    @Before
    fun setUp() {
        dir = Files.createTempDirectory("lanshare-api").toFile()
        hub = LsHub(LsStore(dir))
        hub.load()
        hub.ensureDeviceId()
        hub.setDeviceName("测试机")

        val peer = PeerClient()
        peer.setSelf(hub.self.deviceId, hub.self.name)
        api = LsApi(hub, peer)
        api.port = 8765

        server = HttpServer({ req -> api.handle(req) })
        port = server.start("127.0.0.1", 0, 1)
        api.port = port
    }

    @After
    fun tearDown() {
        server.stop()
    }

    // ------------------------------------------------------------ 基础路由

    @Test
    fun `ping 返回 ok`() {
        val (code, body) = get("/api/ping")
        assertEquals(200, code)
        assertEquals(true, JSONObject(body).getBoolean("ok"))
    }

    @Test
    fun `hello 字段齐全且 TTL 与协议一致`() {
        val (code, body) = get("/api/hello")
        assertEquals(200, code)
        val o = JSONObject(body)
        assertEquals(Proto.LS_APP, o.getString("app"))
        assertEquals(hub.self.deviceId, o.getString("deviceId"))
        assertEquals(Proto.API_VERSION, o.getString("apiVersion"))
        assertEquals("android", o.getString("platform"))
        assertEquals(Proto.ONLINE_TTL, o.getLong("onlineTtl"))
        assertEquals(Proto.STALE_TTL, o.getLong("staleTtl"))
        assertTrue("acceptsInbound 必须下发", o.has("acceptsInbound"))
        assertTrue("filteredNicCount 必须下发", o.has("filteredNicCount"))
        assertEquals(port, o.getInt("port"))
    }

    @Test
    fun `devices 返回规范结构`() {
        val (code, body) = get("/api/devices")
        assertEquals(200, code)
        val o = JSONObject(body)
        assertEquals(true, o.getBoolean("ok"))
        assertTrue(o.has("now"))
        assertTrue(o.has("total"))
        assertEquals(0, o.getJSONArray("devices").length())
    }

    @Test
    fun `conversations 返回空数组`() {
        val (code, body) = get("/api/conversations")
        assertEquals(200, code)
        assertEquals(0, JSONObject(body).getJSONArray("conversations").length())
    }

    @Test
    fun `未知路径 404 且错误体规范`() {
        val (code, body) = get("/api/nope")
        assertEquals(404, code)
        assertFalse(JSONObject(body).getBoolean("ok"))
    }

    @Test
    fun `GET 打 api-message 返回 405`() {
        val (code, _) = get("/api/message")
        assertEquals(405, code)
    }

    // ------------------------------------------------------------ 入站消息

    @Test
    fun `入站文字入库并计入未读`() {
        val (code, body) = postJson(
            "/api/message",
            "{\"text\":\"你好\",\"dir\":\"in\",\"dev\":\"对端\",\"peer\":\"$peerId\"}",
            v2 = true,
        )
        assertEquals(200, code)
        val o = JSONObject(body)
        assertEquals(true, o.getBoolean("ok"))
        val id = o.getLong("id")
        assertTrue("id 必须分配（否则调用方会当成没入库）", id > 0)

        assertEquals(1, hub.messages.size)
        assertEquals(Proto.DIR_IN, hub.messages[0].dir)
        assertEquals(1L, hub.totalUnread())
        assertEquals(peerId, hub.messages[0].from)
        assertEquals(hub.self.deviceId, hub.messages[0].to)
    }

    @Test
    fun `同 msgId 重复提交只留一条`() {
        val body = "{\"text\":\"重复\",\"dir\":\"in\",\"peer\":\"$peerId\",\"msgId\":\"p:1:7\"}"
        val (_, b1) = postJson("/api/message", body, v2 = true)
        val id1 = JSONObject(b1).getLong("id")
        val (_, b2) = postJson("/api/message", body, v2 = true)
        val id2 = JSONObject(b2).getLong("id")

        assertEquals("重复提交必须返回同一条", id1, id2)
        assertEquals("库里不能出现两条", 1, hub.messages.size)
    }

    @Test
    fun `空 text 返回 400`() {
        val (code, _) = postJson("/api/message", "{\"text\":\"\",\"dir\":\"in\"}", v2 = true)
        assertEquals(400, code)
    }

    @Test
    fun `未读清零唯一入口`() {
        postJson("/api/message", "{\"text\":\"a\",\"dir\":\"in\",\"peer\":\"$peerId\"}", v2 = true)
        postJson("/api/message", "{\"text\":\"b\",\"dir\":\"in\",\"peer\":\"$peerId\"}", v2 = true)
        assertEquals(2L, hub.totalUnread())

        // GET /api/messages 绝不清未读（"拉过" ≠ "看过"）
        get("/api/messages?peer=$peerId", v2 = true)
        assertEquals(2L, hub.totalUnread())

        val (code, body) = postJson("/api/messages/read", "{\"peer\":\"$peerId\"}", v2 = true)
        assertEquals(200, code)
        assertEquals(2L, JSONObject(body).getLong("cleared"))
        assertEquals(0L, hub.totalUnread())
    }

    @Test
    fun `messages 支持 since 与 peer 水位`() {
        postJson("/api/message", "{\"text\":\"m1\",\"dir\":\"in\",\"peer\":\"$peerId\",\"msgId\":\"p:1:1\"}", v2 = true)
        postJson("/api/message", "{\"text\":\"m2\",\"dir\":\"in\",\"peer\":\"$peerId\",\"msgId\":\"p:1:2\"}", v2 = true)

        val (_, all) = get("/api/messages?peer=$peerId", v2 = true)
        val o = JSONObject(all)
        assertEquals(2, o.getJSONArray("messages").length())
        assertEquals(1L, o.getLong("lastEpoch"))
        assertEquals(2L, o.getLong("lastSeq"))

        val (_, after) = get("/api/messages?peer=$peerId&sinceEpoch=1&sinceSeq=1", v2 = true)
        assertEquals(1, JSONObject(after).getJSONArray("messages").length())
    }

    // ------------------------------------------------------------ 上传

    private fun upload(name: String, content: String, peer: String = peerId): JSONObject {
        val q = "/api/upload?name=$name&mime=text/plain&dir=in&dev=PC&peer=$peer"
        val bytes = content.toByteArray(Charsets.UTF_8)
        val conn = open(q, v2 = true)
        conn.requestMethod = "POST"
        conn.doOutput = true
        conn.setRequestProperty("Content-Type", "application/octet-stream")
        conn.setFixedLengthStreamingMode(bytes.size)
        conn.outputStream.use { it.write(bytes) }
        val text = conn.inputStream.use { it.readBytes().toString(Charsets.UTF_8) }
        return JSONObject(text)
    }

    @Test
    fun `上传落盘并可回取`() {
        val msg = upload("t1.txt", "hello").getJSONObject("message")
        assertEquals(Proto.KIND_FILE, msg.getString("kind"))
        assertEquals(5L, msg.getLong("fileSize"))

        val fileId = msg.getString("fileId")
        val (code, bytes) = getRaw("/api/file?id=$fileId")
        assertEquals(200, code)
        assertEquals("hello", String(bytes, Charsets.UTF_8))
    }

    @Test
    fun `同名不同内容加序号`() {
        val a = upload("same.txt", "aaa").getJSONObject("message")
        val b = upload("same.txt", "bbb").getJSONObject("message")
        assertEquals("same.txt", a.getString("fileName"))
        assertEquals("same.txt (2)", b.getString("fileName"))
    }

    @Test
    fun `内容相同的文件只留一份`() {
        val a = upload("dup1.txt", "same-content").getJSONObject("message")
        val b = upload("dup2.txt", "same-content").getJSONObject("message")
        // 复用已有的那份，不产生第二个文件
        assertEquals(a.getString("fileName"), b.getString("fileName"))

        val saveDir = File(hub.store.getSaveDir())
        val total = saveDir.walkTopDown().count { it.isFile }
        assertEquals("同内容只应有一个文件", 1, total)
    }

    @Test
    fun `上传空体返回 400`() {
        val conn = open("/api/upload?name=x", v2 = true)
        conn.requestMethod = "POST"
        conn.doOutput = true
        conn.setFixedLengthStreamingMode(0)
        conn.outputStream.use { }
        assertEquals(400, conn.responseCode)
    }

    // ------------------------------------------------------------ 历史

    @Test
    fun `清空会话递增 epoch 与 histRev`() {
        postJson("/api/message", "{\"text\":\"x\",\"dir\":\"in\",\"peer\":\"$peerId\"}", v2 = true)
        val convId = hub.messages[0].convId
        val epoch0 = hub.self.epoch

        val (code, body) = postJson("/api/history/clear?convId=$convId", "", v2 = true)
        assertEquals(200, code)
        assertEquals(1L, JSONObject(body).getLong("removed"))
        assertEquals(epoch0 + 1, hub.self.epoch)
        assertEquals(1L, hub.histRev(convId))
        assertTrue(hub.messages.isEmpty())
    }

    @Test
    fun `响应永远是合法 JSON`() {
        get("/api/hello")
        get("/api/devices")
        postJson("/api/message", "{\"text\":\"中文\\\"引号\",\"dir\":\"in\",\"peer\":\"$peerId\"}", v2 = true)
        assertEquals("拼出过非法 JSON", "", api.lastBadJson)
    }

    @Test
    fun `添加自己返回 400 self 而不是 502`() {
        // 这是自环用例：Api 会去探活"自己这台"，而本进程的 hello 请求要同一把 gate。
        // 若 handle 在持锁期间做探头，两边互等 → 探测超时 → 误报 502 connect_fail。
        val (code, body) = postJson(
            "/api/devices/add",
            "{\"ip\":\"127.0.0.1:$port\"}",
            v2 = true,
        )
        assertEquals("应为 400 self，实际=$code body=$body", 400, code)
        assertTrue(body, body.contains("self"))
    }

    @Test
    fun `添加不存在的地址返回 502`() {
        // 192.0.2.0/24 是 RFC5737 的测试网段，保证不可达；不能用 127.x（会被判成自己）
        val (code, body) = postJson("/api/devices/add", "{\"ip\":\"192.0.2.1:8765\"}", v2 = true)
        assertEquals(502, code)
        assertTrue(body, body.contains("connect_fail"))
    }

    @Test
    fun `空 ip 返回 400`() {
        val (code, _) = postJson("/api/devices/add", "{\"ip\":\"\"}", v2 = true)
        assertEquals(400, code)
    }

    @Test
    fun `重命名写入别名并体现在显示名`() {
        // 必须先 announce 建 peer：入站消息**不会**建 peer（这是设计），
        // 对未登记设备 rename 会按 legacy 键处理，别名就挂到 legacy:* 上了
        hub.applyAnnounce(makeAnnounce(peerId, name = "对端原名"), "192.168.8.50")

        val (code, _) = postJson(
            "/api/devices/rename",
            "{\"deviceId\":\"$peerId\",\"alias\":\"我的小手机\"}",
            v2 = true,
        )
        assertEquals(200, code)
        assertEquals("我的小手机", hub.displayName(peerId))
        // 清空别名回到自报名
        postJson("/api/devices/rename", "{\"deviceId\":\"$peerId\",\"alias\":\"\"}", v2 = true)
        assertEquals("对端原名", hub.displayName(peerId))
    }

    @Test
    fun `删除设备 dry 只报数量不动文件`() {
        hub.applyAnnounce(makeAnnounce(peerId, name = "对端"), "192.168.8.50")
        upload("keep.txt", "keep-me")

        val (code, body) = postJson("/api/devices/remove?dry=1", "{\"deviceId\":\"$peerId\"}", v2 = true)
        assertEquals(200, code)
        val o = JSONObject(body)
        assertEquals(true, o.getBoolean("dry"))
        assertTrue("应报告文件数，实际=$body", o.getInt("fileCount") >= 1)
        // dry 不能真删
        val saveDir = File(hub.store.getSaveDir())
        assertTrue("dry=1 不应删文件", saveDir.walkTopDown().any { it.name == "keep.txt" })
        assertTrue(hub.peers.containsKey(peerId))
    }

    private fun makeAnnounce(id: String, name: String): JSONObject = JSONObject().apply {
        put("v", 2)
        put("app", "TMLS")
        put("deviceId", id)
        put("name", name)
        put("ips", org.json.JSONArray(listOf("192.168.8.50")))
        put("port", 8765)
        put("acceptsInbound", true)
        put("platform", "android")
    }

    // ------------------------------------------------------------ helpers

    private fun open(path: String, v2: Boolean = false): HttpURLConnection {
        val url = URL("http://127.0.0.1:$port$path")
        return (url.openConnection() as HttpURLConnection).apply {
            connectTimeout = 3000
            readTimeout = 5000
            instanceFollowRedirects = false
            if (v2) setRequestProperty("X-Device-Id", "an-test000000001")
            setRequestProperty("Content-Type", "application/json")
        }
    }

    private fun get(path: String, v2: Boolean = false): Pair<Int, String> {
        val conn = open(path, v2)
        conn.requestMethod = "GET"
        val code = conn.responseCode
        val stream = if (code >= 400) conn.errorStream else conn.inputStream
        return code to (stream?.use { it.readBytes().toString(Charsets.UTF_8) } ?: "")
    }

    private fun getRaw(path: String): Pair<Int, ByteArray> {
        val conn = open(path, v2 = true)
        conn.requestMethod = "GET"
        val code = conn.responseCode
        return code to (conn.inputStream.use { it.readBytes() })
    }

    private fun postJson(path: String, body: String, v2: Boolean): Pair<Int, String> {
        val conn = open(path, v2)
        conn.requestMethod = "POST"
        val bytes = body.toByteArray(Charsets.UTF_8)
        conn.doOutput = true
        conn.setFixedLengthStreamingMode(bytes.size)
        conn.outputStream.use { it.write(bytes) }
        val code = conn.responseCode
        val stream = if (code >= 400) conn.errorStream else conn.inputStream
        return code to (stream?.use { it.readBytes().toString(Charsets.UTF_8) } ?: "")
    }
}
