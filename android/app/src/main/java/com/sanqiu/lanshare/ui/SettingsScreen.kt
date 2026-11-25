package com.sanqiu.lanshare.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.ArrowBackIosNew
import androidx.compose.material.icons.outlined.ChevronRight
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.sanqiu.lanshare.BuildConfig
import com.sanqiu.lanshare.core.LsSettings
import com.sanqiu.lanshare.core.LsSettingsStore
import com.sanqiu.lanshare.core.LsStore
import com.sanqiu.lanshare.net.NodeService
import com.sanqiu.lanshare.net.NodeSnapshot
import com.sanqiu.lanshare.net.NodeState
import com.sanqiu.lanshare.probe.AppPaths
import com.sanqiu.lanshare.ui.components.LsCard
import com.sanqiu.lanshare.ui.components.LsHairLine
import com.sanqiu.lanshare.ui.components.LsIconSquare
import com.sanqiu.lanshare.ui.theme.LsMotion
import com.sanqiu.lanshare.ui.theme.LsRadius
import com.sanqiu.lanshare.ui.theme.LsSpace
import com.sanqiu.lanshare.ui.theme.LsTheme

/**
 * 设置页（`UI Design.md` §4.3）。整页浮层，返回键只关这一层。
 *
 * 分组顺序与规范一致：接收 → 外观 → 聊天记录 → 收到的文件 → 底部说明。
 */
