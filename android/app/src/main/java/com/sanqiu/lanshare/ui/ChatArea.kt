package com.sanqiu.lanshare.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import android.util.Log
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.AttachFile
import androidx.compose.material.icons.outlined.Description
import androidx.compose.material.icons.outlined.Image
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.ui.graphics.TransformOrigin
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInWindow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.window.Popup
import androidx.compose.ui.window.PopupProperties
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.PointerEventType
import androidx.compose.ui.input.pointer.isSecondaryPressed
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.sanqiu.lanshare.core.LsMsg
import com.sanqiu.lanshare.net.TransferItem
import com.sanqiu.lanshare.net.Transfers
import com.sanqiu.lanshare.api.ChunkReader
import com.sanqiu.lanshare.core.Proto
import com.sanqiu.lanshare.net.MediaExporter
import com.sanqiu.lanshare.net.NodeState
import com.sanqiu.lanshare.net.ShareIn
import com.sanqiu.lanshare.net.ShareOut
import com.sanqiu.lanshare.ui.components.LsIconSquare
import com.sanqiu.lanshare.ui.components.LsPrimaryButton
import com.sanqiu.lanshare.ui.components.LsTextField
import com.sanqiu.lanshare.ui.theme.LsRadius
import com.sanqiu.lanshare.ui.theme.LsSpace
import com.sanqiu.lanshare.ui.theme.LsTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import java.io.File
import kotlin.math.roundToInt
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 消息区 + 输入区（`UI Design.md` §3.4 / §3.2）。
 *
 * 气泡按**绝对方向**分左右：`out` 是"我发出的"，永远在右。
 * 读消息要拿 `hub.gate`（与 HTTP 线程共用），进入即 `markRead`。
 *
 * 交互三条：
 *  - **点图片** → 全屏看图（[ImageViewerDialog]）；
 *  - **长按文字** → 系统文本选择菜单（`SelectionContainer` 交给系统，自带复制/全选）；
 *  - **右键 / 长按图片或文件** → 自定义菜单（复制文字 / 查看大图 / 分享 / 保存 / 重发）。
 */
