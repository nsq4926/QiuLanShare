package com.sanqiu.lanshare.net

import com.sanqiu.lanshare.api.LsApi
import com.sanqiu.lanshare.core.LsHub
import com.sanqiu.lanshare.core.LsStore
import com.sanqiu.lanshare.core.Proto
import com.sanqiu.lanshare.core.retentionCutoff

/**
 * 对等节点门面：把「服务端 + 出站客户端 + 存储 + 发现」装配成一个可启动/可停止的整体。
 *
 * 到这里 Android 端与 Windows/鸿蒙同构：既能当服务端被连，也能主动连别人。
 *
 * 启动顺序是有讲究的 —— 先把身份和设置读出来，再开端口，最后才广播：
 * 端口号要进 announce（`port` 字段），抢在绑定前广播出去会让对端连到旧端口。
 */
class LsNode(private val store: LsStore) {

    val hub = LsHub(store)
    val peer = PeerClient()
    val api = LsApi(hub, peer)
    val server = HttpServer({ req -> api.handle(req) })

    private var disc: Discovery? = null
    private var probeThread: Thread? = null
    private var notifyThread: Thread? = null

    /** 发现层（UI 显示广播/收包计数用；未启动时为 null） */
    val discovery: Discovery? get() = disc

    @Volatile var started: Boolean = false
        private set

    /** 实际监听端口（递进后的真值） */
    @Volatile var port: Int = 0
        private set

    /** 启动自检的告警（非空不代表启动失败，但必须显示出来） */
    @Volatile var bootWarnings: List<String> = emptyList()
        private set

    @Volatile var lastError: String = ""
        private set

    /**
     * 启动节点。返回自检问题列表（空 = 全绿）。
     *
     * 自检是**体检不是门禁**：convId 算法不一致会导致"同一对设备裂两个会话"，
     * 但让诊断逻辑自己把服务搞挂，比它要检出的问题还难查。
     */
    fun start(): List<String> {
        if (started) return bootWarnings

        val warnings = mutableListOf<String>()
        warnings += Proto.convIdSelfCheck()

        hub.load()
        // 传输记录也要跨重启保留（用户要求：完成后列表不要消失）
        try {
            Transfers.load(store)
        } catch (e: Exception) {
            android.util.Log.w("LanShare", "load transfers failed: ${e.message}")
        }
        hub.ensureDeviceId()
        hub.loadSettings()
        hub.refreshNet()
        val merged = hub.dedupeByMac()
        if (merged > 0) warnings += "启动时按 MAC 合并了 $merged 个旧身份"

        // 保留期裁剪（按天）：启动跑一次即可，运行中不追着删
        val cutoff = retentionCutoff(hub.settings, Proto.nowMs())
        if (cutoff > 0) {
            val purged = hub.purgeBefore(cutoff)
            if (purged > 0) warnings += "按保留期清理了 $purged 条历史"
        }

        peer.setSelf(hub.self.deviceId, hub.self.name)
        api.historyDays = when (hub.settings.retentionMode) {
            com.sanqiu.lanshare.core.LsSettings.RET_DAYS -> hub.settings.retentionDays
            else -> 0
        }

        // 端口递进：8765 起试 3 个；全占则抛错（不允许静默监听随机端口）
        port = server.start("0.0.0.0", Proto.HTTP_PORT_BASE, Proto.HTTP_PORT_TRIES)
        api.port = port

        val d = Discovery(
            payloadProvider = { Announce.build(hub.self, port, hub.acceptsInbound) },
            selfDeviceIdProvider = { hub.self.deviceId },
            nicsProvider = { hub.net.nics },
            onAnnounce = { obj, ip ->
                hub.applyAnnounce(obj, ip)
                api.notifyChanged()
            },
        )
        d.start()
        disc = d
        applyDiscoveryOptions()

        // 通知：入站消息 → 可点进会话的通知；传输 → 单条常驻通知实时刷新
        api.onInbound = { convId, peerId, dev, preview ->
            val unread = synchronized(hub.gate) { hub.convStat(convId).unread }
            LsNotify.publishMessage(convId, peerId, dev, preview, unread)
        }
        startNotifyLoop()

        startProbeLoop()

        started = true
        bootWarnings = warnings
        if (warnings.isNotEmpty()) {
            lastError = warnings.joinToString("; ")
        }
        return warnings
    }

