package com.sanqiu.lanshare.net

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.Socket
import java.net.URL

/**
 * HTTP 服务端端到端断言。
 *
 * 这些用例**真的开 socket**（绑 127.0.0.1 随机端口）再发请求，
 * 不是 mock —— 粘包/半包、Content-Length、状态码这些坑只有跑真连接才现形。
 * 也就是说：P4-2 的服务端契约不必等真机就能验掉一大半。
 */
class HttpSrvTest {

    private lateinit var server: HttpServer
    private var port = 0

    @Before
    fun setUp() {
        server = HttpServer(handler = { req ->
            when {
                req.path == "/api/ping" -> HttpResponse.json("{\"ok\":true}")
                req.path == "/api/echo" -> HttpResponse.json(
                    "{\"method\":\"${req.method}\",\"peer\":\"${req.q("peer")}\"," +
                        "\"wait\":${req.qNum("wait")},\"body\":\"${req.bodyText()}\"," +
                        "\"v2\":${req.isV2()}}"
                )
                req.path == "/api/file" -> HttpResponse.raw(
                    byteArrayOf(1, 2, 3, 4), "application/octet-stream"
                )
                else -> HttpResponse.status(404, "not found")
            }
        })
        // portStart=0 => 由系统分配，避免与本机 8765 冲突
        port = server.start(host = "127.0.0.1", portStart = 0, tries = 1)
    }

    @After
    fun tearDown() {
        server.stop()
    }

    @Test
    fun `GET ping 返回 200 与 JSON`() {
        val (code, body) = get("/api/ping")
        assertEquals(200, code)
        assertEquals("{\"ok\":true}", body)
    }

    @Test
    fun `query 参数被正确解析`() {
        val (code, body) = get("/api/echo?peer=an-abc&wait=25")
        assertEquals(200, code)
        assertTrue("body=$body", body.contains("\"peer\":\"an-abc\""))
        assertTrue("body=$body", body.contains("\"wait\":25"))
    }

    @Test
    fun `POST body 按 Content-Length 完整读出`() {
        val (code, body) = post("/api/echo", "hello=1", v2 = true)
        assertEquals(200, code)
        assertTrue("body=$body", body.contains("\"method\":\"POST\""))
        assertTrue("body=$body", body.contains("\"body\":\"hello=1\""))
    }

    @Test
    fun `带 X-Device-Id 才算 v2`() {
        val (_, b1) = get("/api/echo")
        assertTrue(b1.contains("\"v2\":false"))
        val (_, b2) = post("/api/echo", "x", v2 = true)
        assertTrue(b2.contains("\"v2\":true"))
    }

    @Test
    fun `未知路径返回 404`() {
        val (code, body) = get("/nope")
        assertEquals(404, code)
        assertTrue(body.contains("\"ok\":false"))
    }

    @Test
    fun `二进制响应不被当成文本破坏`() {
        val (code, bytes) = getRaw("/api/file")
        assertEquals(200, code)
        assertEquals(4, bytes.size)
        assertTrue(bytes.contentEquals(byteArrayOf(1, 2, 3, 4)))
    }

    @Test
    fun `请求被拆成两次写入仍能正确解析（半包）`() {
        Socket("127.0.0.1", port).use { s ->
            s.soTimeout = 5000
            val out = OutputStreamWriter(s.getOutputStream(), Charsets.UTF_8)
            // 先写半截请求行 + 一个头
            out.write("GET /api/echo?peer=split HTT")
            out.flush()
            Thread.sleep(120)
            // 再写剩下的
            out.write("P/1.1\r\nHost: x\r\n\r\n")
            out.flush()

            val reader = BufferedReader(InputStreamReader(s.getInputStream(), Charsets.UTF_8))
            val statusLine = reader.readLine() ?: ""
            assertTrue("statusLine=$statusLine", statusLine.startsWith("HTTP/1.1 200"))
            val text = reader.readText()
            assertTrue("resp=$text", text.contains("\"peer\":\"split\""))
        }
    }

    @Test
    fun `两个请求连写在同一连接上时只处理第一个并关闭（无 keep-alive）`() {
        Socket("127.0.0.1", port).use { s ->
            s.soTimeout = 5000
            val out = OutputStreamWriter(s.getOutputStream(), Charsets.UTF_8)
            out.write("GET /api/ping HTTP/1.1\r\nHost: x\r\n\r\nGET /api/ping HTTP/1.1\r\nHost: x\r\n\r\n")
            out.flush()
            val reader = BufferedReader(InputStreamReader(s.getInputStream(), Charsets.UTF_8))
            val all = reader.readText()
            // 契约：每响应后关连接，所以响应体里只应出现一次 {"ok":true}
            val hits = Regex("\\{\"ok\":true\\}").findAll(all).count()
            assertEquals("响应里应只有一份 body，实际=$hits, all=$all", 1, hits)
        }
    }

    @Test
    fun `端口递进在首个端口被占时生效`() {
        val again = HttpServer({ HttpResponse.json("{}") })
        val first = again.start(host = "127.0.0.1", portStart = port, tries = 2)
        try {
            assertEquals("应递进到下一个端口", port + 1, first)
        } finally {
            again.stop()
        }
    }

    // ------------------------------------------------------------ helpers

    private fun get(path: String): Pair<Int, String> {
        val conn = open(path)
        conn.requestMethod = "GET"
        return readText(conn)
    }

    private fun post(path: String, body: String, v2: Boolean): Pair<Int, String> {
        val conn = open(path)
        conn.requestMethod = "POST"
        if (v2) conn.setRequestProperty("X-Device-Id", "an-test")
        conn.doOutput = true
        conn.setRequestProperty("Content-Type", "text/plain; charset=utf-8")
        val bytes = body.toByteArray(Charsets.UTF_8)
        conn.setFixedLengthStreamingMode(bytes.size)
        conn.outputStream.use { it.write(bytes) }
        return readText(conn)
    }

    private fun getRaw(path: String): Pair<Int, ByteArray> {
        val conn = open(path)
        conn.requestMethod = "GET"
        val code = conn.responseCode
        val bytes = conn.inputStream.use { it.readBytes() }
        return code to bytes
    }

    private fun open(path: String): HttpURLConnection {
        val url = URL("http://127.0.0.1:$port$path")
        return (url.openConnection() as HttpURLConnection).apply {
            connectTimeout = 3000
            readTimeout = 5000
            instanceFollowRedirects = false
        }
    }

    private fun readText(conn: HttpURLConnection): Pair<Int, String> {
        val code = conn.responseCode
        val stream = if (code >= 400) conn.errorStream else conn.inputStream
        val text = stream?.use { it.readBytes().toString(Charsets.UTF_8) } ?: ""
        return code to text
    }
}
