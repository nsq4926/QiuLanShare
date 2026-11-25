package com.sanqiu.lanshare.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Add
import androidx.compose.material.icons.outlined.Close
import androidx.compose.material.icons.outlined.DeleteOutline
import androidx.compose.material.icons.outlined.Edit
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.sanqiu.lanshare.core.LsDeviceItem
import com.sanqiu.lanshare.core.Proto
import com.sanqiu.lanshare.net.NodeSnapshot
import com.sanqiu.lanshare.net.NodeState
import com.sanqiu.lanshare.ui.components.LsDeviceName
import com.sanqiu.lanshare.ui.components.LsIconSquare
import com.sanqiu.lanshare.ui.components.LsPrimaryButton
import com.sanqiu.lanshare.ui.components.LsTextField
import com.sanqiu.lanshare.ui.components.StatusDot
import com.sanqiu.lanshare.ui.components.deviceLabel
import com.sanqiu.lanshare.ui.theme.LsMotion
import com.sanqiu.lanshare.ui.theme.LsRadius
import com.sanqiu.lanshare.ui.theme.LsSpace
import com.sanqiu.lanshare.ui.theme.LsTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * 设备抽屉（`UI Design.md` §4.2）。
 *
 * 面板四角 24、四周留 12dp（不贴状态栏、不压手势条），从左侧滑入；
 * 列表项是**独立卡片**而不是密集分割线，状态点 7dp 紧贴设备名。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DeviceDrawerPanel(
    snap: NodeSnapshot,
    selected: String?,
    onSelect: (String) -> Unit,
    onClose: () -> Unit,
    onRefresh: () -> Unit,
) {
    val c = LsTheme.colors
    val scope = rememberCoroutineScope()

    // 抽屉宽度随内容走：按"最长那条设备名"实测宽度推算，再夹在合理区间里。
    // 固定 304dp 的坏处是长名字被截断、短名字又留一大片空白。
    val drawerWidth = rememberDrawerWidth(snap)

    var renameTarget by remember { mutableStateOf<LsDeviceItem?>(null) }
    var deleteTarget by remember { mutableStateOf<LsDeviceItem?>(null) }
    var addText by remember { mutableStateOf("") }
    var addBusy by remember { mutableStateOf(false) }
    var addError by remember { mutableStateOf("") }
    var refreshing by remember { mutableStateOf(false) }

    // 从左侧滑入（240–320ms，位移 + 透明度）
    androidx.compose.animation.AnimatedVisibility(
        visible = true,
        enter = slideInHorizontally(tween(LsMotion.Page, easing = LsMotion.easeOut)) { -it / 4 } +
            fadeIn(tween(LsMotion.Normal)),
    ) {
        Surface(
            modifier = Modifier
                .statusBarsPadding()
                .padding(start = LsSpace.s3, top = LsSpace.s3, bottom = LsSpace.s3)
                .width(drawerWidth)
                .fillMaxHeight(),
            shape = RoundedCornerShape(LsRadius.dialog),
            color = c.panel,
            border = null,
            shadowElevation = 24.dp,
        ) {
            Column(modifier = Modifier.fillMaxHeight().imePadding()) {
                // 标题
                Row(
                    modifier = Modifier.fillMaxWidth().padding(start = LsSpace.s5, end = LsSpace.s3, top = LsSpace.s5),
                    verticalAlignment = Alignment.Top,
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text("设备与会话", style = MaterialTheme.typography.titleLarge, color = c.text)
                        Spacer(Modifier.height(LsSpace.s1))
                        Text(
                            "${snap.devices.size} 台设备 · 同一局域网自动发现",
                            style = MaterialTheme.typography.bodySmall,
                            color = c.faint,
                        )
                    }
                    LsIconSquare(Icons.Outlined.Close, "关闭", onClose)
                }

                Spacer(Modifier.height(LsSpace.s3))

                // 下拉即搜：立刻发一次 announce，不等下一个周期
                PullToRefreshBox(
                    isRefreshing = refreshing,
                    onRefresh = {
                        refreshing = true
                        NodeState.node?.discoverNow()
                        scope.launch {
                            kotlinx.coroutines.delay(900)
                            refreshing = false
                            onRefresh()
                        }
                    },
                    modifier = Modifier.weight(1f).fillMaxWidth(),
                ) {
                    if (snap.devices.isEmpty()) {
                        Box(
                            modifier = Modifier.fillMaxSize().padding(LsSpace.s6),
                            contentAlignment = Alignment.Center,
                        ) {
                            Text(
                                "还没有发现设备。\n下拉立即搜索；确认对端在同一 Wi-Fi、且都开着「被发现」。",
                                style = MaterialTheme.typography.bodySmall,
                                color = c.faint,
                                textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                            )
                        }
                    } else {
                        LazyColumn(
                            modifier = Modifier.fillMaxSize().padding(horizontal = LsSpace.s3),
                            verticalArrangement = Arrangement.spacedBy(LsSpace.s2),
                            contentPadding = androidx.compose.foundation.layout.PaddingValues(
                                top = 4.dp, bottom = LsSpace.s2
                            ),
                        ) {
                            items(snap.devices, key = { it.deviceId }) { d ->
                                DeviceRow(
                                    item = d,
                                    active = d.deviceId == selected,
                                    onClick = { onSelect(d.deviceId) },
                                    onRename = { renameTarget = d },
                                    onDelete = { deleteTarget = d },
                                )
                            }
                        }
                    }
                }

                // 搜索提示：明确告诉用户"这里能下拉"，并给一个点了就搜的入口
                Row(
                    modifier = Modifier.fillMaxWidth().padding(start = LsSpace.s5, end = LsSpace.s5),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        if (refreshing) "↻ 正在搜索附近设备…" else "↓ 下拉搜索设备",
                        style = MaterialTheme.typography.labelSmall,
                        color = c.faint,
                        modifier = Modifier.weight(1f),
                    )
                    if (!refreshing) {
                        Text(
                            "立即搜索",
                            style = MaterialTheme.typography.labelSmall,
                            color = c.accent,
                            modifier = Modifier.clickable {
                                refreshing = true
                                NodeState.node?.discoverNow()
                                scope.launch {
                                    kotlinx.coroutines.delay(900)
                                    refreshing = false
                                    onRefresh()
                                }
                            },
                        )
                    }
                }

                // 手动添加
                Spacer(Modifier.height(LsSpace.s2))
                Column(modifier = Modifier.fillMaxWidth().padding(horizontal = LsSpace.s3)) {
                    if (addError.isNotEmpty()) {
                        Text(
                            addError,
                            style = MaterialTheme.typography.labelSmall,
                            color = c.danger,
                            modifier = Modifier.padding(start = LsSpace.s2, bottom = LsSpace.s1),
                        )
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        LsTextField(
                            value = addText,
                            onValueChange = {
                                addText = it
                                addError = ""
                            },
                            modifier = Modifier.weight(1f),
                            placeholder = "IP 或 IP:端口",
                            leading = {
                                Icon(
                                    Icons.Outlined.Add,
                                    contentDescription = null,
                                    tint = c.sub,
                                    modifier = Modifier.size(18.dp),
                                )
                            },
                        )
                        Spacer(Modifier.width(LsSpace.s2))
                        LsPrimaryButton(
                            text = if (addBusy) "连接中" else "添加",
                            enabled = !addBusy && addText.isNotBlank(),
                            onClick = {
                                val raw = addText.trim()
                                addBusy = true
                                addError = ""
                                scope.launch {
                                    val err = withContext(Dispatchers.IO) { addDevice(raw) }
                                    addBusy = false
                                    if (err.isNotEmpty()) {
                                        addError = err
                                    } else {
                                        addText = ""
                                        onRefresh()
                                    }
                                }
                            },
                        )
                    }
                }
                Spacer(Modifier.height(LsSpace.s3))
            }
        }
    }

    // 改名弹窗
    renameTarget?.let { target ->
        var text by remember(target.deviceId) { mutableStateOf(target.displayName) }
        AlertDialog(
            onDismissRequest = { renameTarget = null },
            shape = RoundedCornerShape(LsRadius.dialog),
            containerColor = c.panel,
            title = { Text("重命名设备", color = c.text) },
            text = {
                Column {
                    Text(
                        "留空恢复对端自报名",
                        style = MaterialTheme.typography.bodySmall,
                        color = c.faint,
                    )
                    Spacer(Modifier.height(LsSpace.s3))
                    LsTextField(value = text, onValueChange = { text = it }, placeholder = "设备名")
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    val hub = NodeState.node?.hub
                    if (hub != null) {
                        synchronized(hub.gate) { hub.setAlias(target.deviceId, text) }
                    }
                    renameTarget = null
                    onRefresh()
                }) { Text("保存", color = c.accent) }
            },
            dismissButton = {
                TextButton(onClick = { renameTarget = null }) { Text("取消", color = c.sub) }
            },
        )
    }

    // 删除确认（先报文件数，不直接删）
    deleteTarget?.let { target ->
        val info = remember(target.deviceId) {
            val hub = NodeState.node?.hub
            if (hub == null) {
                Triple("", 0L, 0L)
            } else {
                synchronized(hub.gate) {
                    val display = hub.displayName(target.deviceId)
                    val dir = hub.store.peerDir(target.deviceId, display)
                    val stat = hub.store.statDir(dir)
                    Triple(dir.absolutePath, stat[0], stat[1])
                }
            }
        }
        AlertDialog(
            onDismissRequest = { deleteTarget = null },
            shape = RoundedCornerShape(LsRadius.dialog),
            containerColor = c.panel,
            title = { Text("删除设备", color = c.text) },
            text = {
                Column {
                    Text(
                        "将删除「${deviceLabel(target.displayName, target.realName)}」及其聊天记录与收件文件（${info.second} 个，${formatSize(info.third)}）。",
                        style = MaterialTheme.typography.bodyLarge,
                        color = c.text,
                    )
                    Spacer(Modifier.height(LsSpace.s2))
                    Text(
                        "删除只是本机移除，不会拉黑；对端再次广播时仍可能出现。",
                        style = MaterialTheme.typography.bodySmall,
                        color = c.faint,
                    )
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    val hub = NodeState.node?.hub
                    if (hub != null) {
                        synchronized(hub.gate) {
                            val display = hub.displayName(target.deviceId)
                            val dir = hub.store.peerDir(target.deviceId, display)
                            hub.store.removeDirRecursive(dir)
                            hub.dropPeer(target.deviceId)
                            hub.suppressDiscovery(target.deviceId, 60_000)
                        }
                    }
                    deleteTarget = null
                    if (selected == target.deviceId) onClose()
                    onRefresh()
                }) { Text("删除", color = c.danger) }
            },
            dismissButton = {
                TextButton(onClick = { deleteTarget = null }) { Text("取消", color = c.sub) }
            },
        )
    }
}