    fun stop() {
        started = false
        runCatching { disc?.stop() }
        disc = null
        probeThread?.interrupt()
        probeThread = null
        notifyThread?.interrupt()
        notifyThread = null
        runCatching { server.stop() }
        // 停服务前把还没落盘的传输记录写下去
        runCatching { Transfers.saveIfDirty(store) }
    }

    /** 下滑刷新 / 立即搜索：刷新网卡 + 立刻广播 */
    fun discoverNow() {
        hub.refreshNet()
        disc?.announceNow()
        api.notifyChanged()
    }

    /** 把设置里的发现策略推给发现层（唯一生效点） */
    fun applyDiscoveryOptions() {
        val s = hub.settings
        disc?.setOptions(s.activeDiscover, hub.acceptsInbound, s.discoverSeconds * 1000L)
    }

    /** 被发现（关掉后仍监听，只是 announce 里如实报 false、也不再自报家门） */
    fun setAcceptsInbound(accepts: Boolean) {
        hub.acceptsInbound = accepts
        hub.settings.acceptsInbound = accepts
        // 必须落盘：以前只改内存，重启又变回"接收" —— 用户以为开关失效
        hub.saveSettings()
        applyDiscoveryOptions()
        api.notifyChanged()
    }

    fun setActiveDiscover(on: Boolean) {
        hub.settings.activeDiscover = on
        hub.saveSettings()
        applyDiscoveryOptions()
        if (on) discoverNow()
        api.notifyChanged()
    }

    fun setDiscoverSeconds(sec: Int) {
        val v = sec.coerceIn(3, 300)
        hub.settings.discoverSeconds = v
        hub.saveSettings()
        applyDiscoveryOptions()
        api.notifyChanged()
    }

    fun setForeground(fg: Boolean) {
        LsNotify.foreground = fg
    }

    fun setFocusedConv(convId: String) {
        LsNotify.focusedConv = convId
    }

    /** 传输进度通知：2 秒刷一次（与应用是否在前台无关） */
    private fun startNotifyLoop() {
        val t = Thread({
            while (started) {
                try {
                    Thread.sleep(2000)
                } catch (e: InterruptedException) {
                    break
                }
                if (!started) break
                // 状态变化才落盘：进度更新不写文件（每片写一次太浪费）
                Transfers.saveIfDirty(store)
                if (Transfers.hasAny()) {
                    Transfers.prune()
                    // 传输列表现在保留历史：通知栏只跟"在传"的数量走，
                    // 否则一次传输之后，那条常驻通知会永远消不掉
                    LsNotify.updateTransfer()
                    api.notifyChanged()
                }
            }
        }, "lanshare-notify")
        t.isDaemon = true
        notifyThread = t
        t.start()
    }

    /**
     * 健康探测：每 5 秒对"最近还活跃"的对端探一次。
     *
     * 只探 `lastSeen` 在 3×STALE_TTL 内的设备 —— 离线几天的设备反复探测既费电，
     * 也会把"早就不在网里"的设备状态刷成不可达，反而看不出它是真的走了。
     * 探测成功才 `touchPeer`（写 lastOkAt + markReachable），失败什么都不做：
     * 状态由 `lastSeen` 自然衰减，不需要"失败就标离线"这种额外状态。
     */
    private fun startProbeLoop() {
        val t = Thread({
            while (started) {
                try {
                    Thread.sleep(Proto.PROBE_INTERVAL_MS)
                } catch (e: InterruptedException) {
                    break
                }
                if (!started) break
                runCatching { probeOnce() }
            }
        }, "lanshare-probe")
        t.isDaemon = true
        probeThread = t
        t.start()
    }

    private fun probeOnce() {
        val now = Proto.nowMs()
        val snapshot = hub.peers.values.toList()
        for (p in snapshot) {
            if (p.ephemeral) continue
            if (now - p.lastSeen > Proto.STALE_TTL * 3) continue
            val ips = hub.sendCandidates(p)
            if (ips.isEmpty()) continue
            val portNum = if (p.port > 0) p.port else Proto.HTTP_PORT_BASE
            for (ip in ips.take(2)) {
                val id = peer.probeHello(ip.ip, portNum, 1200, 1500)
                if (id == p.deviceId) {
                    hub.touchPeer(p.deviceId, ip.ip, portNum)
                    api.notifyChanged()
                    break
                }
            }
        }
    }
}
