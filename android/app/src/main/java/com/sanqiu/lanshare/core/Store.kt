package com.sanqiu.lanshare.core

import com.sanqiu.lanshare.probe.AppPaths
import java.io.File

/**
 * 数据持久化。
 *
 * 三个文件与 Windows/鸿蒙**同名同格式**：
 *  `lanshare_devices.json` / `lanshare_convs.json` / `lanshare_history.jsonl`
 * 全部落在 `filesDir` 下 —— 不申请任何存储权限（`docs/16` §4.5）。
 *
 * 写入一律**原子**（临时文件 + rename）：断电/被杀时留下半个 JSON，
 * 下次启动 load 会静默退回空表，用户看到的是"设备和历史全没了"，
 * 而且没有任何报错。
 */
class LsStore(private val dataDir: File) {

    val saveDir: File get() = File(dataDir, "save")

    fun init() {
        ensureDir(dataDir)
        ensureDir(saveDir)
    }

    fun getDataDir(): String = dataDir.absolutePath

    fun getSaveDir(): String = saveDir.absolutePath

    fun devicesPath(): File = File(dataDir, "lanshare_devices.json")

    fun convsPath(): File = File(dataDir, "lanshare_convs.json")

    fun historyPath(): File = File(dataDir, "lanshare_history.jsonl")

    /** 某个对端的附件目录：`save/<设备名>-<deviceId>/` */
    fun peerDir(peerKey: String, displayName: String): File =
        File(saveDir, deviceFolderName(peerKey, displayName))

    /** 目录统计：[文件数, 总字节]。不存在返回 [0,0]，不抛。 */
    fun statDir(dir: File): LongArray {
        if (!dir.isDirectory) return longArrayOf(0, 0)
        var count = 0L
        var bytes = 0L
        dir.listFiles()?.forEach {
            if (it.isFile) {
                count++
                bytes += it.length()
            }
        }
        return longArrayOf(count, bytes)
    }

    /** 递归删除（只用于"删除设备"这种用户显式动作，调用前必须先 dry=1 给用户看） */
    fun removeDirRecursive(dir: File) {
        if (!dir.exists()) return
        dir.walkBottomUp().forEach { runCatching { it.delete() } }
    }

    fun readText(f: File): String {
        if (!f.exists()) return ""
        return try {
            f.readText(Charsets.UTF_8)
        } catch (e: Exception) {
            ""
        }
    }

    /** 原子写：先写 .tmp 再 rename */
    fun writeText(f: File, text: String): Boolean {
        val tmp = File(f.parentFile, f.name + ".tmp")
        return try {
            ensureDir(f.parentFile ?: dataDir)
            tmp.writeText(text, Charsets.UTF_8)
            if (f.exists()) f.delete()
            tmp.renameTo(f)
        } catch (e: Exception) {
            runCatching { tmp.delete() }
            false
        }
    }

    /** 追加一行（jsonl）。追加不是原子操作，但单行追加损坏只影响最后一行 */
    fun appendLine(f: File, line: String): Boolean = try {
        ensureDir(f.parentFile ?: dataDir)
        f.appendText(line + "\n", Charsets.UTF_8)
        true
    } catch (e: Exception) {
        false
    }

    fun readBytes(f: File): ByteArray? = try {
        if (f.exists()) f.readBytes() else null
    } catch (e: Exception) {
        null
    }

    fun writeBytes(f: File, bytes: ByteArray): Boolean = try {
        ensureDir(f.parentFile ?: dataDir)
        f.writeBytes(bytes)
        true
    } catch (e: Exception) {
        false
    }

    fun fileSize(f: File): Long = if (f.exists()) f.length() else 0L

    fun exists(path: String): Boolean = File(path).exists()

    fun listDir(path: String): List<String> {
        val d = File(path)
        if (!d.isDirectory) return emptyList()
        return d.list()?.toList() ?: emptyList()
    }

    fun unlink(path: String) {
        runCatching { File(path).delete() }
    }

    companion object {
        fun ensureDir(dir: File) {
            if (!dir.exists()) dir.mkdirs()
        }

        /** 默认实例：数据目录走 [AppPaths]（唯一入口，只允许 filesDir） */
        fun default(): LsStore = LsStore(AppPaths.requireDataDir())
    }
}

/** 文件名里不允许出现的字符 */
fun sanitizeName(s: String): String {
    val sb = StringBuilder(s.length)
    for (c in s) {
        when (c) {
            '/', '\\', ':', '*', '?', '"', '<', '>', '|', '\u0000' -> sb.append('_')
            '\n', '\r', '\t' -> sb.append(' ')
            else -> sb.append(c)
        }
    }
    var out = sb.toString().trim().trimEnd('.')
    if (out.isEmpty()) out = "未命名"
    // Windows 保留名 + 长度上限（跨端目录名要能被三方接受）
    val upper = out.uppercase()
    if (upper in setOf("CON", "PRN", "AUX", "NUL", "COM1", "COM2", "LPT1", "LPT2")) {
        out = "_$out"
    }
    return if (out.length > 60) out.substring(0, 60) else out
}

/**
 * 设备子目录名：`<设备名>-<deviceId>`。
 *
 * 排序上 deviceId 在后，改名时按 `-<deviceId>` 后缀沿用旧目录
 * （`docs/08` 工程约定第 10 条）。
 */
fun deviceFolderName(peerKey: String, displayName: String): String {
    val safeId = sanitizeName(peerKey)
    val safeName = sanitizeName(if (displayName.isEmpty()) "设备" else displayName)
    return "$safeName-$safeId"
}