@Composable
fun ChatArea(
    peerId: String,
    modifier: Modifier = Modifier,
    onRefresh: () -> Unit,
) {
    val c = LsTheme.colors
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()

    var msgs by remember(peerId) { mutableStateOf<List<LsMsg>>(emptyList()) }
    var transfers by remember { mutableStateOf<List<TransferItem>>(emptyList()) }
    /** 正在发、但还没入库的文件：会话里先出一行"发送中"，不等整份传完 */
    var outPending by remember { mutableStateOf<List<TransferItem>>(emptyList()) }
    var transferOpen by remember { mutableStateOf(false) }
    var transferCollapsed by remember { mutableStateOf(false) }
    var input by remember { mutableStateOf("") }
    var sending by remember { mutableStateOf(false) }
    var viewing by remember { mutableStateOf<LsMsg?>(null) }

    val listState = rememberLazyListState()

    LaunchedEffect(peerId) {
        while (true) {
            val h = NodeState.node?.hub
            if (h != null) {
                var loaded: List<LsMsg> = emptyList()
                synchronized(h.gate) {
                    loaded = h.conversationMessages(peerId)
                    h.markRead(Proto.makeConvId(h.self.deviceId, peerId))
                }
                msgs = loaded
            }
            Transfers.prune()
            // 每个会话的传输列表相互独立：只显示对端是本会话的那几条
            transfers = Transfers.snapshot().filter { it.peerId == peerId }
            // 已登记但还没入库的出站任务 = 会话里那一行"发送中"
            val known = msgs.map { it.msgId }.toHashSet()
            outPending = transfers.filter {
                it.dir == "out" && it.peerId == peerId && it.status == TransferItem.ACTIVE &&
                    it.msgId.isNotEmpty() && !known.contains(it.msgId)
            }
            // 同时 2 个以上在传 → 自动从输入栏上方展开（用户手动收起的除外）
            if (Transfers.activeCount() >= 2 && !transferCollapsed) transferOpen = true
            // 300 ms：传输列表要"看起来在动"，1 秒一次会明显滞后
            delay(300)
        }
    }

    LaunchedEffect(msgs.size, outPending.size) {
        if (msgs.isNotEmpty() || outPending.isNotEmpty()) {
            listState.animateScrollToItem(msgs.size + outPending.size - 1)
        }
    }

    // 选文件 / 选图片：**流式发送**（按片读 URI），不再把整份文件读进内存
    // 并发闸门：一次能选 99 个文件，全都不加限制地并发读会把内存/带宽打爆。2 个并发有界。
    val sendGate = remember { Semaphore(2) }
    val sendBytes: (Uri) -> Unit = { uri ->
        scope.launch {
            val n = NodeState.node ?: return@launch
            sendGate.withPermit {
                // 先问大小：4 GB 的文件也要能发，但必须先知道总长（分片协议要它）
                val size = withContext(Dispatchers.IO) {
                    runCatching {
                        ctx.contentResolver.openAssetFileDescriptor(uri, "r")?.use { it.length } ?: -1L
                    }.getOrDefault(-1L)
                }
                if (size <= 0) {
                    withContext(Dispatchers.Main) {
                        Toast.makeText(ctx, "读不到文件大小（空文件或无法访问）", Toast.LENGTH_LONG).show()
                    }
                    return@withPermit
                }
                val name = queryDisplayName(ctx, uri)
                val mime = ctx.contentResolver.getType(uri) ?: "application/octet-stream"
                try {
                    withContext(Dispatchers.IO) {
                        n.api.sendOutboundFileStream(
                            peerId, name, mime, size,
                            UriChunkReader(ctx, uri),
                            UriChunkReader(ctx, uri),
                        )
                    }
                } catch (e: Exception) {
                    Log.e("LanShare", "stream send failed: " + uri, e)
                    withContext(Dispatchers.Main) {
                        Toast.makeText(ctx, "发送失败：" + (e.message ?: e.javaClass.simpleName), Toast.LENGTH_LONG).show()
                    }
                }
                onRefresh()
            }
        }
    }

    // 多选：多张图/多个文件同时在传，才会触发「>1 个在传自动展开传输抽屉」
    val pickImage = rememberLauncherForActivityResult(ActivityResultContracts.GetMultipleContents()) { uris ->
        // 有些 ROM 的单选模式只回 data.data、不回 ClipData；为空必须说出来，
        // 否则用户看到的就是"点完了什么都没发生"
        if (uris.isEmpty()) Toast.makeText(ctx, "没有选中任何图片", Toast.LENGTH_SHORT).show()
        uris.forEach { sendBytes(it) }
    }
    val pickFile = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        Log.i("LanShare", "pickFile returned " + uris.size + " uri(s)")
        if (uris.isEmpty()) Toast.makeText(ctx, "没有选中任何文件", Toast.LENGTH_SHORT).show()
        uris.forEach { sendBytes(it) }
    }

    // 系统分享进来的文件（别的应用「分享到秋秋快传」）：有会话才发。
    // 没选设备时 URI 一直留在 ShareIn 里，等用户点开一台设备、本组件被组合出来再发。
    LaunchedEffect(peerId) {
        while (true) {
            val shared = ShareIn.drain()
            if (shared.isNotEmpty()) {
                Log.i("LanShare", "sending " + shared.size + " shared uri(s) to " + peerId)
                shared.forEach { sendBytes(it) }
            }
            delay(500)
        }
    }

    Column(modifier = modifier.imePadding()) {
        // 消息区
        if (msgs.isEmpty() && outPending.isEmpty()) {
            Box(modifier = Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
                Text(
                    "还没有消息\n发一条试试；失败会显示原因，不会假装发出去",
                    style = MaterialTheme.typography.bodySmall,
                    color = c.faint,
                    textAlign = TextAlign.Center,
                )
            }
        } else {
            LazyColumn(
                state = listState,
                modifier = Modifier.weight(1f).fillMaxWidth().padding(horizontal = LsSpace.page),
                verticalArrangement = Arrangement.spacedBy(LsSpace.s2),
                contentPadding = PaddingValues(vertical = LsSpace.s3),
            ) {
                items(msgs, key = { it.id }) { m ->
                    Bubble(
                        m = m,
                        onResend = {
                            scope.launch {
                                withContext(Dispatchers.IO) {
                                    NodeState.node?.api?.sendOutboundText(peerId, m.text, m.msgId)
                                }
                                onRefresh()
                            }
                        },
                        onOpenImage = { viewing = it },
                    )
                }
                // 发送中的文件：先在这里出一行进度，不必等整份传完才上屏
                items(outPending, key = { "out-" + it.id }) { t ->
                    OutgoingPlaceholder(t)
                }
            }
        }

        // 传输抽屉：贴着输入栏向上展开
        if (transfers.isNotEmpty()) {
            TransferPanel(
                items = transfers,
                open = transferOpen,
                onToggle = {
                    transferOpen = !transferOpen
                    transferCollapsed = !transferOpen
                },
                onRemove = { t ->
                    // 在传的 → 取消（循环立刻停）；已结束的 → 删掉这条记录
                    if (t.status == TransferItem.ACTIVE) Transfers.cancel(t.xid)
                    else Transfers.remove(t.xid)
                    transfers = Transfers.snapshot()
                    // 取消在传任务时，会话里那行"发送中"也要跟着消失
                    outPending = outPending.filter { it.xid != t.xid }
                },
            )
        }

        // 输入区（Composer）
        Surface(color = c.panel, shadowElevation = if (c.isDark) 0.dp else 8.dp) {
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = LsSpace.s3, vertical = LsSpace.s3),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                LsIconSquare(
                    Icons.Outlined.Image, "发送图片",
                    onClick = { pickImage.launch("image/*") },
                )
                Spacer(Modifier.width(LsSpace.s2))
                LsIconSquare(
                    Icons.Outlined.AttachFile, "发送文件",
                    onClick = { pickFile.launch(arrayOf("*/*")) },
                )
                Spacer(Modifier.width(LsSpace.s2))
                LsTextField(
                    value = input,
                    onValueChange = { input = it },
                    modifier = Modifier.weight(1f),
                    placeholder = "输入消息…",
                    maxLines = 4,
                )
                Spacer(Modifier.width(LsSpace.s2))
                LsPrimaryButton(
                    text = if (sending) "发送中" else "发送",
                    enabled = !sending && input.isNotBlank(),
                    onClick = {
                        val t = input.trim()
                        if (t.isEmpty()) return@LsPrimaryButton
                        input = ""
                        sending = true
                        scope.launch {
                            withContext(Dispatchers.IO) {
                                NodeState.node?.api?.sendOutboundText(peerId, t, "")
                            }
                            sending = false
                            onRefresh()
                        }
                    },
                )
            }
        }
    }

    // 全屏看图（Dialog 自带返回键处理）。带上本会话所有图片，支持左右滑动切换。
    viewing?.let { m ->
        val images: List<Pair<LsMsg, ViewerItem>> = msgs
            .filter { it.kind == Proto.KIND_IMAGE }
            .mapNotNull { x -> localFileOf(x)?.let { f -> x to ViewerItem(f.absolutePath, x.fileName) } }
        val list: List<ViewerItem> = images.map { it.second }
        if (list.isNotEmpty()) {
            val startIdx = images.indexOfFirst { it.first.id == m.id }.coerceAtLeast(0)
            ImageViewerDialog(
                items = list,
                startIndex = startIdx,
                isImage = m.kind == Proto.KIND_IMAGE,
                onClose = { viewing = null },
            )
        }
    }
}


