package com.sanqiu.lanshare.core

import org.json.JSONObject

/**
 * 应用设置：主题 + 聊天记录期限 + 自动保存。
 *
 * 与设备表**分开存**（`lanshare_settings.json`）：设置丢了最多变回默认值，
 * 混进设备表则一次写坏连身份一起没了。
 */
data class LsSettings(
    var themeMode: String = THEME_SYSTEM,
    var retentionMode: String = RET_NEVER,
    /** 仅在 retentionMode == RET_DAYS 时有效 */
    var retentionDays: Long = 30,
    var autoSaveImage: Boolean = true,
    var autoSaveFile: Boolean = true,
    /** 被发现：广播自己 + 回应别人 + 接收入站 */
    var acceptsInbound: Boolean = true,
    /** 主动发现：每隔 discoverSeconds 秒搜一次 */
    var activeDiscover: Boolean = true,
    var discoverSeconds: Int = 10,
    /**
     * 接收服务（HTTP 监听）总开关。
     * 关掉后本机不监听端口，收不到消息/文件；默认 true —— 打开应用即自动起服务。
     * 以前只有「主动发现 / 被发现」两个开关且都以"服务已在跑"为前提，
     * 服务没起来时两个开关都是灰的，于是没人能把服务开起来（开关全失效）。
     */
    var serverEnabled: Boolean = true,
    /** 历史保留天数（与 /api/history/settings 同一份） */
    var historyDays: Long = 0,
    /**
     * 隐私政策同意时间（毫秒，0 = 未同意）。
     *
     * 上架合规要求：**同意之前不得启动接收服务、不得申请通知权限**。
     * 存在设置文件里而不是 SharedPreferences，是为了让「读设置」只有一处入口
     * （`LsSettingsStore`），卸载/清数据时随沙盒一起消失。
     */
    var privacyAcceptedAt: Long = 0,
) {
    fun toJson(): String = "{\"themeMode\":${Proto.jsonStr(themeMode)}" +
        ",\"retentionMode\":${Proto.jsonStr(retentionMode)}" +
        ",\"retentionDays\":$retentionDays" +
        ",\"autoSaveImage\":${if (autoSaveImage) "true" else "false"}" +
        ",\"autoSaveFile\":${if (autoSaveFile) "true" else "false"}" +
        ",\"acceptsInbound\":${if (acceptsInbound) "true" else "false"}" +
        ",\"activeDiscover\":${if (activeDiscover) "true" else "false"}" +
        ",\"discoverSeconds\":$discoverSeconds" +
        ",\"serverEnabled\":${if (serverEnabled) "true" else "false"}" +
        ",\"historyDays\":$historyDays" +
        ",\"privacyAcceptedAt\":$privacyAcceptedAt}"

    companion object {
        const val THEME_SYSTEM = "system"
        const val THEME_LIGHT = "light"
        const val THEME_DARK = "dark"

        const val RET_NEVER = "never"
        const val RET_DAYS = "days"
        const val RET_ON_EXIT = "onExit"

        fun fromJsonText(text: String): LsSettings {
            val s = LsSettings()
            if (text.isEmpty()) return s
            val o = Proto.parseObj(text)
            val tm = Proto.jStr(o, "themeMode", THEME_SYSTEM)
            if (tm == THEME_LIGHT || tm == THEME_DARK || tm == THEME_SYSTEM) s.themeMode = tm
            val rm = Proto.jStr(o, "retentionMode", RET_NEVER)
            if (rm == RET_DAYS || rm == RET_ON_EXIT || rm == RET_NEVER) s.retentionMode = rm
            val days = Proto.jNum(o, "retentionDays", 30)
            s.retentionDays = when {
                days < 1 -> 1
                days > 3650 -> 3650
                else -> days
            }
            s.autoSaveImage = Proto.jBool(o, "autoSaveImage", true)
            s.autoSaveFile = Proto.jBool(o, "autoSaveFile", true)
            s.acceptsInbound = Proto.jBool(o, "acceptsInbound", true)
            s.activeDiscover = Proto.jBool(o, "activeDiscover", true)
            // 老配置里没有这个键：缺省 true = 与"以前每次启动都在跑"的行为一致
            s.serverEnabled = Proto.jBool(o, "serverEnabled", true)
            val sec = Proto.jNum(o, "discoverSeconds", 10).toInt()
            s.discoverSeconds = when {
                sec < 3 -> 3
                sec > 300 -> 300
                else -> sec
            }
            s.historyDays = Proto.jNum(o, "historyDays", 0)
            // 老配置没有这个键：0 = 未同意 → 老用户升级后也会看到一次同意页，
            // 这正是上架要求的口径（不能因为"以前装过"就默认同意）。
            s.privacyAcceptedAt = Proto.jNum(o, "privacyAcceptedAt", 0)
            return s
        }
    }
}

class LsSettingsStore(private val store: LsStore) {

    private fun path() = java.io.File(store.getDataDir(), "lanshare_settings.json")

    fun load(): LsSettings = LsSettings.fromJsonText(store.readText(path()))

    fun save(s: LsSettings) {
        store.writeText(path(), s.toJson())
    }
}

/** 保留期裁剪点：0 = 不裁剪 */
fun retentionCutoff(settings: LsSettings, nowMs: Long): Long {
    if (settings.retentionMode != LsSettings.RET_DAYS) return 0L
    val days = if (settings.retentionDays < 1) 1 else settings.retentionDays
    return nowMs - days * 24L * 3600L * 1000L
}

/** 退出应用时是否清理历史 */
fun isClearOnExit(settings: LsSettings): Boolean =
    settings.retentionMode == LsSettings.RET_ON_EXIT
