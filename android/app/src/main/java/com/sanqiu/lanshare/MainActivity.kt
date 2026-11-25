package com.sanqiu.lanshare

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import com.sanqiu.lanshare.core.LsSettings
import com.sanqiu.lanshare.core.LsSettingsStore
import com.sanqiu.lanshare.core.LsStore
import com.sanqiu.lanshare.core.Proto
import com.sanqiu.lanshare.net.NodeService
import com.sanqiu.lanshare.net.NodeState
import com.sanqiu.lanshare.net.ShareIn
import com.sanqiu.lanshare.probe.AppPaths
import com.sanqiu.lanshare.ui.AppScreen
import com.sanqiu.lanshare.ui.PrivacyConsentScreen
import com.sanqiu.lanshare.ui.theme.LsTheme

/**
 * 入口。
 *
 * 两处系统级能力（低版本自动降级）：
 *  - `enableEdgeToEdge()`：内容画到状态栏/手势条下方，配 `statusBarsPadding` 使用；
 *  - `LsTheme(darkTheme=…)`：主题随手动/系统切换，组件里不再出现字面色值。
 */
class MainActivity : ComponentActivity() {

    companion object {
        const val EXTRA_AUTOSTART = "autostart"
        /** 点通知进来：要打开的会话 */
        const val EXTRA_DEVICE_ID = "deviceId"
        const val EXTRA_CONV_ID = "convId"
    }

    override fun onResume() {
        super.onResume()
        NodeState.uiForeground = true
        NodeState.node?.setForeground(true)
    }

    override fun onPause() {
        super.onPause()
        NodeState.uiForeground = false
        NodeState.node?.setForeground(false)
    }

    override fun onNewIntent(intent: android.content.Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        routeFromIntent(intent)
        routeShareFromIntent(intent, restoring = false)
    }

    /** 通知带过来的会话：写进 NodeState，界面每秒刷一次时取走 */
    private fun routeFromIntent(i: android.content.Intent?) {
        val dev = i?.getStringExtra(EXTRA_DEVICE_ID) ?: return
        if (dev.isEmpty()) return
        NodeState.pendingDevice = dev
    }

    /**
     * 别的应用「分享到秋秋快传」：把被分享的 URI 入队。
     *
     * 真正的发送交给 `ChatArea`：它才知道当前会话是哪个、发送队列还有几个空位。
     * 还没选设备时先攒着，等用户在抽屉里点一台设备后再一起发。
     *
     * `restoring=true`（转屏/低内存重建）时**不**再收一次：系统会把同一个 Intent
     * 原样重放，再收一遍就变成重复发送。
     */
    private fun routeShareFromIntent(i: android.content.Intent?, restoring: Boolean) {
        if (restoring) return
        val uris = ShareIn.urisOf(i)
        if (uris.isEmpty()) return
        ShareIn.pushAll(uris)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        AppPaths.init(this)
        routeFromIntent(intent)
        routeShareFromIntent(intent, restoring = savedInstanceState != null)
        // 自动化用：am start ... --ez autostart true（服务保持 exported=false）
        // 普通启动也按上次的「接收服务」开关恢复监听（缺省 true）。
        // 不恢复的话：用户关一次再打开应用，界面是"未启动"，而设置里的两个发现开关
        // 都以"服务已在跑"为前提 —— 等于服务再也起不来（就是"开关全失效"那次报障）。
        val forced = intent?.getBooleanExtra(EXTRA_AUTOSTART, false) == true
        // 隐私政策未同意前**不启动接收服务**：不监听端口、不广播、不申请通知权限。
        // 商店审核会实测「首次打开是否先弹同意」，顺序错了就是合规问题。
        val saved = runCatching {
            LsSettingsStore(LsStore(AppPaths.requireDataDir())).load()
        }.getOrNull()
        val consented = (saved?.privacyAcceptedAt ?: 0L) > 0L
        val wantServer = consented && (forced || saved?.serverEnabled != false)
        if (wantServer && NodeState.node == null) {
            NodeService.start(this)
        }
        setContent { AppRoot(needConsent = !consented) }
    }
}

@Composable
private fun AppRoot(needConsent: Boolean) {
    var themeMode by remember { mutableStateOf(LsSettings.THEME_SYSTEM) }
    var consentNeeded by remember { mutableStateOf(needConsent) }
    val ctx = LocalContext.current

    // 节点可能稍后才起来，故短暂轮询一次把已保存的外观读出来。
    // key 用 consentNeeded：同意之后服务才启动，此时要再读一次，否则主题要等下次启动才生效。
    LaunchedEffect(consentNeeded) {
        if (consentNeeded) return@LaunchedEffect
        repeat(20) {
            val m = NodeState.node?.hub?.settings?.themeMode
            if (m != null) {
                themeMode = m
                return@LaunchedEffect
            }
            kotlinx.coroutines.delay(200)
        }
    }

    val dark = when (themeMode) {
        LsSettings.THEME_LIGHT -> false
        LsSettings.THEME_DARK -> true
        else -> isSystemInDarkTheme()
    }

    LsTheme(darkTheme = dark) {
        // 状态栏/导航栏图标颜色必须跟着主题走，否则暗色下图标几乎看不见（边缘到边缘的必然副作用）
        val view = LocalView.current
        if (!view.isInEditMode) {
            SideEffect {
                val act = view.context as? android.app.Activity ?: return@SideEffect
                val controller = androidx.core.view.WindowCompat.getInsetsController(act.window, view)
                controller.isAppearanceLightStatusBars = !dark
                controller.isAppearanceLightNavigationBars = !dark
            }
        }
        Surface(modifier = Modifier.fillMaxSize(), color = LsTheme.colors.bg) {
            if (consentNeeded) {
                PrivacyConsentScreen(
                    onAgree = {
                        runCatching {
                            val store = LsSettingsStore(LsStore(AppPaths.requireDataDir()))
                            val hub = NodeState.node?.hub
                            if (hub != null) {
                                hub.settings.privacyAcceptedAt = Proto.nowMs()
                                hub.saveSettings()
                            } else {
                                val s = store.load()
                                s.privacyAcceptedAt = Proto.nowMs()
                                store.save(s)
                            }
                        }
                        if (NodeState.node == null) NodeService.start(ctx)
                        consentNeeded = false
                    },
                    onDecline = { ctx.findActivity()?.finishAffinity() },
                )
            } else {
                AppScreen(onThemeModeChanged = { themeMode = it })
            }
        }
    }
}

/** LocalContext 常常是 ContextWrapper，一路拆到 Activity 才能退出应用 */
private fun android.content.Context.findActivity(): android.app.Activity? {
    var c: android.content.Context = this
    while (c is android.content.ContextWrapper) {
        if (c is android.app.Activity) return c
        c = c.baseContext
    }
    return c as? android.app.Activity
}
