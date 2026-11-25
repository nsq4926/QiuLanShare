package com.sanqiu.lanshare.ui.components

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material3.Icon
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.sanqiu.lanshare.ui.theme.LsMotion
import com.sanqiu.lanshare.ui.theme.LsRadius
import com.sanqiu.lanshare.ui.theme.LsSpace
import com.sanqiu.lanshare.ui.theme.LsTheme

/**
 * 基础组件（对应 `UI Design.md` §三）。
 *
 * 统一在这里解决三件容易走样的事：**点击区不小于 44dp**、**按压缩放反馈**、
 * **只用令牌色**（组件里不出现字面色值）。
 */

/** 按压反馈：轻微缩放。用 spring 跟手，不用固定时长（低端机也不掉帧，只改 matrix） */
@Composable
private fun Modifier.pressScale(interaction: MutableInteractionSource): Modifier {
    val pressed by interaction.collectIsPressedAsState()
    val scale by animateFloatAsState(
        targetValue = if (pressed) 0.96f else 1f,
        animationSpec = LsMotion.springSnappy,
        label = "pressScale",
    )
    return this.graphicsLayer {
        scaleX = scale
        scaleY = scale
    }
}

/** 主按钮：44dp 高、圆角 14、主色底白字 */
@Composable
fun LsPrimaryButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
) {
    val c = LsTheme.colors
    val interaction = remember { MutableInteractionSource() }
    val bg by animateColorAsState(
        when {
            !enabled -> c.inputBg
            else -> c.accent
        },
        animationSpec = androidx.compose.animation.core.tween(LsMotion.Fast, easing = LsMotion.easeOut),
        label = "btnBg",
    )
    val fg = if (enabled) c.onAccent else c.faint
    Box(
        modifier = modifier
            .height(LsSpace.touch)
            .pressScale(interaction)
            .clip(RoundedCornerShape(LsRadius.control + 2.dp))
            .background(bg)
            .clickable(
                interactionSource = interaction,
                indication = androidx.compose.material3.ripple(color = Color.White),
                enabled = enabled,
                onClick = onClick,
            )
            .padding(horizontal = LsSpace.s5),
        contentAlignment = Alignment.Center,
    ) {
        Text(text, style = MaterialTheme.typography.labelLarge, color = fg)
    }
}

/** 次按钮：浅底正文色 */
@Composable
fun LsSecondaryButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    tint: Color? = null,
) {
    val c = LsTheme.colors
    val interaction = remember { MutableInteractionSource() }
    Box(
        modifier = modifier
            .height(LsSpace.touch)
            .pressScale(interaction)
            .clip(RoundedCornerShape(LsRadius.control))
            .background(c.inputBg)
            .clickable(
                interactionSource = interaction,
                indication = androidx.compose.material3.ripple(),
                enabled = enabled,
                onClick = onClick,
            )
            .padding(horizontal = LsSpace.s4),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text,
            style = MaterialTheme.typography.labelLarge,
            color = tint ?: if (enabled) c.text else c.faint,
        )
    }
}

/** 方形图标按钮：44×44、圆角 12、浅底；`plain=true` 时无底色（列表行内的次级操作） */
@Composable
fun LsIconSquare(
    icon: ImageVector,
    contentDescription: String?,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    active: Boolean = false,
    tint: Color? = null,
    plain: Boolean = false,
) {
    val c = LsTheme.colors
    val interaction = remember { MutableInteractionSource() }
    val bg by animateColorAsState(
        when {
            plain -> Color.Transparent
            active -> c.sel
            else -> c.inputBg
        },
        animationSpec = androidx.compose.animation.core.tween(LsMotion.Fast),
        label = "iconBg",
    )
    Box(
        modifier = modifier
            .size(LsSpace.touch)
            .pressScale(interaction)
            .clip(RoundedCornerShape(LsRadius.control))
            .background(bg)
            .clickable(interactionSource = interaction, indication = androidx.compose.material3.ripple(), onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            icon,
            contentDescription = contentDescription,
            tint = tint ?: if (active) c.accent else c.sub,
            modifier = Modifier.size(21.dp),
        )
    }
}

/**
 * 文字动作按钮：与图标按钮**同高同圆角**（44dp / 12dp），只把"图标"换成两个汉字。
 *
 * 存在的理由：垃圾桶图标表示"清空当前会话"太含糊（容易被读成删除设备/删除记录），
 * 而换成文字后又不能长得像主按钮抢戏，所以保留次级按钮的浅底与尺寸。
 *
 * 左右各 8dp 内边距是算过的：两个汉字在 labelLarge 下约 28dp，28+16=44，
 * 正好等于图标按钮的宽高，视觉上不会"换了个东西"。
 */
@Composable
fun LsTextAction(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    tint: Color? = null,
) {
    val c = LsTheme.colors
    val interaction = remember { MutableInteractionSource() }
    Box(
        modifier = modifier
            .height(LsSpace.touch)
            .widthIn(min = LsSpace.touch)
            .pressScale(interaction)
            .clip(RoundedCornerShape(LsRadius.control))
            .background(c.inputBg)
            .clickable(
                interactionSource = interaction,
                indication = androidx.compose.material3.ripple(),
                onClick = onClick,
            )
            .padding(horizontal = LsSpace.s2),
        contentAlignment = Alignment.Center,
    ) {
        Text(text, style = MaterialTheme.typography.labelLarge, color = tint ?: c.sub)
    }
}