/**
 * 传输抽屉：头部「总大小/已发送 · 速度 · 展开/折叠」，每行一条进度。
 *
 * 配色/圆角/间距全部取主题令牌，和输入栏、抽屉保持一致；
 * 面板底色半透明一点，让它读起来是"浮在聊天上"，而不是又一条工具栏。
 */
@Composable
private fun TransferPanel(
    items: List<TransferItem>,
    open: Boolean,
    onToggle: () -> Unit,
    onRemove: (TransferItem) -> Unit,
) {
    val c = LsTheme.colors
    // 头部只统计"在传"的：列表现在保留历史，把已完成的也算进"总大小"会变成一个假的进度
    val active = items.filter { it.status == TransferItem.ACTIVE }
    val total = active.sumOf { it.total }
    val done = active.sumOf { it.done }
    val speed = active.sumOf { it.speed.toLong() }
    val doneCount = items.count { it.status == TransferItem.DONE }

    Surface(
        color = if (c.isDark) c.panel.copy(alpha = 0.96f) else c.panel.copy(alpha = 0.97f),
        shape = RoundedCornerShape(topStart = LsRadius.card, topEnd = LsRadius.card),
        shadowElevation = 6.dp,
    ) {
        Column(modifier = Modifier.fillMaxWidth()) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable(onClick = onToggle)
                    .padding(horizontal = LsSpace.page, vertical = LsSpace.s2),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    if (active.isNotEmpty()) {
                        // 标签是「总大小/已发送」，值就必须是 total / done（以前打印成 done/total，看着像倒过来了）
                        "总大小/已发送 ${Transfers.fmtSize(total)} / ${Transfers.fmtSize(done)}"
                    } else {
                        "传输记录 $doneCount 条"
                    },
                    style = MaterialTheme.typography.labelSmall,
                    color = c.text,
                    fontWeight = FontWeight.Medium,
                    modifier = Modifier.weight(1f),
                    maxLines = 1,
                )
                Text(
                    if (active.isNotEmpty()) "速度 ${Transfers.fmtSpeed(speed)}" else "",
                    style = MaterialTheme.typography.labelSmall,
                    color = c.sub,
                    maxLines = 1,
                )
                Spacer(Modifier.width(LsSpace.s3))
                Text(
                    if (open) "折叠" else "展开",
                    style = MaterialTheme.typography.labelSmall,
                    color = c.accent,
                    fontWeight = FontWeight.Medium,
                )
            }
            if (open) {
                Column(
                    modifier = Modifier
                        .heightIn(max = 190.dp)
                        .verticalScroll(rememberScrollState())
                        .padding(horizontal = LsSpace.page)
                        .padding(bottom = LsSpace.s2),
                    verticalArrangement = Arrangement.spacedBy(LsSpace.s1),
                ) {
                    items.forEach { t ->
                        val pct = Transfers.pct(t.done, t.total)
                        Column(modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Box(
                                    modifier = Modifier
                                        .background(c.accentSoft, RoundedCornerShape(LsRadius.small))
                                        .padding(horizontal = 4.dp, vertical = 1.dp)
                                ) {
                                    Text(
                                        if (t.dir == "in") "收" else "发",
                                        style = MaterialTheme.typography.labelSmall,
                                        color = c.accent,
                                    )
                                }
                                Spacer(Modifier.width(LsSpace.s2))
                                Text(
                                    t.name.ifEmpty { "文件" },
                                    style = MaterialTheme.typography.labelSmall,
                                    color = c.text,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                    modifier = Modifier.weight(1f),
                                )
                                Text(
                                    "${Transfers.fmtSize(t.done)} / ${Transfers.fmtSize(t.total)}",
                                    style = MaterialTheme.typography.labelSmall,
                                    color = c.sub,
                                    maxLines = 1,
                                )
                                Spacer(Modifier.width(LsSpace.s2))
                                // × ：在传的取消（循环立刻停），已结束的删掉这条记录
                                Box(
                                    modifier = Modifier
                                        .size(22.dp)
                                        .clip(RoundedCornerShape(LsRadius.small))
                                        .clickable { onRemove(t) },
                                    contentAlignment = Alignment.Center,
                                ) {
                                    Text(
                                        "✕",
                                        style = MaterialTheme.typography.labelSmall,
                                        color = c.faint,
                                    )
                                }
                            }
                            Spacer(Modifier.height(4.dp))
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Box(
                                    modifier = Modifier
                                        .weight(1f)
                                        .height(4.dp)
                                        .clip(RoundedCornerShape(2.dp))
                                        .background(c.inputBg)
                                ) {
                                    Box(
                                        modifier = Modifier
                                            .fillMaxWidth(if (pct <= 0) 0f else pct / 100f)
                                            .height(4.dp)
                                            .clip(RoundedCornerShape(2.dp))
                                            .background(
                                                when (t.status) {
                                                    TransferItem.FAILED -> c.danger
                                                    TransferItem.DONE -> c.success
                                                    TransferItem.CANCELLED -> c.faint
                                                    else -> c.accent
                                                }
                                            )
                                    )
                                }
                                Text(
                                    when (t.status) {
                                        TransferItem.ACTIVE -> Transfers.fmtSpeed(t.speed.toLong())
                                        else -> {
                                            // 传完不能显示 0：给这一趟的**平均速度**
                                            val avg = Transfers.avgSpeed(t)
                                            if (avg > 0) "均 " + Transfers.fmtSpeed(avg) else "—"
                                        }
                                    },
                                    style = MaterialTheme.typography.labelSmall,
                                    color = if (t.status == TransferItem.FAILED) c.danger else c.faint,
                                    modifier = Modifier.padding(start = LsSpace.s2).width(76.dp),
                                    maxLines = 1,
                                    textAlign = TextAlign.End,
                                )
                                Text(
                                    if (t.status == TransferItem.ACTIVE) "$pct%" else "—",
                                    style = MaterialTheme.typography.labelSmall,
                                    color = c.faint,
                                    modifier = Modifier.width(36.dp),
                                    textAlign = TextAlign.End,
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}

/** 会话里"正在发送"的一行：与出站气泡同色同形，进度实时刷新（消息入库后自动消失） */
@Composable
private fun OutgoingPlaceholder(t: TransferItem) {
    val c = LsTheme.colors
    val pct = Transfers.pct(t.done, t.total)
    Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
        Surface(
            color = c.outBg,
            shape = RoundedCornerShape(LsRadius.bubble),
            modifier = Modifier.widthIn(max = 300.dp),
        ) {
            Column(modifier = Modifier.padding(horizontal = LsSpace.s3, vertical = LsSpace.s3)) {
                Text(
                    "发送中 · " + t.name.ifEmpty { "文件" },
                    style = MaterialTheme.typography.bodyLarge,
                    color = c.outText,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.height(LsSpace.s1))
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(4.dp)
                        .clip(RoundedCornerShape(2.dp))
                        .background(c.inputBg)
                ) {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth(if (pct <= 0) 0f else pct / 100f)
                            .height(4.dp)
                            .clip(RoundedCornerShape(2.dp))
                            .background(c.accent)
                    )
                }
                Spacer(Modifier.height(4.dp))
                Text(
                    "${Transfers.fmtSize(t.done)} / ${Transfers.fmtSize(t.total)} · $pct%",
                    style = MaterialTheme.typography.labelSmall,
                    color = c.outSub,
                )
            }
        }
    }
}

