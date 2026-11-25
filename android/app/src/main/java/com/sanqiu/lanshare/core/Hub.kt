package com.sanqiu.lanshare.core

import com.sanqiu.lanshare.net.NetInfo
import com.sanqiu.lanshare.net.Nic
import com.sanqiu.lanshare.net.collectNics
import com.sanqiu.lanshare.net.isUsableIPv4
import com.sanqiu.lanshare.net.sameSubnet
import org.json.JSONObject

/**
 * 核心状态机：身份、设备表、消息、会话、未读、histRev。
 *
 * 约束（来自 `docs/05` §三/§六，改之前先看）：
 *  - `deviceId` 只在首次运行生成并持久化，**禁止**由 MAC/主机名派生。
 *  - `seq` 是**本端全局单调**（不是每会话从 1 开始），否则 msgId 跨会话撞号，
 *    接收侧幂等会把第二条当重复**静默丢消息**。
 *  - `msgId = deviceId:epoch:seq`；epoch 在清空历史时 +1。
 *  - 五态判定一律用 `lastReachableAt`（连通过它），`lastSeen` 只决定"还值不值得留在列表里"。
 *  - 冷启动 `lastReachableAt` 恒 0（持久化值只供排障），否则第一屏一堆早已离线的设备显示"在线"。
 */
class LsHub(val store: LsStore) {

    /**
     * 并发闸门：**API 线程与 UI 线程共用这一把**。
     *
     * `HttpServer` 每连接一线程，而这里的 messages/peers 都是可变集合；
     * 界面若绕过这把锁直接读，会撞上 `ConcurrentModificationException` ——
     * 而它发生在遍历半路，表现为"消息列表偶尔少一条"且不报错。
     */
    val gate = Any()

    var self: LsSelf = LsSelf()
    val peers = LinkedHashMap<String, LsPeer>()
    val aliases = LinkedHashMap<String, String>()
    var messages: MutableList<LsMsg> = mutableListOf()
    val convs = LinkedHashMap<String, LsConvInfo>()

    /** 本机是否接收入站（"常驻"开关的语义载体） */
    var acceptsInbound: Boolean = true

    /** 进程内"连通过它"的时刻（不持久化） */
    private val reachableInProc = mutableMapOf<String, Long>()
    private var nextLocalId: Long = 1
    var net: NetInfo = NetInfo()
    var settings: LsSettings = LsSettings()
    private val settingsStore = LsSettingsStore(store)

    /** 删除设备后的自动发现安静期（deviceId → 截止时刻） */
    private val ignoreUntil = mutableMapOf<String, Long>()

    fun loadSettings() {
        settings = settingsStore.load()
        // 单一权威源：开关只认设置文件
        acceptsInbound = settings.acceptsInbound
    }

    fun saveSettings() {
        settingsStore.save(settings)
    }

    // ------------------------------------------------------------ 装载与落盘

    fun load() {
        store.init()
        val file = LsDeviceFile.fromJson(store.readText(store.devicesPath()))
        self = file.self
        peers.clear()
        for (p in file.peers) peers[p.deviceId] = p
        aliases.clear()
        aliases.putAll(file.aliases)
        loadConvs()
        loadHistory()
        refreshNet()
    }

    private fun loadConvs() {
        convs.clear()
        val text = store.readText(store.convsPath())
        if (text.isEmpty()) return
        val o = Proto.parseObj(text)
        val keys = o.keys()
        while (keys.hasNext()) {
            val k = keys.next()
            val v = o.opt(k)
            if (v is JSONObject) convs[k] = LsConvInfo.fromJson(v)
        }
    }

