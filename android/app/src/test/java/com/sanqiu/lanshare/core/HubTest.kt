package com.sanqiu.lanshare.core

import com.sanqiu.lanshare.net.isUsableIPv4
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.file.Files

/**
 * Hub 状态机断言。
 *
 * 每条都对应一个**真实会静默出错**的语义，而不是"覆盖率装饰"：
 *  - 五态算错 → 界面显示"在线"其实连不上；
 *  - convId/peer 记错 → 点会话点不到人；
 *  - TTL 用错地方 → 设备离线 5 分钟后再也发不出去（且不报错）；
 *  - 清空不 bump epoch → 重装/重启后旧 msgId 与新消息撞号，对方当成重复丢弃。
 */
class HubTest {

    private lateinit var dir: java.io.File

    private fun newHub(): LsHub {
        dir = Files.createTempDirectory("lanshare-hub").toFile()
        val hub = LsHub(LsStore(dir))
        hub.load()
        hub.ensureDeviceId()
        hub.setDeviceName("测试机")
        return hub
    }

    private fun announce(
        id: String,
        name: String = "对端",
        ips: List<String> = listOf("192.168.8.50"),
        port: Int = 8765,
        accepts: Boolean = true,
        mac: String = "",
        platform: String = "android",
    ): JSONObject = JSONObject().apply {
        put("v", 2)
        put("app", "TMLS")
        put("deviceId", id)
        put("name", name)
        put("ips", JSONArray(ips))
        put("port", port)
        put("acceptsInbound", accepts)
        put("platform", platform)
        if (mac.isNotEmpty()) put("mac", mac)
    }

    // ------------------------------------------------------------ 身份

    @Test
    fun `deviceId 生成带 an- 前缀且持久化不变`() {
        val hub = newHub()
        val id = hub.self.deviceId
        assertTrue("实际=$id", id.startsWith("an-"))
        assertEquals("an-".length + 12, id.length)

        // 重新 load 同一个目录：必须还是同一个 id（否则对端眼里是"来了台新设备"）
        val again = LsHub(LsStore(dir))
        again.load()
        assertEquals(id, again.self.deviceId)
    }

    @Test
    fun `seq 全局单调 且 msgId 形如 deviceId epoch seq`() {
        val hub = newHub()
        val a = hub.nextSeq()
        val b = hub.nextSeq()
        assertEquals(a + 1, b)
        val msgId = hub.newMsgId()
        val parts = msgId.split(":")
        assertEquals(3, parts.size)
        assertEquals(hub.self.deviceId, parts[0])
        assertEquals(hub.self.epoch.toString(), parts[1])
        assertEquals(hub.self.seq.toString(), parts[2])
    }

    // ------------------------------------------------------------ announce 与五态

    @Test
    fun `announce 只登记为已发现 不冒充在线`() {
        val hub = newHub()
        hub.applyAnnounce(announce("an-peer00000001"), "192.168.8.50")
        val p = hub.findPeer("an-peer00000001")
        assertNotNull(p)
        // 收到广播只证明"存在"，没证明"能连" —— 状态必须是 discovered
        val st = Proto.stateOf(hub.reachableAt(p!!.deviceId), p.acceptsInbound, p.lastSeen, Proto.nowMs())
        assertEquals(Proto.ST_DISCOVERED, st)
    }

    @Test
    fun `touchPeer 之后才是在线`() {
        val hub = newHub()
        hub.applyAnnounce(announce("an-peer00000002"), "192.168.8.50")
        hub.touchPeer("an-peer00000002", "192.168.8.50", 8765)
        val p = hub.findPeer("an-peer00000002")!!
        val st = Proto.stateOf(hub.reachableAt(p.deviceId), p.acceptsInbound, p.lastSeen, Proto.nowMs())
        assertEquals(Proto.ST_ONLINE, st)
        assertEquals(8765, p.port)
    }

    @Test
    fun `不接受入站的对端是在线但不接收`() {
        val hub = newHub()
        hub.applyAnnounce(announce("an-peer00000003", accepts = false), "192.168.8.50")
        hub.touchPeer("an-peer00000003", "192.168.8.50", 8765)
        val p = hub.findPeer("an-peer00000003")!!
        val st = Proto.stateOf(hub.reachableAt(p.deviceId), p.acceptsInbound, p.lastSeen, Proto.nowMs())
        assertEquals(Proto.ST_ONLINE_NO_INBOUND, st)
    }

    @Test
    fun `忽略自己的 announce`() {
        val hub = newHub()
        hub.applyAnnounce(announce(hub.self.deviceId), "192.168.8.9")
        assertNull(hub.findPeer(hub.self.deviceId))
        assertEquals(0, hub.peers.size)
    }

    @Test
    fun `包内地址都不可用时退回源地址`() {
        val hub = newHub()
        // 169.254 是链路本地，不能作为可连地址；应退回广播源地址
        hub.applyAnnounce(announce("an-peer00000004", ips = listOf("169.254.1.1")), "192.168.8.77")
        val p = hub.findPeer("an-peer00000004")!!
        assertEquals(1, p.ips.size)
        assertEquals("192.168.8.77", p.ips[0].ip)
    }