/** 气泡：接收白底细边、发送浅蓝底；失败显示原因 + 重发 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun Bubble(
    m: LsMsg,
    onResend: () -> Unit,
    onOpenImage: (LsMsg) -> Unit,
) {
    val c = LsTheme.colors
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    val isOut = m.dir == Proto.DIR_OUT
    val failed = m.status == Proto.STATUS_FAILED
    val file = rememberLocalFile(m)

    var menuOpen by remember { mutableStateOf(false) }
    // 气泡左上角在窗口中的位置 + 点击点（相对气泡）。菜单按"窗口坐标"定位，
    // 不用 DropdownMenu 的自动翻转：它会莫名其妙翻到会话头上面去。
    var bubbleOrigin by remember { mutableStateOf(Offset.Zero) }
    var pressAt by remember { mutableStateOf(Offset.Zero) }

    val copyText = {
        if (m.text.isNotEmpty()) {
            val cm = ctx.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            cm.setPrimaryClip(ClipData.newPlainText("LanShare", m.text))
            Toast.makeText(ctx, "已复制", Toast.LENGTH_SHORT).show()
        }
    }
    val saveFile = {
        val f = file
        if (f != null) {
            scope.launch {
                val err = withContext(Dispatchers.IO) {
                    MediaExporter(ctx).export(
                        f,
                        m.kind == Proto.KIND_IMAGE,
                        m.fileName.ifEmpty { f.name },
                    )
                }
                Toast.makeText(
                    ctx,
                    if (err.isEmpty()) {
                        "已保存到" + (if (m.kind == Proto.KIND_IMAGE) "相册" else "下载") + "/LanShare"
                    } else {
                        "保存失败：$err"
                    },
                    Toast.LENGTH_SHORT,
                ).show()
            }
        }
    }

    Column(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                // 右键：桌面模式 / 鼠标 / 触控笔的次要键。与长按不冲突
                .onSecondaryClick { pos ->
                    pressAt = pos
                    menuOpen = true
                },
            horizontalArrangement = if (isOut) Arrangement.End else Arrangement.Start,
        ) {
            Box(modifier = Modifier.onGloballyPositioned { bubbleOrigin = it.positionInWindow() }) {
                Surface(
                    color = when {
                        failed -> MaterialTheme.colorScheme.errorContainer
                        isOut -> c.outBg
                        else -> c.card
                    },
                    shape = RoundedCornerShape(LsRadius.bubble),
                    border = if (isOut || failed) null else BorderStroke(1.dp, c.border),
                    modifier = Modifier.widthIn(max = 300.dp),
                ) {
                    Column(modifier = Modifier.padding(horizontal = LsSpace.s3, vertical = LsSpace.s3)) {
                        when (m.kind) {
                            Proto.KIND_IMAGE -> {
                                if (file != null) {
                                    ImageBubble(
                                        path = file.absolutePath,
                                        isOut = isOut,
                                        onOpen = { onOpenImage(m) },
                                        onLongPress = {
                                            pressAt = Offset.Zero
                                            menuOpen = true
                                        },
                                    )
                                } else {
                                    FileBody(m, isOut)
                                }
                            }

                            Proto.KIND_FILE -> FileBody(
                                m = m,
                                isOut = isOut,
                                modifier = Modifier.combinedClickable(
                                    onClick = { saveFile() },
                                    onLongClick = {
                                        pressAt = Offset.Zero
                                        menuOpen = true
                                    },
                                ),
                            )

                            Proto.KIND_SYSTEM -> Text(
                                m.text,
                                style = MaterialTheme.typography.labelSmall,
                                color = if (isOut) c.outSub else c.sub,
                            )

                            // 长按交给系统文本选择菜单（自带复制/全选/翻译等）
                            else -> SelectionContainer {
                                Text(
                                    m.text,
                                    style = MaterialTheme.typography.bodyLarge,
                                    color = if (isOut) c.outText else c.text,
                                )
                            }
                        }

                        if (failed) {
                            Spacer(Modifier.height(LsSpace.s1))
                            Text(
                                "${m.reason.ifEmpty { Proto.REASON_UNREACHABLE }}，未发送",
                                style = MaterialTheme.typography.labelSmall,
                                color = c.danger,
                            )
                            if (m.kind == Proto.KIND_TEXT && m.text.isNotEmpty()) {
                                TextButton(
                                    onClick = onResend,
                                    contentPadding = PaddingValues(0.dp),
                                ) {
                                    Text("重发", style = MaterialTheme.typography.labelLarge, color = c.accent)
                                }
                            }
                        }
                    }
                }

                // 菜单：锚在点击处（右键）或气泡左上角（长按），越界自动回收
                MessageMenu(
                    open = menuOpen,
                    originInWindow = bubbleOrigin,
                    pressAt = pressAt,
                    onDismiss = { menuOpen = false },
                    items = buildList {
                        if (m.text.isNotEmpty() && m.kind == Proto.KIND_TEXT) {
                            add(MenuAction("复制文字") {
                                menuOpen = false
                                copyText()
                            })
                        }
                        if (m.kind == Proto.KIND_IMAGE && file != null) {
                            add(MenuAction("查看大图") {
                                menuOpen = false
                                onOpenImage(m)
                            })
                        }
                        if (file != null) {
                            add(MenuAction("分享") {
                                menuOpen = false
                                val f = file
                                val err = ShareOut.share(
                                    ctx,
                                    f,
                                    m.fileName.ifEmpty { f.name },
                                    m.kind == Proto.KIND_IMAGE,
                                )
                                if (err.isNotEmpty()) {
                                    Toast.makeText(ctx, err, Toast.LENGTH_SHORT).show()
                                }
                            })
                            add(
                                MenuAction(if (m.kind == Proto.KIND_IMAGE) "保存到相册" else "保存到下载") {
                                    menuOpen = false
                                    saveFile()
                                }
                            )
                        }
                        if (failed && m.kind == Proto.KIND_TEXT) {
                            add(MenuAction("重新发送") {
                                menuOpen = false
                                onResend()
                            })
                        }
                    },
                )
            }
        }
        Spacer(Modifier.height(3.dp))
        Text(
            timeText(m.time),
            style = MaterialTheme.typography.labelSmall,
            color = c.faint,
            modifier = Modifier.fillMaxWidth(),
            textAlign = if (isOut) TextAlign.End else TextAlign.Start,
        )
        Spacer(Modifier.height(LsSpace.s1))
    }
}

private class MenuAction(val label: String, val onPick: () -> Unit)

/**
 * 消息操作菜单。
 *
 * 用 `Popup` + 自定义 `PopupPositionProvider` 而不是 `DropdownMenu`：
 * DropdownMenu 的自动翻转在会话列表里会翻到会话头上面去，位置不可控；
 * 右键菜单的语义本来就是"出现在光标处"，位置必须算得准。
 */
