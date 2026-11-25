package com.sanqiu.lanshare.net

import com.sanqiu.lanshare.core.LsSelf
import com.sanqiu.lanshare.core.Proto
import org.json.JSONObject

/**
 * UDP 发现报文。
 *
 * 字段与 Windows 端 `Discovery::BuildPayload` **逐字对齐**（对方按 deviceId 合并）：
 * `v / app / deviceId / name / port / mac / ips[] / acceptsInbound / filteredNicCount / apiVersion`
 * 另带 `platform` —— Windows 端不写这个字段、缺省记成 `windows`，
 * 带着才能在对方设备列表里正确显示平台（这里固定 `android`）。
 *
 * 注意 Windows 端**没有** `t`（probe/announce）字段：两端发的是同一份 announce，
 * 这里保持一致。
 *
 * 报文拼装做成纯函数，是为了能单测 —— 非法 JSON 的症状是
 * "对端静默看不见我"，不报错（本项目历史 bug #1 就是这个）。
 */
object Announce {

    /** 报文体（不含换行），键顺序与鸿蒙端一致，便于逐字 diff */
    fun build(
        self: LsSelf,
        port: Int,
        acceptsInbound: Boolean,
        platform: String = Proto.LS_PLATFORM,
    ): String {
        val ips = self.ips.joinToString(",") { Proto.jsonStr(it) }
        return "{\"v\":2" +
            ",\"app\":${Proto.jsonStr(Proto.LS_TAG)}" +
            ",\"deviceId\":${Proto.jsonStr(self.deviceId)}" +
            ",\"name\":${Proto.jsonStr(self.name)}" +
            ",\"port\":$port" +
            ",\"mac\":${Proto.jsonStr(self.mac)}" +
            ",\"ips\":[$ips]" +
            ",\"acceptsInbound\":${if (acceptsInbound) "true" else "false"}" +
            ",\"filteredNicCount\":${self.filteredNicCount}" +
            ",\"apiVersion\":${Proto.jsonStr(Proto.API_VERSION)}" +
            // 支持分片续传：对端据此决定"分片发"还是"整份发"
            ",\"resumable\":true" +
            ",\"platform\":${Proto.jsonStr(platform)}}"
    }

    /**
     * 解析收到的报文；不像 announce 的返回 null。
     *
     * 判据是 `app == TMLS` **且** `deviceId` 非空 —— 只判 `app` 会把同端口上
     * 别的项目的广播也吃进来；只判 `deviceId` 会接受任意 JSON。
     */
    fun parse(text: String?): JSONObject? {
        if (text == null || text.isEmpty() || text.length > MAX_PACKET_CHARS) return null
        val obj = try {
            JSONObject(text)
        } catch (e: Exception) {
            return null
        }
        if (Proto.jStr(obj, "app") != Proto.LS_TAG) return null
        if (Proto.jStr(obj, "deviceId").isEmpty()) return null
        return obj
    }

    const val MAX_PACKET_CHARS = 4096
}
