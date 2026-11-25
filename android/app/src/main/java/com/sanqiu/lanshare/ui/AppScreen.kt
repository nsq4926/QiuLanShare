package com.sanqiu.lanshare.ui

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.ChatBubbleOutline
import androidx.compose.material.icons.outlined.Menu
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.blur
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.sanqiu.lanshare.core.Proto
import com.sanqiu.lanshare.net.NodeState
import com.sanqiu.lanshare.net.ShareIn
import com.sanqiu.lanshare.ui.components.LsCard
import com.sanqiu.lanshare.ui.components.LsDeviceName
import com.sanqiu.lanshare.ui.components.LsTextAction
import com.sanqiu.lanshare.ui.components.LsEmptyState
import com.sanqiu.lanshare.ui.components.LsHairLine
import com.sanqiu.lanshare.ui.components.LsIconSquare
import com.sanqiu.lanshare.ui.components.LsSecondaryButton
import com.sanqiu.lanshare.ui.components.StatusDot
import com.sanqiu.lanshare.ui.theme.LsMotion
import com.sanqiu.lanshare.ui.theme.LsSpace
import com.sanqiu.lanshare.ui.theme.LsTheme
import kotlinx.coroutines.delay

/**
 * 应用骨架：顶栏 + 会话头 + 内容 + 抽屉/设置浮层。
 *
 * 布局蓝图见 `UI Design.md` §4.1：**始终以聊天为中心**，不加 Tab；
 * 未选设备时用品牌空态原位替换消息区与输入区，而不是跳页。
 */