@Composable
private fun MessageMenu(
    open: Boolean,
    originInWindow: Offset,
    pressAt: Offset,
    onDismiss: () -> Unit,
    items: List<MenuAction>,
) {
    if (!open || items.isEmpty()) return
    val c = LsTheme.colors
    val density = LocalDensity.current
    val margin = with(density) { 8.dp.roundToPx() }

    // 入场：从左上角轻微放大 + 淡入，跟手感的 spring
    var shown by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) { shown = true }
    val scale by androidx.compose.animation.core.animateFloatAsState(
        targetValue = if (shown) 1f else 0.92f,
        animationSpec = spring(dampingRatio = Spring.DampingRatioNoBouncy, stiffness = Spring.StiffnessMedium),
        label = "menuScale",
    )
    val alpha by androidx.compose.animation.core.animateFloatAsState(
        targetValue = if (shown) 1f else 0f,
        animationSpec = tween(120),
        label = "menuAlpha",
    )

    val point = with(density) {
        IntOffset(
            (originInWindow.x + pressAt.x).roundToInt(),
            (originInWindow.y + pressAt.y).roundToInt(),
        )
    }

    Popup(
        popupPositionProvider = PointPositionProvider(point, margin),
        onDismissRequest = onDismiss,
        properties = PopupProperties(focusable = true),
    ) {
        Surface(
            shape = RoundedCornerShape(LsRadius.control + 2.dp),
            color = c.panel,
            border = BorderStroke(1.dp, c.border),
            shadowElevation = 16.dp,
            // width(IntrinsicSize.Max)：Popup 的测量约束是"尽可能大"，
            // 不加这句菜单会横着撑满整行（真机上看起来像一条通栏面板）
            modifier = Modifier
                .widthIn(max = 240.dp)
                .width(androidx.compose.foundation.layout.IntrinsicSize.Max)
                .graphicsLayer {
                    scaleX = scale
                    scaleY = scale
                    this.alpha = alpha
                    transformOrigin = TransformOrigin(0f, 0f)
                },
        ) {
            Column {
                items.forEach { a ->
                    Text(
                        a.label,
                        style = MaterialTheme.typography.bodyLarge,
                        color = c.text,
                        modifier = Modifier
                            .fillMaxWidth()
                            .clickable(onClick = a.onPick)
                            .padding(horizontal = LsSpace.s5, vertical = LsSpace.s4),
                    )
                }
            }
        }
    }
}

