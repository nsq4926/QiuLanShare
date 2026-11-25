#include "Plugin.h"
#include "TransferHub.h"
#include "HttpServer.h"
#include "InstanceLock.h"
#include "Discovery.h"
#include "Common.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
    void CopyTextToClipboard(HWND hwnd, const std::wstring& text)
    {
        if (!::OpenClipboard(hwnd)) return;
        ::EmptyClipboard();
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h != nullptr)
        {
            void* dst = ::GlobalLock(h);
            if (dst != nullptr)
            {
                std::memcpy(dst, text.c_str(), bytes);
                ::GlobalUnlock(h);
                ::SetClipboardData(CF_UNICODETEXT, h);
            }
        }
        ::CloseClipboard();
    }

    // 不要在主程序 UI 线程里直接 ShellExecute：它是通过 DDE/COM 跟外壳（explorer）
    // 说话的，外壳忙的时候会把 UI 线程一起拖住，任务栏窗口跟着不响应。
    // 丢到工作线程去，接口立刻返回。
    void ShellOpenDetached(const std::wstring& target)
    {
        const std::wstring copy = target;
        std::thread([copy]()
        {
            ::ShellExecuteW(nullptr, L"open", copy.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }).detach();
    }

    // 选择文件夹的逻辑已移到 Common::PickFolder（网页端换目录也要用同一份）
}

// ============================ 显示项目：IP ============================
const wchar_t* CItemIp::GetItemName() const { return L"\u5c40\u57df\u7f51\u5730\u5740"; }
const wchar_t* CItemIp::GetItemId() const { return L"TMLanShareIp"; }
const wchar_t* CItemIp::GetItemLableText() const { return L"IP"; }
const wchar_t* CItemIp::GetItemValueText() const { return m_owner->IpText().c_str(); }
const wchar_t* CItemIp::GetItemValueSampleText() const { return L"000.000.000.000"; }

int CItemIp::OnMouseEvent(MouseEventType type, int, int, void* hWnd, int)
{
    // 单击和双击都打开界面：主程序可能把双击手势配给了自己的功能，
    // 那样插件就收不到 MT_DBCLICKED。押一个手势上不安全，两个都接。
    if (type == MT_LCLICKED || type == MT_DBCLICKED)
    {
        m_owner->OpenWebUi(hWnd);
        return 1;
    }
    return 0;
}

// ========================== 显示项目：未读数 ==========================
const wchar_t* CItemUnread::GetItemName() const { return L"\u672a\u8bfb\u6d88\u606f"; }
const wchar_t* CItemUnread::GetItemId() const { return L"TMLanShareUnread"; }
const wchar_t* CItemUnread::GetItemLableText() const { return L"\u4e92\u901a"; }
const wchar_t* CItemUnread::GetItemValueText() const { return m_owner->UnreadText().c_str(); }
const wchar_t* CItemUnread::GetItemValueSampleText() const { return L"000"; }

int CItemUnread::OnMouseEvent(MouseEventType type, int, int, void* hWnd, int)
{
    if (type == MT_LCLICKED || type == MT_DBCLICKED)
    {
        m_owner->OpenWebUi(hWnd);
        return 1;
    }
    return 0;
}

// ============================== 插件主体 ==============================
CLanSharePlugin& CLanSharePlugin::Instance()
{
    // 故意用 new 且不释放：插件对象生命周期与进程一致，避免 DLL 卸载时的析构顺序问题
    static CLanSharePlugin* inst = new CLanSharePlugin();
    return *inst;
}

CLanSharePlugin::CLanSharePlugin()
    : m_itemIp(this)
    , m_itemUnread(this)
{
}

void CLanSharePlugin::OnInitialize(ITrafficMonitor* pApp)
{
    m_app = pApp;
    EnsureStarted();
}