    private fun loadHistory() {
        messages = mutableListOf()
        nextLocalId = 1
        val text = store.readText(store.historyPath())
        if (text.isEmpty()) return
        for (raw in text.split('\n')) {
            val line = raw.trim()
            if (line.isEmpty()) continue
            val m = LsMsg.fromJson(Proto.parseObj(line))
            if (m.id <= 0) m.id = nextLocalId
            if (m.id >= nextLocalId) nextLocalId = m.id + 1
            if (m.msgId.isEmpty()) {
                // 老历史行只读推导，绝不回写
                m.msgId = "legacy:${m.dev}:0:${m.id}"
                m.epoch = 0
                m.seq = m.id
                if (m.dir == Proto.DIR_OUT) {
                    m.from = self.deviceId
                    m.to = "legacy:${m.dev}"
                } else {
                    m.from = "legacy:${m.dev}"
                    m.to = self.deviceId
                }
            }
            if (m.convId.isEmpty()) m.convId = Proto.makeConvId(m.from, m.to)
            if (m.peerTs == 0L) m.peerTs = m.time
            if (m.recvTs == 0L) m.recvTs = m.time
            messages.add(m)
        }
    }

    fun saveDevices() {
        val f = LsDeviceFile()
        f.self = self
        f.peers = peers.values.toMutableList()
        f.aliases = LinkedHashMap(aliases)
        store.writeText(store.devicesPath(), f.toJson())
    }

    fun saveConvs() {
        val parts = convs.entries.joinToString(",") {
            Proto.jsonStr(it.key) + ":" + it.value.toJson()
        }
        store.writeText(store.convsPath(), "{$parts}")
    }

    fun rewriteHistory() {
        val text = messages.joinToString("\n") { it.toJson() }
        store.writeText(store.historyPath(), if (messages.isEmpty()) "" else "$text\n")
    }

    // ------------------------------------------------------------ 身份

    fun ensureDeviceId() {
        if (self.deviceId.isNotEmpty()) return
        // 首次运行生成一次，之后永不改变（由 MAC/主机名派生是错的）
        self.deviceId = Proto.DEVICE_ID_PREFIX + Proto.randomHex(12)
        self.epoch = 1
        saveDevices()
    }

    fun setDeviceName(name: String) {
        self.name = name
        saveDevices()
    }

    fun refreshNet(preferredIfName: String = "") {
        net = collectNics(preferredIfName)
        self.ips = net.nics.map { it.ip }.toMutableList()
        self.filteredNicCount = net.filteredNicCount.toLong()
        self.keptNicCount = net.keptNicCount.toLong()
    }

    /** 本端下一个消息序号（本端全局单调，跨会话） */
    fun nextSeq(): Long {
        self.seq += 1
        saveDevices()
        return self.seq
    }

    fun bumpEpoch() {
        self.epoch += 1
        saveDevices()
    }

    fun newMsgId(): String = Proto.makeMsgId(self.deviceId, self.epoch, nextSeq())

    // ------------------------------------------------------------ 设备表

    fun findPeer(deviceId: String): LsPeer? = peers[deviceId]

    /**
     * 按 deviceId upsert。
     * **绝不能按 IP 去重** —— 同机双实例是两个 deviceId、同一 IP，那是两台设备。
     */
    fun upsertPeer(deviceId: String): LsPeer {
        peers[deviceId]?.let { return it }
        val p = LsPeer(deviceId = deviceId, firstSeen = Proto.nowMs())
        peers[deviceId] = p
        return p
    }