@Composable
fun AppScreen(onThemeModeChanged: (String) -> Unit) {
    var snap by remember { mutableStateOf(NodeState.snapshot()) }
    // rememberSaveable：Activity 重建（转屏、低内存回收后返回）不该把会话丢了
    var selected by rememberSaveable { mutableStateOf<String?>(null) }
    var drawerOpen by remember { mutableStateOf(false) }
    var settingsOpen by remember { mutableStateOf(false) }
    // 关于与隐私：从设置页里进，盖在设置页之上（返回先关它，再关设置）
    var aboutOpen by remember { mutableStateOf(false) }

    LaunchedEffect(Unit) {
        while (true) {
            delay(1000)
            // 点通知进来：直接切到那条会话
            val pend = NodeState.takePendingDevice()
            if (pend.isNotEmpty()) {
                selected = pend
                drawerOpen = false
            }
            snap = NodeState.snapshot()
        }
    }

    val c = LsTheme.colors
    val ctx = androidx.compose.ui.platform.LocalContext.current

    // 通知权限（Android 13+ 需要运行时申请）：不给就收不到消息与传输进度提醒
    val permLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { }
    LaunchedEffect(Unit) {
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(ctx, Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            permLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }

    // 浮层打开时背后内容轻微模糊（Android 12+ 真模糊，低版本自动忽略）+ 遮罩
    val blurRadius by animateDpAsState(
        targetValue = if (drawerOpen || settingsOpen || aboutOpen) 12.dp else 0.dp,
        animationSpec = tween(LsMotion.Page, easing = LsMotion.easeOut),
        label = "blur",
    )

    // 返回键的层级必须**互斥**，否则多个 BackHandler 同时 enabled 时只有最后注册的那个生效，
    // 结果是"在设置页按返回退出了 App"这种看起来随机的行为。
    //
    // 顺序：关于页 → 设置页 → 抽屉 → 会话（回到设备列表，不换会话）→ 交给系统退出
    BackHandler(enabled = aboutOpen) { aboutOpen = false }
    BackHandler(enabled = !aboutOpen && settingsOpen) { settingsOpen = false }
    BackHandler(enabled = !aboutOpen && !settingsOpen && drawerOpen) { drawerOpen = false }
    // 会话页按返回：打开设备列表而不是退出（也**不会**动当前选中的会话）
    BackHandler(enabled = !aboutOpen && !settingsOpen && !drawerOpen && selected != null) { drawerOpen = true }

    Box(modifier = Modifier.fillMaxSize().background(c.bg)) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .then(if (blurRadius > 0.dp) Modifier.blur(blurRadius) else Modifier),
        ) {
            TopBar(
                deviceName = snap.deviceName,
                ip = snap.primaryIp,
                port = snap.port,
                running = snap.running,
                onSettings = { settingsOpen = true },
            )
            LsHairLine()

            val current = selected
            if (current == null) {
                SessionHeaderEmpty(onOpenDrawer = { drawerOpen = true })
                LsHairLine()
                Box(modifier = Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
                    // 别的应用刚分享进来、但还没选设备：把"东西已经收到了"说出来，
                    // 否则用户分享完回到这个页面，看到的是普通空态，以为分享丢了。
                    val pendingShare = ShareIn.pendingCount()
                    LsEmptyState(
                        icon = Icons.Outlined.ChatBubbleOutline,
                        title = "选择设备，开始局域网会话",
                        hint = if (pendingShare > 0) {
                            "已收到 $pendingShare 个分享的文件\n选择设备后自动发送"
                        } else {
                            "点左上角设备按钮打开列表\n同一 Wi-Fi 下的设备会自动出现"
                        },
                        modifier = Modifier.padding(horizontal = LsSpace.s8),
                    )
                }
            } else {
                SessionHeader(
                    name = snap.devices.firstOrNull { it.deviceId == current }?.displayName
                        ?: NodeState.node?.hub?.displayName(current) ?: current,
                    realName = snap.devices.firstOrNull { it.deviceId == current }?.realName ?: "",
                    state = snap.devices.firstOrNull { it.deviceId == current }?.state ?: Proto.ST_OFFLINE,
                    ip = snap.devices.firstOrNull { it.deviceId == current }?.ip ?: "",
                    onOpenDrawer = { drawerOpen = true },
                    onClear = {
                        NodeState.node?.let { n ->
                            val convId = Proto.makeConvId(n.hub.self.deviceId, current)
                            synchronized(n.hub.gate) { n.hub.clearConversation(convId) }
                            // 清会话 = 连这条会话的传输记录一起清。
                            // 以前只有 HTTP 的 /api/history/clear 里做了，UI 直接调 hub 就漏了。
                            com.sanqiu.lanshare.net.Transfers.dropPeer(current)
                            snap = NodeState.snapshot()
                        }
                    },
                )
                LsHairLine()
                ChatArea(
                    peerId = current,
                    modifier = Modifier.weight(1f).fillMaxWidth(),
                    onRefresh = { snap = NodeState.snapshot() },
                )
            }
            Spacer(Modifier.navigationBarsPadding())
        }

        // 抽屉
        AnimatedVisibility(
            visible = drawerOpen,
            enter = fadeIn(tween(LsMotion.Fast)),
            exit = fadeOut(tween(LsMotion.Fast)),
        ) {
            DeviceDrawerOverlay(
                snap = snap,
                selected = selected,
                onSelect = {
                    selected = it
                    drawerOpen = false
                },
                onClose = { drawerOpen = false },
                onRefresh = { snap = NodeState.snapshot() },
            )
        }

        // 设置页（整页浮层，从右滑入）
        AnimatedVisibility(
            visible = settingsOpen,
            enter = slideInHorizontally(tween(LsMotion.Page, easing = LsMotion.easeOut)) { it },
            exit = slideOutHorizontally(tween(LsMotion.Normal, easing = LsMotion.easeOut)) { it },
        ) {
            SettingsScreen(
                snap = snap,
                onBack = { settingsOpen = false },
                onRefresh = { snap = NodeState.snapshot() },
                onThemeModeChanged = onThemeModeChanged,
                onOpenAbout = { aboutOpen = true },
            )
        }

        // 关于与隐私（盖在设置页之上）
        AnimatedVisibility(
            visible = aboutOpen,
            enter = slideInHorizontally(tween(LsMotion.Page, easing = LsMotion.easeOut)) { it },
            exit = slideOutHorizontally(tween(LsMotion.Normal, easing = LsMotion.easeOut)) { it },
        ) {
            AboutScreen(snap = snap, onBack = { aboutOpen = false })
        }
    }
}