/** 设备卡片：状态点 + 名称/状态/平台·IP + 改名/删除 */
@Composable
private fun DeviceRow(
    item: LsDeviceItem,
    active: Boolean,
    onClick: () -> Unit,
    onRename: () -> Unit,
    onDelete: () -> Unit,
) {
    val c = LsTheme.colors
    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick),
        shape = RoundedCornerShape(LsRadius.card),
        color = if (active) c.sel else c.card,
        border = androidx.compose.foundation.BorderStroke(1.dp, c.border),
    ) {
        Row(
            modifier = Modifier.padding(start = LsSpace.s4, end = LsSpace.s2, top = LsSpace.s3, bottom = LsSpace.s3),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    StatusDot(item.state)
                    Spacer(Modifier.width(LsSpace.s2))
                    LsDeviceName(
                        name = item.displayName,
                        realName = item.realName,
                        style = MaterialTheme.typography.titleSmall,
                        color = c.text,
                        modifier = Modifier.weight(1f, fill = false),
                    )
                    if (item.unread > 0) {
                        Spacer(Modifier.width(LsSpace.s2))
                        Box(
                            modifier = Modifier
                                .background(c.accent, RoundedCornerShape(LsRadius.small))
                                .padding(horizontal = 6.dp, vertical = 1.dp)
                        ) {
                            Text(
                                if (item.unread > 99) "99+" else item.unread.toString(),
                                style = MaterialTheme.typography.labelSmall,
                                color = c.onAccent,
                            )
                        }
                    }
                }
                Spacer(Modifier.height(2.dp))
                Text(Proto.stateLabel(item.state), style = MaterialTheme.typography.bodySmall, color = c.sub)
                if (item.ip.isNotEmpty() || item.platform.isNotEmpty()) {
                    Text(
                        listOf(item.platform.ifEmpty { "未知平台" }, item.ip.ifEmpty { "未获取" }).joinToString(" · "),
                        style = MaterialTheme.typography.labelSmall,
                        color = c.faint,
                        maxLines = 1,
                    )
                }
            }
            LsIconSquare(Icons.Outlined.Edit, "重命名", onRename, tint = c.sub, plain = true)
            Spacer(Modifier.width(2.dp))
            LsIconSquare(Icons.Outlined.DeleteOutline, "删除", onDelete, tint = c.danger, plain = true)
        }
    }
}