@Composable
fun SettingsScreen(
    snap: NodeSnapshot,
    onBack: () -> Unit,
    onRefresh: () -> Unit,
    onThemeModeChanged: (String) -> Unit,
    onOpenAbout: () -> Unit,
) {
    val c = LsTheme.colors
    val ctx = LocalContext.current
    val hub = NodeState.node?.hub
    // 服务没启动时 hub 为 null，以前这里 new 一个临时 LsSettings：开关改了只进临时对象，
    // 既没落盘也没生效，用户看到的就是"开关失效"。所以停着也要能从磁盘读写。
    val settingsStore = remember {
        runCatching { LsSettingsStore(LsStore(AppPaths.requireDataDir())) }.getOrNull()
    }
    val settings = remember(snap.running, snap.deviceId, snap.messageCount) {
        hub?.settings ?: settingsStore?.load() ?: LsSettings()
    }
    // 唯一写入点：跑着交给 Hub（同一份内存对象 + 落盘），停着直接落盘
    val persist: () -> Unit = {
        val h = NodeState.node?.hub
        if (h != null) h.saveSettings() else settingsStore?.save(settings)
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(c.bg)
            .statusBarsPadding()
            .navigationBarsPadding()
            .verticalScroll(rememberScrollState())
            .padding(bottom = LsSpace.s6),
    ) {
        // 顶栏
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(c.panel)
                .padding(horizontal = LsSpace.s3, vertical = LsSpace.s3),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            LsIconSquare(Icons.Outlined.ArrowBackIosNew, "返回", onBack)
            Spacer(Modifier.width(LsSpace.s3))
            Column {
                Text("设置", style = MaterialTheme.typography.titleLarge, color = c.text)
                Text("连接、外观与存储", style = MaterialTheme.typography.labelSmall, color = c.faint)
            }
        }
        LsHairLine()

        Column(
            modifier = Modifier.padding(horizontal = LsSpace.page, vertical = LsSpace.s4),
            verticalArrangement = Arrangement.spacedBy(LsSpace.s4),
        ) {
            // 发现与接收
            // 「接收服务」是主开关：它决定本机是否监听 8765（收消息/收文件/被发现都靠它）。
            // 少了它，「主动发现 / 被发现」在服务没起来时是灰的 → 谁也没法把服务开起来，
            // 这正是 MatePad 与 OPPO 上"设置里的发现和接收开关失效"的真因。
            SettingGroup("发现与接收") {
                SettingSwitchRow(
                    title = "接收服务",
                    subtitle = if (snap.running) {
                        "正在监听 ${snap.primaryIp.ifEmpty { "0.0.0.0" }}:${snap.port}，可收发消息与文件"
                    } else {
                        "未启动：本机不监听端口，别人也找不到本机"
                    },
                    checked = snap.running,
                    onCheckedChange = { on ->
                        settings.serverEnabled = on
                        // 先落盘再起停：服务起来时会重新读这份设置，
                        // 顺序反过来就会"这次关掉了、下次启动又自己开"
                        persist()
                        if (on) NodeService.start(ctx) else NodeService.stop(ctx)
                        onRefresh()
                    },
                )
                Spacer(Modifier.height(LsSpace.s3))
                LsHairLine()
                Spacer(Modifier.height(LsSpace.s3))
                SettingSwitchRow(
                    title = "主动发现",
                    subtitle = if (snap.running) {
                        "每隔 ${settings.discoverSeconds} 秒搜索一次附近设备"
                    } else {
                        "每隔 ${settings.discoverSeconds} 秒搜索一次附近设备（启动接收后生效）"
                    },
                    checked = settings.activeDiscover,
                    onCheckedChange = { v ->
                        settings.activeDiscover = v
                        val n = NodeState.node
                        if (n != null) n.setActiveDiscover(v) else persist()
                        onRefresh()
                    },
                )
                if (settings.activeDiscover) {
                    Spacer(Modifier.height(LsSpace.s3))
                    SegmentedRow(
                        options = listOf("5" to "5 秒", "10" to "10 秒", "30" to "30 秒", "60" to "60 秒"),
                        selected = settings.discoverSeconds.toString(),
                        onSelect = { v ->
                            settings.discoverSeconds = v.toInt()
                            val n = NodeState.node
                            if (n != null) n.setDiscoverSeconds(v.toInt()) else persist()
                            onRefresh()
                        },
                    )
                }
                Spacer(Modifier.height(LsSpace.s3))
                LsHairLine()
                Spacer(Modifier.height(LsSpace.s3))
                SettingSwitchRow(
                    title = "被发现",
                    subtitle = if (if (snap.running) snap.acceptsInbound else settings.acceptsInbound) {
                        "可被其他设备发现，并接收文件与消息"
                    } else {
                        "当前不可被发现：对端显示「在线 · 不接收」"
                    },
                    checked = if (snap.running) snap.acceptsInbound else settings.acceptsInbound,
                    onCheckedChange = { v ->
                        settings.acceptsInbound = v
                        val n = NodeState.node
                        if (n != null) n.setAcceptsInbound(v) else persist()
                        onRefresh()
                    },
                )
                Spacer(Modifier.height(LsSpace.s2))
                Text(
                    "关掉「被发现」后本机不再广播；已建立的会话仍可主动发消息给对方。",
                    style = MaterialTheme.typography.labelSmall,
                    color = c.faint,
                )
                if (settingsStore == null) {
                    Spacer(Modifier.height(LsSpace.s2))
                    Text(
                        "设置文件不可写，以上开关不会保存 —— 请检查应用存储权限",
                        style = MaterialTheme.typography.labelSmall,
                        color = c.danger,
                    )
                }
                Spacer(Modifier.height(LsSpace.s3))
                LsHairLine()
                Spacer(Modifier.height(LsSpace.s3))
                // 上架前移除了 REQUEST_IGNORE_BATTERY_OPTIMIZATIONS：
                // 改成跳系统电池优化页手动放行，权限清单干净，用户也能自己决定。
                SettingLinkRow(
                    title = "后台保活",
                    subtitle = "系统电池优化可能冻结后台传输，点此前往系统设置放行",
                    onClick = { openBatteryOptimizationSettings(ctx) },
                )
            }

            // 外观
            SettingGroup("外观") {
                SegmentedRow(
                    options = listOf(
                        LsSettings.THEME_SYSTEM to "跟随系统",
                        LsSettings.THEME_LIGHT to "亮色",
                        LsSettings.THEME_DARK to "暗色",
                    ),
                    selected = settings.themeMode,
                    onSelect = { mode ->
                        settings.themeMode = mode
                        persist()
                        onThemeModeChanged(mode)
                        onRefresh()
                    },
                )
                Spacer(Modifier.height(LsSpace.s2))
                Text(
                    "亮色与暗色分别配置表面、气泡、状态和阴影",
                    style = MaterialTheme.typography.labelSmall,
                    color = c.faint,
                )
            }

            // 聊天记录
            SettingGroup("聊天记录") {
                val options = listOf(
                    Triple(LsSettings.RET_NEVER, 0L, "不清理"),
                    Triple(LsSettings.RET_DAYS, 7L, "保留 7 天"),
                    Triple(LsSettings.RET_DAYS, 30L, "保留 30 天"),
                    Triple(LsSettings.RET_DAYS, 90L, "保留 90 天"),
                    Triple(LsSettings.RET_ON_EXIT, 0L, "退出应用时清理"),
                )
                options.forEach { (mode, days, label) ->
                    val active = settings.retentionMode == mode &&
                        (mode != LsSettings.RET_DAYS || settings.retentionDays == days)
                    OptionRow(label, active) {
                        settings.retentionMode = mode
                        if (mode == LsSettings.RET_DAYS) settings.retentionDays = days
                        persist()
                        // 立即按新保留期裁一次，别等下次启动
                        if (mode == LsSettings.RET_DAYS) {
                            val cutoff = com.sanqiu.lanshare.core.retentionCutoff(
                                settings, com.sanqiu.lanshare.core.Proto.nowMs()
                            )
                            if (cutoff > 0) {
                                hub?.let { h -> synchronized(h.gate) { h.purgeBefore(cutoff) } }
                            }
                        }
                        onRefresh()
                    }
                    Spacer(Modifier.height(LsSpace.s2))
                }
            }

            // 收到的文件
            SettingGroup("收到的文件") {
                SettingSwitchRow(
                    title = "图片自动存图库",
                    subtitle = "系统会弹一次确认，同意后写入相册",
                    checked = settings.autoSaveImage,
                    onCheckedChange = {
                        settings.autoSaveImage = it
                        persist()
                        onRefresh()
                    },
                )
                Spacer(Modifier.height(LsSpace.s3))
                SettingSwitchRow(
                    title = "文件自动保存",
                    subtitle = "通过系统保存框写入 Download，默认只需确认一次",
                    checked = settings.autoSaveFile,
                    onCheckedChange = {
                        settings.autoSaveFile = it
                        persist()
                        onRefresh()
                    },
                )
            }

            // 关于与隐私（上架合规入口：版本号、隐私政策、权限清单、单机自测）
            SettingGroup("关于与隐私") {
                SettingLinkRow(
                    title = "关于与隐私",
                    subtitle = "版本 ${BuildConfig.VERSION_NAME} · 隐私政策 · 权限说明 · 单机自测",
                    onClick = onOpenAbout,
                )
            }

            Text(
                "图片与文件会先保存在应用内，供预览与历史消息使用，再按上述设置保存到系统。删除应用时会删除应用内副本。",
                style = MaterialTheme.typography.labelSmall,
                color = c.faint,
            )
        }
    }
}

