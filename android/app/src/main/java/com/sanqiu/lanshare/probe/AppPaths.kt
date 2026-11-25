package com.sanqiu.lanshare.probe

import android.annotation.SuppressLint
import android.content.Context
import java.io.File

/**
 * 数据目录唯一入口。
 *
 * 全端约定（`docs/16` §4.5）：**所有数据落 `filesDir`，不申请存储权限**。
 * 这里必须是唯一出处 —— 散落在各处的 `context.filesDir` 迟早会有一处写成
 * `getExternalFilesDir`，症状是换设备后历史"凭空消失"。
 */
object AppPaths {

    @SuppressLint("StaticFieldLeak")
    @Volatile
    private var appContext: Context? = null

    fun init(context: Context) {
        appContext = context.applicationContext
    }

    /** 未初始化时返回 null，让调用方显式处理，不要伪造一个路径出来 */
    fun dataDirOrNull(): File? = appContext?.filesDir

    fun requireDataDir(): File {
        val ctx = appContext ?: error("AppPaths 未初始化：应在 Application.onCreate 或 Activity.onCreate 调用 init()")
        val dir = ctx.filesDir
        if (!dir.exists() && !dir.mkdirs()) {
            error("数据目录创建失败：${dir.absolutePath}")
        }
        return dir
    }
}