    @Test
    fun `包内与源地址都不可用时不登记新设备`() {
        val hub = newHub()
        hub.applyAnnounce(announce("an-peer00000005", ips = listOf("127.0.0.1")), "0.0.0.0")
        assertNull(hub.findPeer("an-peer00000005"))
    }

    @Test
    fun `删除后有安静期 不再被自动发现加回`() {
        val hub = newHub()
        hub.applyAnnounce(announce("an-peer00000006"), "192.168.8.50")
        hub.dropPeer("an-peer00000006")
        hub.suppressDiscovery("an-peer00000006", 60_000)
        hub.applyAnnounce(announce("an-peer00000006"), "192.168.8.50")
        assertNull(hub.findPeer("an-peer00000006"))
    }

    // ------------------------------------------------------------ 消息与会话

    private fun hubWithPeer(): Triple<LsHub, String, String> {
        val hub = newHub()
        val peerId = "an-peer00000009"
        hub.applyAnnounce(announce(peerId, name = "小手机"), "192.168.8.50")
        hub.touchPeer(peerId, "192.168.8.50", 8765)
        val convId = Proto.makeConvId(hub.self.deviceId, peerId)
        return Triple(hub, peerId, convId)
    }

    @Test
    fun `出站消息把对端登记为会话 peer`() {
        val (hub, peerId, convId) = hubWithPeer()
        val msgId = hub.newMsgId()
        val m = LsMsg(
            id = hub.peekNextId(), dir = Proto.DIR_OUT, text = "hi",
            msgId = msgId, convId = convId, from = hub.self.deviceId, to = peerId,
            epoch = hub.self.epoch, seq = hub.self.seq, time = Proto.nowMs(), dev = "小手机",
        )
        hub.appendMessage(m)

        val info = hub.convs[convId]
        assertNotNull(info)
        // 写成 m.from 会把本机登记成对端，界面点进去找不到人
        assertEquals(peerId, info!!.peerDeviceId)
        assertEquals(0L, hub.totalUnread())
    }

    @Test
    fun `入站消息计入未读 markRead 清零`() {
        val (hub, peerId, convId) = hubWithPeer()
        for (i in 1..2) {
            hub.appendMessage(
                LsMsg(
                    id = hub.peekNextId(), dir = Proto.DIR_IN, text = "in$i",
                    msgId = "peer:1:$i", convId = convId, from = peerId, to = hub.self.deviceId,
                    epoch = 1, seq = i.toLong(), time = Proto.nowMs(), unread = true,
                )
            )
        }
        assertEquals(2L, hub.totalUnread())
        assertEquals(2L, hub.convStat(convId).unread)

        assertEquals(2L, hub.markRead(convId))
        assertEquals(0L, hub.totalUnread())
    }

    @Test
    fun `系统消息不计入未读`() {
        val (hub, peerId, convId) = hubWithPeer()
        hub.appendMessage(
            LsMsg(
                id = hub.peekNextId(), dir = Proto.DIR_IN, kind = Proto.KIND_SYSTEM, text = "系统",
                convId = convId, from = peerId, to = hub.self.deviceId, time = Proto.nowMs(), unread = true,
            )
        )
        assertEquals(0L, hub.totalUnread())
    }

    @Test
    fun `清空会话递增 histRev 与 epoch`() {
        val (hub, peerId, convId) = hubWithPeer()
        hub.appendMessage(
            LsMsg(
                id = hub.peekNextId(), dir = Proto.DIR_IN, text = "x",
                convId = convId, from = peerId, to = hub.self.deviceId, time = Proto.nowMs(), unread = true,
            )
        )
        val rev0 = hub.histRev(convId)
        val epoch0 = hub.self.epoch
        val removed = hub.clearConversation(convId)
        assertEquals(1L, removed)
        assertEquals(rev0 + 1, hub.histRev(convId))
        assertEquals(epoch0 + 1, hub.self.epoch)
        assertTrue(hub.messages.isEmpty())
    }

    @Test
    fun `queryMessages 的 since 与 epoch seq 水位`() {
        val (hub, peerId, convId) = hubWithPeer()
        for (i in 1..3) {
            hub.appendMessage(
                LsMsg(
                    id = hub.peekNextId(), dir = Proto.DIR_IN, text = "m$i",
                    msgId = "p:1:$i", convId = convId, from = peerId, to = hub.self.deviceId,
                    epoch = 1, seq = i.toLong(), peerTs = i.toLong(), time = Proto.nowMs(),
                )
            )
        }
        // 全局 id 水位
        val all = hub.queryMessages(peerId, -1, -1, 0, false)
        assertEquals(3, all.size)
        val afterFirst = hub.queryMessages(peerId, -1, -1, all[0].id, false)
        assertEquals(2, afterFirst.size)

        // per-peer (epoch,seq) 水位：只要 seq > 1 的
        val afterSeq1 = hub.queryMessages(peerId, 1, 1, 0, false)
        assertEquals(2, afterSeq1.size)
        assertEquals(2L, afterSeq1[0].seq)
    }

