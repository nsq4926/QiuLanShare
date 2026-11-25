package com.sanqiu.lanshare.net

import android.util.Log
import com.sanqiu.lanshare.core.Proto
import org.json.JSONObject
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit

/**
 * UDP 发现：绑定 8766，周期广播 announce，收包后回调上层。
 *
 * 与鸿蒙（`common/Disc.ets`）同构，关键行为一条不改：
 *  - 绑定失败**必须重试**（3 秒）。以前只试一次就永久放弃，表现是
 *    "开机后一直发现不到设备，重启应用才好"——端口被上一轮进程占着、网络未就绪都会吃掉这一次失败。
 *  - 收到 announce **单播回源一发**，2 秒限流：不回包就要等对端自己下一轮 3 秒广播，
 *    广播一丢用户就感觉"发现慢"；不加限流则两边互回打转。
 *  - 忽略自己发的包（`deviceId` 相同）。
 *
 * 出站报文由 [payloadProvider] 提供（上层持有 port / ips / acceptsInbound）。
 */
class Discovery(
    private val payloadProvider: () -> String,
    private val selfDeviceIdProvider: () -> String,
    private val nicsProvider: () -> List<Nic>,
    private val onAnnounce: (JSONObject, String) -> Unit,
) {
    @Volatile private var socket: DatagramSocket? = null
    @Volatile private var receiveThread: Thread? = null
    @Volatile var running: Boolean = false
        private set
    @Volatile var lastError: String = ""
        private set
    @Volatile var announceCount: Int = 0
        private set
    @Volatile var receivedCount: Int = 0
        private set

    private var scheduler: ScheduledExecutorService? = null
    private var lastShotAt = 0L
    private var lastAnnounceAt = 0L
    private var retryScheduled = false

    /** 主动发现：周期广播 + 登记别人 */
    @Volatile private var active = true
    /** 被发现：广播自己 + 回应别人（关掉后对端找不到本机） */
    @Volatile private var discoverable = true
    @Volatile private var intervalMs = Proto.ANNOUNCE_INTERVAL_MS.toLong()

    fun start() {
        if (socket != null) return
        scheduler = Executors.newScheduledThreadPool(2) { r ->
            Thread(r, "lanshare-disc").apply { isDaemon = true }
        }
        tryBind()
    }

    fun stop() {
        running = false
        runCatching { socket?.close() }
        socket = null
        receiveThread?.interrupt()
        receiveThread = null
        scheduler?.shutdownNow()
        scheduler = null
        retryScheduled = false
    }

    private fun tryBind() {
        if (scheduler?.isShutdown == true) return
        try {
            val ds = DatagramSocket(null)
            ds.reuseAddress = true
            ds.broadcast = true
            ds.soTimeout = 1000
            ds.bind(InetSocketAddress(Proto.UDP_PORT))
            socket = ds
            running = true
            lastError = ""
            Log.i(TAG, "udp bound on ${Proto.UDP_PORT}")

            val t = Thread({ receiveLoop(ds) }, "lanshare-disc-rx")
            t.isDaemon = true
            receiveThread = t
            t.start()

            // 起步先发一发，再用 1 秒心跳控制"到点没到点"（周期可配）
            lastAnnounceAt = System.currentTimeMillis()
            announceOnce()
            scheduler?.scheduleWithFixedDelay(
                { runCatching { tick() } },
                1000,
                1000,
                TimeUnit.MILLISECONDS,
            )
        } catch (e: Exception) {
            running = false
            lastError = "udp bind ${Proto.UDP_PORT} failed: ${e.javaClass.simpleName}: ${e.message}"
            Log.w(TAG, lastError)
            scheduleRetry()
        }
    }

    /** 绑定失败重试：不重试就变成"开机永远发现不到设备" */
    private fun scheduleRetry() {
        if (retryScheduled) return
        retryScheduled = true
        scheduler?.schedule({
            retryScheduled = false
            if (socket == null && scheduler?.isShutdown != true) tryBind()
        }, 3000, TimeUnit.MILLISECONDS)
    }

    /**
     * 改发现策略（设置页用）。不用重启 socket：
     * 关掉主动发现只是停广播，端口仍要听 —— 否则「被发现」也没法应答。
     */
    fun setOptions(active: Boolean, discoverable: Boolean, intervalMs: Long) {
        this.active = active
        this.discoverable = discoverable
        this.intervalMs = maxOf(1000L, intervalMs)
        if (active && running) announceOnce()
    }

    /** 下拉刷新 / 立即搜索 */
    fun announceNow() {
        lastAnnounceAt = System.currentTimeMillis()
        announceOnce()
    }

    private fun tick() {
        if (!active || !running) return
        val now = System.currentTimeMillis()
        if (now - lastAnnounceAt < intervalMs) return
        lastAnnounceAt = now
        announceOnce()
    }

    private fun announceOnce() {
        val ds = socket ?: return
        if (!running) return
        // 「被发现」关掉就不再自报家门
        if (!discoverable) return
        val payload = payloadProvider()
        val bytes = payload.toByteArray(Charsets.UTF_8)
        var sent = 0
        for (target in broadcastTargets(nicsProvider())) {
            try {
                val pkt = DatagramPacket(bytes, bytes.size, InetAddress.getByName(target), Proto.UDP_PORT)
                ds.send(pkt)
                sent++
            } catch (e: Exception) {
                // 广播失败常见（AP 隔离 / 目标不可路由），不影响其它目标
                lastError = "send $target failed: ${e.javaClass.simpleName}"
            }
        }
        if (sent > 0) announceCount++
    }

    private fun receiveLoop(ds: DatagramSocket) {
        val buf = ByteArray(8192)
        while (running && !ds.isClosed) {
            val pkt = DatagramPacket(buf, buf.size)
            try {
                ds.receive(pkt)
            } catch (e: Exception) {
                if (ds.isClosed || !running) break
                // soTimeout 是常态，其它异常只记不退出
                if (e !is java.net.SocketTimeoutException) {
                    lastError = "receive failed: ${e.javaClass.simpleName}: ${e.message}"
                }
                continue
            }
            try {
                val text = String(pkt.data, 0, pkt.length, Charsets.UTF_8)
                val obj = Announce.parse(text) ?: continue
                val sourceIp = pkt.address?.hostAddress ?: ""
                // 自己发的包：deviceId 相同直接丢（否则会把自己加进设备表）
                if (Proto.jStr(obj, "deviceId") == selfDeviceIdProvider()) continue
                receivedCount++
                // 「被发现」：不管主动发现开没开，都要回应别人的广播
                replyToSource(sourceIp)
                // 「主动发现」关掉就不再登记新设备
                if (active) onAnnounce(obj, sourceIp)
            } catch (e: Exception) {
                lastError = "handle packet failed: ${e.javaClass.simpleName}: ${e.message}"
            }
        }
    }

    /** 收到即单播回源一发（2 秒限流） */
    private fun replyToSource(sourceIp: String) {
        if (sourceIp.isEmpty() || !discoverable) return
        val now = System.currentTimeMillis()
        if (now - lastShotAt <= 2000) return
        lastShotAt = now
        val ds = socket ?: return
        val bytes = payloadProvider().toByteArray(Charsets.UTF_8)
        runCatching {
            ds.send(DatagramPacket(bytes, bytes.size, InetAddress.getByName(sourceIp), Proto.UDP_PORT))
        }
    }

    companion object {
        private const val TAG = "LanShareDiscovery"
    }
}