void CLanSharePlugin::EnsureStarted()
{
    if (m_started) return;

    const unsigned long long now = ::GetTickCount64();
    if (now - m_lastRetry < 10000ULL) return;   // 失败后 10 秒再试，避免每个 tick 都折腾
    m_lastRetry = now;

    TransferHub& hub = TransferHub::Instance();
    InstanceLock& lock = InstanceLock::Instance();

    // winsock 必须在这里就绪：第二实例走的是"纯客户端"分支，不会调用
    // HttpServer::Start()，而那里原本是全进程唯一的一次 WSAStartup。
    // 少了这一步，客户端实例的每一次 socket() 都会拿到 10093(WSANOTINITIALISED)。
    tml::EnsureWinsock();

    // 先取锁：它同时决定了 ini 路径、数据目录与基准端口，
    // 所以必须在 hub.Init() 之前调用（Init 依赖这两个路径做数据隔离）。
    const InstanceLockResult lr = lock.Acquire();
    m_lockReason = lr.reason;

    // 设备表先于 hub.Init：hub.Init 会加载历史，而老历史行（没有 msgId/from/to）
    // 需要按 dir 推导方向，推导要用本机 deviceId —— 它在设备表里。
    DeviceRegistry& registry = DeviceRegistry::Instance();
    hub.AttachRegistry(&registry);
    registry.Init(lr.dataDir, lock.InstanceName());

    if (!hub.IsInited()) hub.Init(lr.iniPath, lr.dataDir, lr.dataDirExplicit);

    {
        // 一次枚举拿到 ip/mac/统计，三者同源（分两次查会出现拼装组合）
        const tml::LocalIdentity ident = tml::GetLocalIdentity();
        registry.UpdateSelf(hub.DeviceName(), ident.mac, ident.ips,
                            ident.filteredNicCount, ident.keptNicCount);
    }

    // 同一台机器换过 deviceId（重装 / 清过配置 / 换过数据目录）时，旧身份会以
    // "离线设备"永久留在表里 —— 用户看到的就是"一个设备显示好几项"。
    // 启动时按 MAC 并一次，并把它遗留的会话一起清掉（否则设备合并了、列表还多几行）。
    {
        std::vector<std::string> stale;
        const int merged = registry.DedupeByMac(&stale);
        for (size_t i = 0; i < stale.size(); ++i) hub.DropPeerHistory(stale[i]);
        if (merged > 0)
            tml::AppendLogLine(tml::JoinPath(lr.dataDir, L"lanshare.log"),
                               "dedupe by mac: merged " + std::to_string(merged) + " stale peer(s)");
    }

    if (!lr.primary)
    {
        // 第二实例：只作客户端。不监听，也就不可能抢端口、不会写第二份设备表。
        // m_started 置真，避免每个 tick 重复取锁；这是刻意的"一次判定、长期有效"。
        m_clientOnly = true;
        m_started = true;
        // 纯客户端实例也要参与发现：它照样要 announce（5b 形态靠这个让对端看到两台设备）
        Discovery::Instance().Start(0, lr.dataDir, lr.iniPath);
        RefreshTexts();
        return;
    }

    // 端口递进：候选由 InstanceLock 统一给出（TMLS_PORT 显式指定时不递进）
    const std::vector<unsigned short> candidates = lock.PortCandidates();
    unsigned short bound = 0;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (HttpServer::Instance().Start(candidates[i]))
        {
            bound = candidates[i];
            break;
        }
        // Start() 失败已把原因写进 LastError()，RefreshTexts 会显示出来
    }

    if (bound == 0)
    {
        // 候选端口全被占：保持 m_started = false，10 秒后重试
        RefreshTexts();
        return;
    }

    // 实际端口回填：/api/hello、任务栏、复制地址从此都以它为准
    hub.SetRuntimePort(bound);

    // 发现服务：announce 里报的必须是**实际**端口，所以只能在绑定成功之后启动
    Discovery::Instance().Start(bound, lr.dataDir, lr.iniPath);
    lock.SetEffectivePort(bound);
    lock.WriteDisplayPort(bound);

    m_started = true;
    RefreshTexts();
}