    @Test
    fun `purgeBefore 裁旧消息并清掉空壳会话`() {
        val (hub, peerId, convId) = hubWithPeer()
        val old = Proto.nowMs() - 40L * 24 * 3600 * 1000
        hub.appendMessage(
            LsMsg(
                id = hub.peekNextId(), dir = Proto.DIR_IN, text = "旧",
                convId = convId, from = peerId, to = hub.self.deviceId, time = old,
            )
        )
        val removed = hub.purgeBefore(Proto.nowMs() - 30L * 24 * 3600 * 1000)
        assertEquals(1L, removed)
        assertTrue(hub.messages.isEmpty())
        assertNull("只剩空壳的会话应被清掉", hub.convs[convId])
    }

    // ------------------------------------------------------------ TTL 铁律

    @Test
    fun `displayIps 剔过期地址 但 sendCandidates 不剔`() {
        val (hub, peerId, _) = hubWithPeer()
        val p = hub.findPeer(peerId)!!
        // 该地址 10 分钟前成功过（IP_FRESH_MS 是 5 分钟）
        p.ips[0].lastOkAt = Proto.nowMs() - 10L * 60 * 1000

        assertEquals("展示要剔掉过期地址", 0, hub.displayIps(p).size)
        assertEquals("发送候选必须保留，否则离线 5 分钟就再也发不出去", 1, hub.sendCandidates(p).size)
    }

    @Test
    fun `刚发现的地址 lastOkAt 为 0 时不会被 TTL 剔掉`() {
        // 注意：不能先 touchPeer —— 那会把 lastOkAt 写成当前时刻，就不是"刚发现"了
        val hub = newHub()
        val peerId = "an-peer00000030"
        hub.applyAnnounce(announce(peerId, ips = listOf("192.168.8.51")), "192.168.8.51")
        val p = hub.findPeer(peerId)!!
        assertEquals(0L, p.ips[0].lastOkAt)
        assertEquals("刚发现的设备 ips 不能是空的", 1, hub.displayIps(p).size)
    }

    // ------------------------------------------------------------ 落盘

    @Test
    fun `重启后设备与历史都还在 且未读清零`() {
        val (hub, peerId, convId) = hubWithPeer()
        hub.appendMessage(
            LsMsg(
                id = hub.peekNextId(), dir = Proto.DIR_IN, text = "记住我",
                convId = convId, from = peerId, to = hub.self.deviceId, time = Proto.nowMs(), unread = true,
            )
        )
        val again = LsHub(LsStore(dir))
        again.load()
        assertNotNull(again.findPeer(peerId))
        assertEquals(1, again.messages.size)
        assertEquals("记住我", again.messages[0].text)
        // 历史行一律视为已读，避免重启后未读爆表
        assertEquals(0L, again.totalUnread())
    }

    @Test
    fun `同 MAC 的旧身份被合并 但在线的不动`() {
        val hub = newHub()
        val mac = "AA:BB:CC:DD:EE:FF"
        hub.applyAnnounce(announce("an-old000000001", ips = listOf("192.168.8.60"), mac = mac), "192.168.8.60")
        // 旧身份离线（lastSeen 很早）
        hub.findPeer("an-old000000001")!!.apply { lastSeen = Proto.nowMs() - 10L * 60 * 1000 }

        hub.applyAnnounce(announce("an-new000000001", ips = listOf("192.168.8.61"), mac = mac), "192.168.8.61")
        assertNull("离线的旧身份应被合并掉", hub.findPeer("an-old000000001"))
        assertNotNull(hub.findPeer("an-new000000001"))

        // 在线的不动
        hub.touchPeer("an-new000000001", "192.168.8.61", 8765)
        hub.applyAnnounce(announce("an-new000000002", ips = listOf("192.168.8.62"), mac = mac), "192.168.8.62")
        assertNotNull("在线身份不应被按 MAC 合并", hub.findPeer("an-new000000001"))
    }

    @Test
    fun `同网段地址在展示排序里更靠前`() {
        val hub = newHub()
        val peerId = "an-peer00000020"
        hub.applyAnnounce(
            announce(peerId, ips = listOf("10.9.9.9", "192.168.8.50")), "192.168.8.50"
        )
        val p = hub.findPeer(peerId)!!
        // 让本机 net 与 192.168.8.x 同网段（不依赖真实网卡：直接构造）
        hub.net = com.sanqiu.lanshare.net.NetInfo(
            nics = listOf(com.sanqiu.lanshare.net.Nic("192.168.8.5", 24, "wlan0")),
            filteredNicCount = 0, keptNicCount = 1,
        )
        val ordered = hub.displayIps(p)
        assertEquals("192.168.8.50", ordered[0].ip)
    }
}