    /** 收到对端 announce：更新身份信息与地址并集 */
    fun applyAnnounce(payload: JSONObject, sourceIp: String) {
        val deviceId = Proto.jStr(payload, "deviceId")
        if (deviceId.isEmpty() || deviceId == self.deviceId) return
        val now = Proto.nowMs()

        // 删除后的安静期：刚删掉的设备不该被下一轮 announce 立刻加回来
        // （只压自动发现 —— 消息入站与手动添加照旧，删除不是拉黑）
        if (isDiscoverySuppressed(deviceId, now)) return

        // 地址先用可用性筛：127/0.0.0.0/169.254 都不是能连的地址；
        // 筛完为空则退回**包里来的源地址**（广播/单播的源地址一定可路由）；
        // 两者都不可用则不登记 —— 否则列表里多一条"点开永远连不上"的设备
        val ipsRaw = Proto.jArr(payload, "ips")
        val usable = mutableListOf<String>()
        for (i in 0 until ipsRaw.length()) {
            val v = ipsRaw.opt(i)
            if (v !is String) continue
            if (!isUsableIPv4(v)) continue
            if (!usable.contains(v)) usable.add(v)
        }
        if (usable.isEmpty() && isUsableIPv4(sourceIp)) usable.add(sourceIp)
        if (usable.isEmpty()) {
            // 已有历史地址的设备仍要更新 lastSeen（否则被误判离线），新设备直接丢
            val old = findPeer(deviceId) ?: return
            if (old.ips.isEmpty()) return
        }

        // 同一台机器换过 deviceId（重装/清数据/测试实例）时旧身份会永久留成"离线设备"，
        // MAC 是"同一台机器"最可靠的证据；**在线的一律不动**（否则同机双实例退化成一台）
        val mac = Proto.jStr(payload, "mac")
        val keptAlias = mergeByMac(deviceId, mac, now)

        val p = upsertPeer(deviceId)
        if (keptAlias.isNotEmpty() && p.alias.isEmpty()) p.alias = keptAlias
        if (mac.isNotEmpty() && p.mac.isNotEmpty() && p.mac != mac) {
            p.macHistory.add(p.mac)
            if (p.macHistory.size > 8) p.macHistory.removeAt(0)
        }
        if (mac.isNotEmpty()) p.mac = mac
        val name = Proto.jStr(payload, "name")
        if (name.isNotEmpty()) p.name = name
        val platform = Proto.jStr(payload, "platform", "windows")
        if (platform.isNotEmpty()) p.platform = platform
        p.apiVersion = Proto.jStr(payload, "apiVersion", p.apiVersion)
        p.resumable = Proto.jBool(payload, "resumable", p.resumable)
        p.filteredNicCount = Proto.jNum(payload, "filteredNicCount", p.filteredNicCount)
        p.acceptsInbound = Proto.jBool(payload, "acceptsInbound", true)
        val port = Proto.jNum(payload, "port")
        if (port > 0) p.port = port.toInt()
        p.lastSeen = now
        p.state = "online"

        for (ip in usable) addPeerIp(p, ip, 0L)
        p.convId = Proto.makeConvId(self.deviceId, p.deviceId)
        saveDevices()
    }

    // ------------------------------------------------------------ 删除压制 / 同 MAC 合并

    fun suppressDiscovery(deviceId: String, ms: Long) {
        if (deviceId.isEmpty()) return
        ignoreUntil[deviceId] = Proto.nowMs() + (if (ms > 0) ms else 0)
    }

    fun isDiscoverySuppressed(deviceId: String, now: Long): Boolean {
        val until = ignoreUntil[deviceId] ?: return false
        return until > now
    }

    /**
     * 按 MAC 把"同一台机器的旧身份"并掉，返回被并掉身份上用户起过的别名。
     * 只并**非在线**的：同机双实例两条都在线且 MAC 相同，无条件合并会把 17b 做废。
     */
    private fun mergeByMac(incomingId: String, mac: String, now: Long): String {
        if (mac.isEmpty() || mac == self.mac) return ""
        var keptAlias = ""
        for (id in peers.keys.toList()) {
            if (id == incomingId) continue
            val p = peers[id] ?: continue
            if (p.mac != mac) continue
            val st = Proto.stateOf(reachableAt(id), p.acceptsInbound, p.lastSeen, now)
            if (st == Proto.ST_ONLINE) continue
            if (p.alias.isNotEmpty()) keptAlias = p.alias
            dropPeer(id)
        }
        return keptAlias
    }

    /**
     * 启动时全表按 MAC 并一次（对应 Windows DedupeByMac）。
     * 保留同 MAC 里 lastSeen 最新的那条，其余删掉并清会话。
     */
    fun dedupeByMac(): Int {
        val now = Proto.nowMs()
        val keeper = mutableMapOf<String, String>()
        for (p in peers.values) {
            if (p.mac.isEmpty() || p.mac == self.mac) continue
            val cur = keeper[p.mac]
            if (cur == null) {
                keeper[p.mac] = p.deviceId
                continue
            }
            val curPeer = peers[cur]
            if (curPeer != null && p.lastSeen > curPeer.lastSeen) keeper[p.mac] = p.deviceId
        }
        var removed = 0
        for (p in peers.values.toList()) {
            if (p.mac.isEmpty() || p.mac == self.mac) continue
            if (keeper[p.mac] == p.deviceId) continue
            val st = Proto.stateOf(reachableAt(p.deviceId), p.acceptsInbound, p.lastSeen, now)
            if (st == Proto.ST_ONLINE) continue
            dropPeer(p.deviceId)
            removed++
        }
        if (removed > 0) {
            saveDevices()
            saveConvs()
        }
        return removed
    }