/**
 * 设备显示名：改过名之后，把**对端自报的真实名字**用括号缀在后面。
 *
 * 为什么要缀：别名是本地私有的，用户在两台机器上看到的名字不一致；
 * 只显示别名时，对端说"我这叫 DBY-W09"就完全对不上号。
 * 括号部分用小一号的 faint 色，读起来是附注而不是名字本身。
 */
@Composable
fun LsDeviceName(
    name: String,
    realName: String,
    style: androidx.compose.ui.text.TextStyle,
    color: Color,
    modifier: Modifier = Modifier,
    maxLines: Int = 1,
) {
    val c = LsTheme.colors
    val annotated = remember(name, realName) {
        val real = if (realName.isNotEmpty() && realName != name) realName else ""
        if (real.isEmpty()) {
            androidx.compose.ui.text.AnnotatedString(name)
        } else {
            androidx.compose.ui.text.buildAnnotatedString {
                append(name)
                pushStyle(
                    androidx.compose.ui.text.SpanStyle(
                        color = c.faint,
                        fontSize = style.fontSize * 0.86f,
                        fontWeight = FontWeight.Normal,
                    )
                )
                append("（$real）")
                pop()
            }
        }
    }
    Text(
        annotated,
        style = style.copy(color = color),
        modifier = modifier,
        maxLines = maxLines,
        overflow = TextOverflow.Ellipsis,
    )
}

/** 非 Composable 场景（对话框、通知）用的同名拼接 */
fun deviceLabel(name: String, realName: String): String =
    if (realName.isNotEmpty() && realName != name) "$name（$realName）" else name

/** 卡片：16 圆角、1dp 边、极淡阴影 */
@Composable
fun LsCard(
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit,
) {
    val c = LsTheme.colors
    Surface(
        modifier = modifier,
        shape = RoundedCornerShape(LsRadius.card),
        color = c.card,
        border = BorderStroke(1.dp, c.border),
        shadowElevation = if (c.isDark) 0.dp else 1.dp,
    ) {
        content()
    }
}

/** 输入框：浅底无边框，圆角 14；占位用 faint */
@Composable
fun LsTextField(
    value: String,
    onValueChange: (String) -> Unit,
    modifier: Modifier = Modifier,
    placeholder: String = "",
    singleLine: Boolean = true,
    maxLines: Int = 4,
    leading: (@Composable () -> Unit)? = null,
) {
    val c = LsTheme.colors
    Row(
        modifier = modifier
            .defaultMinSize(minHeight = LsSpace.touch)
            .clip(RoundedCornerShape(LsRadius.control + 2.dp))
            .background(c.inputBg)
            .padding(horizontal = LsSpace.s3, vertical = LsSpace.s2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (leading != null) {
            leading()
            Spacer(Modifier.width(LsSpace.s2))
        }
        Box(modifier = Modifier.weight(1f), contentAlignment = Alignment.CenterStart) {
            if (value.isEmpty()) {
                Text(placeholder, style = MaterialTheme.typography.bodyLarge, color = c.faint)
            }
            BasicTextField(
                value = value,
                onValueChange = onValueChange,
                singleLine = singleLine,
                maxLines = maxLines,
                textStyle = LocalTextStyle.current.merge(
                    MaterialTheme.typography.bodyLarge.copy(color = c.text)
                ),
                cursorBrush = SolidColor(c.accent),
                modifier = Modifier.fillMaxWidth(),
            )
        }
    }
}

/** 状态点：7dp 圆。在线绿、不接收橙、其余灰 */
@Composable
fun StatusDot(state: String, modifier: Modifier = Modifier) {
    val c = LsTheme.colors
    val color = when (state) {
        "online" -> c.success
        "online_no_inbound" -> c.warning
        else -> c.faint
    }
    Box(modifier = modifier.size(7.dp).clip(CircleShape).background(color))
}

/** 空态：图标容器 + 一句主提示 + 一句补充（不堆第三个操作） */
@Composable
fun LsEmptyState(
    icon: ImageVector,
    title: String,
    hint: String,
    modifier: Modifier = Modifier,
) {
    val c = LsTheme.colors
    Column(
        modifier = modifier,
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Box(
            modifier = Modifier
                .size(84.dp)
                .clip(RoundedCornerShape(28.dp))
                .background(c.accentSoft),
            contentAlignment = Alignment.Center,
        ) {
            Icon(icon, contentDescription = null, tint = c.accent, modifier = Modifier.size(36.dp))
        }
        Spacer(Modifier.height(LsSpace.s6))
        Text(title, style = MaterialTheme.typography.titleMedium, color = c.text, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(LsSpace.s2))
        Text(
            hint,
            style = MaterialTheme.typography.bodySmall,
            color = c.sub,
            textAlign = androidx.compose.ui.text.style.TextAlign.Center,
        )
    }
}

/** 顶栏/会话头里的细分割线 */
@Composable
fun LsHairLine(modifier: Modifier = Modifier) {
    Box(
        modifier
            .fillMaxWidth()
            .height(1.dp)
            .background(LsTheme.colors.border)
    )
}

/** 设备行等场景的单行省略文本 */
@Composable
fun LsOneLine(text: String, style: androidx.compose.ui.text.TextStyle, color: Color) {
    Text(text, style = style, color = color, maxLines = 1, overflow = TextOverflow.Ellipsis)
}
