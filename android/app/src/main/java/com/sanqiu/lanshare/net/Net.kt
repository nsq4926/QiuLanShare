package com.sanqiu.lanshare.net

import java.net.Inet4Address
import java.net.NetworkInterface

/**
 * 本机网卡与广播地址计算。
 *
 * 与鸿蒙（`common/Net.ets`）同构，四处差异：
 *  1. Android **有** `NetworkInterface.isVirtual`，但它对 `virnic` 这类厂商虚拟网卡返回 false，
 *     所以**不能只靠它**（MatePad 实测：`virnic` 10.213.168.4/32，isVirtual=false）。
 *  2. 本机 MAC 同样取不到（API 23 起返回 null / 02:00:…）→ 一律「未获取」。
 *  3. 多了「默认网卡优先」：`ConnectivityManager` 能给出当前上网的接口名，让 `primaryIp()` 更准。
 *  4. **新增一条硬规则：算不出子网广播的网卡不算可用**（见 [pickNics]）。
 *     理由：广播发现的前提就是能广播；`/32` 网卡（如 MatePad 的 virnic）塞进 ips[] 会让
 *     对端拿到一个连不上的地址，而症状只是「能发现、连不上」。
 */

/** 一张可用网卡的地址信息 */
data class Nic(
    val ip: String,
    val prefix: Int,
    val ifName: String,
)

/** 本机网络概况 */
data class NetInfo(
    val nics: List<Nic> = emptyList(),
    val filteredNicCount: Int = 0,
    val keptNicCount: Int = 0,
) {
    /** 对外主地址：优先默认网卡，其次第一张；没有则空串 */
    fun primaryIp(): String = nics.firstOrNull()?.ip ?: ""
}

/** 虚拟/隧道网卡名前缀（命中即排除）。`virnic` 是华为的虚拟网卡，实测存在于 MatePad。 */
private val VIRTUAL_PREFIX = listOf(
    "tun", "tap", "ppp", "ipsec", "vpn", "docker", "veth", "br-", "rmnet",
    "rmnet_data", "ibt", "cdc", "wwan", "usbmodem", "virnic", "p2p", "dummy",
)

fun isVirtualIface(name: String): Boolean {
    val n = name.lowercase()
    return VIRTUAL_PREFIX.any { n.startsWith(it) }
}

/** 是否可用 IPv4（排除环回 / 链路本地 / 无效） */
fun isUsableIPv4(ip: String): Boolean {
    if (ip.isEmpty() || ip == "0.0.0.0") return false
    if (ip.startsWith("127.")) return false
    if (ip.startsWith("169.254.")) return false
    val parts = ip.split('.')
    if (parts.size != 4) return false
    for (p in parts) {
        if (p.isEmpty()) return false
        val v = p.toIntOrNull() ?: return false
        if (v < 0 || v > 255) return false
    }
    return true
}

fun ipv4ToLong(ip: String): Long {
    val p = ip.split('.')
    return (p[0].toLong() shl 24) or (p[1].toLong() shl 16) or (p[2].toLong() shl 8) or p[3].toLong()
}

fun longToIpv4(v: Long): String {
    val x = v and 0xFFFFFFFFL
    return "${(x shr 24) and 0xFF}.${(x shr 16) and 0xFF}.${(x shr 8) and 0xFF}.${x and 0xFF}"
}

/**
 * 子网广播地址。
 *
 * `/31`（点对点）与 `/32`（单地址）**没有广播地址**，必须返回空串 ——
 * 靠公式兜底会算出个不存在的地址，包发出去没人收，而且不报错。
 */
fun subnetBroadcast(ip: String, prefix: Int): String {
    if (!isUsableIPv4(ip)) return ""
    if (prefix < 0 || prefix > 30) return ""
    val n = ipv4ToLong(ip)
    val mask = if (prefix == 0) 0L else (0xFFFFFFFFL shl (32 - prefix)) and 0xFFFFFFFFL
    val b = (n or (mask.inv() and 0xFFFFFFFFL)) and 0xFFFFFFFFL
    return longToIpv4(b)
}

