// WebView2 承载：加载核心的本地 HTTP 页面（同源，/api/* 无需改一个字）。
// 用官方 C/C++ API + WRL 回调，不依赖 C++/WinRT 或额外 NuGet 运行时。
#include "App.h"
#include "../src/Common.h"
#include <wrl.h>
#include "WebView2.h"

#include <string>

using namespace Microsoft::WRL;

namespace
{
    ComPtr<ICoreWebView2Environment>    g_env;
    ComPtr<ICoreWebView2Controller>     g_controller;
    ComPtr<ICoreWebView2>               g_webview;
    HWND                                g_host = nullptr;
    std::wstring                        g_url;
    bool                                g_failed = false;
    // 页面还没加载完时，收到的"定位会话"请求先存着，导航完成后补做。
    // **不能丢**：通知点击可能早于页面 ready，丢了就是"点了没反应"。
    std::string                         g_pendingConv;

    void ResizeWebView()
    {
        if (!g_controller || !g_host) return;
        RECT rc{};
        if (!::GetClientRect(g_host, &rc)) return;
        if (rc.right <= rc.left || rc.bottom <= rc.top) return;
        // 进程是 PerMonitorV2 感知（见 app\LanShare.manifest），GetClientRect
        // 返回物理像素，WebView2 的 SetBounds 也用物理像素 —— 两者一致。
        // zoom factor 传 1.0：让 WebView2 按显示器 DPI 自行缩放，
        // 页面才是"原生清晰"，不会被拉伸发虚。
        g_controller->SetBoundsAndZoomFactor(RECT{ 0, 0, rc.right, rc.bottom }, 1.0);
    }
}

bool WebViewInit(HWND host, const wchar_t* url)
{
    g_host = host;
    g_url = url ? url : L"";

    // 用 exe 同级的 WebView2UserData 作为用户数据目录，绿色版不往 %LOCALAPPDATA% 写。
    wchar_t exeDir[MAX_PATH] = { 0 };
    DWORD n = ::GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    std::wstring dir(exeDir);
    const size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = dir.substr(0, pos);
    const std::wstring userData = dir + L"\\LanShare\\WebView2UserData";
    ::CreateDirectoryW(userData.c_str(), nullptr);

    // CreateCoreWebView2EnvironmentWithOptions 是异步的：回调里再创建 controller。
    // 必须传 nullptr browserExecutableFolder（用系统 Evergreen Runtime）、
    // userDataFolder 传我们的绿色版目录。
    HRESULT hr = ::CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userData.c_str(),
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || env == nullptr)
                {
                    g_failed = true;
                    // WebView2 Runtime 没装：明确告知，不静默白屏
                    ::MessageBoxW(nullptr,
                        L"无法初始化界面引擎（WebView2）。\n"
                        L"请安装 Microsoft Edge WebView2 Runtime 后重试。\n"
                        L"后台服务仍可通过手机访问 http://<本机IP>:<端口>/ 使用。",
                        L"秋秋快传 · 界面引擎不可用", MB_OK | MB_ICONWARNING);
                    return result;
                }

                g_env = env;
                HRESULT hr2 = g_env->CreateCoreWebView2Controller(
                    g_host,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT result2, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result2) || controller == nullptr)
                            {
                                g_failed = true;
                                return result2;
                            }
                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);
                            if (g_webview)
                            {
                                // 本地 HTTP 页面：与 /api/* 同源，网页零改动。
                                // 带 ?desk=1 → 页面自己切"桌面模式"样式（卡片铺满客户区）。
                                // **不再用脚本注入 CSS**：document-created 阶段
                                // documentElement 可能为 null，整段注入会静默失效，
                                // 表现为窗口上下留缝（用户实测）。页面自己判断最稳。
                                std::wstring url = g_url;
                                if (url.find(L'?') == std::wstring::npos) url += L"?desk=1";
                                else url += L"&desk=1";
                                g_webview->Navigate(url.c_str());

                                // 导航完成后再补做"定位会话"——
                                // 页面没加载完时调 ExecuteScript 是无效的。
                                EventRegistrationToken navToken{};
                                g_webview->add_NavigationCompleted(
                                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                        [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                            if (g_pendingConv.empty()) return S_OK;
                                            const std::string conv = g_pendingConv;
                                            g_pendingConv.clear();
                                            WebViewSelectConversation(conv);
                                            return S_OK;
                                        }).Get(),
                                    &navToken);
                            }
                            ResizeWebView();
                            return S_OK;
                        }).Get());
                if (FAILED(hr2)) g_failed = true;
                return S_OK;
            }).Get());

    if (FAILED(hr))
    {
        g_failed = true;
        return false;
    }
    return true;
}

void WebViewResize()
{
    ResizeWebView();
}

void WebViewSelectConversation(const std::string& convId)
{
    if (convId.empty()) return;

    // 页面还没加载完 → 先记下来，导航完成后补做。
    // **不能丢**：点气泡可能早于页面 ready，丢了就是"点了定位不到那条消息"。
    if (!g_webview)
    {
        g_pendingConv = convId;
        return;
    }

    // 用 JSON 字符串传参，避免 convId 里的特殊字符破坏 JS 源码。
    // convId 形如 "c-xxxxxxxxxxxxxxxx"（hex），但仍走 JSON 转义更稳。
    std::string json = tml::JsonEscape(convId);
    const std::wstring js =
        L"window.__lanShareSelectConv && window.__lanShareSelectConv('" +
        tml::Utf8ToWide(json) + L"');";

    g_webview->ExecuteScript(js.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT hr, LPCWSTR result) -> HRESULT {
                return S_OK;
            }).Get());
}

void WebViewShutdown()
{
    if (g_controller)
    {
        g_controller->Close();
        g_controller.Reset();
    }
    g_webview.Reset();
    g_env.Reset();
}
