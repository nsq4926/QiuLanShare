package com.sanqiu.lanshare.probe

import android.os.Build
import java.net.Inet4Address
import java.net.NetworkInterface

/**
 * 环境事实采集（只读）。
 *
 * 设计原则：**每一项都要能回答"它红的时候说明什么"**。
 * 采不到就如实写"未获取"，不要用默认值伪装成功 —— 鸿蒙端那条
 * 「MAC 显示未获取」的口径在这里同样适用。
 */
object EnvProbe {

    fun collect(): List<Pair<String, List<Pair<String, String>>>> = listOf(
        "设备" to deviceRows(),
        "网卡" to nicRows(),
        "存储" to storageRows(),
    )

    private fun deviceRows(): List<Pair<String, String>> = listOf(
        "型号" to Build.MODEL,
        "厂商" to Build.MANUFACTURER,
        "品牌" to Build.BRAND,
        "设备名" to Build.DEVICE,
        "Android" to Build.VERSION.RELEASE,
        "API" to Build.VERSION.SDK_INT.toString(),
        "ABI" to Build.SUPPORTED_ABIS.joinToString(","),
        "指纹" to Build.FINGERPRINT,
    )

    /**
     * 网卡枚举：这里的结果直接决定 P4-2 的发现能否工作。
     *
     * 关注的不是"有几个地址"，而是：
     *  1. 有没有能算出**子网广播地址**的 IPv4（`broadcast != null`）；
     *  2. 有没有把 VPN/虚拟网卡混进来（要按名+地址双判过滤）。
     */
    private fun nicRows(): List<Pair<String, String>> {
        val rows = mutableListOf<Pair<String, String>>()
        val nics = runCatching { NetworkInterface.getNetworkInterfaces()?.toList().orEmpty() }
            .getOrElse {
                rows += "错误" to "枚举网卡失败: ${it.javaClass.simpleName} ${it.message}"
                return rows
            }
        nics.forEach { nic ->
            val name = nic.name ?: "?"
            val mac = runCatching { nic.hardwareAddress }.getOrNull()
            val macText = if (mac == null || mac.isEmpty()) {
                "未获取"
            } else {
                // Byte 直接用 %02X 会因有符号扩展出 3 位，必须先 and 0xFF
                mac.joinToString(":") { "%02X".format(it.toInt() and 0xFF) }
            }
            val addrs = runCatching { nic.interfaceAddresses }.getOrNull().orEmpty()
                .filter { it.address is Inet4Address }
            if (addrs.isEmpty()) {
                // 只列有 IPv4 的网卡，避免被 ipv6/回环刷屏
                return@forEach
            }
            addrs.forEach { ia ->
                // Kotlin 把 Java 的 getBroadcast() 映射为属性 broadcast（不是 broadcastAddress）
                val bcast = ia.broadcast?.hostAddress ?: "无（算不出广播地址）"
                rows += name to "ip=${ia.address.hostAddress} prefix=${ia.networkPrefixLength} " +
                    "bcast=$bcast mac=$macText virtual=${runCatching { nic.isVirtual }.getOrDefault(false)}"
            }
        }
        if (rows.isEmpty()) {
            rows += "结果" to "没有可用的 IPv4 网卡（飞行模式？Wi-Fi 未连？）"
        }
        return rows
    }

    private fun storageRows(): List<Pair<String, String>> {
        val dir = AppPaths.dataDirOrNull()
        return listOf(
            "私有目录" to (dir?.absolutePath ?: "未初始化"),
            "可写" to (if (dir != null && dir.canWrite()) "是" else "否"),
            "总空间" to (if (dir != null) "${dir.totalSpace / 1024 / 1024} MB" else "-"),
            "可用空间" to (if (dir != null) "${dir.usableSpace / 1024 / 1024} MB" else "-"),
        )
    }
}
