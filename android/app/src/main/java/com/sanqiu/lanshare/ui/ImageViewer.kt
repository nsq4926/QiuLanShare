package com.sanqiu.lanshare.ui

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animate
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.rememberTransformableState
import androidx.compose.foundation.gestures.transformable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Close
import androidx.compose.material.icons.outlined.Download
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.sanqiu.lanshare.net.MediaExporter
import com.sanqiu.lanshare.ui.theme.LsSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import kotlin.math.abs

/** 看图器里的一页：本地文件路径 + 显示名 */
data class ViewerItem(val path: String, val name: String)

/**
 * 全屏看图。
 *
 * 用 `Dialog(usePlatformDefaultWidth = false)` 而不是普通浮层：只有独立窗口才能盖住
 * 顶栏、输入区和系统栏，得到真正的全屏；返回键也由 Dialog 接管，不必和主界面抢 BackHandler。
 *
 * 手势：
 *  - **左右滑动切换上一张/下一张**（未放大时；放大后横向拖动用于平移，不切换）
 *  - 双指缩放 / 单指拖动画布 / 双击在 1× 与 2.5× 之间切换 / 单击（缩放过则先归位）关闭
 *
 * 为什么把"切换"放在外层 Box 上：`transformable` 在放大时会吃掉横向拖动，
 * 外层就收不到事件 —— 正好实现"放大后不切图"。未放大时不给 `transformable`，
 * 横向拖动才能落到外层，两个手势不会打架。
 */
