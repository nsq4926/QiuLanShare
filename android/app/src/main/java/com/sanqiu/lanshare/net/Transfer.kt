package com.sanqiu.lanshare.net

import com.sanqiu.lanshare.core.Proto

/**
 * 传输任务表（收 / 发都记在这里）。
 *
 * UI 每 2 秒要一份「总大小 / 总速度 / 总进度」，通知栏也要同一份口径；
 * 断点续传又必须由发起方记住"这片发到哪了"。三者同源，所以单独放一个对象。
 *
 * 只存内存：进程被杀之后队列不保留（对端按 msgId 幂等，重发不会变两条）。
 */
data class TransferItem(
    val id: Int,
    val xid: String,
    /** in = 收，out = 发 */
    val dir: String,
    val name: String,
    var total: Long,
    var done: Long = 0,
    var status: String = ACTIVE,
    var peerId: String = "",
    var msgId: String = "",
    var startedAt: Long = System.currentTimeMillis(),
    var lastAt: Long = System.currentTimeMillis(),
    var lastDone: Long = 0,
    var speed: Double = 0.0,
    var error: String = "",
    /** 结束时间（终态时写）；平均速度 = total / (endedAt - startedAt) */
    var endedAt: Long = 0,
) {
    companion object {
        const val ACTIVE = "active"
        const val DONE = "done"
        const val FAILED = "failed"
        /** 用户点了 × 取消：循环立刻停，记录留着（再点 × 才删除） */
        const val CANCELLED = "cancelled"
    }
}

object Transfers {
    /** 分片 256 KB：太小放大 HTTP 开销，太大进度条一跳一大截 */
    // 1 MB/片：256 KB 时每片都要新建一条 TCP 连接（本项目 Connection: close），
    // 真机实测 10 MB：256 KB 片 2.0 MB/s，1 MB 片 6.0 MB/s；再大（4 MB）对慢手机反而变差。
    const val CHUNK = 1024 * 1024

    /** 记录条数上限（只保护内存；磁盘上的文件不受影响） */
    private const val MAX_KEEP = 200

    private val lock = Any()
    private val items = mutableListOf<TransferItem>()
    private var nextId = 1

    /** 有状态变化待落盘（进度更新**不**置脏：每 1 MB 写一次文件太浪费） */
    @Volatile private var dirty = false

    private fun markDirtyLocked() { dirty = true }

    /** 落盘：记录跨重启保留（用户要求"完成后列表不要消失"） */
    fun saveIfDirty(store: com.sanqiu.lanshare.core.LsStore) {
        if (!dirty) return
        dirty = false
        val snap = synchronized(lock) { items.takeLast(MAX_KEEP) }
        val sb = StringBuilder("{\"v\":1,\"items\":[")
        for ((i, t) in snap.withIndex()) {
            if (i > 0) sb.append(',')
            sb.append("{\"xid\":").append(jsonStr(t.xid))
                .append(",\"name\":").append(jsonStr(t.name))
                .append(",\"dir\":").append(jsonStr(t.dir))
                .append(",\"total\":").append(t.total)
                .append(",\"done\":").append(t.done)
                .append(",\"status\":").append(jsonStr(t.status))
                .append(",\"peerId\":").append(jsonStr(t.peerId))
                .append(",\"msgId\":").append(jsonStr(t.msgId))
                .append(",\"startedAt\":").append(t.startedAt)
                .append(",\"endedAt\":").append(t.endedAt)
                .append(",\"error\":").append(jsonStr(t.error))
                .append('}')
        }
        sb.append("]}")
        val f = java.io.File(store.getDataDir(), "lanshare_transfers.json")
        if (!store.writeText(f, sb.toString())) {
            // 写不下去也要留痕：下次启动会少几条记录，用户只会觉得"记录又没了"
            android.util.Log.w("LanShare", "save transfers failed: ${f.absolutePath}")
        }
    }

