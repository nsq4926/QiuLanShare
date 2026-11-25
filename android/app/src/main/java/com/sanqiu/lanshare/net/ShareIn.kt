package com.sanqiu.lanshare.net

import android.content.Intent
import android.net.Uri

/**
 * 分享入口：别的应用把文件/图片分享到本应用时，URI 先攒在这里。
 *
 * 为什么不在 `MainActivity` 里直接发：
 *  - 发文件需要**已经选好一个会话**（`ChatArea` 才知道 peerId 和发送队列）；
 *  - 用户可能还没选设备 —— 那就先存着，等他在抽屉里点一台设备、
 *    `ChatArea` 被组合出来时再一起发。丢在 Ability 里发会变成"发给谁？"的猜测。
 *
 * 语义与 `NodeState.pendingDevice` 一致：**取走即清**（drain），
 * 避免切会话/转屏时同一条分享被重复发送。
 */
object ShareIn {

    private val queue = ArrayDeque<Uri>()

    @Synchronized
    fun pushAll(uris: List<Uri>) {
        queue.addAll(uris)
    }

    @Synchronized
    fun drain(): List<Uri> {
        if (queue.isEmpty()) return emptyList()
        val out = queue.toList()
        queue.clear()
        return out
    }

    @Synchronized
    fun pendingCount(): Int = queue.size

    /**
     * 从启动/复用的 Intent 里取被分享的 URI。
     *
     * 只认 `ACTION_SEND` / `ACTION_SEND_MULTIPLE`；其它 Intent（通知点击、启动器）
     * 一律返回空表，不能顺手把 `data` 当成文件。
     */
    @Suppress("DEPRECATION")
    fun urisOf(intent: Intent?): List<Uri> {
        if (intent == null) return emptyList()
        return when (intent.action) {
            Intent.ACTION_SEND ->
                listOfNotNull(intent.getParcelableExtra<Uri>(Intent.EXTRA_STREAM))

            Intent.ACTION_SEND_MULTIPLE ->
                intent.getParcelableArrayListExtra<Uri>(Intent.EXTRA_STREAM) ?: emptyList()

            else -> emptyList()
        }
    }
}
