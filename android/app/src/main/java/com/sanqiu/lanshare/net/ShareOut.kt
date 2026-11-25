package com.sanqiu.lanshare.net

import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.webkit.MimeTypeMap
import androidx.core.content.FileProvider
import java.io.File
import java.util.Locale

/**
 * 分享出口：把收到的图片/文件交给系统分享面板（长按菜单里的「分享」）。
 *
 * 为什么必须走 FileProvider：
 *  - Android 7+ 直接 `file://` 分享会抛 `FileUriExposedException`；
 *  - 私有目录里的文件也不能让别的应用随便读，只能"授一次读权限"给被选中的那个应用。
 *
 * 失败**必须说出来**（返回原因给界面 toast），不能静默什么都不发生 ——
 * 这个项目里"点了没反应"的报障基本都是吞异常吞出来的。
 */
object ShareOut {

    /** 与 AndroidManifest 里 provider 的 authorities 后缀保持一致 */
    private const val AUTHORITY_SUFFIX = ".fileprovider"

    /**
     * 按扩展名猜 MIME。猜不到时：图片给 `image/*`，其余给 `application/octet-stream`
     * —— 给 `*/*` 会让系统面板把"能接任何东西"的应用全列出来，反而更难选。
     */
    fun mimeOf(file: File, isImage: Boolean): String {
        val ext = file.extension.lowercase(Locale.ROOT)
        if (ext.isNotEmpty()) {
            val t = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext)
            if (!t.isNullOrEmpty()) return t
        }
        return if (isImage) "image/*" else "application/octet-stream"
    }

    /**
     * 拉起系统分享面板。
     *
     * @return 空串 = 已拉起；非空 = 失败原因（界面 toast 出来）
     */
    fun share(ctx: Context, file: File, displayName: String, isImage: Boolean): String {
        if (!file.exists()) return "文件已不在本机"
        val uri: Uri = try {
            FileProvider.getUriForFile(ctx, ctx.packageName + AUTHORITY_SUFFIX, file)
        } catch (e: Exception) {
            return "无法生成分享链接：${e.javaClass.simpleName}"
        }
        val name = displayName.ifEmpty { file.name }
        val send = Intent(Intent.ACTION_SEND).apply {
            type = mimeOf(file, isImage)
            putExtra(Intent.EXTRA_STREAM, uri)
            putExtra(Intent.EXTRA_SUBJECT, name)
            // ClipData 也要带上：部分 ROM 的分享面板只认 clipData 里的 URI 来授读权限
            clipData = ClipData.newRawUri(name, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        val chooser = Intent.createChooser(send, "分享到").apply {
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            // Context 不是 Activity 时必须带 NEW_TASK，否则直接抛异常
            if (ctx !is android.app.Activity) addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        return try {
            ctx.startActivity(chooser)
            ""
        } catch (e: Exception) {
            "没有可用的分享应用"
        }
    }
}