/**
 * 抽屉的目标宽度：让最长设备名**一行放得下**，再夹在 [280, 屏幕 92% 或 428] 之间。
 *
 * 用 `TextMeasurer` 实测而不是按字数估：中英文/全角括号的宽度差很多，估出来的值必然偏。
 * 固定的部分（状态点、未读徽标、两个图标按钮、各处内边距）单独累加，与卡片布局一一对应。
 */
@Composable
private fun rememberDrawerWidth(snap: NodeSnapshot): androidx.compose.ui.unit.Dp {
    val measurer = androidx.compose.ui.text.rememberTextMeasurer()
    val density = androidx.compose.ui.platform.LocalDensity.current
    val cfg = androidx.compose.ui.platform.LocalConfiguration.current
    val style = MaterialTheme.typography.titleSmall

    val longestPx = remember(snap.devices, style) {
        snap.devices.maxOfOrNull { d ->
            measurer.measure(
                androidx.compose.ui.text.AnnotatedString(deviceLabel(d.displayName, d.realName)),
                style = style,
                maxLines = 1,
            ).size.width
        } ?: 0
    }
    val hasUnread = snap.devices.any { it.unread > 0 }

    // 固定占用（与卡片布局逐项对应）：
    // 12(面板内左) + 12(列表左) + 16(卡片左) + 7(状态点) + 8 + [名字] + 8 + [徽标] + 90(改名/删除两个 44 钮 + 2 间隔) + 8(卡片右) + 12(列表右)
    // 上一版漏了那 90dp，结果名字明明量过也被截成 "HUAWEI Pura X V…"
    val fixed = 12 + 12 + 16 + 7 + 8 + 8 + 90 + 8 + 12 + 12 + if (hasUnread) 34 else 0
    val wanted = with(density) { longestPx.toDp() } + fixed.dp
    val cap = minOf(cfg.screenWidthDp.dp * 0.92f, 428.dp)
    return wanted.coerceIn(280.dp, cap)
}