    /** 删掉一个对端：设备条目 + 它的会话 + 它的消息（合并后不留孤儿会话） */
    fun dropPeer(deviceId: String) {
        peers.remove(deviceId)
        reachableInProc.remove(deviceId)
        val convId = Proto.makeConvId(self.deviceId, deviceId)
        var touched = false
        val kept = mutableListOf<LsMsg>()
        for (m in messages) {
            if (m.convId == convId || m.from == deviceId || m.to == deviceId) {
                touched = true
                continue
            }
            kept.add(m)
        }
        if (touched) {
            messages = kept
            rewriteHistory()
        }
        if (convs.remove(convId) != null) saveConvs()
        saveDevices()
    }

    fun addPeerIp(p: LsPeer, ip: String, lastOkAt: Long) {
        if (ip.isEmpty()) return
        for (it in p.ips) {
            if (it.ip == ip) {
                if (lastOkAt > 0) it.lastOkAt = lastOkAt
                return
            }
        }
        p.ips.add(LsIp(ip, lastOkAt))
        if (p.ips.size > 16) p.ips.removeAt(0)
    }

    /** 一次成功通信后回写该地址的 lastOkAt（失败不回写、不清零） */
    fun touchPeer(deviceId: String, ip: String, port: Int) {
        val p = upsertPeer(deviceId)
        if (port > 0) p.port = port
        if (ip.isNotEmpty()) addPeerIp(p, ip, Proto.nowMs())
        p.lastSeen = Proto.nowMs()
        markReachable(deviceId, Proto.nowMs())
    }

    /**
     * 标记"连通过它"（进程内，冷启动恒 0）。
     * **只标记已登记的对端，不新建条目** —— 否则网页端那个随机 deviceId
     * 每开一个标签就会在设备列表里多出一台"设备"。
     */
    fun markReachable(deviceId: String, at: Long) {
        if (!peers.containsKey(deviceId)) return
        reachableInProc[deviceId] = at
        val p = peers[deviceId] ?: return
        if (at > p.lastSeen) p.lastSeen = at
    }

    fun reachableAt(deviceId: String): Long = reachableInProc[deviceId] ?: 0L

    /**
     * **发送候选**：只打分排序，**不按 TTL 剔除**。
     *
     * 曾经错做成"和展示一样先剔 TTL"，后果是设备离线超过 5 分钟发送候选就空了 →
     * 直接判"对方不可达"，连一次尝试都没有。Windows 的 OrderCandidates 也不过滤 TTL。
     */
    fun sendCandidates(p: LsPeer): List<LsIp> {
        val now = Proto.nowMs()
        val list = p.ips.filter { it.ip.isNotEmpty() && isUsableIPv4(it.ip) }
        return scoreAndSort(list, now)
    }

    /** **展示用**：先剔 TTL 过期项，再按 ①lastOkAt 新鲜 ②同网段 ③原序 排 */
    fun displayIps(p: LsPeer): List<LsIp> {
        val now = Proto.nowMs()
        val list = p.ips.filter {
            if (it.ip.isEmpty() || !isUsableIPv4(it.ip)) return@filter false
            // lastOkAt==0 表示"还没用过"，不能按 TTL 剔 —— 否则刚发现的设备 ips 是空的
            !(it.lastOkAt > 0 && (now - it.lastOkAt) > Proto.IP_FRESH_MS)
        }
        return scoreAndSort(list, now)
    }

    fun sortedIps(p: LsPeer): List<LsIp> = displayIps(p)

