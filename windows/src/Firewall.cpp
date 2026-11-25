#include "Firewall.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <string>

#include "Common.h"

#pragma comment(lib, "ole32.lib")

namespace tml
{
namespace Firewall
{
    const wchar_t* const kTcpRuleName = L"TrafficMonitorLanShare-TCP-In";
    const wchar_t* const kUdpRuleName = L"TrafficMonitorLanShare-UDP-In";
    const int kTcpPortLow  = 8765;
    const int kTcpPortHigh = 8767;
    const int kUdpPort     = 8766;

    namespace
    {
        // 跑一条命令并**捕获输出**。用 CREATE_NO_WINDOW：否则每次调用都会闪一个黑框。
        // 返回退出码；out 拿到 stdout（UTF-8）。
        int RunCommand(const std::wstring& cmdLine, std::string& out)
        {
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;
            HANDLE hRead = nullptr, hWrite = nullptr;
            if (!::CreatePipe(&hRead, &hWrite, &sa, 0)) return -1;
            ::SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.hStdOutput = hWrite;
            si.hStdError = hWrite;
            // 不用 STARTF_NO_SHOWWINDOW：窗口是否显示由 CreateProcess 的
            // CREATE_NO_WINDOW 决定，这里只管把 stdout/stderr 接上管道。
            si.dwFlags = STARTF_USESTDHANDLES;
            PROCESS_INFORMATION pi{};

            std::wstring mutableCmd = L"cmd.exe /c " + cmdLine;
            const BOOL ok = ::CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            if (!ok) { ::CloseHandle(hRead); ::CloseHandle(hWrite); return -1; }
            ::CloseHandle(hWrite);

            char buf[4096];
            DWORD n = 0;
            std::string acc;
            while (::ReadFile(hRead, buf, sizeof(buf), &n, nullptr) && n > 0) acc.append(buf, n);
            ::CloseHandle(hRead);
            ::WaitForSingleObject(pi.hProcess, 15000);
            DWORD code = 0;
            ::GetExitCodeProcess(pi.hProcess, &code);
            ::CloseHandle(pi.hProcess);
            ::CloseHandle(pi.hThread);
            out = acc;
            return static_cast<int>(code);
        }

        std::wstring RuleQueryCmd(const wchar_t* name)
        {
            return L"netsh advfirewall firewall show rule name=\"" + std::wstring(name) + L"\"";
        }

        std::wstring AddRuleCmd(const wchar_t* name, const wchar_t* proto, const wchar_t* ports)
        {
            // profile=private,domain：**不含公用**。
            // 家里/公司网络被误标为"公用"很常见，但默认放开公用会扩大暴露面。
            return L"netsh advfirewall firewall add rule name=\"" + std::wstring(name) +
                   L"\" dir=in action=allow protocol=" + std::wstring(proto) +
                   L" localport=" + std::wstring(ports) + L" profile=private,domain";
        }
    }

    bool IsAdmin()
    {
        // 用 OpenProcessToken + CheckTokenMembership，而不是"进程令牌提升"那套：
        // 后者在非提权进程里会返回 FALSE，**分不清"不是管理员"和"查不了"**。
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

        BOOL isMember = FALSE;
        PSID adminSid = nullptr;
        SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
        // AllocateAndInitializeSid 的原型是**固定 11 个参数**（8 个子权限槽 + pSid），
        // 少传不行 —— 这就是刚才"函数不接受 9 个参数"的原因。
        if (::AllocateAndInitializeSid(&ntAuthority, 2,
                                        SECURITY_BUILTIN_DOMAIN_RID,
                                        DOMAIN_ALIAS_RID_ADMINS,
                                        0, 0, 0, 0, 0, 0, &adminSid))
        {
            ::CheckTokenMembership(token, adminSid, &isMember);
            ::FreeSid(adminSid);
        }
        ::CloseHandle(token);
        return isMember != FALSE;
    }