/** 手动添加：探活 → 落库。返回空串表示成功，否则是给用户看的失败原因 */
private fun addDevice(raw: String): String {
    val n = NodeState.node ?: return "节点未启动"
    val parts = com.sanqiu.lanshare.api.LsApi.splitHostPort(raw, "")
    val ip = parts.first
    val port = parts.second.toIntOrNull() ?: Proto.HTTP_PORT_BASE
    if (ip.isEmpty()) return "请输入 IP"
    if (ip == "127.0.0.1" || ip == "localhost" || n.hub.self.ips.contains(ip)) return "这是本机地址"

    val deviceId = n.peer.probeHello(ip, port)
    if (deviceId.isEmpty()) return "连不上 $ip:$port"
    if (deviceId == n.hub.self.deviceId) return "这是本机"
    synchronized(n.hub.gate) {
        val p = n.hub.upsertPeer(deviceId)
        p.port = port
        p.lastSeen = Proto.nowMs()
        n.hub.addPeerIp(p, ip, Proto.nowMs())
        n.hub.markReachable(deviceId, Proto.nowMs())
        n.hub.saveDevices()
    }
    return ""
}

private fun formatSize(bytes: Long): String = when {
    bytes >= 1024L * 1024 -> "%.1f MB".format(bytes / 1024.0 / 1024.0)
    bytes >= 1024L -> "%.1f KB".format(bytes / 1024.0)
    else -> "$bytes B"
}