    /** 打分排序：① lastOkAt 未过期 +4 ② 同网段 +2 ③ 保持原序（稳定） */
    private fun scoreAndSort(list: List<LsIp>, now: Long): List<LsIp> {
        val scored = list.map { ip ->
            var s = 0
            if (ip.lastOkAt > 0 && (now - ip.lastOkAt) <= Proto.IP_FRESH_MS) s += 4
            if (isSameSubnetWithLocal(ip.ip)) s += 2
            s
        }
        return list.indices.sortedWith(compareByDescending<Int> { scored[it] }.thenBy { it })
            .map { list[it] }
    }

    private fun isSameSubnetWithLocal(ip: String): Boolean =
        net.nics.any { sameSubnet(ip, it.ip, it.prefix) }

    fun convIdOf(peerKey: String): String = Proto.makeConvId(self.deviceId, peerKey)

    // ------------------------------------------------------------ 显示名

    fun aliasOf(deviceId: String): String =
        aliases[deviceId] ?: peers[deviceId]?.alias ?: ""

    fun realNameOf(deviceId: String): String = peers[deviceId]?.name ?: ""

    /**
     * 改别名。空串 = 清除（恢复对端自报名）。
     * 同时写 peers.alias 与 aliases 两张表：前者跟设备走，后者在设备被合并/重建时兜住。
     */
    fun setAlias(deviceId: String, alias: String) {
        val name = alias.trim()
        if (name.isEmpty()) {
            aliases.remove(deviceId)
            peers[deviceId]?.alias = ""
        } else {
            aliases[deviceId] = name
            peers[deviceId]?.alias = name
        }
        saveDevices()
    }

    fun displayName(peerKey: String): String {
        aliases[peerKey]?.let { if (it.isNotEmpty()) return it }
        peers[peerKey]?.let { p ->
            if (p.alias.isNotEmpty()) return p.alias
            if (p.name.isNotEmpty()) return p.name
            return if (peerKey.length >= 8) peerKey.substring(0, 8) else peerKey
        }
        if (peerKey.startsWith("legacy:")) {
            val n = peerKey.substring(7)
            return if (n.isNotEmpty()) n else "未知设备"
        }
        if (peerKey.startsWith("unknown:")) {
            val n = peerKey.substring(8)
            return if (n.isNotEmpty()) n else "未知设备"
        }
        return "未知设备"
    }

    /** 设备列表（`/api/devices` 的数据源，每行一段 JSON） */
    fun deviceRows(now: Long): List<String> {
        val rows = mutableListOf<DeviceRow>()
        for (p in peers.values) {
            if (p.ephemeral) continue // 网页端身份不进设备表
            val st = Proto.stateOf(reachableAt(p.deviceId), p.acceptsInbound, p.lastSeen, now)
            val ips = displayIps(p)
            val convId = if (p.convId.isNotEmpty()) p.convId else Proto.makeConvId(self.deviceId, p.deviceId)
            val stat = convStat(convId)
            val rev = histRev(convId)
            val ipJson = ips.joinToString(",") { it.toJson() }
            val json = "{\"deviceId\":${Proto.jsonStr(p.deviceId)}" +
                ",\"displayName\":${Proto.jsonStr(displayName(p.deviceId))}" +
                ",\"name\":${Proto.jsonStr(p.name)}" +
                ",\"alias\":${Proto.jsonStr(p.alias)}" +
                ",\"platform\":${Proto.jsonStr(p.platform)}" +
                ",\"mac\":${Proto.jsonStr(p.mac)}" +
                ",\"port\":${p.port}" +
                ",\"state\":${Proto.jsonStr(st)}" +
                ",\"acceptsInbound\":${if (p.acceptsInbound) "true" else "false"}" +
                ",\"lastSeen\":${p.lastSeen}" +
                ",\"lastReachableAt\":${reachableAt(p.deviceId)}" +
                ",\"ips\":[$ipJson]" +
                ",\"convId\":${Proto.jsonStr(convId)}" +
                ",\"unread\":${stat.unread}" +
                ",\"histRev\":$rev" +
                ",\"apiVersion\":${Proto.jsonStr(p.apiVersion)}" +
                ",\"filteredNicCount\":${p.filteredNicCount}}"
            rows.add(DeviceRow(Proto.stateRank(st), p.lastSeen, p.deviceId, json))
        }
        rows.sortWith(
            compareBy<DeviceRow> { it.rank }.thenByDescending { it.lastSeen }.thenBy { it.deviceId }
        )
        return rows.take(200).map { it.json }
    }

