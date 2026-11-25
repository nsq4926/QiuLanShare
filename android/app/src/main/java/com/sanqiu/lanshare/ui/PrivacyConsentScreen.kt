package com.sanqiu.lanshare.ui

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
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Check
import androidx.compose.material.icons.outlined.ChevronRight
import androidx.compose.material.icons.outlined.Shield
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
import com.sanqiu.lanshare.core.AppLinks
import com.sanqiu.lanshare.ui.components.LsCard
import com.sanqiu.lanshare.ui.components.LsPrimaryButton
import com.sanqiu.lanshare.ui.components.LsSecondaryButton
import com.sanqiu.lanshare.ui.theme.LsSpace
import com.sanqiu.lanshare.ui.theme.LsTheme

/**
 * 首次启动的隐私同意页。
 *
 * 合规要点（商店审核会逐条看）：
 *  - 同意之前**不启动接收服务**（不监听端口、不广播）、不申请通知权限；
 *  - 隐私政策必须能点开（链接打不开等于没有）；
 *  - 拒绝要能退出，不能只给「同意」一条路。
 */
@Composable
fun PrivacyConsentScreen(onAgree: () -> Unit, onDecline: () -> Unit) {
    val c = LsTheme.colors
    val ctx = LocalContext.current

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(c.bg)
            .statusBarsPadding()
            .navigationBarsPadding(),
    ) {
        Column(
            modifier = Modifier
                .weight(1f)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = LsSpace.page),
        ) {
            Spacer(Modifier.height(LsSpace.s8))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(44.dp)
                        .clip(CircleShape)
                        .background(c.accentSoft),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(
                        Icons.Outlined.Shield,
                        contentDescription = null,
                        tint = c.accent,
                        modifier = Modifier.size(22.dp),
                    )
                }
                Spacer(Modifier.width(LsSpace.s3))
                Column {
                    Text(
                        AppLinks.APP_NAME,
                        style = MaterialTheme.typography.titleLarge,
                        color = c.text,
                    )
                    Text(
                        "局域网文件与消息互传 · ${AppLinks.APP_NAME_EN}",
                        style = MaterialTheme.typography.labelSmall,
                        color = c.faint,
                    )
                }
            }

            Spacer(Modifier.height(LsSpace.s6))
            Text(
                "使用前，请阅读并同意《隐私政策》",
                style = MaterialTheme.typography.titleMedium,
                color = c.text,
            )
            Spacer(Modifier.height(LsSpace.s2))
            Text(
                "我们把该说的写清楚，不藏在设置里：",
                style = MaterialTheme.typography.bodySmall,
                color = c.sub,
            )

            // 链接放在**首屏可见**的位置：审核实测时不会去滚动页面找隐私政策
            Spacer(Modifier.height(LsSpace.s2))
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(androidx.compose.foundation.shape.RoundedCornerShape(8.dp))
                    .clickable { openExternalUrl(ctx, AppLinks.PRIVACY_URL) }
                    .padding(vertical = LsSpace.s2),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    "查看《隐私政策》全文",
                    style = MaterialTheme.typography.bodyLarge,
                    color = c.accent,
                    fontWeight = FontWeight.Medium,
                    modifier = Modifier.weight(1f),
                )
                Icon(
                    Icons.Outlined.ChevronRight,
                    contentDescription = null,
                    tint = c.accent,
                    modifier = Modifier.size(18.dp),
                )
            }
            Text(
                "点击用浏览器打开腾讯文档；同意后也可在「设置 → 关于与隐私」中查看。",
                style = MaterialTheme.typography.labelSmall,
                color = c.faint,
            )

            Spacer(Modifier.height(LsSpace.s4))
            LsCard(modifier = Modifier.fillMaxWidth()) {
                Column(modifier = Modifier.padding(LsSpace.s4)) {
                    Bullet("不注册、不登录，没有云端服务器")
                    Bullet("不收集姓名、手机号、位置、通讯录、剪贴板、设备标识")
                    Bullet("不接入广告或统计 SDK，应用内没有第三方 SDK")
                    Bullet("文字、图片与文件只在同一 Wi-Fi 内点对点传输")
                }
            }

            Spacer(Modifier.height(LsSpace.s4))
            LsCard(modifier = Modifier.fillMaxWidth()) {
                Column(modifier = Modifier.padding(LsSpace.s4)) {
                    Text("需要的系统权限", style = MaterialTheme.typography.titleSmall, color = c.text)
                    Spacer(Modifier.height(LsSpace.s3))
                    PermissionRow("网络", "在局域网内收发消息与文件（端口 8765 起）")
                    PermissionRow("网络状态", "读取本机局域网 IP，判断 Wi-Fi 是否可用")
                    PermissionRow("前台服务", "切到后台后继续传输大文件，避免中途中断")
                    PermissionRow("通知", "新消息与传输进度提醒，可在系统设置里关闭")
                    Spacer(Modifier.height(LsSpace.s2))
                    Text(
                        "不申请：位置、通讯录、相机、麦克风、读取应用列表。",
                        style = MaterialTheme.typography.labelSmall,
                        color = c.faint,
                    )
                }
            }

            Spacer(Modifier.height(LsSpace.s6))
        }

        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = LsSpace.page, vertical = LsSpace.s4),
            verticalArrangement = Arrangement.spacedBy(LsSpace.s3),
        ) {
            LsPrimaryButton("同意并继续", onClick = onAgree, modifier = Modifier.fillMaxWidth())
            LsSecondaryButton("不同意，退出", onClick = onDecline, modifier = Modifier.fillMaxWidth())
        }
    }
}

@Composable
private fun Bullet(text: String) {
    val c = LsTheme.colors
    Row(modifier = Modifier.padding(vertical = 3.dp)) {
        Icon(
            Icons.Outlined.Check,
            contentDescription = null,
            tint = c.success,
            modifier = Modifier
                .padding(top = 2.dp)
                .size(14.dp),
        )
        Spacer(Modifier.width(LsSpace.s2))
        Text(text, style = MaterialTheme.typography.bodySmall, color = c.sub)
    }
}

@Composable
private fun PermissionRow(name: String, desc: String) {
    val c = LsTheme.colors
    Row(modifier = Modifier.padding(vertical = 3.dp)) {
        Text(
            name,
            style = MaterialTheme.typography.bodySmall,
            color = c.text,
            fontWeight = FontWeight.Medium,
            modifier = Modifier.width(72.dp),
        )
        Text(desc, style = MaterialTheme.typography.bodySmall, color = c.sub)
    }
}
