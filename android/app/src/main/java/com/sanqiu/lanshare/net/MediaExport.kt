package com.sanqiu.lanshare.net

import android.content.ContentValues
import android.content.Context
import android.media.MediaScannerConnection
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import java.io.File

/**
 * 附件出口：把收到的图片/文件"再导出一份"到相册或下载目录。
 *
 * 为什么需要在 App 私有目录之外再留一份：
 * 私有目录用户看不见，收完图却在相册里找不到，等于没收到。
 *
 * 两条路径，行为不同但都**必须如实反馈**：
 *  - API 29+（分区存储）：走 `MediaStore`，写 `Pictures/QiuLanShare` 或 `Download/QiuLanShare`，不需要存储权限；
 *  - API 23–28：直接写公共目录 + `MediaScanner` 扫描，**需要 WRITE_EXTERNAL_STORAGE**，
 *    没权限时必须返回失败并说明，不能假装成功。
 *
 * 导出失败**不影响消息入库**：文件仍在私有目录里可点开，只是相册里看不到。
 */
interface AttachmentExporter {
    /**
     * @return 空串 = 成功；非空 = 失败原因（会显示在界面，不吞掉）
     */
    fun export(file: File, isImage: Boolean, displayName: String): String
}

class MediaExporter(private val ctx: Context) : AttachmentExporter {

    override fun export(file: File, isImage: Boolean, displayName: String): String {
        if (!file.exists()) return "源文件不存在"
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            exportViaMediaStore(file, isImage, displayName)
        } else {
            exportToPublicDir(file, isImage, displayName)
        }
    }

    /** API 29+：走 MediaStore，不碰权限 */
    private fun exportViaMediaStore(file: File, isImage: Boolean, displayName: String): String {
        val resolver = ctx.contentResolver
        val collection = if (isImage) {
            MediaStore.Images.Media.EXTERNAL_CONTENT_URI
        } else {
            MediaStore.Downloads.EXTERNAL_CONTENT_URI
        }
        val relative = if (isImage) {
            "${Environment.DIRECTORY_PICTURES}/QiuLanShare"
        } else {
            "${Environment.DIRECTORY_DOWNLOADS}/QiuLanShare"
        }
        val values = ContentValues().apply {
            put(MediaStore.MediaColumns.DISPLAY_NAME, displayName)
            put(MediaStore.MediaColumns.RELATIVE_PATH, relative)
            put(MediaStore.MediaColumns.IS_PENDING, 1)
        }
        val uri = try {
            resolver.insert(collection, values) ?: return "无法创建媒体条目"
        } catch (e: Exception) {
            return "创建条目失败：${e.javaClass.simpleName}"
        }
        return try {
            resolver.openOutputStream(uri)?.use { out ->
                file.inputStream().use { it.copyTo(out) }
            } ?: return "无法写入媒体条目"
            values.clear()
            values.put(MediaStore.MediaColumns.IS_PENDING, 0)
            resolver.update(uri, values, null, null)
            ""
        } catch (e: Exception) {
            // 写一半失败：把半截条目删掉，避免相册里出现 0 字节图片
            runCatching { resolver.delete(uri, null, null) }
            "写入失败：${e.javaClass.simpleName}"
        }
    }

    /** API 23–28：写公共目录 + 扫描。需要 WRITE_EXTERNAL_STORAGE */
    private fun exportToPublicDir(file: File, isImage: Boolean, displayName: String): String {
        val base = if (isImage) {
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES)
        } else {
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
        }
        val dir = File(base, "QiuLanShare")
        if (!dir.exists() && !dir.mkdirs()) return "无法创建目录 ${dir.name}"
        // 同名不覆盖：加 (2)、(3)…
        var target = File(dir, displayName)
        var n = 2
        val stem = displayName.substringBeforeLast('.', displayName)
        val ext = displayName.substringAfterLast('.', "")
        while (target.exists()) {
            val name = if (ext.isEmpty()) "$stem ($n)" else "$stem ($n).$ext"
            target = File(dir, name)
            n++
            if (n > 999) break
        }
        return try {
            file.inputStream().use { input ->
                target.outputStream().use { out -> input.copyTo(out) }
            }
            MediaScannerConnection.scanFile(ctx, arrayOf(target.absolutePath), null, null)
            ""
        } catch (e: Exception) {
            "写入失败：${e.javaClass.simpleName}（可能是缺少存储权限）"
        }
    }

    companion object {
        /** 导出统计（界面显示"未导出 N 条"用） */
        @Volatile var exportFailures: Int = 0
            private set

        fun noteFailure() {
            exportFailures += 1
        }
    }
}
