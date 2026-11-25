package com.sanqiu.lanshare.net

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Build
import android.util.Log
import com.sanqiu.lanshare.MainActivity

/**
 * 系统通知。
 *
 * 两条要求：
 *  1. 后台收到消息 → 通知栏一条，**点它直接进那条会话**（Intent 带 deviceId/convId）；
 *  2. 收发文件 → 同一条常驻通知实时刷新「总大小 · 总速度 · 总进度」，不刷屏。
 *
 * 通知用系统 Notification.Builder 而不是 NotificationCompat：
 * 少一个依赖，并且本项目 minSdk 23 的分支已经在这里判断过版本。
 */
object LsNotify {
    private const val TAG = "LanShareNotify"
    private const val CH_MSG = "lanshare_msg"
    private const val CH_XFER = "lanshare_xfer"
    private const val ID_MSG_BASE = 10000
    private const val ID_XFER = 20001

    @Volatile private var ctx: Context? = null
    /** 应用在前台且正在看这个会话时不弹（用户已经看见了） */
    @Volatile var focusedConv: String = ""
    @Volatile var foreground: Boolean = false
    private var xferShown = false

    fun init(c: Context) {
        ctx = c.applicationContext
        val nm = ctx!!.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(
                NotificationChannel(CH_MSG, "新消息", NotificationManager.IMPORTANCE_HIGH).apply {
                    description = "收到局域网消息时提醒"
                }
            )
            nm.createNotificationChannel(
                NotificationChannel(CH_XFER, "文件传输", NotificationManager.IMPORTANCE_LOW).apply {
                    description = "收发文件时的进度"
                    setShowBadge(false)
                }
            )
        }
    }

    private fun builder(channel: String): Notification.Builder {
        val c = ctx ?: throw IllegalStateException("notify not inited")
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(c, channel)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(c)
        }
    }

    private fun msgId(convId: String): Int = ID_MSG_BASE + (convId.hashCode() and 0x7FFFFFFF) % 9000

    /** 收到一条消息。preview 只放摘要（图片/文件不泄露正文） */
    fun publishMessage(convId: String, deviceId: String, dev: String, preview: String, unread: Long) {
        val c = ctx ?: return
        if (foreground && focusedConv == convId) return
        val id = msgId(convId)
        val intent = Intent(c, MainActivity::class.java).apply {
            putExtra(MainActivity.EXTRA_DEVICE_ID, deviceId)
            putExtra(MainActivity.EXTRA_CONV_ID, convId)
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_NEW_TASK
        }
        val pi = PendingIntent.getActivity(
            c, id, intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        val text = if (preview.isEmpty()) "收到一条消息" else preview
        val n = builder(CH_MSG)
            .setContentTitle(dev.ifEmpty { "秋秋快传" })
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_notify_chat)
            .setContentIntent(pi)
            .setAutoCancel(true)
            .setNumber(unread.toInt())
            .setWhen(System.currentTimeMillis())
            .build()
        runCatching { (c.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).notify(id, n) }
            .onFailure { Log.w(TAG, "publish message failed: ${it.javaClass.simpleName}") }
    }

    /** 传输进度：单条常驻通知，实时改写（含系统进度条） */
    fun updateTransfer() {
        val c = ctx ?: run { Log.w(TAG, "updateTransfer: ctx null"); return }
        val nm = c.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Transfers.activeCount() <= 0) {
            if (xferShown) {
                xferShown = false
                runCatching { nm.cancel(ID_XFER) }
            }
            return
        }
        val sum = Transfers.summary()
        val total = sum[0]
        val done = sum[1]
        val speed = sum[2]
        val pct = Transfers.pct(done, total)
        val active = Transfers.activeCount()
        Transfers.snapshot().firstOrNull { it.status == TransferItem.ACTIVE }?.let { first ->
            // 头部文案严格给三样：总大小 / 总速度 / 总进度
            val title = "${first.name} · $pct%"
            // 通知栏窄，用「已传 x / y」——带标签，不会让人猜哪个是总量
            val text = "已传 ${Transfers.fmtSize(done)} / ${Transfers.fmtSize(total)} · ${Transfers.fmtSpeed(speed)}"
            val n = builder(CH_XFER)
                .setContentTitle(if (active > 1) "$title（${active} 个文件）" else title)
                .setContentText(text)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setProgress(100, pct, false)
                .build()
            runCatching { nm.notify(ID_XFER, n) }
                .onFailure { Log.w(TAG, "publish transfer failed: ${it.javaClass.simpleName}") }
            xferShown = true
        }
    }

    fun cancelMessage(convId: String) {
        val c = ctx ?: return
        runCatching {
            (c.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).cancel(msgId(convId))
        }
    }
}