@Composable
private fun TopBar(
    deviceName: String,
    ip: String,
    port: Int,
    running: Boolean,
    onSettings: () -> Unit,
) {
    val c = LsTheme.colors
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(c.panel)
            .statusBarsPadding()
            .padding(start = LsSpace.page, end = LsSpace.s3, top = LsSpace.s3, bottom = LsSpace.s3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text("秋秋快传", style = MaterialTheme.typography.titleLarge, color = c.text)
            Spacer(Modifier.height(LsSpace.s1))
            Row(verticalAlignment = Alignment.CenterVertically) {
                StatusDot(if (running) Proto.ST_ONLINE else Proto.ST_OFFLINE)
                Spacer(Modifier.padding(4.dp))
                Text(
                    if (running) {
                        "$deviceName · ${ip.ifEmpty { "无地址" }}:$port"
                    } else {
                        "未启动 · 点右侧设置里的接收开关启动"
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = c.sub,
                    maxLines = 1,
                )
            }
        }
        LsIconSquare(Icons.Outlined.Settings, "设置", onSettings)
    }
}

@Composable
private fun SessionHeaderEmpty(onOpenDrawer: () -> Unit) {
    val c = LsTheme.colors
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(c.panel)
            .padding(horizontal = LsSpace.s3, vertical = LsSpace.s3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        LsIconSquare(Icons.Outlined.Menu, "设备列表", onOpenDrawer)
        Spacer(Modifier.padding(6.dp))
        Column(modifier = Modifier.weight(1f)) {
            Text("未选择设备", style = MaterialTheme.typography.titleMedium, color = c.text)
            Spacer(Modifier.height(2.dp))
            Text("从设备列表选择一个会话", style = MaterialTheme.typography.bodySmall, color = c.faint)
        }
    }
}

@Composable
private fun SessionHeader(
    name: String,
    realName: String,
    state: String,
    ip: String,
    onOpenDrawer: () -> Unit,
    onClear: () -> Unit,
) {
    val c = LsTheme.colors
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(c.panel)
            .padding(horizontal = LsSpace.s3, vertical = LsSpace.s3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        LsIconSquare(Icons.Outlined.Menu, "设备列表", onOpenDrawer)
        Spacer(Modifier.padding(6.dp))
        Column(modifier = Modifier.weight(1f)) {
            LsDeviceName(
                name = name,
                realName = realName,
                style = MaterialTheme.typography.titleMedium,
                color = c.text,
            )
            Spacer(Modifier.height(2.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                StatusDot(state)
                Spacer(Modifier.padding(4.dp))
                Text(
                    listOf(Proto.stateLabel(state), ip.ifEmpty { "无地址" }).joinToString(" · "),
                    style = MaterialTheme.typography.bodySmall,
                    color = c.sub,
                    maxLines = 1,
                )
            }
        }
        // 垃圾桶图标表示"清空会话"太含糊 → 直接用文字，尺寸与图标按钮一致
        LsTextAction("清空", onClear, tint = c.sub)
    }
}

/** 抽屉容器：遮罩 + 从左侧滑入的面板（四角 24、四周留 12） */
@Composable
private fun DeviceDrawerOverlay(
    snap: com.sanqiu.lanshare.net.NodeSnapshot,
    selected: String?,
    onSelect: (String) -> Unit,
    onClose: () -> Unit,
    onRefresh: () -> Unit,
) {
    val c = LsTheme.colors
    Box(modifier = Modifier.fillMaxSize()) {
        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(c.scrim)
                .let { m ->
                    m.then(
                        Modifier.clickable(
                            interactionSource = remember { androidx.compose.foundation.interaction.MutableInteractionSource() },
                            indication = null,
                            onClick = onClose,
                        )
                    )
                }
        )
        DeviceDrawerPanel(
            snap = snap,
            selected = selected,
            onSelect = onSelect,
            onClose = onClose,
            onRefresh = onRefresh,
        )
    }
}