/** 分组卡片：标题左侧一条 3dp 主色竖条 */
@Composable
private fun SettingGroup(title: String, content: @Composable ColumnScope.() -> Unit) {
    val c = LsTheme.colors
    LsCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(LsSpace.s4)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(width = 3.dp, height = 14.dp)
                        .clip(RoundedCornerShape(2.dp))
                        .background(c.accent)
                )
                Spacer(Modifier.width(LsSpace.s2))
                Text(title, style = MaterialTheme.typography.titleSmall, color = c.text)
            }
            Spacer(Modifier.height(LsSpace.s3))
            content()
        }
    }
}

@Composable
private fun SettingSwitchRow(
    title: String,
    subtitle: String,
    checked: Boolean,
    enabled: Boolean = true,
    onCheckedChange: (Boolean) -> Unit,
) {
    val c = LsTheme.colors
    Row(verticalAlignment = Alignment.CenterVertically) {
        Column(modifier = Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge, color = c.text)
            Spacer(Modifier.height(2.dp))
            Text(subtitle, style = MaterialTheme.typography.labelSmall, color = c.faint)
        }
        Spacer(Modifier.width(LsSpace.s3))
        Switch(
            checked = checked,
            enabled = enabled,
            onCheckedChange = onCheckedChange,
            // 缩小到 0.85：Material3 默认尺寸在平板上偏大，与参考图的轻盈感不符
            modifier = Modifier.scale(0.85f),
            colors = SwitchDefaults.colors(
                checkedThumbColor = Color.White,
                checkedTrackColor = c.accent,
                uncheckedThumbColor = Color.White,
                uncheckedTrackColor = if (c.isDark) c.border else c.faint.copy(alpha = 0.6f),
            ),
        )
    }
}