    /** 预看下一个可用 id（不消耗）。入站消息要在算 msgId 前先拿到 id */
    fun peekNextId(): Long = nextLocalId

    /** 设备列表（UI 用，结构化） */
    fun deviceItems(now: Long): List<LsDeviceItem> {
        val out = mutableListOf<LsDeviceItem>()
        for (p in peers.values) {
            if (p.ephemeral) continue
            val item = LsDeviceItem()
            item.deviceId = p.deviceId
            item.displayName = displayName(p.deviceId)
            item.realName = p.name
            item.platform = p.platform
            val ips = displayIps(p)
            item.ip = ips.firstOrNull()?.ip ?: ""
            item.mac = p.mac
            item.state = Proto.stateOf(reachableAt(p.deviceId), p.acceptsInbound, p.lastSeen, now)
            item.acceptsInbound = p.acceptsInbound
            val convId = if (p.convId.isNotEmpty()) p.convId else Proto.makeConvId(self.deviceId, p.deviceId)
            val stat = convStat(convId)
            item.unread = stat.unread
            item.lastTs = stat.lastTs
            lastMessage(convId)?.let { item.preview = previewOf(it) }
            out.add(item)
        }
        // 排序：**最近有消息的排最前**（收发即置顶），其次状态、再按名字。
        // 与 Windows/网页端"排序只认消息时间"的口径一致。
        out.sortWith(
            compareByDescending<LsDeviceItem> { it.lastTs }
                .thenBy { Proto.stateRank(it.state) }
                .thenBy { it.displayName }
        )
        return out
    }

    // ------------------------------------------------------------ 消息

    fun appendMessage(m: LsMsg) {
        // 幂等兜底：同 msgId 只入库一次。
        //
        // 调用方（Api.buildInbound）已经查过一次重，但那里返回"已存在的那条"之后，
        // 调用方仍会走 appendMessage —— 结果是同一条被 push 两次：
        // 界面上两个气泡、磁盘上两行、重启后还是两条。
        // 放在这里而不是只靠调用方，是因为入库点只有这一个，守住它就够了。
        if (m.msgId.isNotEmpty() && messages.any { it.msgId == m.msgId }) return

        if (m.id <= 0) {
            m.id = nextLocalId
            nextLocalId = m.id + 1
        }
        if (m.id >= nextLocalId) nextLocalId = m.id + 1
        messages.add(m)
        store.appendLine(store.historyPath(), m.toJson())
        if (m.convId.isNotEmpty()) {
            // 会话登记的 peer 是**对端**：出站看 to，入站看 from。
            // 写成 m.from 时出站消息会把本机自己登记成对端，界面点进去找不到人。
            val peerId = if (m.dir == Proto.DIR_OUT) m.to else m.from
            val info = ensureConv(m.convId, peerId)
            if (m.dev.isNotEmpty()) info.lastKnownName = m.dev
            saveConvs()
        }
    }

    fun ensureConv(convId: String, peerDeviceId: String): LsConvInfo {
        val existing = convs[convId]
        if (existing == null) {
            val c = LsConvInfo(peerDeviceId = peerDeviceId, rev = 0)
            convs[convId] = c
            return c
        }
        if (peerDeviceId.isNotEmpty() && existing.peerDeviceId != peerDeviceId) {
            existing.peerDeviceId = peerDeviceId
        }
        return existing
    }

    /**
     * 会话行显示名。顺序与 Windows ConversationsJson 一致：
     * 会话级别名 > 设备表 alias > name > **lastKnownName** > 去 legacy: 前缀 > 未知设备。
     * `lastKnownName` 这一档不能省：设备被删或改名后，会话行要还能显示"当时那个名字"。
     */
    fun convDisplayName(info: LsConvInfo): String {
        val a = aliases[info.peerDeviceId]
        if (a != null && a.isNotEmpty()) return a
        if (peers.containsKey(info.peerDeviceId)) return displayName(info.peerDeviceId)
        if (info.lastKnownName.isNotEmpty()) return info.lastKnownName
        if (info.peerDeviceId.startsWith("legacy:")) {
            val n = info.peerDeviceId.substring(7)
            if (n.isNotEmpty()) return n
        }
        if (info.peerDeviceId.startsWith("unknown:")) {
            val n = info.peerDeviceId.substring(8)
            if (n.isNotEmpty()) return n
        }
        return "未知设备"
    }

