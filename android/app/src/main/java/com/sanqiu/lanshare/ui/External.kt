package com.sanqiu.lanshare.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.widget.Toast

/**
 * 对外跳转与复制：失败必须说出来。
 *
 * 隐私政策链接打不开时静默失败 = 用户以为"点了没反应"，
 * 而这恰好是审核会实测的一步（点隐私政策必须能打开）。
 */
fun openExternalUrl(ctx: Context, url: String) {
    val intent = Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    val ok = runCatching { ctx.startActivity(intent) }.isSuccess
    if (!ok) toast(ctx, "没有可打开链接的应用，请手动访问：$url")
}

fun copyToClipboard(ctx: Context, label: String, text: String, toastText: String = "已复制") {
    val cm = ctx.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
    if (cm == null) {
        toast(ctx, "当前系统不支持剪贴板")
        return
    }
    cm.setPrimaryClip(ClipData.newPlainText(label, text))
    toast(ctx, toastText)
}

private fun toast(ctx: Context, text: String) {
    Toast.makeText(ctx, text, Toast.LENGTH_SHORT).show()
}

/**
 * 跳到系统的「电池优化」列表，让用户自己把本应用设为「不优化」。
 *
 * 为什么不用 `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` 直接弹授权框：
 * 那是商店重点盘问的敏感权限（属于「可绕过系统省电策略」），
 * 而本应用只需要"传输别被冻结"，用户手动放行完全等价。
 */
fun openBatteryOptimizationSettings(ctx: Context) {
    val candidates = listOf(
        Intent(android.provider.Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS),
        Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS)
            .setData(Uri.fromParts("package", ctx.packageName, null)),
        Intent(android.provider.Settings.ACTION_SETTINGS),
    )
    for (intent in candidates) {
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        if (runCatching { ctx.startActivity(intent) }.isSuccess) return
    }
    toast(ctx, "打不开系统设置，请手动在「设置 → 电池」里关闭对本应用的优化")
}