/** 把菜单放在指定窗口坐标上；右边/下边放不下就往回收，不翻页不闪 */
private class PointPositionProvider(
    private val point: IntOffset,
    private val margin: Int,
) : androidx.compose.ui.window.PopupPositionProvider {
    override fun calculatePosition(
        anchorBounds: androidx.compose.ui.unit.IntRect,
        windowSize: androidx.compose.ui.unit.IntSize,
        layoutDirection: androidx.compose.ui.unit.LayoutDirection,
        popupContentSize: androidx.compose.ui.unit.IntSize,
    ): IntOffset {
        var x = point.x
        var y = point.y
        if (x + popupContentSize.width + margin > windowSize.width) {
            x = windowSize.width - popupContentSize.width - margin
        }
        if (y + popupContentSize.height + margin > windowSize.height) {
            // 下方不够就贴在点击点上方
            y = point.y - popupContentSize.height
        }
        return IntOffset(
            x.coerceIn(margin, (windowSize.width - popupContentSize.width - margin).coerceAtLeast(margin)),
            y.coerceIn(margin, (windowSize.height - popupContentSize.height - margin).coerceAtLeast(margin)),
        )
    }
}

/** 图片气泡：真缩略图，点开全屏 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun ImageBubble(
    path: String,
    isOut: Boolean,
    onOpen: () -> Unit,
    onLongPress: (Offset) -> Unit,
) {
    val bmp by rememberBitmap(path, 1024)
    val image = bmp ?: return
    val ratio = image.width.toFloat() / image.height.toFloat()
    // 定尺寸而不是交给 intrinsic：一张 4000px 的原图会按 px→dp 变成一屏那么大
    val (w, h) = if (ratio >= 1f) {
        val ww = 220.dp
        ww to (ww / ratio)
    } else {
        val hh = 240.dp
        (hh * ratio) to hh
    }
    Box {
        Image(
            bitmap = image,
            contentDescription = "图片消息",
            contentScale = ContentScale.Crop,
            modifier = Modifier
                .size(w, h)
                .clip(RoundedCornerShape(LsRadius.control))
                .combinedClickable(
                    onClick = onOpen,
                    onLongClick = { onLongPress(Offset.Zero) },
                ),
        )
    }
}

/** 文件/图片（无本地副本时）：图标容器 + 文件名 + 大小 */
@Composable
private fun FileBody(
    m: LsMsg,
    isOut: Boolean,
    modifier: Modifier = Modifier,
) {
    val c = LsTheme.colors
    Row(modifier = modifier, verticalAlignment = Alignment.CenterVertically) {
        Box(
            modifier = Modifier
                .size(36.dp)
                .background(
                    if (isOut) c.accentSoft else c.inputBg,
                    RoundedCornerShape(LsRadius.control),
                ),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                if (m.kind == Proto.KIND_IMAGE) Icons.Outlined.Image else Icons.Outlined.Description,
                contentDescription = null,
                tint = if (isOut) c.outSub else c.accent,
                modifier = Modifier.size(18.dp),
            )
        }
        Spacer(Modifier.width(LsSpace.s2))
        Column(modifier = Modifier.widthIn(max = 200.dp)) {
            Text(
                m.fileName.ifEmpty { if (m.kind == Proto.KIND_IMAGE) "图片" else "文件" },
                style = MaterialTheme.typography.bodyLarge,
                color = if (isOut) c.outText else c.text,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                formatSize(m.fileSize),
                style = MaterialTheme.typography.labelSmall,
                color = if (isOut) c.outSub else c.faint,
            )
        }
    }
}

