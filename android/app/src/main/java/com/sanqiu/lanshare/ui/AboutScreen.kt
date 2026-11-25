package com.sanqiu.lanshare.ui

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
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.ArrowBackIosNew
import androidx.compose.material.icons.outlined.ChevronRight
import androidx.compose.material.icons.outlined.ContentCopy
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.sanqiu.lanshare.BuildConfig
import com.sanqiu.lanshare.core.AppLinks
import com.sanqiu.lanshare.net.NodeSnapshot
import com.sanqiu.lanshare.ui.components.LsCard
import com.sanqiu.lanshare.ui.components.LsHairLine
import com.sanqiu.lanshare.ui.components.LsIconSquare
import com.sanqiu.lanshare.ui.components.LsSecondaryButton
import com.sanqiu.lanshare.ui.theme.LsSpace
import com.sanqiu.lanshare.ui.theme.LsTheme

/**
 * 关于与隐私（`UI Design.md` §4.3 的最后一个分组展开成整页）。
 *
 * 三个东西必须在这里，缺一个上架就会被问：
 *  1. 版本号与包名（审核核对安装包用）；
 *  2. 能点开的隐私政策 + 权限清单（合规要求"应用内可查"）；
 *  3. 单机自测说明（局域网应用的最大审核障碍：审核员只有一台手机）。
 */
@Composable
fun AboutScreen(snap: NodeSnapshot, onBack: () -> Unit) {
    val c = LsTheme.colors
    val ctx = LocalContext.current
    val addr = if (snap.primaryIp.isEmpty()) "" else "${snap.primaryIp}:${snap.port}"

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(c.bg)
            .statusBarsPadding()
            .navigationBarsPadding()
            .verticalScroll(rememberScrollState())
            .padding(bottom = LsSpace.s6),
    ) {
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
                Text("关于与隐私", style = MaterialTheme.typography.titleLarge, color = c.text)
                Text("版本、权限与自测", style = MaterialTheme.typography.labelSmall, color = c.faint)
            }
        }
        LsHairLine()

        Column(
            modifier = Modifier.padding(horizontal = LsSpace.page, vertical = LsSpace.s4),
            verticalArrangement = Arrangement.spacedBy(LsSpace.s4),
        ) {
            Group("应用信息") {
                InfoRow("名称", AppLinks.APP_NAME)
                InfoRow("版本", "${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})")
                InfoRow("包名", BuildConfig.APPLICATION_ID)
                InfoRow("设备标识", snap.deviceId.ifEmpty { "启动接收服务后生成" })
            }

            Group("隐私与权限") {
                LinkRow(
                    title = "隐私政策",
                    subtitle = "在浏览器中打开全文（腾讯文档）",
                    onClick = { openExternalUrl(ctx, AppLinks.PRIVACY_URL) },
                )
                Spacer(Modifier.height(LsSpace.s3))
                LsHairLine()
                Spacer(Modifier.height(LsSpace.s3))
                Text(
                    "本应用不收集、不上传任何个人信息，没有账号体系，也不接入广告或统计 SDK。\n\n" +
                        "网络：局域网内收发消息与文件。\n" +
                        "网络状态：读取本机局域网 IP，判断 Wi-Fi 是否可用。\n" +
                        "前台服务：切到后台后继续传输大文件。\n" +
                        "通知：新消息与传输进度提醒，可在系统设置里关闭。\n\n" +
                        "聊天记录与文件副本保存在应用沙盒内；卸载应用即删除。",
                    style = MaterialTheme.typography.bodySmall,
                    color = c.sub,
                )
            }

            Group("没有第二台设备？") {
                Text(
                    "本应用靠两台设备在同一 Wi-Fi 内互通，单机只能看到空设备列表。自测三步：",
                    style = MaterialTheme.typography.bodySmall,
                    color = c.sub,
                )
                Spacer(Modifier.height(LsSpace.s3))
                Step("1", "让另一台手机、平板或电脑连上同一个 Wi-Fi")
                Step("2", "在另一台设备上安装同一版本，或运行 Windows 端")
                Step("3", "打开应用等待对方出现；不出现就在设备列表里手动添加下面的地址")
                Spacer(Modifier.height(LsSpace.s3))
                if (addr.isEmpty()) {
                    Text(
                        "接收服务未启动：先在「设置 → 发现与接收」里打开接收服务。",
                        style = MaterialTheme.typography.labelSmall,
                        color = c.warning,
                    )
                } else {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            "本机地址  $addr",
                            style = MaterialTheme.typography.bodyLarge,
                            color = c.text,
                            fontWeight = FontWeight.Medium,
                            modifier = Modifier.weight(1f),
                        )
                        LsSecondaryButton(
                            text = "复制",
                            onClick = { copyToClipboard(ctx, "本机地址", addr, "已复制 $addr") },
                        )
                    }
                }
            }

            Group("反馈") {
                if (AppLinks.CONTACT_EMAIL.isNotEmpty()) {
                    LinkRow(
                        title = "联系作者",
                        subtitle = AppLinks.CONTACT_EMAIL,
                        onClick = { openExternalUrl(ctx, "mailto:${AppLinks.CONTACT_EMAIL}") },
                    )
                } else {
                    Text(
                        "反馈请走应用商店的评论区或开发者主页。",
                        style = MaterialTheme.typography.bodySmall,
                        color = c.sub,
                    )
                }
            }

            Text(
                "秋秋快传 ${BuildConfig.VERSION_NAME} · 纯局域网点对点传输，无服务器、无账号、无广告。",
                style = MaterialTheme.typography.labelSmall,
                color = c.faint,
            )
        }
    }
}

@Composable
private fun Group(title: String, content: @Composable ColumnScope.() -> Unit) {
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
private fun InfoRow(label: String, value: String) {
    val c = LsTheme.colors
    Row(modifier = Modifier.padding(vertical = 3.dp)) {
        Text(
            label,
            style = MaterialTheme.typography.bodySmall,
            color = c.faint,
            modifier = Modifier.width(84.dp),
        )
        Text(value, style = MaterialTheme.typography.bodySmall, color = c.text)
    }
}

@Composable
private fun LinkRow(title: String, subtitle: String, onClick: () -> Unit) {
    val c = LsTheme.colors
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(8.dp))
            .clickable(onClick = onClick)
            .padding(vertical = LsSpace.s2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge, color = c.accent)
            Spacer(Modifier.height(2.dp))
            Text(subtitle, style = MaterialTheme.typography.labelSmall, color = c.faint)
        }
        Icon(
            Icons.Outlined.ChevronRight,
            contentDescription = null,
            tint = c.faint,
            modifier = Modifier.size(18.dp),
        )
    }
}

@Composable
private fun Step(index: String, text: String) {
    val c = LsTheme.colors
    Row(modifier = Modifier.padding(vertical = 3.dp), verticalAlignment = Alignment.Top) {
        Box(
            modifier = Modifier
                .size(18.dp)
                .clip(RoundedCornerShape(9.dp))
                .background(c.accentSoft),
            contentAlignment = Alignment.Center,
        ) {
            Text(index, style = MaterialTheme.typography.labelSmall, color = c.accent)
        }
        Spacer(Modifier.width(LsSpace.s2))
        Text(text, style = MaterialTheme.typography.bodySmall, color = c.sub)
    }
}
