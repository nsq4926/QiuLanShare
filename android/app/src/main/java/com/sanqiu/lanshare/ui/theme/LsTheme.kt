package com.sanqiu.lanshare.ui.theme

import androidx.compose.animation.core.CubicBezierEasing
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.spring
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * 设计令牌（对应 `UI Design.md` §二）。
 *
 * 亮/暗是**两套独立主题**，不是明度反转：暗色单独决定炭灰层级、文字亮度、边框强弱。
 * 组件里不得出现字面色值，一律读这里。
 */
data class LsColors(
    val bg: Color,
    val panel: Color,
    val card: Color,
    val inputBg: Color,
    val sel: Color,
    val accent: Color,
    val accentSoft: Color,
    val outBg: Color,
    val outText: Color,
    val outSub: Color,
    val text: Color,
    val sub: Color,
    val faint: Color,
    val border: Color,
    val danger: Color,
    val success: Color,
    val warning: Color,
    val onAccent: Color,
    val scrim: Color,
    val isDark: Boolean,
)

val LightColors = LsColors(
    bg = Color(0xFFF5F6FA),
    panel = Color(0xFFFFFFFF),
    card = Color(0xFFFFFFFF),
    inputBg = Color(0xFFF3F4F8),
    sel = Color(0xFFEEF0FF),
    accent = Color(0xFF4B5FD5),
    accentSoft = Color(0xFFE8ECFF),
    outBg = Color(0xFFE8ECFF),
    outText = Color(0xFF273476),
    outSub = Color(0xFF6873A4),
    text = Color(0xFF20242C),
    sub = Color(0xFF69707D),
    faint = Color(0xFF969CA8),
    border = Color(0xFFE7E9F0),
    danger = Color(0xFFD94B54),
    success = Color(0xFF20A779),
    warning = Color(0xFFC77A24),
    onAccent = Color(0xFFFFFFFF),
    scrim = Color(0x55111322),
    isDark = false,
)

val DarkColors = LsColors(
    bg = Color(0xFF101116),
    panel = Color(0xFF191B22),
    card = Color(0xFF22252E),
    inputBg = Color(0xFF252832),
    sel = Color(0xFF2A2D4A),
    accent = Color(0xFF6576DF),
    accentSoft = Color(0xFF292F50),
    outBg = Color(0xFF2B3153),
    outText = Color(0xFFE1E5FF),
    outSub = Color(0xFFAAB4E0),
    text = Color(0xFFF1F2F6),
    sub = Color(0xFFA7ACB8),
    faint = Color(0xFF747B89),
    border = Color(0xFF30333E),
    danger = Color(0xFFF06A73),
    success = Color(0xFF48C79A),
    warning = Color(0xFFE0A15B),
    onAccent = Color(0xFFFFFFFF),
    scrim = Color(0x99000000),
    isDark = true,
)

val LocalLsColors = staticCompositionLocalOf { LightColors }

/**
 * 动效令牌（§五）。
 *
 * 原则：普通反馈 ≤220ms、页面级 ≤320ms；位置/尺寸频繁变化优先走透明度与缩放。
 * 弹簧交给系统曲线，不自造弹跳。
 */
object LsMotion {
    const val Fast = 160
    const val Normal = 220
    const val Page = 300

    val easeOut = CubicBezierEasing(0.2f, 0f, 0f, 1f)

    /** 面板/抽屉：柔和落位 */
    val springPanel = spring<Float>(dampingRatio = 0.82f, stiffness = Spring.StiffnessMediumLow)

    /** 按压、选中：跟手 */
    val springSnappy = spring<Float>(dampingRatio = 0.9f, stiffness = 900f)

    /** 气泡入场 */
    val springBubble = spring<Float>(dampingRatio = 0.78f, stiffness = 500f)
}

/** 4dp 基准网格（§2.3） */
object LsSpace {
    val s1 = 4.dp
    val s2 = 8.dp
    val s3 = 12.dp
    val s4 = 16.dp
    val s5 = 20.dp
    val s6 = 24.dp
    val s8 = 32.dp