/**
 * 右键（次要键按下）回调。
 *
 * 只认 `buttons.isSecondaryPressed`，不认"长按"：长按要留给系统文本选择/自定义菜单，
 * 两者混在一个手势里会互相抢事件，最后哪个都不跟手。
 */
private fun Modifier.onSecondaryClick(onClick: (Offset) -> Unit): Modifier = this.pointerInput(Unit) {
    awaitPointerEventScope {
        while (true) {
            val e = awaitPointerEvent(PointerEventPass.Main)
            if (e.type == PointerEventType.Press && e.buttons.isSecondaryPressed) {
                val pos = e.changes.firstOrNull()?.position ?: Offset.Zero
                e.changes.forEach { it.consume() }
                onClick(pos)
            }
        }
    }
}

/** 消息对应的本地副本（收到的会落盘，发出的也留了一份） */
private fun localFileOf(m: LsMsg): File? {
    if (m.kind == Proto.KIND_TEXT || m.kind == Proto.KIND_SYSTEM) return null
    val hub = NodeState.node?.hub ?: return null
    val rel = if (m.fileRel.isNotEmpty()) m.fileRel else m.fileName
    if (rel.isEmpty()) return null
    val f = File(hub.store.getSaveDir(), rel)
    return if (f.exists()) f else null
}