void CLanSharePlugin::RefreshTexts()
{
    TransferHub& hub = TransferHub::Instance();

    m_ipText = tml::Utf8ToWide(hub.LocalIp());

    // 抢不到锁说明后台正在处理传输，沿用上次的未读数即可，绝不在这里等
    int unread = 0;
    if (hub.TryUnreadCount(unread))
    {
        // 只有真的变了才改字符串：值不变也赋值的话，主程序可能认为显示内容变了而重绘任务栏窗口
        const std::wstring next = std::to_wstring(unread);
        if (next != m_unreadText) m_unreadText = next;
    }

    std::wstring tip = L"\u79cb\u79cb\u5feb\u4f20  ";
    tip += m_ipText;
    tip += L":";
    tip += std::to_wstring(hub.Port());

    // 身份被重建过（设备表坏到连 deviceId 都读不出来）：必须让用户看见，
    // 否则"我的聊天记录没了"会变成一个无法解释的现象——其实是旧会话挂在了旧身份下。
    if (DeviceRegistry::Instance().IdentityRegenerated())
    {
        tip += L"\n\u8bbe\u5907\u8eab\u4efd\u5df2\u91cd\u5efa\uff0c\u5176\u4ed6\u8bbe\u5907\u4f1a\u628a\u4f60\u5f53\u4f5c\u65b0\u8bbe\u5907\uff08\u65e7\u4f1a\u8bdd\u770b\u4e0d\u5230\uff09";
    }

    if (m_clientOnly)
    {
        // 第二实例：不监听，界面地址指向第一实例（端口从 ini 的 last_port 读回）
        tip += L"\n\u53e6\u4e00\u5b9e\u4f8b\u5df2\u5728\u8fd0\u884c\uff0c\u672c\u5b9e\u4f8b\u4ec5\u4f5c\u5ba2\u6237\u7aef\uff08\u4e0d\u76d1\u542c\u7aef\u53e3\uff09";
        if (!m_lockReason.empty())
        {
            tip += L"\n";
            tip += m_lockReason;
        }
        tip += L"\n\u5355\u51fb/\u53cc\u51fb\u6253\u5f00\u4f20\u8f93\u754c\u9762";
        m_tooltip = tip;
        return;
    }

    if (!HttpServer::Instance().IsRunning())
    {
        tip += L"\n\u670d\u52a1\u672a\u542f\u52a8\uff1a";
        tip += tml::Utf8ToWide(HttpServer::Instance().LastError());
    }
    else
    {
        tip += L"\n\u672a\u8bfb\u6d88\u606f\uff1a";
        tip += m_unreadText;
        tip += L"\n\u4ea4\u6362\u76ee\u5f55\uff1a";
        tip += hub.ShareDir();
        tip += L"\n\u5355\u51fb/\u53cc\u51fb\u6253\u5f00\u4f20\u8f93\u754c\u9762";
    }
    m_tooltip = tip;
}

void CLanSharePlugin::DataRequired()
{
    EnsureStarted();
    ++m_tick;

    if (m_tick % 10 == 1)
    {
        RefreshTexts();
    }
    else
    {
        int unread = 0;
        if (TransferHub::Instance().TryUnreadCount(unread))
        {
            const std::wstring next = std::to_wstring(unread);
            if (next != m_unreadText) m_unreadText = next;
        }
    }

    // 故意不调 ITrafficMonitor::ShowNotifyMessage。
    // 那条路走托盘气泡，在隐藏托盘图标（config.ini 的 show_notify_icon=false）
    // 等配置下会把主程序的 UI 线程卡住，进而把任务栏和 explorer 一起拖死。
    // 本插件只保留"未读数"这一种提醒方式：不碰 UI 线程，最安全。
}