    bool ProbeSelfInbound(const std::string& lanIp, int port, int timeoutMs)
    {
        if (lanIp.empty() || port <= 0 || port > 65535) return false;

        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return false;

        u_long nonBlocking = 1;
        ::ioctlsocket(s, FIONBIO, &nonBlocking);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = ::htons(static_cast<unsigned short>(port));
        if (::inet_pton(AF_INET, lanIp.c_str(), &addr.sin_addr) != 1)
        {
            ::closesocket(s);
            return false;
        }

        int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        bool connected = false;
        if (rc == 0)
        {
            connected = true;
        }
        else if (::WSAGetLastError() == WSAEWOULDBLOCK)
        {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(s, &wfds);
            timeval tv{};
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            const int sel = ::select(0, nullptr, &wfds, nullptr, &tv);
            if (sel > 0)
            {
                int err = 0;
                int len = sizeof(err);
                if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) == 0)
                    connected = (err == 0);
            }
        }
        ::closesocket(s);
        return connected;
    }

    bool HasAllowRule()
    {
        // 1) 我们自己的两条规则。**名字是 ASCII，逐字匹配**，
        //    所以不受 netsh 输出的本地化影响（它的标签是中文，名字不是）。
        std::string out;
        if (RunCommand(RuleQueryCmd(kTcpRuleName), out) == 0 &&
            out.find("TrafficMonitorLanShare-TCP-In") != std::string::npos) return true;
        std::string out2;
        if (RunCommand(RuleQueryCmd(kUdpRuleName), out2) == 0 &&
            out2.find("TrafficMonitorLanShare-UDP-In") != std::string::npos) return true;

        // 2) 任何针对 TrafficMonitor.exe 的入站允许规则
        //    （用户可能手工"允许某个程序"，而不是放行端口）。
        //    **只匹配 ASCII 的可执行文件路径**：netsh 的其余输出随系统语言变，
        //    靠那些标签匹配会时灵时不灵。
        std::string all;
        if (RunCommand(L"netsh advfirewall firewall show rule name=all dir=in", all) != 0) return false;
        std::string lower = all;
        for (size_t i = 0; i < lower.size(); ++i)
            if (lower[i] >= 'A' && lower[i] <= 'Z') lower[i] = static_cast<char>(lower[i] - 'A' + 'a');
        if (lower.find("trafficmonitor.exe") != std::string::npos) return true;
        return false;
    }

    bool AddAllowRules(std::string& detail)
    {
        if (!IsAdmin())
        {
            // 非管理员**不尝试提权**：只展示手动步骤。
            detail = "not_admin: 需要管理员权限才能添加防火墙规则；请手动放行 TCP 8765-8767 与 UDP 8766（仅专用网络）";
            return false;
        }

        detail.clear();
        bool any = false;
        std::string out;

        // 只补缺失的那条，已存在就跳过 —— **不改、不删用户已有规则**。
        if (!HasAllowRule())
        {
            const int rc = RunCommand(AddRuleCmd(kTcpRuleName, L"TCP", L"8765-8767"), out);
            if (rc == 0) { detail += "added:TrafficMonitorLanShare-TCP-In(TCP 8765-8767) "; any = true; }
            else { detail += "failed:TCP(" + std::to_string(rc) + ") "; }
        }
        else
        {
            detail += "skipped:TrafficMonitorLanShare-TCP-In(已存在) ";
        }

        // UDP 单独判：TCP 规则可能已在而 UDP 不在（用户手动加过其中一个）。
        std::string out2;
        const int rcU = RunCommand(RuleQueryCmd(kUdpRuleName), out2);
        const bool udpExists = (rcU == 0 && out2.find("TrafficMonitorLanShare-UDP-In") != std::string::npos);
        if (!udpExists)
        {
            const int rc = RunCommand(AddRuleCmd(kUdpRuleName, L"UDP", L"8766"), out);
            if (rc == 0) { detail += "added:TrafficMonitorLanShare-UDP-In(UDP 8766)"; any = true; }
            else { detail += "failed:UDP(" + std::to_string(rc) + ")"; }
        }
        else
        {
            if (!detail.empty() && detail[detail.size() - 1] == ' ') detail += "skipped:TrafficMonitorLanShare-UDP-In(已存在)";
            else detail += " skipped:TrafficMonitorLanShare-UDP-In(已存在)";
        }

        return any;
    }

    Status Inspect(const std::string& lanIp, int port)
    {
        Status st;
        st.lanIp = lanIp;
        st.port = port;
        st.isAdmin = IsAdmin();
        st.ruleExists = HasAllowRule();

        const bool selfOk = ProbeSelfInbound(lanIp, port, 800);
        // 防误报闸 ①：失败时再探一次（间隔 400ms），滤掉瞬时抖动。
        bool confirmed = selfOk;
        if (!selfOk)
        {
            ::Sleep(400);
            confirmed = ProbeSelfInbound(lanIp, port, 800);
        }
        st.selfProbeOk = confirmed;

        // 判定以**配置证据**为主，而不是自检。
        //
        // 为什么不能用自检当主判据：**Windows 上本机连自己的 LAN IP 不经过防火墙过滤**
        // —— 实测"一条放行规则都没有，自检照样通过"。拿它当主判据，这条检测就永远
        // 是"通"，永远不提示，等于没做。
        //
        // 只看规则会不会漏判？会 —— 用户可能手工"允许某个程序"而不是放行端口；
        // HasAllowRule() 里已经一并检查了程序规则，所以漏判的口子堵上了。
        //
        // 判据 A 的自检仍然保留在 reason 里：它是**辅助信息**，
        // 真正能作为"入站被拦"的**直接证据**只能来自对端（我们发出去的东西被拒）。
        st.blocked = (!st.ruleExists);
        if (st.blocked)
            st.reason = "未找到任何允许入站的规则（端口规则或程序规则都没有），入站很可能被拦；自检 " +
                        std::string(confirmed ? "通过（本机回环不经过过滤，不能作为放行依据）" : "也失败");
        else
            st.reason = "已存在允许入站的规则";
        return st;
    }

    namespace
    {
        const wchar_t* kFwSection = L"firewall";
        const long long kPromptIntervalMs = 24LL * 60 * 60 * 1000;   // 24 小时

        std::wstring Widen(const std::string& s)
        {
            if (s.empty()) return std::wstring();
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(static_cast<size_t>(n), L'\0');
            if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
            return w;
        }

        std::string Narrow(const std::wstring& w)
        {
            if (w.empty()) return std::string();
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(n), '\0');
            if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
            return s;
        }
    }

    bool ShouldPrompt(const std::wstring& iniPath, const std::string& networkKey, long long nowMs)
    {
        if (iniPath.empty()) return true;
        wchar_t buf[64] = { 0 };
        ::GetPrivateProfileStringW(kFwSection, L"never", L"0", buf, 64, iniPath.c_str());
        if (buf[0] == L'1') return false;

        buf[0] = 0;
        ::GetPrivateProfileStringW(kFwSection, L"lastKey", L"", buf, 64, iniPath.c_str());
        const std::wstring lastKey = buf;
        if (lastKey.empty() || Narrow(lastKey) != networkKey) return true;   // 换网络 → 重新允许提示一次

        long long lastTime = 0;
        buf[0] = 0;
        ::GetPrivateProfileStringW(kFwSection, L"lastTime", L"0", buf, 64, iniPath.c_str());
        lastTime = _wtoi64(buf);
        if (lastTime <= 0) return true;
        return (nowMs - lastTime) >= kPromptIntervalMs;
    }

    void MarkPrompted(const std::wstring& iniPath, const std::string& networkKey, long long nowMs)
    {
        if (iniPath.empty()) return;
        wchar_t num[32] = { 0 };
        _i64tow_s(nowMs, num, 32, 10);
        ::WritePrivateProfileStringW(kFwSection, L"lastKey", Widen(networkKey).c_str(), iniPath.c_str());
        ::WritePrivateProfileStringW(kFwSection, L"lastTime", num, iniPath.c_str());
    }

    void SetNeverPrompt(const std::wstring& iniPath)
    {
        if (iniPath.empty()) return;
        ::WritePrivateProfileStringW(kFwSection, L"never", L"1", iniPath.c_str());
    }
}
}