@Composable
private fun rememberLocalFile(m: LsMsg): File? =
    remember(m.id, m.fileRel, m.fileName, m.kind) { localFileOf(m) }

private fun timeText(ms: Long): String =
    SimpleDateFormat("MM-dd HH:mm", Locale.getDefault()).format(Date(ms))

private fun formatSize(bytes: Long): String = when {
    bytes <= 0 -> ""
    bytes >= 1024L * 1024 -> "%.1f MB".format(bytes / 1024.0 / 1024.0)
    bytes >= 1024L -> "%.1f KB".format(bytes / 1024.0)
    else -> "$bytes B"
}

/**
 * 读 picker 给的 URI。
 *
 * 必须有三条路，因为不同 ROM 给的 URI 形态不同（真机踩过）：
 *   1. `content://` 正常打开；
 *   2. DocumentsUI 在某些定制 ROM 上对 `content://…externalstorage.documents/…`
 *      直接抛 `ENOENT`（OPPO A57 / Android 6 实测）→ 用返回路径按文件读；
 *   3. `file://` 直接读路径。
 * 三条都不行才抛，调用方负责把原因响亮地告诉用户。
 */


/**
 * 把 picker 的 URI 变成"按位置读"的读取器。
 * 一次只在内存里留一片（1 MB）；offset 可以回跳（409 续传时要按服务端的进度重新对齐）。
 */
private class UriChunkReader(private val ctx: Context, private val uri: Uri) : ChunkReader {
    private var ins: java.io.InputStream? = null
    private var pos = 0L

    private fun close() {
        runCatching { ins?.close() }
        ins = null
    }

    private fun openAt(offset: Long): Boolean {
        close()
        val stream = try {
            ctx.contentResolver.openInputStream(uri)
        } catch (e: Exception) {
            Log.e("LanShare", "open " + uri + " failed", e)
            null
        } ?: return false
        var skipped = 0L
        try {
            while (skipped < offset) {
                val k = stream.skip(offset - skipped)
                if (k <= 0) { stream.close(); return false }
                skipped += k
            }
        } catch (e: Exception) {
            runCatching { stream.close() }
            return false
        }
        ins = stream
        pos = offset
        return true
    }

    override fun readAt(offset: Long, len: Int): ByteArray? {
        if (ins == null || offset != pos) {
            if (!openAt(offset)) return null
        }
        val buf = ByteArray(len)
        var read = 0
        try {
            while (read < len) {
                val k = ins!!.read(buf, read, len - read)
                if (k < 0) break
                read += k
            }
        } catch (e: Exception) {
            Log.e("LanShare", "read at " + offset + " failed", e)
            close()
            return null
        }
        if (read <= 0) { close(); return null }
        pos += read
        return if (read == len) buf else buf.copyOf(read)
    }
}

private fun queryDisplayName(ctx: Context, uri: Uri): String {
    runCatching {
        ctx.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cur ->
            if (cur.moveToFirst()) {
                val idx = cur.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (idx >= 0) {
                    val name = cur.getString(idx)
                    if (!name.isNullOrBlank()) return name
                }
            }
        }
    }
    return uri.lastPathSegment?.substringAfterLast('/') ?: "file"
}