IPluginItem* CLanSharePlugin::GetItem(int index)
{
    if (index == 0) return &m_itemIp;
    if (index == 1) return &m_itemUnread;
    return nullptr;   // 越界必须返回空指针
}

const wchar_t* CLanSharePlugin::GetInfo(PluginInfoIndex index)
{
    switch (index)
    {
    case TMI_NAME:
        return L"\u79cb\u79cb\u5feb\u4f20 (QiuLanShare)";
    case TMI_DESCRIPTION:
        return L"\u5728\u4efb\u52a1\u680f\u663e\u793a\u672c\u673a\u5c40\u57df\u7f51\u5730\u5740\uff0c"
               L"\u5e76\u5728\u540e\u53f0\u63d0\u4f9b\u6587\u4ef6/\u6d88\u606f/\u56fe\u7247\u4e92\u4f20\u670d\u52a1\u3002"
               L"\u624b\u673a\u7aef\u8bbf\u95ee http://\u672c\u673aIP:8765 \u5373\u53ef\u4f7f\u7528\u3002";
    case TMI_AUTHOR:
        return L"";
    case TMI_COPYRIGHT:
        return L"";
    case TMI_VERSION:
        return L"1.0.0";
    case TMI_URL:
        return L"";
    default:
        return L"";
    }
}

ITMPlugin::OptionReturn CLanSharePlugin::ShowOptionsDialog(void* hParent)
{
    (void)hParent;

    // 防止连点堆出一摞对话框
    static std::atomic<bool> s_busy{ false };
    if (s_busy.exchange(true)) return OR_OPTION_UNCHANGED;

    // 把整个交互（提示框 + 系统目录选择）放到**独立的 STA 线程**里做，
    // 主程序 UI 线程立刻返回，不参与任何 shell 调用。
    //
    // 为什么必须这样：只要 shell 对话框跑在主程序 UI 线程上就有两个坑——
    //   1) 该线程的 COM 套间未必是 STA，CoInitializeEx 会返回 RPC_E_CHANGED_MODE，
    //      等于没初始化，shell 在 MTA 上开对话框会卡死；
    //   2) 弹窗的 owner 是任务栏窗口，仍旧把外壳进程牵扯进来。
    // 换到自己的线程 + owner 全传 nullptr，最坏情况也只是这个对话框卡住，
    // 不会拖死主程序，更不会连累 explorer。
    std::thread([]()
    {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const bool needUninit = SUCCEEDED(hr);

        TransferHub& hub = TransferHub::Instance();
        const int days = hub.HistoryDays();

        std::wstring msg = L"\u79cb\u79cb\u5feb\u4f20 (QiuLanShare)\n\n";
        msg += L"\u624b\u673a\u8bbf\u95ee\u5730\u5740\uff1ahttp://";
        msg += tml::Utf8ToWide(hub.LocalIp());
        msg += L":";
        msg += std::to_wstring(hub.Port());
        msg += L"\n\u6587\u4ef6\u4fdd\u5b58\u76ee\u5f55\uff1a";
        msg += hub.ShareDir();
        msg += L"\n\u804a\u5929\u5386\u53f2\u6587\u4ef6\uff1a";
        msg += hub.HistoryPath();
        msg += L"\n\u5386\u53f2\u81ea\u52a8\u6e05\u7406\uff1a";
        msg += (days > 0) ? (std::to_wstring(days) + L" \u5929\u524d") : L"\u4ece\u4e0d";
        msg += L"\n\n";

        if (!HttpServer::Instance().IsRunning())
        {
            msg += L"\u6ce8\u610f\uff1a\u670d\u52a1\u672a\u542f\u52a8\uff0c";
            msg += tml::Utf8ToWide(HttpServer::Instance().LastError());
            msg += L"\n\n";
        }

        // 这里**只显示现状**。更换/打开目录已经移到网页端的设置里——
        // 那里能顺带看清"文件会怎么放"（按设备分文件夹），比 MessageBox 的一行字清楚得多。
        msg += L"\u66f4\u6362\u4fdd\u5b58\u76ee\u5f55\u3001\u6253\u5f00\u76ee\u5f55\u3001\u4fee\u6539\u4fdd\u7559\u5929\u6570\uff0c\n";
        msg += L"\u8bf7\u6253\u5f00\u7f51\u9875\u7aef\uff1ahttp://";
        msg += tml::Utf8ToWide(hub.LocalIp());
        msg += L":";
        msg += std::to_wstring(hub.Port());
        msg += L" \u53f3\u4e0a\u89d2\u7684\u300c\u8bbe\u7f6e\u300d\u3002";

        ::MessageBoxW(nullptr, msg.c_str(), L"\u79cb\u79cb\u5feb\u4f20 \u8bbe\u7f6e",
                      MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);

        if (needUninit) ::CoUninitialize();
        s_busy.store(false);
    }).detach();

    return OR_OPTION_UNCHANGED;
}