    fun histRev(convId: String): Long = convs[convId]?.rev ?: 0L

    data class ConvStat(val count: Long, val unread: Long, val lastTs: Long)

    /** 会话统计：[消息条数, 未读数, 最后一条消息时间] */
    fun convStat(convId: String): ConvStat {
        var count = 0L
        var unread = 0L
        var lastTs = 0L
        for (m in messages) {
            if (m.convId != convId) continue
            count++
            if (m.isUnreadCountable()) unread++
            if (m.time > lastTs) lastTs = m.time
        }
        return ConvStat(count, unread, lastTs)
    }

    /** 会话最后一条消息（按 time, seq），没有则 null */
    fun lastMessage(convId: String): LsMsg? {
        var best: LsMsg? = null
        for (m in messages) {
            if (m.convId != convId) continue
            val b = best
            if (b == null || m.time > b.time || (m.time == b.time && m.seq > b.seq)) best = m
        }
        return best
    }

    fun totalUnread(): Long = messages.count { it.isUnreadCountable() }.toLong()

    /**
     * 拉取消息。过滤顺序与 Windows QueryMessages 逐字一致：
     *  1. legacy 通道跳过 system
     *  2. legacy 通道只看 from/to 以 `legacy:` 开头的
     *  3. 全局 id 水位（since）**始终**生效
     *  4. peer 非空 → 会话过滤 + per-peer (epoch,seq) 水位
     */
    fun queryMessages(
        peer: String,
        sinceEpoch: Long,
        sinceSeq: Long,
        since: Long,
        legacy: Boolean,
    ): List<LsMsg> {
        val out = mutableListOf<LsMsg>()
        for (m in messages) {
            if (legacy) {
                if (m.kind == Proto.KIND_SYSTEM) continue
                if (!m.from.startsWith("legacy:") && !m.to.startsWith("legacy:")) continue
            }
            if (since > 0 && m.id <= since) continue
            if (peer.isNotEmpty()) {
                if (m.from != peer && m.to != peer) continue
                if (sinceEpoch >= 0) {
                    if (m.epoch < sinceEpoch) continue
                    if (m.epoch == sinceEpoch && m.seq <= sinceSeq) continue
                }
            }
            out.add(m)
        }
        out.sortWith(compareBy<LsMsg> { it.peerTs }.thenBy { it.seq }.thenBy { it.id })
        return out
    }

    /** 该对端已知的最大 (epoch, seq) */
    fun peerWatermark(peer: String): LongArray {
        var epoch = 0L
        var seq = 0L
        for (m in messages) {
            if (m.from != peer) continue
            if (m.epoch > epoch || (m.epoch == epoch && m.seq > seq)) {
                epoch = m.epoch
                seq = m.seq
            }
        }
        return longArrayOf(epoch, seq)
    }

    /** 未读清零（**唯一入口**） */
    fun markRead(convId: String): Long {
        var cleared = 0L
        for (m in messages) {
            if (m.convId != convId && m.from != convId && m.to != convId) continue
            if (m.unread) {
                m.unread = false
                cleared++
            }
        }
        if (cleared > 0) rewriteHistory()
        return cleared
    }

    /** 清空某会话（只清本端） */
    fun clearConversation(convId: String): Long {
        val removed = messages.count { it.convId == convId }.toLong()
        messages = messages.filter { it.convId != convId }.toMutableList()
        val info = ensureConv(convId, "")
        info.rev += 1
        bumpEpoch()
        rewriteHistory()
        saveConvs()
        return removed
    }