    /** 启动加载：上次没跑完的（active）标成 failed —— 否则会显示一条永远不动的进度 */
    fun load(store: com.sanqiu.lanshare.core.LsStore) {
        val f = java.io.File(store.getDataDir(), "lanshare_transfers.json")
        val text = store.readText(f)
        if (text.isBlank()) return
        val arr = try {
            org.json.JSONObject(text).optJSONArray("items") ?: return
        } catch (e: Exception) {
            android.util.Log.w("LanShare", "load transfers failed: ${e.message}")
            return
        }
        synchronized(lock) {
            items.clear()
            nextId = 1
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                val xid = o.optString("xid")
                if (xid.isEmpty()) continue
                val t = TransferItem(
                    id = nextId++,
                    xid = xid,
                    dir = o.optString("dir", "in"),
                    name = o.optString("name", "file"),
                    total = o.optLong("total", 0),
                    done = o.optLong("done", 0),
                    status = o.optString("status", TransferItem.DONE),
                    peerId = o.optString("peerId"),
                    msgId = o.optString("msgId"),
                    startedAt = o.optLong("startedAt", System.currentTimeMillis()),
                )
                t.endedAt = o.optLong("endedAt", 0)
                t.error = o.optString("error")
                t.lastAt = System.currentTimeMillis()
                if (t.status == TransferItem.ACTIVE) {
                    // 上次进程被杀/冻结：没有谁在传了，如实标失败并写明原因
                    t.status = TransferItem.FAILED
                    t.error = "interrupted"
                    t.endedAt = System.currentTimeMillis()
                }
                items.add(t)
            }
        }
    }

    /**
     * 列表快照：**新的在上**，且**每条都是副本**。
     *
     * ⚠️ 必须 copy：直接返回原对象时，进度变了但 List 里元素还是同一批引用，
     * `List.equals` 逐元素比较结果相等 → Compose 认为 state 没变、不重组 ——
     * 表现就是"传输列表数据不刷新"（服务端在跑、进度也在涨，界面就是不动）。
     */
    fun snapshot(): List<TransferItem> =
        synchronized(lock) { items.sortedByDescending { it.startedAt }.map { it.copy() } }

    fun beginOut(peerId: String, name: String, total: Long, xid: String, msgId: String): TransferItem =
        synchronized(lock) {
            items.firstOrNull { it.xid == xid } ?: TransferItem(
                id = nextId++, xid = xid, dir = "out", name = name, total = total,
                peerId = peerId, msgId = msgId,
            ).also { items += it; markDirtyLocked() }
        }

    fun beginIn(xid: String, peerId: String, name: String, total: Long): TransferItem =
        synchronized(lock) {
            items.firstOrNull { it.xid == xid } ?: TransferItem(
                id = nextId++, xid = xid, dir = "in", name = name, total = total, peerId = peerId,
            ).also { items += it; markDirtyLocked() }
        }

    fun setProgress(it: TransferItem, done: Long) = synchronized(lock) {
        if (it.status != TransferItem.ACTIVE) return@synchronized
        val d = done.coerceIn(0L, if (it.total > 0) it.total else done)
        val now = System.currentTimeMillis()
        val dt = now - it.lastAt
        if (dt >= 200) {
            val inst = (d - it.lastDone) * 1000.0 / dt
            it.speed = if (it.speed <= 0) inst else it.speed * 0.55 + inst * 0.45
            it.lastAt = now
            it.lastDone = d
        }
        it.done = d
        if (it.total > 0 && d >= it.total) {
            it.status = TransferItem.DONE
            it.speed = 0.0
        }
    }

    fun finish(it: TransferItem, ok: Boolean, error: String = "") = synchronized(lock) {
        // 已取消的不能再被"失败"覆盖，否则界面上又变回一条普通失败记录
        if (it.status == TransferItem.CANCELLED) return@synchronized
        it.status = if (ok) TransferItem.DONE else TransferItem.FAILED
        it.speed = 0.0
        it.error = error
        it.endedAt = System.currentTimeMillis()
        if (ok) {
            if (it.total <= 0) it.total = it.done
            it.done = it.total
        }
        dirty = true
    }

    /** 平均速度（B/s）：终态按 total/(endedAt-startedAt)，在传返回 0（用瞬时速度） */
    fun avgSpeed(t: TransferItem): Long {
        val end = if (t.status == TransferItem.ACTIVE) return 0L else (t.endedAt.takeIf { it > 0 } ?: t.lastAt)
        val ms = end - t.startedAt
        if (ms <= 200 || t.total <= 0) return 0L
        return t.total * 1000L / ms
    }

    /** 清空全部传输记录（"清空全部历史"时调用） */
    fun dropAll(): Int = synchronized(lock) {
        val n = items.size
        items.clear()
        markDirtyLocked()
        n
    }

    fun activeCount(): Int = synchronized(lock) { items.count { it.status == TransferItem.ACTIVE } }

    fun hasAny(): Boolean = synchronized(lock) { items.isNotEmpty() }

    /** [总字节, 已完成, 总速度] —— 抽屉头与通知栏共用，只统计**在传**的 */
    fun summary(): LongArray = synchronized(lock) {
        var total = 0L
        var done = 0L
        var speed = 0.0
        for (it in items) {
            if (it.status != TransferItem.ACTIVE) continue
            total += it.total
            done += it.done
            speed += it.speed
        }
        longArrayOf(total, done, speed.toLong())
    }

    /**
     * 不再按时间清理：完成的记录要一直留着（用户要能回看"传过什么"），
     * 只有显式删除、或清空会话时才移除。这里只做条数上限保护，防止内存无限涨。
     */
    fun prune() = synchronized(lock) {
        if (items.size <= MAX_KEEP) return@synchronized
        val drop = items.filter { it.status == TransferItem.DONE }
            .sortedBy { it.startedAt }
            .take(items.size - MAX_KEEP)
            .map { it.id }
            .toHashSet()
        items.removeAll { it.id in drop }
    }

    /** 取消一条在传任务（false = 没有这条或已经结束） */
    fun cancel(xid: String): Boolean = synchronized(lock) {
        val it = items.firstOrNull { it.xid == xid } ?: return@synchronized false
        if (it.status != TransferItem.ACTIVE) return@synchronized false
        it.status = TransferItem.CANCELLED
        it.speed = 0.0
        it.endedAt = System.currentTimeMillis()
        markDirtyLocked()
        true
    }

    fun isCancelled(xid: String): Boolean = synchronized(lock) {
        items.firstOrNull { it.xid == xid }?.status == TransferItem.CANCELLED
    }

    /** 删除一条记录（在传的先取消再删） */
    fun remove(xid: String): Boolean = synchronized(lock) {
        val it = items.firstOrNull { it.xid == xid } ?: return@synchronized false
        it.status = TransferItem.CANCELLED
        it.endedAt = System.currentTimeMillis()
        items.remove(it)
        markDirtyLocked()
        true
    }

    /** 清空某个会话的传输记录（"清空会话"时调用，与消息记录同生命周期） */
    fun dropPeer(peerId: String): Int = synchronized(lock) {
        if (peerId.isEmpty()) return@synchronized 0
        val before = items.size
        items.removeAll { it.peerId == peerId }
        markDirtyLocked()
        before - items.size
    }

    private fun jsonStr(s: String): String {
        val sb = StringBuilder("\"")
        for (c in s) {
            when (c) {
                '\"' -> sb.append("\\\"")
                '\\' -> sb.append("\\\\")
                '\n' -> sb.append("\\n")
                '\r' -> sb.append("\\r")
                '\t' -> sb.append("\\t")
                else -> if (c < ' ') sb.append(String.format("\\u%04x", c.code)) else sb.append(c)
            }
        }
        return sb.append('\"').toString()
    }

    fun fmtSize(n: Long): String = when {
        n <= 0 -> "0 B"
        n < 1024 -> "$n B"
        n < 1024L * 1024 -> "%.1f KB".format(n / 1024.0)
        n < 1024L * 1024 * 1024 -> "%.1f MB".format(n / 1024.0 / 1024.0)
        else -> "%.2f GB".format(n / 1024.0 / 1024.0 / 1024.0)
    }

    fun fmtSpeed(bps: Long): String = when {
        bps <= 0 -> "0 MB/s"
        bps < 1024 -> "$bps B/s"
        bps < 1024L * 1024 -> "${bps / 1024} KB/s"
        else -> "%.1f MB/s".format(bps / 1024.0 / 1024.0)
    }

    fun pct(done: Long, total: Long): Int =
        if (total <= 0) 0 else ((done * 100 / total).coerceIn(0, 100)).toInt()

    fun xid(): String = "x" + Proto.nowMs().toString(36) + (0..0xFFFFFF).random().toString(36)
}