@Composable
fun ImageViewerDialog(
    items: List<ViewerItem>,
    startIndex: Int,
    isImage: Boolean,
    onClose: () -> Unit,
) {
    if (items.isEmpty()) {
        onClose()
        return
    }
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()

    var index by remember { mutableIntStateOf(startIndex.coerceIn(0, items.size - 1)) }
    val current = items.getOrNull(index) ?: items[0]
    val bitmap by rememberBitmap(current.path, 2048)

    var scale by remember { mutableFloatStateOf(1f) }
    var dx by remember { mutableFloatStateOf(0f) }
    var dy by remember { mutableFloatStateOf(0f) }
    /** 翻页时整页的水平位移（px）：跟手拖动 + 松手后滑出、新页滑入 */
    var dragX by remember { mutableFloatStateOf(0f) }
    /** 图片实际绘制区域（px），用来把放大后的平移夹在边界内 */
    var box by remember { mutableStateOf(IntSize.Zero) }

    // 换页：缩放与位移必须归零，否则"上一张放大到 3×"会带到下一张
    LaunchedEffect(index) {
        scale = 1f
        dx = 0f
        dy = 0f
    }

    // 入场：缩放 + 透明度一起进来，比"直接出现"更像系统看图器
    var appeared by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) { appeared = true }
    val enter by animateFloatAsState(
        targetValue = if (appeared) 1f else 0.88f,
        animationSpec = spring(dampingRatio = Spring.DampingRatioLowBouncy, stiffness = Spring.StiffnessMediumLow),
        label = "viewerEnter",
    )
    val fade by animateFloatAsState(
        targetValue = if (appeared) 1f else 0f,
        animationSpec = tween(180),
        label = "viewerFade",
    )

    fun reset() {
        scale = 1f
        dx = 0f
        dy = 0f
    }

    val transformState = rememberTransformableState { zoomChange, panChange, _ ->
        val next = (scale * zoomChange).coerceIn(1f, 6f)
        scale = next
        if (next <= 1.001f) {
            dx = 0f
            dy = 0f
        } else {
            // 放大后能平移的范围就是"多出来的那半边"，超出就夹住，不让图被拖出屏幕
            val maxX = (next - 1f) * box.width / 2f
            val maxY = (next - 1f) * box.height / 2f
            dx = (dx + panChange.x).coerceIn(-maxX, maxX)
            dy = (dy + panChange.y).coerceIn(-maxY, maxY)
        }
    }

    val zoomed = scale > 1.001f

    val density = LocalDensity.current
    val screenW = with(density) { LocalConfiguration.current.screenWidthDp.dp.toPx() }
    val slop = with(density) { 60.dp.toPx() }

    /**
     * 松手后把当前页滑出去、换页、再把新页从另一侧滑进来。
     *
     * 直接 `index += 1` 是瞬切（用户报的"左右切换没有动画"），而且拖动过程中页面不动，
     * 手感像在拨一张贴纸。这里让位移跟手，松手补一段 160~200ms 的滑动。
     */
    suspend fun settlePage(dir: Int) {
        if (dir == 0) {
            animate(dragX, 0f, animationSpec = tween(180)) { v, _ -> dragX = v }
            return
        }
        val out = if (dir > 0) -screenW else screenW
        animate(dragX, out, animationSpec = tween(160)) { v, _ -> dragX = v }
        index = (index + dir).coerceIn(0, items.size - 1)
        dragX = -out
        animate(dragX, 0f, animationSpec = tween(200)) { v, _ -> dragX = v }
    }

    Dialog(
        onDismissRequest = onClose,
        properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false),
    ) {
        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(Color.Black.copy(alpha = 0.94f * fade))
                // 左右滑动切图（只在未放大时；放大后交给 transformable 平移）
                .pointerInput(items.size, index, zoomed) {
                    if (zoomed) return@pointerInput
                    var dragged = 0f
                    detectHorizontalDragGestures(
                        onDragStart = {
                            dragged = 0f
                            dragX = 0f
                        },
                        onDragEnd = {
                            val dir = when {
                                dragged <= -slop && index < items.size - 1 -> 1
                                dragged >= slop && index > 0 -> -1
                                else -> 0
                            }
                            scope.launch { settlePage(dir) }
                            dragged = 0f
                        },
                        onDragCancel = {
                            scope.launch {
                                animate(dragX, 0f, animationSpec = tween(180)) { v, _ -> dragX = v }
                            }
                            dragged = 0f
                        },
                        onHorizontalDrag = { _, amount ->
                            dragged += amount
                            // 到头了还给一点阻尼，让"没有下一张"是可感知的，而不是硬停
                            val atEdge = (dragged > 0 && index == 0) ||
                                (dragged < 0 && index == items.size - 1)
                            dragX = if (atEdge) dragged * 0.25f else dragged.coerceIn(-screenW, screenW)
                        },
                    )
                }
                // 点空白处关闭
                .clickable(
                    interactionSource = remember { androidx.compose.foundation.interaction.MutableInteractionSource() },
                    indication = null,
                    onClick = onClose,
                ),
            contentAlignment = Alignment.Center,
        ) {
            if (bitmap != null) {
                Image(
                    bitmap = bitmap!!,
                    contentDescription = current.name.ifEmpty { "图片" },
                    contentScale = ContentScale.Fit,
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(vertical = 64.dp)
                        .onSizeChanged { box = it }
                        .graphicsLayer {
                            scaleX = scale * enter
                            scaleY = scale * enter
                            translationX = dx + dragX
                            translationY = dy
                            // 翻页时淡一点：两页叠在一起的瞬间不会显得脏
                            alpha = 1f - (abs(dragX) / screenW).coerceIn(0f, 0.4f)
                        }
                        // ⚠️ transformable 必须**始终**挂着。早先写成
                        // `if (zoomed) Modifier.transformable(...)`，而 zoomed 由 scale 推出：
                        // 1× 时没挂 → 双指事件根本收不到 → 永远放不大（用户报的"安卓没有放大功能"）。
                        // 用 canPan 只挡"单指平移"：1× 时横向拖动留给外层翻页，双指缩放始终可用。
                        .then(
                            Modifier.transformable(
                                state = transformState,
                                canPan = { scale > 1.001f },
                                lockRotationOnZoomPan = true,
                            )
                        )
                        .pointerInput(Unit) {
                            detectTapGestures(
                                onTap = { if (scale > 1.001f) reset() else onClose() },
                                onDoubleTap = {
                                    if (scale > 1.001f) {
                                        reset()
                                    } else {
                                        scale = 2.5f
                                        dx = 0f
                                        dy = 0f
                                    }
                                },
                            )
                        },
                )
            } else {
                Text("图片无法解码", color = Color.White)
            }

            // 顶部操作条：页码 + 关闭 + 保存（深色玻璃圆钮，不抢注意力）
            Row(
                modifier = Modifier
                    .align(Alignment.TopEnd)
                    .statusBarsPadding()
                    .padding(horizontal = LsSpace.s3, vertical = LsSpace.s2)
                    .fillMaxWidth(),
                horizontalArrangement = Arrangement.End,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (items.size > 1) {
                    Text(
                        "${index + 1} / ${items.size}",
                        color = Color.White.copy(alpha = 0.85f),
                        style = MaterialTheme.typography.labelMedium,
                        modifier = Modifier.padding(end = LsSpace.s3),
                    )
                }
                GlassIcon(Icons.Outlined.Close, "关闭", onClose)
                Box(Modifier.size(LsSpace.s2))
                GlassIcon(Icons.Outlined.Download, "保存", {
                    val file = File(current.path)
                    val name = current.name.ifEmpty { file.name }
                    scope.launch {
                        val err = withContext(Dispatchers.IO) {
                            MediaExporter(ctx).export(file, isImage, name)
                        }
                        android.widget.Toast.makeText(
                            ctx,
                            if (err.isEmpty()) {
                                "已保存到" + (if (isImage) "相册" else "下载") + "/LanShare"
                            } else {
                                "保存失败：$err"
                            },
                            android.widget.Toast.LENGTH_SHORT,
                        ).show()
                    }
                })
            }
        }
    }
}

/** 深色背景上的玻璃圆形按钮 */
@Composable
private fun GlassIcon(icon: ImageVector, desc: String, onClick: () -> Unit) {
    Box(
        modifier = Modifier
            .size(40.dp)
            .clip(CircleShape)
            .background(Color.White.copy(alpha = 0.16f))
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, contentDescription = desc, tint = Color.White, modifier = Modifier.size(20.dp))
    }
}

/** 按采样率解码：列表里几十张图时不至于把内存打爆 */
internal fun decodeSampled(path: String, maxPx: Int): Bitmap? = runCatching {
    val f = File(path)
    if (!f.exists()) return@runCatching null
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeFile(path, bounds)
    if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return@runCatching null
    var sample = 1
    while (bounds.outWidth / sample > maxPx || bounds.outHeight / sample > maxPx) sample *= 2
    BitmapFactory.decodeFile(path, BitmapFactory.Options().apply { inSampleSize = sample })
}.getOrNull()

/** 解码到 Compose 位图；IO 线程做，进列表不卡帧 */
@Composable
internal fun rememberBitmap(path: String, maxPx: Int): State<ImageBitmap?> =
    produceState<ImageBitmap?>(null, path, maxPx) {
        value = withContext(Dispatchers.IO) { decodeSampled(path, maxPx)?.asImageBitmap() }
    }