const wchar_t* CLanSharePlugin::GetTooltipInfo()
{
    return m_tooltip.c_str();
}

int CLanSharePlugin::GetCommandCount()
{
    return 4;
}

const wchar_t* CLanSharePlugin::GetCommandName(int command_index)
{
    switch (command_index)
    {
    case 0: return L"\u6253\u5f00\u4f20\u8f93\u754c\u9762";
    case 1: return L"\u590d\u5236\u672c\u673a\u5730\u5740";
    case 2: return L"\u6253\u5f00\u4fdd\u5b58\u76ee\u5f55";
    case 3: return L"\u6e05\u7a7a\u804a\u5929\u5386\u53f2";
    default: return nullptr;
    }
}

void CLanSharePlugin::OnPluginCommand(int command_index, void* hWnd, void*)
{
    switch (command_index)
    {
    case 0: OpenWebUi(hWnd); break;
    case 1: CopyAddress(hWnd); break;
    case 2: OpenShareFolder(hWnd); break;
    case 3:
    {
        const int ret = ::MessageBoxW(static_cast<HWND>(hWnd),
            L"\u786e\u5b9a\u8981\u6e05\u7a7a\u5168\u90e8\u804a\u5929\u5386\u53f2\u5417\uff1f\n"
            L"\uff08\u5df2\u4fdd\u5b58\u7684\u6587\u4ef6\u4e0d\u4f1a\u88ab\u5220\u9664\uff09",
            L"\u79cb\u79cb\u5feb\u4f20", MB_YESNO | MB_ICONQUESTION);
        if (ret == IDYES)
        {
            TransferHub::Instance().ClearHistory();
            RefreshTexts();
        }
        break;
    }
    default: break;
    }
}

void CLanSharePlugin::OpenWebUi(void* hWnd)
{
    (void)hWnd;
    // 单击 + 双击都会到这里：800ms 内只真正打开一次，避免一次点击开两个标签页
    const unsigned long long now = ::GetTickCount64();
    if (now - m_lastOpen < 800ULL) return;
    m_lastOpen = now;

    EnsureStarted();
    TransferHub& hub = TransferHub::Instance();
    const std::wstring url = L"http://127.0.0.1:" + std::to_wstring(hub.Port()) + L"/";
    ShellOpenDetached(url);
}

void CLanSharePlugin::CopyAddress(void* hWnd)
{
    EnsureStarted();
    TransferHub& hub = TransferHub::Instance();
    const std::wstring addr = L"http://" + tml::Utf8ToWide(hub.LocalIp()) + L":" + std::to_wstring(hub.Port()) + L"/";
    CopyTextToClipboard(static_cast<HWND>(hWnd), addr);
}

void CLanSharePlugin::OpenShareFolder(void* hWnd)
{
    (void)hWnd;
    EnsureStarted();
    ShellOpenDetached(TransferHub::Instance().ShareDir());
}