    /** 页面左右留白 */
    val page = 16.dp

    /** 最小点击区 */
    val touch = 44.dp
}

/** 圆角（§2.4） */
object LsRadius {
    val small = 8.dp
    val control = 12.dp
    val card = 16.dp
    val bubble = 20.dp
    val dialog = 24.dp
}

private val LsTypography = Typography(
    // 页面主标题 20sp/700
    titleLarge = TextStyle(fontSize = 20.sp, lineHeight = 26.sp, fontWeight = FontWeight.Bold),
    // 会话标题 17sp/600
    titleMedium = TextStyle(fontSize = 17.sp, lineHeight = 22.sp, fontWeight = FontWeight.SemiBold),
    // 分组标题 16sp/600
    titleSmall = TextStyle(fontSize = 16.sp, lineHeight = 22.sp, fontWeight = FontWeight.SemiBold),
    // 正文 15sp/400
    bodyLarge = TextStyle(fontSize = 15.sp, lineHeight = 22.sp, fontWeight = FontWeight.Normal),
    bodyMedium = TextStyle(fontSize = 15.sp, lineHeight = 22.sp, fontWeight = FontWeight.Normal),
    // 辅助 13sp
    bodySmall = TextStyle(fontSize = 13.sp, lineHeight = 18.sp, fontWeight = FontWeight.Normal),
    // 按钮 14sp/500
    labelLarge = TextStyle(fontSize = 14.sp, lineHeight = 20.sp, fontWeight = FontWeight.Medium),
    // 标注 11sp
    labelSmall = TextStyle(fontSize = 11.sp, lineHeight = 16.sp, fontWeight = FontWeight.Normal),
    labelMedium = TextStyle(fontSize = 12.sp, lineHeight = 16.sp, fontWeight = FontWeight.Medium),
)

private val LsShapes = Shapes(
    extraSmall = androidx.compose.foundation.shape.RoundedCornerShape(LsRadius.small),
    small = androidx.compose.foundation.shape.RoundedCornerShape(LsRadius.control),
    medium = androidx.compose.foundation.shape.RoundedCornerShape(LsRadius.card),
    large = androidx.compose.foundation.shape.RoundedCornerShape(LsRadius.bubble),
    extraLarge = androidx.compose.foundation.shape.RoundedCornerShape(LsRadius.dialog),
)

private fun LsColors.toMaterialScheme(dark: Boolean) = if (dark) {
    darkColorScheme(
        primary = accent,
        onPrimary = onAccent,
        primaryContainer = accentSoft,
        onPrimaryContainer = text,
        secondary = accent,
        background = bg,
        onBackground = text,
        surface = panel,
        onSurface = text,
        surfaceVariant = card,
        onSurfaceVariant = sub,
        surfaceContainer = inputBg,
        outline = border,
        error = danger,
    )
} else {
    lightColorScheme(
        primary = accent,
        onPrimary = onAccent,
        primaryContainer = accentSoft,
        onPrimaryContainer = text,
        secondary = accent,
        background = bg,
        onBackground = text,
        surface = panel,
        onSurface = text,
        surfaceVariant = card,
        onSurfaceVariant = sub,
        surfaceContainer = inputBg,
        outline = border,
        error = danger,
    )
}

@Composable
fun LsTheme(
    darkTheme: Boolean = androidx.compose.foundation.isSystemInDarkTheme(),
    /** Android 12+ 可选跟随壁纸取色；默认关，保持品牌一致 */
    dynamicPrimary: Color? = null,
    content: @Composable () -> Unit,
) {
    val base = if (darkTheme) DarkColors else LightColors
    val colors = if (dynamicPrimary != null) base.copy(accent = dynamicPrimary) else base
    CompositionLocalProvider(LocalLsColors provides colors) {
        MaterialTheme(
            colorScheme = colors.toMaterialScheme(darkTheme),
            typography = LsTypography,
            shapes = LsShapes,
            content = content,
        )
    }
}

/** 取值入口：`LsTheme.colors` */
object LsTheme {
    val colors: LsColors
        @Composable get() = LocalLsColors.current
}
