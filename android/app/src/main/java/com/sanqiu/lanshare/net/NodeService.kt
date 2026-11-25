package com.sanqiu.lanshare.net

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import com.sanqiu.lanshare.MainActivity
import com.sanqiu.lanshare.core.LsStore
import com.sanqiu.lanshare.probe.AppPaths

/**
 * 前台服务：承载对等节点（HTTP 服务端 + UDP 发现 + 健康探测）。
 *
 * 为什么必须是前台服务：锁屏/后台时普通 Service 会被冻结，`accept` 不再被调用，
 * 对端表现为"能发现、连不上"。P4-0 已实测：前台服务下锁屏 25 分钟仍可被 PC 连上。
 *
 * 启动失败必须响亮：端口全占、数据目录不可写等错误写进 [NodeState.lastError]
 * 并弹通知，不允许"服务启动了但没监听"。
 */
class NodeService : Service() {

    private var node: LsNode? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        startAsForeground("正在启动…")

        try {
            AppPaths.init(this)
            LsNotify.init(this)
            val store = LsStore(AppPaths.requireDataDir())
            val n = LsNode(store)
            // ⚠️ 顺序不能反：start() 内部才 load() 设备表。
            // 在 start() 之前调 setDeviceName 会把**尚未装载的空身份**写盘
            // （deviceId=""），下次启动读到空 id 就又生成一个新的 ——
            // 症状是每启动一次 deviceId 就变，对端眼里"来了台新设备"，
            // 旧会话进不去、未读归零、设备列表里堆幽灵条目。
            val warnings = n.start()
            // 附件出口：收完图能在相册里找到（失败会回写到 api.lastExportError）
            n.api.exporter = MediaExporter(this)
            if (n.hub.self.name.isEmpty()) {
                runCatching { n.hub.setDeviceName(Build.MODEL ?: "Android") }
            }
            node = n
            NodeState.node = n
            NodeState.startedAt = System.currentTimeMillis()
            NodeState.bootWarnings = warnings
            NodeState.lastError = if (warnings.isEmpty()) "" else warnings.joinToString("; ")
            // 界面若已经在前台，补一次（服务可能晚于 Activity 起来）
            n.setForeground(NodeState.uiForeground)
            updateNotification("监听 0.0.0.0:${n.port}")
        } catch (e: Exception) {
            NodeState.lastError = "${e.javaClass.simpleName}: ${e.message}"
            updateNotification("启动失败：${NodeState.lastError}")
            stopSelf()
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        runCatching { node?.stop() }
        NodeState.node = null
        super.onDestroy()
    }

    // ------------------------------------------------------------ 通知

    private fun startAsForeground(text: String) {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(
                CHANNEL_ID, "秋秋快传", NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "节点运行时常驻通知"
                setShowBadge(false)
            }
            nm.createNotificationChannel(ch)
        }
        val notification = buildNotification(text)
        when {
            Build.VERSION.SDK_INT >= 34 -> startForeground(
                NOTIF_ID, notification,
                android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
            )
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q -> startForeground(
                NOTIF_ID, notification,
                android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
            )
            else -> startForeground(NOTIF_ID, notification)
        }
    }

    private fun updateNotification(text: String) {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        runCatching { nm.notify(NOTIF_ID, buildNotification(text)) }
    }

    private fun buildNotification(text: String): Notification {
        val pi = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL_ID)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
        }
        return builder
            .setContentTitle("秋秋快传运行中")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_upload_done)
            .setContentIntent(pi)
            .setOngoing(true)
            .build()
    }

    companion object {
        private const val CHANNEL_ID = "lanshare_node"
        private const val NOTIF_ID = 1001
        const val ACTION_STOP = "com.sanqiu.lanshare.node.STOP"

        fun start(ctx: Context) {
            val i = Intent(ctx, NodeService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                ctx.startForegroundService(i)
            } else {
                ctx.startService(i)
            }
        }

        fun stop(ctx: Context) {
            ctx.startService(Intent(ctx, NodeService::class.java).setAction(ACTION_STOP))
        }
    }
}
