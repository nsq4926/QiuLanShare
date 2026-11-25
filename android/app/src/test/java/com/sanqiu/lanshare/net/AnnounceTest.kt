package com.sanqiu.lanshare.net

import com.sanqiu.lanshare.core.LsSelf
import com.sanqiu.lanshare.core.Proto
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * announce 报文契约断言。
 *
 * 这段是本项目历史 bug #1 的回归护栏：**非法 JSON 的症状是"对端静默看不见我"**，
 * 不报错、不抛异常，接口还回 200。所以报文必须既"能拼出来"也"能被自己解析回去"。
 */
class AnnounceTest {

    private fun self() = LsSelf(
        deviceId = "an-0123456789ab",
        name = "OPPO A57",
        mac = "",
        ips = mutableListOf("192.168.8.23"),
        filteredNicCount = 1,
        keptNicCount = 1,
        epoch = 1,
        seq = 0,
    )

    @Test
    fun `报文是合法 JSON 且字段齐全`() {
        val text = Announce.build(self(), port = 8765, acceptsInbound = true)
        val obj = JSONObject(text) // 解析不了会直接抛，等于断言"合法"

        val required = listOf(
            "v", "app", "deviceId", "name", "port", "mac", "ips",
            "acceptsInbound", "filteredNicCount", "apiVersion", "platform",
        )
        val keys = obj.keys().asSequence().toList()
        for (k in required) {
            assertTrue("缺字段 $k，实际=$keys", keys.contains(k))
        }
    }

    @Test
    fun `关键取值与协议一致`() {
        val obj = JSONObject(Announce.build(self(), port = 8766, acceptsInbound = false))
        assertEquals(2, obj.getInt("v"))
        assertEquals("TMLS", obj.getString("app"))
        assertEquals("an-0123456789ab", obj.getString("deviceId"))
        assertEquals(8766, obj.getInt("port"))
        assertEquals("2", obj.getString("apiVersion"))
        assertEquals("android", obj.getString("platform"))
        assertEquals(false, obj.getBoolean("acceptsInbound"))
        assertEquals("192.168.8.23", obj.getJSONArray("ips").getString(0))
    }

    @Test
    fun `设备名含引号与反斜杠时仍是合法 JSON`() {
        val s = self().copy(name = "a\"b\\c")
        val text = Announce.build(s, port = 8765, acceptsInbound = true)
        val obj = JSONObject(text)
        assertEquals("a\"b\\c", obj.getString("name"))
    }

    @Test
    fun `ips 为空数组也合法`() {
        val s = self().copy(ips = mutableListOf())
        val obj = JSONObject(Announce.build(s, port = 8765, acceptsInbound = true))
        assertEquals(0, obj.getJSONArray("ips").length())
    }

    @Test
    fun `parse 接受自己的报文`() {
        val text = Announce.build(self(), port = 8765, acceptsInbound = true)
        val parsed = Announce.parse(text)
        assertNotNull(parsed)
        assertEquals("an-0123456789ab", Proto.jStr(parsed, "deviceId"))
    }

    @Test
    fun `parse 拒绝其它 app 的广播`() {
        assertNull(Announce.parse("{\"app\":\"OTHER\",\"deviceId\":\"x\"}"))
    }

    @Test
    fun `parse 拒绝缺 deviceId 的报文`() {
        assertNull(Announce.parse("{\"app\":\"TMLS\",\"name\":\"x\"}"))
    }

    @Test
    fun `parse 拒绝非 JSON 与超长报文`() {
        assertNull(Announce.parse("not json"))
        assertNull(Announce.parse(""))
        assertNull(Announce.parse(null))
        assertNull(Announce.parse("{\"app\":\"TMLS\",\"deviceId\":\"x\"," + "\"pad\":\"".padEnd(5000, 'a') + "\"}"))
    }
}
