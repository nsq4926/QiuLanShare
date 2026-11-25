package com.sanqiu.lanshare.net

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 网卡筛选与广播地址断言。
 *
 * 用例里的数据**取自 MatePad 11 的实测 dump**（不是编的）：
 *   wlan1  192.168.8.64/24   bcast 192.168.8.255
 *   virnic 10.213.168.4/32   无广播地址（华为虚拟网卡，isVirtual 返回 false）
 *   wlan0  192.168.8.11/24   bcast 192.168.8.255
 * 这就是 P4-3 的真实输入：过滤错一条，对端就会拿到连不上的地址。
 */
class NetTest {

    private val matePadDump = listOf(
        Nic("192.168.8.64", 24, "wlan1"),
        Nic("10.213.168.4", 32, "virnic"),
        Nic("192.168.8.11", 24, "wlan0"),
    )

    @Test
    fun `子网广播地址按前缀计算`() {
        assertEquals("192.168.8.255", subnetBroadcast("192.168.8.11", 24))
        assertEquals("192.168.8.127", subnetBroadcast("192.168.8.11", 25))
        assertEquals("192.168.8.3", subnetBroadcast("192.168.8.1", 30))
    }

    @Test
    fun `点对点与单地址没有广播地址`() {
        // /31 与 /32 必须返回空串：靠公式兜底会算出不存在的地址，发出去没人收且不报错
        assertEquals("", subnetBroadcast("192.168.8.11", 31))
        assertEquals("", subnetBroadcast("10.213.168.4", 32))
    }

    @Test
    fun `不可用地址一律返回空`() {
        assertEquals("", subnetBroadcast("127.0.0.1", 8))
        assertEquals("", subnetBroadcast("169.254.1.1", 16))
        assertEquals("", subnetBroadcast("", 24))
        assertEquals("", subnetBroadcast("abc", 24))
    }

    @Test
    fun `可用IPv4判定`() {
        assertTrue(isUsableIPv4("192.168.8.11"))
        assertTrue(isUsableIPv4("10.0.0.1"))
        assertTrue(!isUsableIPv4("0.0.0.0"))
        assertTrue(!isUsableIPv4("127.0.0.1"))
        assertTrue(!isUsableIPv4("169.254.3.4"))
        assertTrue(!isUsableIPv4("192.168.8"))
        assertTrue(!isUsableIPv4("192.168.8.256"))
    }

    @Test
    fun `同网段比较按前缀逐位`() {
        assertTrue(sameSubnet("192.168.8.11", "192.168.8.64", 24))
        assertTrue(!sameSubnet("192.168.8.11", "192.168.9.11", 24))
        // /25 把 192.168.8.0 与 192.168.8.128 划到两个网段
        assertTrue(!sameSubnet("192.168.8.11", "192.168.8.200", 25))
    }

    @Test
    fun `virnic 被排除 不进入 ips`() {
        val info = pickNics(matePadDump)
        val ips = info.nics.map { it.ip }
        assertEquals(2, info.nics.size)
        assertTrue("不应包含 virnic 的地址: $ips", ips.none { it.startsWith("10.213.") })
        assertTrue(ips.contains("192.168.8.11"))
        assertTrue(ips.contains("192.168.8.64"))
    }

    @Test
    fun `默认网卡优先成为 primaryIp`() {
        val info = pickNics(matePadDump, preferredIfName = "wlan0")
        assertEquals("192.168.8.11", info.primaryIp())

        val info2 = pickNics(matePadDump, preferredIfName = "wlan1")
        assertEquals("192.168.8.64", info2.primaryIp())
    }

    @Test
    fun `只有不可广播的网卡时返回空`() {
        val info = pickNics(listOf(Nic("10.213.168.4", 32, "virnic")))
        assertEquals(0, info.nics.size)
        assertEquals("", info.primaryIp())
    }

    @Test
    fun `可广播的网卡全被名字规则排除时放宽`() {
        // 护栏：宁可放宽名字过滤，也不能把唯一可用的局域网口丢掉
        val info = pickNics(listOf(Nic("192.168.5.9", 24, "vpn0")))
        assertEquals(1, info.nics.size)
        assertEquals("192.168.5.9", info.primaryIp())
    }

    @Test
    fun `广播目标包含受限广播且去重`() {
        val targets = broadcastTargets(pickNics(matePadDump).nics)
        assertTrue(targets.contains("255.255.255.255"))
        assertTrue(targets.contains("192.168.8.255"))
        // 两张网卡广播地址相同 => 只出现一次
        assertEquals(2, targets.size)
    }

    @Test
    fun `无广播地址的网卡不贡献广播目标`() {
        val targets = broadcastTargets(listOf(Nic("10.213.168.4", 32, "virnic")))
        assertEquals(listOf("255.255.255.255"), targets)
    }
}