/** 可点击的设置行（跳系统设置 / 打开关于页）：右侧一个 chevron，整行可点 */
@Composable
private fun SettingLinkRow(
    title: String,
    subtitle: String,
    onClick: () -> Unit,
) {
    val c = LsTheme.colors
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(LsRadius.control))
            .clickable(onClick = onClick)
            .padding(vertical = LsSpace.s2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge, color = c.text)
            Spacer(Modifier.height(2.dp))
            Text(subtitle, style = MaterialTheme.typography.labelSmall, color = c.faint)
        }
        Spacer(Modifier.width(LsSpace.s3))
        Icon(
            Icons.Outlined.ChevronRight,
            contentDescription = null,
            tint = c.faint,
            modifier = Modifier.size(18.dp),
        )
    }
}

/** 胶囊分段选择器（外观） */
@Composable
private fun SegmentedRow(
    options: List<Pair<String, String>>,
    selected: String,
    onSelect: (String) -> Unit,
) {
    val c = LsTheme.colors
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(LsRadius.control + 2.dp))
            .background(c.inputBg)
            .padding(4.dp),
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        options.forEach { (value, label) ->
            val active = value == selected
            val bg by animateColorAsState(
                if (active) c.accent else Color.Transparent,
                animationSpec = tween(LsMotion.Normal, easing = LsMotion.easeOut),
                label = "segBg",
            )
            val fg by animateColorAsState(
                if (active) c.onAccent else c.sub,
                animationSpec = tween(LsMotion.Normal),
                label = "segFg",
            )
            Box(
                modifier = Modifier
                    .weight(1f)
                    .height(38.dp)
                    .clip(RoundedCornerShape(LsRadius.small + 2.dp))
                    .background(bg)
                    .clickable { onSelect(value) },
                contentAlignment = Alignment.Center,
            ) {
                Text(label, style = MaterialTheme.typography.labelLarge, color = fg)
            }
        }
    }
}

/** 选项行（聊天记录）：选中浅主色底 */
@Composable
private fun OptionRow(label: String, active: Boolean, onClick: () -> Unit) {
    val c = LsTheme.colors
    val bg by animateColorAsState(
        if (active) c.sel else c.inputBg,
        animationSpec = tween(LsMotion.Fast),
        label = "optBg",
    )
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(LsRadius.control))
            .background(bg)
            .clickable(onClick = onClick)
            .padding(horizontal = LsSpace.s4, vertical = LsSpace.s3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            label,
            style = MaterialTheme.typography.bodyLarge,
            color = if (active) c.accent else c.text,
            fontWeight = if (active) FontWeight.Medium else FontWeight.Normal,
            modifier = Modifier.weight(1f),
        )
        if (active) {
            Box(
                modifier = Modifier
                    .size(6.dp)
                    .clip(RoundedCornerShape(3.dp))
                    .background(c.accent)
            )
        }
    }
}