/** 两个 IPv4 是否同网段（按前缀逐位比较，不是"前三段相同"） */
fun sameSubnet(a: String, b: String, prefix: Int): Boolean {
    if (!isUsableIPv4(a) || !isUsableIPv4(b)) return false
    if (prefix <= 0 || prefix > 32) return false
    val mask = if (prefix == 0) 0L else (0xFFFFFFFFL shl (32 - prefix)) and 0xFFFFFFFFL
    return (ipv4ToLong(a) and mask) == (ipv4ToLong(b) and mask)
}

/**
 * 从枚举结果里挑可用网卡（纯函数，便于单测）。
 *
 * 三层，与 Windows/鸿蒙的"四步流水线"对齐，但把顺序改成**先看能不能广播**：
 *  1. 硬过滤：IPv4 必须可用；
 *  2. 硬过滤：**算不出子网广播的（/31、/32）直接排除**，不参与后续放宽；
 *  3. 软过滤：名字像虚拟网卡的先排除；若一个不剩则放宽这一层（护栏）。
 *
 * `preferredIfName` 命中的网卡排到最前（`primaryIp()` 用它）。
 */
fun pickNics(all: List<Nic>, preferredIfName: String = ""): NetInfo {
    val usableAddr = all.filter { isUsableIPv4(it.ip) }
    if (usableAddr.isEmpty()) return NetInfo()

    val broadcastable = usableAddr.filter { subnetBroadcast(it.ip, it.prefix).isNotEmpty() }
    // 全部都是 /31 或 /32：说明本机没有可广播的局域网口（热点/点对点）
    if (broadcastable.isEmpty()) {
        return NetInfo(nics = emptyList(), filteredNicCount = usableAddr.size, keptNicCount = 0)
    }

    val named = broadcastable.filter { !isVirtualIface(it.ifName) }
    val (kept, filteredOut) = if (named.isNotEmpty()) {
        named to (broadcastable.size - named.size)
    } else {
        // 护栏：名字规则把能广播的网卡全排除了，宁可放宽也不用空列表
        broadcastable to 0
    }

    val ordered = kept.sortedWith(
        compareByDescending<Nic> { preferredIfName.isNotEmpty() && it.ifName == preferredIfName }
            .thenByDescending { it.prefix in 8..30 }
            .thenBy { it.ifName }
            .thenBy { it.ip }
    )
    return NetInfo(
        nics = ordered,
        filteredNicCount = (all.size - kept.size).coerceAtLeast(0),
        keptNicCount = kept.size,
    )
}

/** 广播目标集合：受限广播 + 每张网卡的子网广播（去重） */
fun broadcastTargets(nics: List<Nic>): List<String> {
    val out = linkedSetOf<String>()
    out += "255.255.255.255"
    for (n in nics) {
        val b = subnetBroadcast(n.ip, n.prefix)
        if (b.isNotEmpty()) out += b
    }
    return out.toList()
}

/**
 * 枚举本机网卡。
 *
 * 只用 `NetworkInterface`（无需 ConnectivityManager 权限），默认网卡名由调用方传入
 * （Android 侧从 `ConnectivityManager.getLinkProperties().interfaceName` 取）。
 */
fun collectNics(preferredIfName: String = ""): NetInfo {
    val all = mutableListOf<Nic>()
    val ifaces = try {
        NetworkInterface.getNetworkInterfaces()?.toList().orEmpty()
    } catch (e: Exception) {
        return NetInfo()
    }
    for (nic in ifaces) {
        val name = nic.name ?: continue
        if (name == "lo") continue
        val addrs = try {
            nic.interfaceAddresses
        } catch (e: Exception) {
            continue
        }.orEmpty()
        for (ia in addrs) {
            val addr = ia.address
            if (addr !is Inet4Address) continue
            val ip = addr.hostAddress ?: continue
            all += Nic(ip = ip, prefix = ia.networkPrefixLength.toInt(), ifName = name)
        }
    }
    return pickNics(all, preferredIfName)
}
