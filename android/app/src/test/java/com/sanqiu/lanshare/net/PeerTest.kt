package com.sanqiu.lanshare.net

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.concurrent.atomic.AtomicReference

/**
 * 出站客户端契约断言。
 *
 * 重点不是"能发出去"，而是**发出去的东西对端能不能按 v2 认出来**：
 * 少了 `X-Device-Id` 就会被当 legacy，`dir` 写反了气泡会出现在对面左边。
 * 这些错了都不报错，只是"消息偶尔显示不对"。
 */
class PeerTest {

    private val captured = AtomicReference<HttpRequest?>(null)

    private fun startServer(status: Int = 200): Pair<HttpServer, Int> {
        val server = HttpServer({ req ->
            captured.set(req)
            if (status == 200) {
                HttpResponse.json("{\"ok\":true,\"id\":1,\"msgId\":\"an-peer:1:1\",\"status\":\"sent\"}")
            } else {
                HttpResponse.status(status, "boom")
            }
        })
        return server to server.start("127.0.0.1", 0, 1)
    }

    @Test
    fun `sendText 的 body 与请求头符合契约`() {
        val (server, port) = startServer()
        try {
            val peer = PeerClient().apply { setSelf("an-self00000001", "我的手机") }
            val r = peer.sendText("127.0.0.1", port, "你好\"世界", "")
            assertTrue("应成功: $r", r.ok)
            assertEquals(200, r.status)

            val req = captured.get()!!
            // v2 的唯一判据
            assertEquals("an-self00000001", req.header("x-device-id"))
            assertEquals("POST", req.method)
            assertEquals("/api/message", req.path)

            val body = JSONObject(req.bodyText())
            // 服务器之间投递：dir 恒为 in（对端视角是我收到的一条）
            assertEquals("in", body.getString("dir"))
            // peer 是发送方自己
            assertEquals("an-self00000001", body.getString("peer"))
            assertEquals("我的手机", body.getString("dev"))
            // 中文与引号必须正确转义，否则对端解析失败 → 静默收不到
            assertEquals("你好\"世界", body.getString("text"))
            assertFalse("不传 msgId 时不应出现该键", body.has("msgId"))
        } finally {
            server.stop()
        }
    }

    @Test
    fun `带 msgId 时重发沿用同一 id`() {
        val (server, port) = startServer()
        try {
            val peer = PeerClient().apply { setSelf("an-self00000001", "手机") }
            peer.sendText("127.0.0.1", port, "重复发", "an-self:2:9")
            val body = JSONObject(captured.get()!!.bodyText())
            assertEquals("an-self:2:9", body.getString("msgId"))
        } finally {
            server.stop()
        }
    }

    @Test
    fun `upload 走 query 传元数据 且 body 是裸字节`() {
        val (server, port) = startServer()
        try {
            val peer = PeerClient().apply { setSelf("an-self00000001", "手机 A") }
            val bytes = byteArrayOf(1, 2, 3, 4, 5)
            val r = peer.upload("127.0.0.1", port, "图 1.png", "image/png", "an-self:1:2", bytes)
            assertTrue(r.ok)
            val req = captured.get()!!
            assertEquals("/api/upload", req.path)
            assertEquals("图 1.png", req.q("name"))
            assertEquals("image/png", req.q("mime"))
            assertEquals("in", req.q("dir"))
            assertEquals("手机 A", req.q("dev"))
            assertEquals("an-self00000001", req.q("peer"))
            assertEquals("an-self:1:2", req.q("msgId"))
            assertTrue("上传必须是裸字节，不是 multipart", req.body.contentEquals(bytes))
        } finally {
            server.stop()
        }
    }

    @Test
    fun `probeHello 取出对端 deviceId`() {
        val server = HttpServer({
            HttpResponse.json("{\"app\":\"LanShare\",\"deviceId\":\"an-peer00000009\",\"apiVersion\":\"2\"}")
        })
        val port = server.start("127.0.0.1", 0, 1)
        try {
            val peer = PeerClient().apply { setSelf("an-self00000001", "手机") }
            assertEquals("an-peer00000009", peer.probeHello("127.0.0.1", port))
        } finally {
            server.stop()
        }
    }

    @Test
    fun `对端返回非200时结果为不可达`() {
        val (server, port) = startServer(status = 503)
        try {
            val peer = PeerClient().apply { setSelf("an-self00000001", "手机") }
            val r = peer.sendText("127.0.0.1", port, "x", "")
            assertFalse(r.ok)
            assertEquals(com.sanqiu.lanshare.core.Proto.REASON_UNREACHABLE, r.reason)
            assertEquals("http503", r.detail)
        } finally {
            server.stop()
        }
    }

    @Test
    fun `连不上时结果为不可达而不是异常`() {
        val peer = PeerClient().apply { setSelf("an-self00000001", "手机") }
        // 1 号端口不会有服务
        val r = peer.sendText("127.0.0.1", 1, "x", "")
        assertFalse(r.ok)
        assertEquals(com.sanqiu.lanshare.core.Proto.REASON_UNREACHABLE, r.reason)
    }

    @Test
    fun `peekString 容忍转义与截断`() {
        assertEquals("a", Protocol.peekString("{\"k\":\"a\"}", "k"))
        assertEquals("a\"b", Protocol.peekString("{\"k\":\"a\\\"b\"}", "k"))
        assertEquals("", Protocol.peekString("{\"other\":\"x\"}", "k"))
        assertEquals("", Protocol.peekString("{\"k\":", "k"))
    }
}