    /**
     * 按保留期裁掉旧消息，返回删除条数。
     * 只剩空壳的会话记录也一并删掉（消息没了、会话还在 → 列表里点进去空白）。
     */
    fun purgeBefore(cutoff: Long): Long {
        if (cutoff <= 0) return 0L
        val removed = messages.count { it.time < cutoff }.toLong()
        if (removed == 0L) return 0L
        messages = messages.filter { it.time >= cutoff }.toMutableList()
        rewriteHistory()

        // 清掉没有任何消息的会话壳
        val alive = messages.map { it.convId }.toHashSet()
        val deadKeys = convs.keys.filter { !alive.contains(it) }
        if (deadKeys.isNotEmpty()) {
            for (k in deadKeys) convs.remove(k)
            saveConvs()
        }
        return removed
    }

    /** 删除单条消息（长按菜单）；返回是否删掉 */
    fun removeMessage(msgId: String, id: Long): Boolean {
        var hit = false
        val kept = mutableListOf<LsMsg>()
        for (m in messages) {
            if (!hit && ((msgId.isNotEmpty() && m.msgId == msgId) || (msgId.isEmpty() && m.id == id))) {
                hit = true
                continue
            }
            kept.add(m)
        }
        if (!hit) return false
        messages = kept
        rewriteHistory()
        return true
    }

    /** 把消息 ID 水位压回 0（清空历史后） */
    fun resetWatermark() {
        nextLocalId = 1
        bumpEpoch()
    }

    fun clearAll() {
        messages = mutableListOf()
        for (c in convs.values) c.rev += 1
        bumpEpoch()
        rewriteHistory()
        saveConvs()
    }

    /** 会话列表（`/api/conversations`，每行一段 JSON） */
    fun conversationRows(): List<String> {
        val rows = mutableListOf<ConvRow>()
        for ((convId, info) in convs) {
            val stat = convStat(convId)
            var lastTs = 0L
            var lastId = 0L
            for (m in messages) {
                if (m.convId != convId) continue
                if (m.peerTs > lastTs) {
                    lastTs = m.peerTs
                    lastId = m.id
                }
            }
            val json = "{\"convId\":${Proto.jsonStr(convId)}" +
                ",\"deviceId\":${Proto.jsonStr(info.peerDeviceId)}" +
                ",\"displayName\":${Proto.jsonStr(convDisplayName(info))}" +
                ",\"lastTs\":$lastTs" +
                ",\"lastMessageId\":$lastId" +
                ",\"count\":${stat.count}" +
                ",\"unread\":${stat.unread}" +
                ",\"legacy\":${if (info.peerDeviceId.startsWith("legacy:")) "true" else "false"}" +
                ",\"histRev\":${info.rev}}"
            rows.add(ConvRow(lastTs, json))
        }
        rows.sortByDescending { it.lastTs }
        return rows.map { it.json }
    }

    /** 某个会话的消息（UI 用） */
    fun conversationMessages(peerKey: String): List<LsMsg> {
        val out = messages.filter { it.from == peerKey || it.to == peerKey }.toMutableList()
        out.sortWith(compareBy<LsMsg> { it.peerTs }.thenBy { it.seq }.thenBy { it.id })
        return out
    }

    /** 把一个名字/标识解析成会话键 */
    fun resolvePeerKey(nameOrId: String): String {
        if (nameOrId.isEmpty()) return "unknown:anonymous"
        if (peers.containsKey(nameOrId)) return nameOrId
        for (p in peers.values) {
            if (p.name == nameOrId || p.alias == nameOrId) return p.deviceId
        }
        return "unknown:$nameOrId"
    }

    fun legacyKey(dev: String): String = Proto.makeLegacyKey(dev)

    companion object {
        /** 列表里的消息摘要：文件只给类型不给文件名（列表窄，长文件名会占满整行） */
        fun previewOf(m: LsMsg): String = when (m.kind) {
            Proto.KIND_IMAGE -> "[图片]"
            Proto.KIND_FILE -> "[文件]"
            else -> m.text
        }
    }
}

/** 排序中间结构（Kotlin 不允许在函数里定义 data class，故提到顶层） */
private class DeviceRow(val rank: Int, val lastSeen: Long, val deviceId: String, val json: String)

private class ConvRow(val lastTs: Long, val json: String)
