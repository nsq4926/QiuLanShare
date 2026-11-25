package com.sanqiu.lanshare.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 协议纯函数断言。
 *
 * 这里每一条都对应一个**真实发生过的故障形态**，不是"覆盖率装饰"：
 * convId 算错 → 同一对设备裂成两个会话、消息偶尔不显示、不报错。
 */
class ProtoTest {

    @Test
    fun `fnv1a64 三个向量与 Windows 端逐位一致`() {
        // offset basis 是 1469598103934665603（少一位），三个向量由 Windows 端算出
        assertEquals("14650fb0739d0383", Proto.fnv1a64Hex(Proto.utf8Bytes("")))
        assertEquals("44bd8ad473cd9906", Proto.fnv1a64Hex(Proto.utf8Bytes("a")))
        assertEquals("88fad7c0a8ff07f2", Proto.fnv1a64Hex(Proto.utf8Bytes("foobar")))
    }

    @Test
    fun `convId 自检无问题`() {
        val problems = Proto.convIdSelfCheck()
        assertTrue("自检失败: $problems", problems.isEmpty())
    }

    @Test
    fun `convId 与调用顺序无关`() {
        assertEquals(
            Proto.makeConvId("pc-7fca9eb2bf2b", "hm-ce6e76a67aa3"),
            Proto.makeConvId("hm-ce6e76a67aa3", "pc-7fca9eb2bf2b"),
        )
    }

    @Test
    fun `convId 前缀固定为 c- 且长度固定`() {
        val id = Proto.makeConvId("an-000000000001", "pc-000000000002")
        assertTrue(id.startsWith("c-"))
        assertEquals(2 + 16, id.length)
    }

    @Test
    fun `msgId 格式为 deviceId epoch seq`() {
        assertEquals("an-abc:3:118", Proto.makeMsgId("an-abc", 3, 118))
    }

    @Test
    fun `legacy 键幂等`() {
        assertEquals("legacy:unknown", Proto.makeLegacyKey(""))
        assertEquals("legacy:x", Proto.makeLegacyKey("x"))
        assertEquals("legacy:x", Proto.makeLegacyKey("legacy:x"))
    }

    @Test
    fun `五态边界值与 Windows 端一致`() {
        val now = 1_000_000L
        // 在线
        assertEquals(Proto.ST_ONLINE, Proto.stateOf(now - 1_000, true, now - 1_000, now))
        // 在线但不接收
        assertEquals(Proto.ST_ONLINE_NO_INBOUND, Proto.stateOf(now - 1_000, false, now - 1_000, now))
        // 曾经可达但已超 ONLINE_TTL -> 暂时连不上
        assertEquals(Proto.ST_UNREACHABLE, Proto.stateOf(now - 40_000, true, now - 40_000, now))
        // 发现过但从未可达 -> 已发现
        assertEquals(Proto.ST_DISCOVERED, Proto.stateOf(0, true, now - 40_000, now))
        // 超出 STALE_TTL -> 离线
        assertEquals(Proto.ST_OFFLINE, Proto.stateOf(0, true, now - 200_000, now))
    }

    @Test
    fun `ONLINE_TTL 必须小于 STALE_TTL`() {
        // 相等会让 unreachable 恒假（docs/13 对齐清单第 14 条）
        assertTrue(Proto.ONLINE_TTL < Proto.STALE_TTL)
    }

    @Test
    fun `query 解析支持无等号与空值`() {
        val m = Proto.parseQuery("peer=an-1&wait=25&flag&empty=")
        assertEquals("an-1", m["peer"])
        assertEquals("25", m["wait"])
        assertEquals("", m["flag"])
        assertEquals("", m["empty"])
    }

    @Test
    fun `pctEncode 保留 unreserved 并编码中文`() {
        assertEquals("abc-._~", Proto.pctEncode("abc-._~"))
        assertEquals("%E4%B8%AD", Proto.pctEncode("中"))
    }

    @Test
    fun `jsonStr 转义引号反斜杠与控制字符`() {
        assertEquals("\"a\\\"b\"", Proto.jsonStr("a\"b"))
        assertEquals("\"a\\\\b\"", Proto.jsonStr("a\\b"))
        assertEquals("\"a\\nb\"", Proto.jsonStr("a\nb"))
        // 非 ASCII 原样透传（与 Windows 端 JsonEscape 同口径）
        assertEquals("\"中文\"", Proto.jsonStr("中文"))
    }

    @Test
    fun `randomHex 长度与字符集`() {
        val h = Proto.randomHex(12)
        assertEquals(12, h.length)
        assertTrue(h.all { it in '0'..'9' || it in 'a'..'f' })
    }
}
