package com.sanqiu.lanshare.net

import com.sanqiu.lanshare.core.LsDeviceItem
import com.sanqiu.lanshare.core.Proto

/**
 * 节点运行态快照。
 *
 * 与探针那套一样：**UI 每秒整取一份快照整体替换 State**。
 * 只递增 tick 不会重跑 Compose 里读值的分支（局部重组），表现为
 * "节点在跑、界面说没跑" —— 这个坑已经踩过一次。
 */
data class NodeSnapshot(
    val running: Boolean = false,
    val port: Int = 0,
    val deviceId: String = "",
    val deviceName: String = "",
    val acceptsInbound: Boolean = true,
    val ips: List<String> = emptyList(),
    val devices: List<LsDeviceItem> = emptyList(),
    val totalUnread: Long = 0,
    val startedAt: Long = 0,
    val warnings: List<String> = emptyList(),
    val error: String = "",
    val announceCount: Int = 0,
    val receivedCount: Int = 0,
    val messageCount: Long = 0,
    val conversationCount: Long = 0,
    /** 导出到相册/下载失败的次数（失败不阻断收消息，但必须让人看见） */
    val exportFailures: Int = 0,
    val exportError: String = "",
) {
    /** 主地址（顶栏显示用） */
    val primaryIp: String get() = ips.firstOrNull() ?: ""
}

object NodeState {

    @Volatile var node: LsNode? = null

    @Volatile var startedAt: Long = 0L

    @Volatile var lastError: String = ""

    @Volatile var bootWarnings: List<String> = emptyList()

    /** 界面是否在前台（通知抑制用；Service 起来时据此补一次） */
    @Volatile var uiForeground: Boolean = false

    /** 点通知要打开的会话（界面取走即清） */
    @Volatile var pendingDevice: String = ""

    fun takePendingDevice(): String {
        val v = pendingDevice
        pendingDevice = ""
        return v
    }

    fun snapshot(): NodeSnapshot {
        val n = node ?: return NodeSnapshot(startedAt = startedAt, error = lastError, warnings = bootWarnings)
        val hub = n.hub
        val now = Proto.nowMs()
        // 与 HTTP 线程共用 hub.gate：deviceItems 会遍历 peers/messages，
        // 不加锁会撞 ConcurrentModificationException（表现为列表偶尔少一条）
        return synchronized(hub.gate) {
            NodeSnapshot(
                running = n.started,
                port = n.port,
                deviceId = hub.self.deviceId,
                deviceName = hub.self.name,
                acceptsInbound = hub.acceptsInbound,
                ips = hub.self.ips.toList(),
                devices = hub.deviceItems(now),
                totalUnread = hub.totalUnread(),
                startedAt = startedAt,
                warnings = bootWarnings,
                error = lastError,
                announceCount = n.discovery?.announceCount ?: 0,
                receivedCount = n.discovery?.receivedCount ?: 0,
                messageCount = hub.messages.size.toLong(),
                conversationCount = hub.convs.size.toLong(),
                exportFailures = MediaExporter.exportFailures,
                exportError = n.api.lastExportError,
            )
        }
    }
}
