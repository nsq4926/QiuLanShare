#include "HttpServer.h"
#include "PeerClient.h"
#include "Discovery.h"
#include "TransferHub.h"
#include "InstanceLock.h"
#include "Firewall.h"
#include "Common.h"
#include "HostBridge.h"
#include "resource.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace
{
    constexpr long long kMaxUpload = 4096LL * 1024 * 1024;   // 4 GB（分片收发都是流式）
    constexpr size_t    kMaxHead = 64 * 1024;
    constexpr size_t    kMaxBody = 4 * 1024 * 1024;
    constexpr int       kMaxConn = 64;                      // 并发连接上限（= 线程上限）

    std::string ToLower(std::string s)
    {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::string Trim(const std::string& s)
    {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string();
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    // ---- T04 兼容层分流 ----
    //
    // 走 v2 还是 legacy，**只由"请求是否带 X-Device-Id"决定**，绝不由 convId 决定。
    // 按 convId 判会出事：v2 客户端首轮请求还没拿到 convId，就会被吸进 legacy 通道，
    // 于是同一个设备多出一个会话、消息归错设备，而且两边都不报错。
    std::string DeviceIdHeader(const HttpRequest& req)
    {
        const std::map<std::string, std::string>::const_iterator it = req.headers.find("x-device-id");
        if (it == req.headers.end()) return std::string();
        return Trim(it->second);
    }

    bool IsLegacyRequest(const HttpRequest& req)
    {
        return DeviceIdHeader(req).empty();
    }

    // 续传标识只允许字母数字与 _-：它会直接进文件名，放行 "/" 就是路径穿越
    bool IsSafeXid(const std::string& xid)
    {
        if (xid.empty() || xid.size() > 64) return false;
        for (char c : xid)
        {
            const unsigned char u = static_cast<unsigned char>(c);
            if (!(std::isalnum(u) || c == '_' || c == '-')) return false;
        }
        return true;
    }


    // legacy 会话键由 TransferHub::MakeLegacyKey() 统一产出**唯一**一份实现。
    // 这里曾经各拼一次（HttpServer 拼 legacy:<dev>、LoadHistory 拼 unknown:<dev>），
    // 后果是"老客户端升级后拉自己那段历史一条都拉不到，且不报错"。

    bool SendAll(SOCKET s, const void* data, size_t len)
    {
        const char* p = static_cast<const char*>(data);
        while (len > 0)
        {
            int n = ::send(s, p, static_cast<int>(len), 0);
            if (n <= 0) return false;
            p += n;
            len -= static_cast<size_t>(n);
        }
        return true;
    }

    const char* StatusText(int code)
    {
        switch (code)
        {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default:  return "OK";
        }
    }

    std::string UrlEncode(const std::string& s)
    {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : s)
        {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~')
            {
                out += static_cast<char>(c);
            }
            else
            {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0xF];
            }
        }
        return out;
    }

    std::string LoadIndexHtml()
    {
        // 1) 磁盘覆盖：plugins\web\index.html，改界面不用重新编译 DLL
        const std::wstring diskPath = tml::JoinPath(tml::JoinPath(tml::GetModuleDir(), L"web"), L"index.html");
        std::string html;
        if (tml::ReadWholeFile(diskPath, html) && !html.empty()) return html;

        // 2) 编译进 DLL 的资源
        HMODULE mod = static_cast<HMODULE>(tml::GetSelfModule());
        HRSRC hr = ::FindResourceW(mod, MAKEINTRESOURCEW(IDR_INDEX_HTML), RT_RCDATA);
        if (hr != nullptr)
        {
            HGLOBAL hg = ::LoadResource(mod, hr);
            DWORD size = ::SizeofResource(mod, hr);
            if (hg != nullptr && size > 0)
            {
                const void* p = ::LockResource(hg);
                if (p != nullptr) return std::string(static_cast<const char*>(p), size);
            }
        }

        // 3) 兜底
        return "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>秋秋快传</title></head>"
               "<body style=\"font-family:system-ui;padding:40px\"><h2>秋秋快传 (QiuLanShare)</h2>"
               "<p>未找到 web/index.html，也没有编译进资源。</p>"
               "<p>请把 index.html 放到插件目录的 web 子文件夹里。</p></body></html>";
    }

    bool SendResponseHead(SOCKET s, int status, const std::string& contentType,
                          long long contentLength,
                          const std::vector<std::pair<std::string, std::string>>& extra)
    {
        std::ostringstream os;
        os << "HTTP/1.1 " << status << " " << StatusText(status) << "\r\n";
        os << "Content-Type: " << contentType << "\r\n";
        os << "Content-Length: " << contentLength << "\r\n";
        os << "Cache-Control: no-store\r\n";
        os << "Access-Control-Allow-Origin: *\r\n";
        os << "Access-Control-Allow-Headers: *\r\n";
        os << "Connection: close\r\n";
        for (const auto& h : extra) os << h.first << ": " << h.second << "\r\n";
        os << "\r\n";
        const std::string head = os.str();
        return SendAll(s, head.data(), head.size());
    }

    // ---- 请求头解析 ----
    bool ParseRequestHead(const std::string& head, HttpRequest& req)
    {
        size_t lineEnd = head.find("\r\n");
        if (lineEnd == std::string::npos) return false;
        std::string first = head.substr(0, lineEnd);

        size_t sp1 = first.find(' ');
        if (sp1 == std::string::npos) return false;
        size_t sp2 = first.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) return false;

        req.method = first.substr(0, sp1);
        std::string target = first.substr(sp1 + 1, sp2 - sp1 - 1);

        size_t q = target.find('?');
        if (q == std::string::npos)
        {
            req.path = target;
        }
        else
        {
            req.path = target.substr(0, q);
            req.query = target.substr(q + 1);
        }

        size_t pos = lineEnd + 2;
        while (pos < head.size())
        {
            size_t e = head.find("\r\n", pos);
            // head 是去掉末尾 "\r\n\r\n" 之后的片段，最后一行后面没有 CRLF，
            // 必须当成行尾处理，否则 Content-Length 这类"最后一个头"会被整条丢弃。
            if (e == std::string::npos) e = head.size();

            std::string line = head.substr(pos, e - pos);
            pos = (e >= head.size()) ? head.size() : e + 2;

            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            req.headers[ToLower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
        }
        return true;
    }
}

HttpServer& HttpServer::Instance()
{
    static HttpServer* inst = new HttpServer();   // 故意不析构：避免 DLL 卸载时线程访问已销毁对象
    return *inst;
}

std::string HttpServer::LastError()
{
    std::lock_guard<std::mutex> lk(m_errorMutex);
    return m_lastError;
}

bool HttpServer::Start(unsigned short port)
{
    if (m_running.load()) return true;

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::lock_guard<std::mutex> lk(m_errorMutex);
        m_lastError = "WSAStartup 失败";
        return false;
    }

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
    {
        ::WSACleanup();
        std::lock_guard<std::mutex> lk(m_errorMutex);
        m_lastError = "创建 socket 失败";
        return false;
    }

    // 端口递进（8765 -> 8766 -> 8767）的前提是"端口被别人占着就必须 bind 失败"。
    // 这里绝对不能设 SO_REUSEADDR：Windows 下它允许 bind 到一个已有监听者的端口，
    // bind 会"成功"，于是 8765 永远不会让位，两个进程同时 listen，
    // 连接被老进程接走，/api/hello 还报一个没人接的端口 —— 正是 T01 要消灭的假象。
    // SO_EXCLUSIVEADDRUSE 让占用即 WSAEADDRINUSE(10048)，递进才会真的发生。
    BOOL exclusive = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = ::htons(port);

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        const int err = ::WSAGetLastError();
        ::closesocket(s);
        ::WSACleanup();
        std::lock_guard<std::mutex> lk(m_errorMutex);
        m_lastError = "端口 " + std::to_string(port) + " 无法监听（错误码 " + std::to_string(err) +
                      "，可能已被其他程序占用）";
        return false;
    }

    if (::listen(s, 32) == SOCKET_ERROR)
    {
        ::closesocket(s);
        ::WSACleanup();
        std::lock_guard<std::mutex> lk(m_errorMutex);
        m_lastError = "listen 调用失败";
        return false;
    }

    m_listenSock = static_cast<std::uintptr_t>(s);
    m_stopping = false;
    m_running = true;
    TransferHub::Instance().Resume();
    m_acceptThread = std::thread(&HttpServer::AcceptLoop, this);
    return true;
}

void HttpServer::Stop()
{
    if (!m_running.exchange(false)) return;
    m_stopping = true;

    TransferHub::Instance().Shutdown();   // 唤醒所有长轮询

    if (m_listenSock != 0)
    {
        ::closesocket(static_cast<SOCKET>(m_listenSock));
        m_listenSock = 0;
    }
    if (m_acceptThread.joinable()) m_acceptThread.join();

    {
        std::lock_guard<std::mutex> lk(m_clientsMutex);
        for (auto c : m_clients) ::shutdown(static_cast<SOCKET>(c), SD_BOTH);
        m_clients.clear();
    }

    ::Sleep(100);
    ::WSACleanup();
}

void HttpServer::AcceptLoop()
{
    while (!m_stopping.load())
    {
        sockaddr_in peer{};
        int peerLen = sizeof(peer);
        SOCKET c = ::accept(static_cast<SOCKET>(m_listenSock), reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (c == INVALID_SOCKET)
        {
            if (m_stopping.load()) break;
            ::Sleep(50);
            continue;
        }

        DWORD rcvTimeout = 70000;
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcvTimeout), sizeof(rcvTimeout));
        ::setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&rcvTimeout), sizeof(rcvTimeout));
        // 关 Nagle：响应头与 body 分两次 send，开着它每次要等对端 ACK（分片上传每片一次）
        BOOL nodelay = TRUE;
        ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        // 超出并发上限：直接回 503 关掉，不新建线程
        if (m_connCount.load() >= kMaxConn)
        {
            static const char* kBusy = "HTTP/1.1 503 Service Unavailable\r\n"
                                       "Content-Type: text/plain\r\nContent-Length: 4\r\n"
                                       "Connection: close\r\n\r\nbusy";
            SendAll(c, kBusy, std::strlen(kBusy));
            ::closesocket(c);
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(m_clientsMutex);
            m_clients.push_back(static_cast<std::uintptr_t>(c));
        }

        const std::uintptr_t raw = static_cast<std::uintptr_t>(c);
        m_connCount.fetch_add(1);
        std::thread([this, raw]()
        {
            HandleClient(raw);
            {
                std::lock_guard<std::mutex> lk(m_clientsMutex);
                m_clients.erase(std::remove(m_clients.begin(), m_clients.end(), raw), m_clients.end());
            }
            ::closesocket(static_cast<SOCKET>(raw));
            m_connCount.fetch_sub(1);
        }).detach();
    }
}

void HttpServer::StreamUpload(std::uintptr_t sockRaw, const HttpRequest& req, long long contentLength)
{
    const SOCKET s = static_cast<SOCKET>(sockRaw);
    const std::string rawName = tml::QueryParam(req.query, "name");
    const std::string name = rawName.empty() ? std::string("file") : rawName;
    std::string rawMime = tml::QueryParam(req.query, "mime");
    std::string dir = tml::QueryParam(req.query, "dir");
    const std::string dev = tml::QueryParam(req.query, "dev");

    // 清洗 MIME，避免把换行等字符塞进响应头
    std::string mime;
    for (char c : rawMime)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '/' || c == '+' || c == '-' || c == '.') mime += c;
    }
    if (mime.empty()) mime = "application/octet-stream";
    if (dir != "out") dir = "in";

    auto fail = [&](int code, const char* msg)
    {
        const std::string body = std::string("{\"ok\":false,\"error\":\"") + msg + "\"}";
        SendResponseHead(s, code, "application/json; charset=utf-8",
                         static_cast<long long>(body.size()), {});
        SendAll(s, body.data(), body.size());
    };

    // ---- 分片 / 续传（xid + offset + total）----
    //
    // 手机端把大文件切成片追加到同一个 .part，收满 total 才入库。
    // 与整份上传**互不影响**：不带 xid 的请求仍走下面的老逻辑。
    const std::string xid = tml::QueryParam(req.query, "xid");
    if (!xid.empty())
    {
        if (!IsSafeXid(xid))
        {
            fail(400, "bad xid");
            return;
        }
        const long long total = std::atoll(tml::QueryParam(req.query, "total").c_str());
        const long long offset = std::atoll(tml::QueryParam(req.query, "offset").c_str());
        if (total <= 0 || total > kMaxUpload)
        {
            fail(413, "file too large");
            return;
        }
        if (offset < 0 || offset > total || contentLength > total - offset)
        {
            fail(400, "bad offset");
            return;
        }

        auto& hub = TransferHub::Instance();
        const std::wstring saveDir = hub.ShareDir();
        tml::EnsureDir(saveDir);
        const std::wstring partPath = tml::JoinPath(saveDir, L".~" + tml::Utf8ToWide(xid) + L".part");

        long long have = 0;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (::GetFileAttributesExW(partPath.c_str(), GetFileExInfoStandard, &fad))
        {
            have = (static_cast<long long>(fad.nFileSizeHigh) << 32) | static_cast<long long>(fad.nFileSizeLow);
        }
        if (offset == 0)
        {
            have = 0;
        }
        else if (have != offset)
        {
            // 409 + 真实进度：客户端据此从 have 继续，而不是整份重来
            const std::string body = "{\"ok\":false,\"error\":\"offset_mismatch\",\"received\":" +
                                     std::to_string(have) + "}";
            SendResponseHead(s, 409, "application/json; charset=utf-8",
                             static_cast<long long>(body.size()), {});
            SendAll(s, body.data(), body.size());
            return;
        }

        FILE* fp = nullptr;
        if (_wfopen_s(&fp, partPath.c_str(), offset == 0 ? L"wb" : L"ab") != 0 || fp == nullptr)
        {
            fail(500, "cannot open part");
            return;
        }
        // 对端标识先算出来：传输记录要按 peer 归档（清空会话时整批清）
        std::string peerForRec = tml::QueryParam(req.query, "peer");
        if (IsLegacyRequest(req) && peerForRec.empty()) peerForRec = TransferHub::MakeLegacyKey(dev);
        const long long trId = hub.BeginTransfer(name, total, dir, peerForRec, xid);
        hub.UpdateTransfer(trId, have);

        long long got = have;
        if (!req.body.empty())
        {
            fwrite(req.body.data(), 1, req.body.size(), fp);
            got += static_cast<long long>(req.body.size());
        }
        long long wantTotal = contentLength - static_cast<long long>(req.body.size());
        char cbuf[64 * 1024];
        bool cancelled = false;
        while (wantTotal > 0)
        {
            // 用户点了 ×：立刻停，别再把这半截文件收下去
            if (hub.IsTransferCancelled(xid)) { cancelled = true; break; }
            const int want = static_cast<int>(std::min<long long>(static_cast<long long>(sizeof(cbuf)), wantTotal));
            const int n = ::recv(s, cbuf, want, 0);
            if (n <= 0) break;
            fwrite(cbuf, 1, static_cast<size_t>(n), fp);
            got += n;
            wantTotal -= n;
            hub.UpdateTransfer(trId, got);
        }
        fclose(fp);

        if (cancelled)
        {
            // 取消：.part 已没有意义（客户端也停了），删掉免得占空间；
            // 记录保持 cancelled，网页那边点 ✕ 已把它标灰
            ::DeleteFileW(partPath.c_str());
            hub.EndTransfer(trId, false, "cancelled");
            const std::string body = "{\"ok\":false,\"error\":\"cancelled\"}";
            SendResponseHead(s, 200, "application/json; charset=utf-8",
                             static_cast<long long>(body.size()), {});
            SendAll(s, body.data(), body.size());
            return;
        }

        if (got != offset + contentLength)
        {
            const std::string body = "{\"ok\":false,\"error\":\"incomplete\",\"received\":" +
                                     std::to_string(got) + "}";
            SendResponseHead(s, 400, "application/json; charset=utf-8",
                             static_cast<long long>(body.size()), {});
            SendAll(s, body.data(), body.size());
            return;
        }
        if (got < total)
        {
            const std::string body = "{\"ok\":true,\"partial\":true,\"received\":" +
                                     std::to_string(got) + "}";
            SendResponseHead(s, 200, "application/json; charset=utf-8",
                             static_cast<long long>(body.size()), {});
            SendAll(s, body.data(), body.size());
            return;
        }

        // 收齐：交给 AddFile 归档（同一磁盘 → 直接改名）
        // ⚠ dir 必须用查询参数：网页端的分片是**出站**（dir=out），写死 "in" 会把
        // 自己发出去的文件记成"对方发来的"，未读、左右气泡、目录归属全错。
        const std::string msgIdIn2 = tml::QueryParam(req.query, "msgId");
        std::string peerId2 = tml::QueryParam(req.query, "peer");
        if (IsLegacyRequest(req) && peerId2.empty()) peerId2 = TransferHub::MakeLegacyKey(dev);
        const ShareMessage m2 = hub.AddFile(partPath, name, mime, dir, dev, peerId2, msgIdIn2);
        if (m2.fileId.empty() && m2.msgId.empty())
        {
            hub.EndTransfer(trId, false, "cannot save file");
            ::DeleteFileW(partPath.c_str());
            fail(500, "cannot save file");
            return;
        }
        hub.EndTransfer(trId, true);
        // 出站分片：收齐后必须真的投递一次，否则文件只落在本机、对端永远收不到
        if (dir == "out")
        {
            const PeerSendResult r2 = PeerClient::Instance().SendFile(peerId2, m2.fileId.empty()
                                          ? std::wstring() : hub.FilePathById(m2.fileId),
                                      m2.fileName, m2.mime, m2.msgId);
            if (!r2.ok)
            {
                hub.SetMessageStatus(m2.msgId, "failed");
                const std::string bodyFail = std::string("{\"ok\":true,\"failed\":true,\"reason\":\"") +
                                             tml::JsonEscape(r2.reason) + "\",\"message\":" +
                                             TransferHub::ToJson(m2) + "}";
                SendResponseHead(s, 200, "application/json; charset=utf-8",
                                 static_cast<long long>(bodyFail.size()), {});
                SendAll(s, bodyFail.data(), bodyFail.size());
                return;
            }
        }
        const std::string body = "{\"ok\":true,\"received\":" + std::to_string(total) +
                                 ",\"message\":" + TransferHub::ToJson(m2) + "}";
        SendResponseHead(s, 200, "application/json; charset=utf-8",
                         static_cast<long long>(body.size()), {});
        SendAll(s, body.data(), body.size());
        return;
    }

    if (contentLength <= 0)
    {
        fail(400, "empty body");
        return;
    }
    if (contentLength > kMaxUpload)
    {
        fail(413, "file too large");
        return;
    }

    auto& hub = TransferHub::Instance();
    // 直接写进保存目录（用 ~up_xxx.part 临时名，成功后再改名），
    // 既不在目录里新建子文件夹，也不会多出一份复制
    const std::wstring saveDir = hub.ShareDir();
    tml::EnsureDir(saveDir);

    wchar_t tmpName[80] = { 0 };
    swprintf_s(tmpName, L"~up_%lld_%u.part", tml::NowMs(), ::GetCurrentThreadId());
    const std::wstring tmpPath = tml::JoinPath(saveDir, tmpName);

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, tmpPath.c_str(), L"wb") != 0 || fp == nullptr)
    {
        fail(500, "cannot create temp file");
        return;
    }

    // 登记一条"正在传输"，网页据此显示 大小 / 百分比 / 速度
    std::string peerPre = tml::QueryParam(req.query, "peer");
    if (IsLegacyRequest(req) && peerPre.empty()) peerPre = TransferHub::MakeLegacyKey(dev);
    const long long trId = hub.BeginTransfer(name, contentLength, dir, peerPre);

    // 已随请求头一起读进缓冲的 body 片段
    if (!req.body.empty())
    {
        fwrite(req.body.data(), 1, req.body.size(), fp);
    }
    long long received = static_cast<long long>(req.body.size());

    char buf[64 * 1024];
    while (received < contentLength)
    {
        int want = static_cast<int>(std::min<long long>(static_cast<long long>(sizeof(buf)), contentLength - received));
        int n = ::recv(s, buf, want, 0);
        if (n <= 0) break;
        fwrite(buf, 1, static_cast<size_t>(n), fp);
        received += n;
        hub.UpdateTransfer(trId, received);
    }
    fclose(fp);

    if (received != contentLength)
    {
        hub.EndTransfer(trId, false, "incomplete");
        ::DeleteFileW(tmpPath.c_str());
        fail(400, "incomplete upload");
        return;
    }

    // T03：msgId / peer 可选，重发时沿用同一个 msgId 即可幂等
    const std::string msgIdIn = tml::QueryParam(req.query, "msgId");
    std::string peerId = tml::QueryParam(req.query, "peer");
    // T04：无 X-Device-Id = 老端，并进 legacy:<dev> 会话（与文本消息同一套分流）
    if (IsLegacyRequest(req) && peerId.empty()) peerId = TransferHub::MakeLegacyKey(dev);
    const ShareMessage msg = hub.AddFile(tmpPath, name, mime, dir, dev, peerId, msgIdIn);
    if (msg.fileId.empty() && msg.msgId.empty())
    {
        // AddFile 落盘失败（保存目录不可写 / 磁盘满）时返回空记录。
        // 这里必须如实报错：回 200 + 一条没有 fileId 的消息，网页会渲染出一个
        // 点开就 404 的气泡，用户只会以为"文件坏了"。
        hub.EndTransfer(trId, false, "cannot save file");
        fail(500, "cannot save file");
        return;
    }
    hub.EndTransfer(trId, true);

    // T05：出站文件也要真的投递。文件是先落盘入库再发（AddFile 已在内部完成归档），
    // 所以这里失败只补一次状态修正——与文本的"先分配后入库"不同，文件路径少一次
    // 重写就得多搬一次字节，不值得。
    if (dir == "out")
    {
        const PeerSendResult r = PeerClient::Instance().SendFile(peerId, msg.fileId.empty()
                                     ? std::wstring() : hub.FilePathById(msg.fileId),
                                 msg.fileName, msg.mime, msg.msgId);
        if (!r.ok)
        {
            hub.SetMessageStatus(msg.msgId, "failed");
            const std::string body = std::string("{\"ok\":true,\"failed\":true,\"reason\":\"") +
                                     tml::JsonEscape(r.reason) + "\",\"message\":" +
                                     TransferHub::ToJson(msg) + "}";
            SendResponseHead(s, 200, "application/json; charset=utf-8", static_cast<long long>(body.size()), {});
            SendAll(s, body.data(), body.size());
            return;
        }
    }
    const std::string body = "{\"ok\":true,\"message\":" + TransferHub::ToJson(msg) + "}";
    SendResponseHead(s, 200, "application/json; charset=utf-8", static_cast<long long>(body.size()), {});
    SendAll(s, body.data(), body.size());
}

void HttpServer::HandleClient(std::uintptr_t sockRaw)
{
    const SOCKET s = static_cast<SOCKET>(sockRaw);

    std::string buffer;
    char tmp[16 * 1024];
    size_t headEnd = std::string::npos;

    while (true)
    {
        headEnd = buffer.find("\r\n\r\n");
        if (headEnd != std::string::npos) break;
        if (buffer.size() > kMaxHead) return;
        int n = ::recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) return;
        buffer.append(tmp, static_cast<size_t>(n));
    }

    HttpRequest req;
    req.sock = sockRaw;
    if (!ParseRequestHead(buffer.substr(0, headEnd), req)) return;

    // 请求头之后可能已经带上了部分 body，先保存起来
    const std::string extra = buffer.substr(headEnd + 4);
    buffer.clear();

    if (req.method == "OPTIONS")
    {
        SendResponseHead(s, 204, "text/plain", 0, {});
        return;
    }

    long long contentLength = 0;
    auto itLen = req.headers.find("content-length");
    if (itLen != req.headers.end()) contentLength = _strtoi64(itLen->second.c_str(), nullptr, 10);
    if (contentLength < 0) contentLength = 0;

    // ---------- 上传：流式写盘，不进内存 ----------
    if (req.path == "/api/upload" && req.method == "POST")
    {
        req.body = extra;
        StreamUpload(sockRaw, req, contentLength);
        return;
    }

    // ---------- 其余请求：body 读进内存 ----------
    if (contentLength > static_cast<long long>(kMaxBody))
    {
        const std::string body = "{\"ok\":false,\"error\":\"body too large\"}";
        SendResponseHead(s, 413, "application/json; charset=utf-8", static_cast<long long>(body.size()), {});
        SendAll(s, body.data(), body.size());
        return;
    }

    req.body = extra;
    while (static_cast<long long>(req.body.size()) < contentLength)
    {
        int want = static_cast<int>(std::min<long long>(static_cast<long long>(sizeof(tmp)),
            contentLength - static_cast<long long>(req.body.size())));
        int n = ::recv(s, tmp, want, 0);
        if (n <= 0) break;
        req.body.append(tmp, static_cast<size_t>(n));
    }

    HttpResponse res;
    auto& hub = TransferHub::Instance();

    // 带 X-Device-Id 的请求 = 对端主动连上了我们，这是一次**成功的 HTTP 往返**，
    // 要记 lastReachableAt（四态判定用它，不用 lastSeen：它一直在广播但连不上时
    // lastSeen 同样很新）。
    {
        const std::string caller = DeviceIdHeader(req);
        if (!caller.empty())
            DeviceRegistry::Instance().MarkPeerReachable(caller, tml::NowMs());
    }

    do
    {
        if (req.path == "/api/ping")
        {
            res.body = "{\"ok\":true}";
            break;
        }

        // ---- 防火墙入站放行：检测 + 一键放行（不静默加）----
        // 端点、判据、规则名与端口的**唯一权威源**是 docs/05 §3.5。
        // 三条不做的事：不静默加规则 / 不改删已有规则 / 非管理员不尝试提权。
        if (req.path == "/api/firewall" || req.path == "/api/firewall/allow" || req.path == "/api/firewall/never")
        {
            const std::wstring iniPath = tml::JoinPath(tml::GetModuleDir(), L"lanshare.ini");
            const std::string lanIp = hub.LocalIp();
            const int port = hub.Port();
            // 网络标识：取网段前缀（/24）。
            // 设计稿写的是"网段 + 网关 MAC"，网关 MAC 还没取，先用前缀代替 ——
            // 它的作用只是"同一网络 24h 内别反复提示"，精度够用。
            std::string netKey = lanIp;
            const size_t p3 = lanIp.rfind('.');
            if (p3 != std::string::npos)
            {
                const size_t p2 = lanIp.rfind('.', p3 - 1);
                if (p2 != std::string::npos) netKey = lanIp.substr(0, p2 + 1) + "0/24";
            }

            if (req.path == "/api/firewall")
            {
                const tml::Firewall::Status st = tml::Firewall::Inspect(lanIp, port);
                const bool should = st.blocked && tml::Firewall::ShouldPrompt(iniPath, netKey, tml::NowMs());
                res.body = "{\"ok\":true,\"blocked\":" + std::string(st.blocked ? "true" : "false") +
                           ",\"isAdmin\":" + std::string(st.isAdmin ? "true" : "false") +
                           ",\"ruleExists\":" + std::string(st.ruleExists ? "true" : "false") +
                           ",\"selfProbeOk\":" + std::string(st.selfProbeOk ? "true" : "false") +
                           ",\"port\":" + std::to_string(port) +
                           ",\"lanIp\":\"" + tml::JsonEscape(lanIp) + "\"" +
                           ",\"reason\":\"" + tml::JsonEscape(st.reason) + "\"" +
                           ",\"shouldPrompt\":" + std::string(should ? "true" : "false") +
                           ",\"tcpRule\":\"" + tml::JsonEscape(tml::WideToUtf8(tml::Firewall::kTcpRuleName)) + "\"" +
                           ",\"udpRule\":\"" + tml::JsonEscape(tml::WideToUtf8(tml::Firewall::kUdpRuleName)) + "\"" +
                           ",\"tcpPorts\":\"" + std::to_string(tml::Firewall::kTcpPortLow) + "-" + std::to_string(tml::Firewall::kTcpPortHigh) + "\"" +
                           ",\"udpPort\":" + std::to_string(tml::Firewall::kUdpPort) + "}";
                break;
            }

            if (req.path == "/api/firewall/never")
            {
                // 用户勾了"不再提示"：永久记住（网络属性，记在插件配置里，不记设备表）
                tml::Firewall::SetNeverPrompt(iniPath);
                res.body = "{\"ok\":true}";
                break;
            }

            // POST /api/firewall/allow —— 用户点了"一键放行"才会走到这里。
            if (req.method != "POST")
            {
                res.status = 405;
                res.body = "{\"ok\":false,\"error\":\"use POST\"}";
                break;
            }
            std::string detail;
            const bool ok = tml::Firewall::AddAllowRules(detail);
            if (ok) tml::Firewall::MarkPrompted(iniPath, netKey, tml::NowMs());
            res.body = "{\"ok\":" + std::string(ok ? "true" : "false") +
                       ",\"detail\":\"" + tml::JsonEscape(detail) + "\"}";
            break;
        }

        if (req.path == "/api/transfer")
        {
            res.body = hub.TransfersJson();
            break;
        }

        // 传输列表的 ✕：在传的取消，已结束的删记录。
        // 一条端点两种语义由 mode 决定（默认 remove），避免再加第三个路由。
        //   POST /api/transfer/remove?id=12            → 删记录（在传的先取消）
        //   POST /api/transfer/remove?id=12&mode=cancel→ 只取消，记录留着显示"已取消"
        if (req.path == "/api/transfer/remove")
        {
            if (req.method != "POST")
            {
                res.status = 405;
                res.body = "{\"ok\":false,\"error\":\"use POST\"}";
                break;
            }
            const std::string mode = tml::QueryParam(req.query, "mode");
            if (tml::QueryParam(req.query, "all") == "1")
            {
                res.body = "{\"ok\":true,\"removed\":" + std::to_string(hub.DropAllTransfers()) + "}";
                break;
            }
            const std::string idText = tml::QueryParam(req.query, "id");
            if (idText.empty())
            {
                res.status = 400;
                res.body = "{\"ok\":false,\"error\":\"id required\"}";
                break;
            }
            const long long id = _strtoi64(idText.c_str(), nullptr, 10);
            const bool ok = (mode == "cancel") ? hub.CancelTransfer(id) : hub.RemoveTransfer(id);
            res.body = std::string("{\"ok\":true,\"affected\":") + (ok ? "true" : "false") + "}";
            break;
        }

        if (req.path == "/api/upload/status")
        {
            // 续传前问"你收到多少了"。.part 路径是确定式（saveDir/.~<xid>.part），
            // 不需要内存映射表，进程重启后也能答对。
            const std::string xid = tml::QueryParam(req.query, "xid");
            long long have = 0;
            if (IsSafeXid(xid))
            {
                const std::wstring partPath = tml::JoinPath(
                    TransferHub::Instance().ShareDir(), L".~" + tml::Utf8ToWide(xid) + L".part");
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (::GetFileAttributesExW(partPath.c_str(), GetFileExInfoStandard, &fad))
                {
                    have = (static_cast<long long>(fad.nFileSizeHigh) << 32) |
                           static_cast<long long>(fad.nFileSizeLow);
                }
            }
            res.body = "{\"ok\":true,\"received\":" + std::to_string(have) + "}";
            break;
        }


        if (req.path == "/api/hello")
        {
            // T01：新增 deviceId / mac / apiVersion / port。
            // 老客户端（鸿蒙 7）只读它认识的字段，多余字段会被忽略，所以这里是纯增量。
            // port 必须是"实际监听端口"而不是 ini 里的意图端口，否则端口递进后
            // 对端会连到一个没人监听的端口上。
            std::ostringstream os;
            os << "{\"app\":\"LanShare\",\"name\":\"" << tml::JsonEscape(hub.DeviceName())
               << "\",\"ip\":\"" << hub.LocalIp() << "\",\"port\":" << hub.Port()
               << ",\"version\":\"1.1\",\"device\":\"pc\""
               << ",\"deviceId\":\"" << tml::JsonEscape(hub.DeviceId()) << "\""
               << ",\"mac\":\"" << tml::JsonEscape(hub.MacAddress()) << "\""
               << ",\"apiVersion\":\"" << TransferHub::ApiVersion() << "\""
               // 手机端据此决定"分片发（可续传）"还是"整份发"
               << ",\"resumable\":true"""
               // 契约要求 hello 与 announce 同带：验收 18 靠对端从这里拿到
               << ",\"filteredNicCount\":" << tml::GetNicFilter().filteredNicCount
               // 五态判定的两个时间窗（毫秒，只读）。暴露出来是为了让验收脚本
               // 读到**运行期实际用的常量**：脚本自己写死 30000/120000 再比大小，
               // 那条断言与实现无关、永远通过；而这两个值一旦被配成相等，
               // unreachable 会恒为假、中间态静默消失，而所有单条状态断言仍全绿。
               << ",\"onlineTtl\":" << DeviceRegistry::OnlineTtlMs()
               << ",\"staleTtl\":" << DeviceRegistry::StaleTtlMs()
               // 本端代次：清空全部历史时 +1（且只 +1 一次）。
               // 暴露出来是为了让验收脚本能断言"清全部 → epoch 恰好 +1"，
           // 否则这条只能靠人读代码确认。
               << ",\"epoch\":" << DeviceRegistry::Instance().Epoch()
               << ",\"instance\":\"" << tml::JsonEscape(InstanceLock::Instance().InstanceName()) << "\""
               << ",\"peer\":\"" << tml::JsonEscape(hub.PeerName()) << "\""
               << ",\"historyDays\":" << hub.HistoryDays()
               // 发现策略：网页设置页据此回显，手机端也能从这里判断"这台 PC 收不收"
               << ",\"acceptsInbound\":" << (Discovery::Instance().Discoverable() ? "true" : "false")
               << ",\"platform\":\"windows\""
               << ",\"activeDiscover\":" << (Discovery::Instance().ActiveDiscover() ? "true" : "false")
               << ",\"discoverSeconds\":" << (Discovery::Instance().IntervalMs() / 1000)
               << ",\"saveDir\":\"" << tml::JsonEscape(tml::WideToUtf8(hub.ShareDir())) << "\"}";
            res.body = os.str();
            break;
        }

        if (req.path == "/api/history/clear")
        {
            // T04：支持只清某个会话的历史（legacy 会话的三种处置之一：允许删除）
            const std::string convId = tml::QueryParam(req.query, "convId");
            if (!convId.empty())
            {
                // 先反查对端：清会话 = 把它的传输记录一起清（列表与消息同生命周期）
                const std::string peerOfConv = hub.PeerOfConv(convId);
                const int removed = hub.ClearConversation(convId);
                hub.DropTransfersForPeer(peerOfConv);
                res.body = "{\"ok\":true,\"removed\":" + std::to_string(removed) + "}";
                break;
            }
            // 带 dev 时**只清该对端的那一个 legacy 会话**。
            // 旧客户端（v1）没有 convId 概念，它清空时只能报"我清了什么"；
            // 若这里退化成"清全部"，会把同一台电脑上其它设备的会话一起清掉。
            // 会话键必须由 TransferHub::MakeLegacyKey() 产出 —— 与消息入库时用的是同一把钥匙。
            const std::string clearDev = tml::QueryParam(req.query, "dev");
            if (!clearDev.empty())
            {
                // MakeLegacyKey 得到的是**对端标识**（legacy:<设备名>），
                // 而 ClearConversation 按 **convId**（哈希 c-xxx）匹配 —— 两者不是同一个键，
                // 必须先反查一次 convId，否则 removed 恒为 0 且不报错。
                const std::string legacyPeer = TransferHub::MakeLegacyKey(clearDev);
                const std::string legacyConv = hub.ConvIdOfPeer(legacyPeer);
                const int removed = legacyConv.empty() ? 0 : hub.ClearConversation(legacyConv);
                hub.DropTransfersForPeer(legacyPeer);
                res.body = "{\"ok\":true,\"removed\":" + std::to_string(removed) + "}";
                break;
            }
            hub.ClearHistory();
            hub.DropAllTransfers();
            res.body = "{\"ok\":true}";
            break;
        }

        if (req.path == "/api/history/settings")
        {
            const std::string sDays = tml::QueryParam(req.query, "days");
            int days = sDays.empty() ? 0 : atoi(sDays.c_str());
            if (days < 0) days = 0;
            hub.SetHistoryDays(days);
            res.body = "{\"ok\":true,\"days\":" + std::to_string(days) + "}";
            break;
        }

        if (req.path == "/api/messages/read")
        {
            if (req.method != "POST")
            {
                res.status = 405;
                res.body = "{\"ok\":false,\"error\":\"use POST\"}";
                break;
            }
            std::string key;
            if (!tml::JsonGetString(req.body, "convId", key) || key.empty())
            {
                if (!tml::JsonGetString(req.body, "peer", key) || key.empty())
                    key = tml::QueryParam(req.query, "peer");
            }
            if (key.empty())
            {
                res.status = 400;
                res.body = "{\"ok\":false,\"error\":\"convId or peer required\"}";
                break;
            }
            const int cleared = hub.MarkRead(key);
            res.body = "{\"ok\":true,\"cleared\":" + std::to_string(cleared) + "}";
            break;
        }

        if (req.path == "/api/messages")
        {
            // 手机每次轮询都带上自己的设备名，这样换设备后名字能立刻更新
            hub.SetPeerName(tml::QueryParam(req.query, "dev"));

            long long since = 0;
            const std::string sSince = tml::QueryParam(req.query, "since");
            if (!sSince.empty()) since = _strtoi64(sSince.c_str(), nullptr, 10);

            int wait = 0;
            const std::string sWait = tml::QueryParam(req.query, "wait");
            if (!sWait.empty()) wait = atoi(sWait.c_str());
            if (wait < 0) wait = 0;
            if (wait > 25) wait = 25;

            MessageQuery q;
            q.sinceGlobal = since;
            q.waitSeconds = wait;

            // T04：分流只看 X-Device-Id。
            // legacy：**只按"会话键前缀"过滤，不按具体 dev 过滤**。
            //   为什么不能按 dev 过滤：PC 管理页调 /api/message 时不带 dev（dir=out 那条），
            //   它的会话键是 legacy:unknown，而手机的是 legacy:<手机名>。若按 dev 过滤，
            //   手机就看不到"电脑网页发给它"的消息、电脑网页也看不到手机的消息——
            //   两边都以为记录被清了，却不报任何错。
            //   前缀过滤同样能做到"旧包与新客户端不串台"：legacy 通道只看 legacy:* 会话。
            // v2    ：只认显式 peer。**不能退回去用 dev** —— 老端每次轮询都带 dev
            //         （用于更新对端名字），拿它当过滤条件会让"改个名字旧消息就消失"。
            if (IsLegacyRequest(req))
            {
                // PC 自己的管理页也不带 X-Device-Id，但它要看全部会话（包括它发给新客户端的）。
                // 老客户端从不带 viewer=pc，用它区分"PC 自己的 UI"与"老设备"，
                // 而不是去猜设备类型。
                q.legacy = (tml::QueryParam(req.query, "viewer") != "pc");
            }
            else
            {
                const std::string peerParam = tml::QueryParam(req.query, "peer");
                if (!peerParam.empty()) q.peerDeviceId = peerParam;
            }

            const std::string sEpoch = tml::QueryParam(req.query, "sinceEpoch");
            const std::string sSeq = tml::QueryParam(req.query, "sinceSeq");
            if (!sEpoch.empty()) q.sinceEpoch = _strtoi64(sEpoch.c_str(), nullptr, 10);
            if (!sSeq.empty()) q.sinceSeq = _strtoi64(sSeq.c_str(), nullptr, 10);

            const std::vector<ShareMessage> msgs = hub.QueryMessages(q);

            long long lastId = since;
            long long lastEpoch = (q.sinceEpoch >= 0) ? q.sinceEpoch : 0;
            long long lastSeq = (q.sinceSeq >= 0) ? q.sinceSeq : 0;
            std::ostringstream arr;
            arr << "[";
            for (size_t i = 0; i < msgs.size(); ++i)
            {
                if (i > 0) arr << ",";
                arr << TransferHub::ToJson(msgs[i]);
                if (msgs[i].id > lastId) lastId = msgs[i].id;
                // 水位只跟着"本对端"的消息推进，否则会把别的会话的号带进来
                if (!q.peerDeviceId.empty())
                {
                    if (msgs[i].epoch > lastEpoch ||
                        (msgs[i].epoch == lastEpoch && msgs[i].seq > lastSeq))
                    {
                        lastEpoch = msgs[i].epoch;
                        lastSeq = msgs[i].seq;
                    }
                }
            }
            arr << "]";

            std::ostringstream os;
            os << "{\"ok\":true,\"lastId\":" << lastId
               << ",\"lastEpoch\":" << lastEpoch
               << ",\"lastSeq\":" << lastSeq
               << ",\"messages\":" << arr.str() << "}";
            res.body = os.str();

            // **拉取不清零**：网页多标签、手机后台轮询都会拉，"拉取即清零"会让用户
            // 没看到红点，未读就没了。清零只走 POST /api/messages/read。
            break;
        }

        if (req.path == "/api/devices/add")
        {
            // 手动添加设备（兜底通道）。广播/组播都收不到时（AP 隔离、跨网段），
            // 用户仍能靠"手输 IP"把对端拉进列表——PRD 第九节第 4 条要求这条可用。
            // **与 /api/discover 分开**：那个是只读别名，这个是写操作，混在一起会让
            // "GET /api/discover 会改设备表"这种事悄悄成立。
            if (req.method != "POST")
            {
                res.status = 405;
                res.body = "{\"ok\":false,\"error\":\"use POST\"}";
                break;
            }

            std::string ip;
            if (!tml::JsonGetString(req.body, "ip", ip)) ip = tml::QueryParam(req.query, "ip");
            std::string sPort;
            if (!tml::JsonGetString(req.body, "port", sPort)) sPort = tml::QueryParam(req.query, "port");

            // 容错：用户常把 "http://192.168.1.9:8765/" 整段粘进来。剥掉协议、路径、
            // 首尾空白，再按冒号拆主机与端口——不这么做，粘贴一次就报"连不上"，
            // 用户会以为是对方的问题。
            auto trim = [](std::string& s)
            {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
                    s.erase(s.begin());
                while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
                    s.pop_back();
            };
            trim(ip);
            trim(sPort);
            if (ip.compare(0, 7, "http://") == 0) ip.erase(0, 7);
            const size_t slash = ip.find('/');
            if (slash != std::string::npos) ip.erase(slash);
            const size_t colon = ip.rfind(':');
            if (colon != std::string::npos && ip.find(':') == colon)   // 只有一个冒号才算 ip:port
            {
                if (sPort.empty()) sPort = ip.substr(colon + 1);
                ip.erase(colon);
            }

            unsigned short port = static_cast<unsigned short>(sPort.empty() ? 8765 : atoi(sPort.c_str()));

            std::string err;
            std::string deviceId;
            if (!Discovery::Instance().AddPeerByAddress(ip, port, err, deviceId))
            {
                // 三种失败要分开说：用户能据此判断"填错了""对方没开插件""填的是自己"
                res.status = (err == "connect_fail") ? 502 : 400;
                res.body = "{\"ok\":false,\"error\":\"" + tml::JsonEscape(err) + "\"}";
                break;
            }
            res.body = "{\"ok\":true,\"deviceId\":\"" + tml::JsonEscape(deviceId) + "\"}";
            break;
        }

        // 发现策略：主动发现 / 被发现 / 广播周期。
        // 一条写入路径：Discovery 存内存 + 回写 ini，HTTP 只做参数校验与如实回报，
        // 避免"网页关了、下一轮 announce 又发出去"这种分离状态。
        if (req.path == "/api/discovery")
        {
            if (req.method == "GET")
            {
                res.body = "{\"ok\":true," + Discovery::Instance().OptionsJson() + "}";
                break;
            }
            if (req.method != "POST")
            {
                res.status = 405;
                res.body = "{\"ok\":false,\"error\":\"use POST\"}";
                break;
            }
            tml::JsonValue v;
            if (!tml::JsonParse(req.body, v) || v.kind != tml::JsonValue::Object)
            {
                res.status = 400;
                res.body = "{\"ok\":false,\"error\":\"bad_json\"}";
                break;
            }
            Discovery& disc = Discovery::Instance();
            const bool active = v.GetBool("active", disc.ActiveDiscover());
            const bool discoverable = v.GetBool("discoverable", disc.Discoverable());
            const long long sec = v.GetInt("seconds", disc.IntervalMs() / 1000);
            disc.SetOptions(active, discoverable, static_cast<int>(sec) * 1000);
            if (!disc.SaveOptions())
            {
                // 存不下来就别说 ok：下次重启会回到旧值，用户以为开关坏了
                res.status = 500;
                res.body = "{\"ok\":false,\"error\":\"ini_write_failed\"}";
                break;
            }
            res.body = "{\"ok\":true," + disc.OptionsJson() + "}";
            break;
        }

        if (req.path == "/api/devices" || req.path == "/api/discover")
        {
            // /api/discover 是 /api/devices 的**别名**：同一个函数、同一份 payload。
            // 两份代码迟早会分叉，而"整串比较相等"的断言会先红——那时已经晚了一步。
            bool inited = false;
            const std::string payload = DeviceRegistry::Instance().DevicesJson(tml::NowMs(), 200, inited);
            if (!inited)
            {
                // 未初始化必须 503：200 + 空数组会把"配置没起来"说成"附近没有设备"
                res.status = 503;
                res.body = "{\"ok\":false,\"error\":\"not_initialized\",\"reason\":\"" +
                           tml::JsonEscape(DeviceRegistry::Instance().PersistReason()) + "\"}";
                break;
            }
            res.body = payload;
            break;
        }

        if (req.path == "/api/conversations")
        {
            res.body = "{\"ok\":true,\"conversations\":" + hub.ConversationsJson() + "}";
            break;
        }

        if (req.path == "/api/settings/save-dir")
        {
            // 保存目录的读/换/打开。**只放网页端**：插件的设置弹窗只显示现状，
            // 因为换目录必须连带说清"文件会按设备分文件夹"，一行 MessageBox 讲不明白。
            //
            // choose 会弹系统目录选择器（STA 子线程，owner=nullptr）：
            // 该请求会一直挂到用户选完或取消，属于预期行为。
            if (req.method != "POST")
            {
                res.status = 405;
                res.body = "{\"ok\":false,\"error\":\"use POST\"}";
                break;
            }
            const std::string action = tml::QueryParam(req.query, "action");

            if (action == "open")
            {
                const std::wstring dir = hub.ShareDir();
                std::thread([dir]()
                {
                    // 同样要抢前台：资源管理器新开的窗口默认会被压在后面
                    ::AllowSetForegroundWindow(static_cast<DWORD>(-1));
                    ::ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }).detach();
                res.body = "{\"ok\":true}";
                break;
            }

            std::wstring chosen;
            if (action == "set")
            {
                const std::string raw = tml::QueryParam(req.query, "dir");
                if (raw.empty())
                {
                    res.status = 400;
                    res.body = "{\"ok\":false,\"error\":\"dir required\"}";
                    break;
                }
                chosen = tml::Utf8ToWide(raw);
            }
            else if (action == "choose")
            {
                std::wstring picked;
                std::thread t([&picked]() { picked = tml::PickFolder(); });
                t.join();
                if (picked.empty())
                {
                    res.body = "{\"ok\":false,\"error\":\"cancelled\"}";
                    break;
                }
                chosen = picked;
            }
            else
            {
                res.status = 400;
                res.body = "{\"ok\":false,\"error\":\"unknown action\"}";
                break;
            }

            // 先验证可写再落配置：选到只读目录会让之后每次落盘都静默失败。
            if (!tml::EnsureDir(chosen))
            {
                res.status = 400;
                res.body = "{\"ok\":false,\"error\":\"not_writable\"}";
                break;
            }
            hub.SetSaveDir(chosen);
            res.body = "{\"ok\":true,\"saveDir\":\"" +
                       tml::JsonEscape(tml::WideToUtf8(hub.ShareDir())) + "\"}";
            break;
        }

        if (req.path == "/api/autostart")
        {
            // 开机自启：只有独立 exe 注册了宿主 hook 才支持。
            // 插件宿主没注册 → supported:false，网页据此隐藏开关（不能假装成功）。
            auto& hooks = tml::host::GetHooks();
            if (!hooks.getAutostart || !hooks.setAutostart)
            {
                res.body = "{\"ok\":true,\"supported\":false,\"enabled\":false}";
                break;
            }
            if (req.method == "GET")
            {
                bool enabled = false;
                const bool ok = hooks.getAutostart(enabled);
                res.status = ok ? 200 : 500;
                res.body = std::string("{\"ok\":") + (ok ? "true" : "false") +
                           ",\"supported\":true,\"enabled\":" + (enabled ? "true" : "false") + "}";
                break;
            }
            if (req.method == "POST")
            {
                bool enable = true;
                std::string v;
                if (tml::JsonGetString(req.body, "enable", v)) enable = (v == "true" || v == "1");
                // 也接受 ?enable=0/1，方便不构造 JSON 的调用方
                const std::string q = tml::QueryParam(req.query, "enable");
                if (q == "0") enable = false;
                else if (q == "1") enable = true;

                const bool ok = hooks.setAutostart(enable);
                res.status = ok ? 200 : 500;
                res.body = std::string("{\"ok\":") + (ok ? "true" : "false") +
                           ",\"supported\":true,\"enabled\":" + (ok && enable ? "true" : "false") + "}";
                break;
            }
            res.status = 405;
            res.body = "{\"ok\":false,\"error\":\"use GET or POST\"}";
            break;
        }

        if (req.path == "/api/devices/remove")
        {
            // 删除设备（用户显式动作）。分两步：`dry=1` 只回报"要删什么"，
            // 界面先把它摆给用户看（路径 / 文件数 / 总大小），确认后再真删。
            // 直接删的后果太硬：删错了没有回收站，而用户点这一下时多半以为只是"从列表里去掉"。
            if (req.method != "POST")
            {
                res.status = 405;
                res.body = "{\"ok\":false,\"error\":\"use POST\"}";
                break;
            }
            std::string key;
            if (!tml::JsonGetString(req.body, "deviceId", key) || key.empty())
                key = tml::QueryParam(req.query, "deviceId");
            const bool dry = (tml::QueryParam(req.query, "dry") == "1");
            const bool withFiles = (tml::QueryParam(req.query, "files") != "0");
            if (key.empty())
            {
                res.status = 400;
                res.body = "{\"ok\":false,\"error\":\"deviceId required\"}";
                break;
            }

            // 显示名：v2 取设备表（含别名），legacy 取会话里记录的名字
            std::string displayName;
            {
                PeerDevice pd;
                if (DeviceRegistry::Instance().FindPeer(key, pd)) displayName = pd.DisplayName();
            }
            if (displayName.empty() && key.rfind("legacy:", 0) == 0) displayName = key.substr(7);
            if (displayName.empty()) displayName = key;

            std::wstring folderPath;
            int fileCount = 0;
            long long bytes = 0;
            const bool folderExists = hub.DeviceFolderInfo(key, displayName, folderPath, fileCount, bytes);

            if (dry)
            {
                res.body = "{\"ok\":true,\"dry\":true,\"path\":\"" + tml::JsonEscape(tml::WideToUtf8(folderPath)) +
                           "\",\"exists\":" + (folderExists ? "true" : "false") +
                           ",\"fileCount\":" + std::to_string(fileCount) +
                           ",\"sizeBytes\":" + std::to_string(bytes) + "}";
                break;
            }

            int removedFiles = 0;
            if (withFiles && folderExists) hub.DeviceFolderRemove(key, displayName, removedFiles);

            const std::string convId = hub.ConvIdOfPeer(key);
            if (!convId.empty()) hub.RemoveConversation(convId);

            // 删除前先看它是否还活着：活着的会被压制 60s 不再自动发现（见 RemovePeer）
            bool wasAlive = false;
            {
                PeerDevice pd;
                if (DeviceRegistry::Instance().FindPeer(key, pd)) wasAlive = (pd.state != "offline");
            }

            // legacy 会话**没有设备条目**，RemovePeer 会返回 false——那是正常的，不是失败
            const bool peerRemoved = DeviceRegistry::Instance().RemovePeer(key);

            res.body = "{\"ok\":true,\"removedFiles\":" + std::to_string(removedFiles) +
                       ",\"peerRemoved\":" + (peerRemoved ? "true" : "false") +
                       ",\"discoverySuppressedMs\":" + (wasAlive ? "60000" : "0") + "}";
            break;
        }

        if (req.path == "/api/devices/rename")
        {
            // T04：给会话改名。legacy 会话（没有 deviceId）只写进设备表的 aliases 映射，
            // **不建任何设备条目**——否则它会以"永远离线"的伪设备身份混进在线列表。
            if (req.method != "POST")
            {
                res.status = 405;
                res.body = "{\"ok\":false,\"error\":\"use POST\"}";
                break;
            }
            std::string key;
            if (!tml::JsonGetString(req.body, "deviceId", key) || key.empty())
                key = tml::QueryParam(req.query, "deviceId");
            std::string alias;
            if (!tml::JsonGetString(req.body, "alias", alias))
                alias = tml::QueryParam(req.query, "alias");

            if (key.empty())
            {
                res.status = 400;
                res.body = "{\"ok\":false,\"error\":\"deviceId required\"}";
                break;
            }

            // 传设备名也接受：统一折算成会话键，避免调用方各自拼一次
            if (key.rfind("legacy:", 0) != 0 && key.rfind("unknown:", 0) != 0)
            {
                PeerDevice probe;
                if (!DeviceRegistry::Instance().FindPeer(key, probe))
                    key = TransferHub::MakeLegacyKey(key);
            }

            const bool ok = DeviceRegistry::Instance().SetAliasForKey(key, alias);
            res.body = std::string("{\"ok\":") + (ok ? "true" : "false") +
                       ",\"deviceId\":\"" + tml::JsonEscape(key) + "\"}";
            break;
        }

        if (req.path == "/api/message")
        {
            if (req.method != "POST")
            {
                res.status = 405;
                res.body = "{\"ok\":false,\"error\":\"use POST\"}";
                break;
            }
            std::string text;
            if (!tml::JsonGetString(req.body, "text", text) || text.empty())
            {
                res.status = 400;
                res.body = "{\"ok\":false,\"error\":\"text required\"}";
                break;
            }
            std::string dir = "out";
            std::string d;
            if (tml::JsonGetString(req.body, "dir", d) && (d == "out" || d == "in")) dir = d;

            std::string dev;
            if (!tml::JsonGetString(req.body, "dev", dev))
                dev = tml::QueryParam(req.query, "dev");

            // T03：可选 msgId（重发沿用同一个）与 peer（对端 deviceId）。
            // 都不给 = 老客户端，走 legacy 推导。
            std::string msgIdIn;
            if (!tml::JsonGetString(req.body, "msgId", msgIdIn))
                msgIdIn = tml::QueryParam(req.query, "msgId");
            std::string peerId;
            if (!tml::JsonGetString(req.body, "peer", peerId))
                peerId = tml::QueryParam(req.query, "peer");

            // T04：legacy 请求（无 X-Device-Id）默认并进 legacy:<dev> 会话。
            // **但显式给了 peer 就必须用它**：PC 自己的管理页也是"无头"请求，
            // 它要能指定发给某个新客户端；若这里无条件覆盖成 legacy:unknown，
            // "PC 发给新客户端"这条路直接断掉，而且表现为"对方当前不可达"。
            if (IsLegacyRequest(req) && peerId.empty()) peerId = TransferHub::MakeLegacyKey(dev);

            std::string finalMsgId;
            long long id = 0;
            std::string status;
            std::string reason;
            std::string detail;

            if (dir == "out")
            {
                // T05：出站先分配身份，再真正投递，最后一次入库。
                // 老的"只记本地、不投递"行为在这里结束——那是"发送成功但对端没收到"。
                ShareMessage existing;
                if (!msgIdIn.empty() && hub.FindByMsgId(msgIdIn, existing))
                {
                    // 重发：**必须真的再投一次**。
                    //
                    // 旧实现在这里只做"幂等返回、不重复投递"，结果是：对端关掉再打开，
                    // 用户点「重发」，服务端什么都不发、状态继续是 failed——界面上看起来
                    // 就是"重发功能失效"（用户报的正是这个）。幂等要保证的是"不产生第二条
                    // 消息"，不是"不再尝试投递"，这两件事被混成了一件。
                    //
                    // 入站重复（对端重传）仍然走纯幂等：那是别人发来的，本端不该再往外发。
                    id = existing.id;
                    finalMsgId = existing.msgId;
                    if (existing.dir == "out")
                    {
                        const std::string target = peerId.empty() ? existing.to : peerId;
                        const PeerSendResult r = PeerClient::Instance().SendText(target, existing.text, existing.msgId);
                        status = r.ok ? "sent" : "failed";
                        reason = r.ok ? std::string() : r.reason;
                        detail = r.detail;
                        // 状态变了才写盘：SetMessageStatus 内部会判断是否真的变了
                        hub.SetMessageStatus(existing.msgId, status);
                    }
                    else
                    {
                        status = existing.status;
                    }
                }
                else
                {
                    ShareMessage out;
                    out.kind = "text";
                    out.text = text;
                    if (!hub.BeginOutbound(peerId, out))
                    {
                        res.body = "{\"ok\":false,\"error\":\"duplicate msgId\"}";
                        break;
                    }
                    const PeerSendResult r = PeerClient::Instance().SendText(peerId, text, out.msgId);
                    id = hub.FinishOutbound(out, r.ok ? "sent" : "failed");
                    finalMsgId = out.msgId;
                    status = r.ok ? "sent" : "failed";
                    reason = r.ok ? std::string() : r.reason;
                    detail = r.detail;
                }
            }
            else
            {
                id = hub.AddText(text, dir, dev, peerId, msgIdIn, &finalMsgId);
            }

            res.body = "{\"ok\":true,\"id\":" + std::to_string(id) +
                       ",\"msgId\":\"" + tml::JsonEscape(finalMsgId) + "\"";
            if (!status.empty()) res.body += ",\"status\":\"" + tml::JsonEscape(status) + "\"";
            if (!reason.empty()) res.body += ",\"reason\":\"" + tml::JsonEscape(reason) + "\"";
            if (!detail.empty()) res.body += ",\"detail\":\"" + tml::JsonEscape(detail) + "\"";
            res.body += "}";
            break;
        }

        if (req.path == "/api/file")
        {
            const std::string id = tml::QueryParam(req.query, "id");
            if (id.empty())
            {
                res.status = 400;
                res.body = "{\"ok\":false,\"error\":\"id required\"}";
                break;
            }
            const std::wstring path = hub.FilePathById(id);
            if (path.empty() || ::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                res.status = 404;
                res.body = "{\"ok\":false,\"error\":\"file not found\"}";
                break;
            }

            std::string name = "file";
            std::string mime = "application/octet-stream";
            hub.FileInfoById(id, name, mime);

            res.sendFile = path;
            res.contentType = mime;
            // 中文文件名必须走 RFC 5987 的 filename*，不能直接塞进头部
            const std::string disposition = (tml::QueryParam(req.query, "inline") == "1") ? "inline" : "attachment";
            res.headers.push_back({ "Content-Disposition", disposition + "; filename*=UTF-8''" + UrlEncode(name) });
            break;
        }

        if (req.path == "/" || req.path == "/index.html")
        {
            res.contentType = "text/html; charset=utf-8";
            res.body = LoadIndexHtml();
            break;
        }

        res.status = 404;
        res.contentType = "application/json; charset=utf-8";
        res.body = "{\"ok\":false,\"error\":\"not found\"}";
    } while (false);

    if (!res.sendFile.empty())
    {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, res.sendFile.c_str(), L"rb") != 0 || fp == nullptr)
        {
            res.sendFile.clear();
            res.status = 500;
            res.contentType = "application/json; charset=utf-8";
            res.body = "{\"ok\":false,\"error\":\"cannot open file\"}";
        }
        else
        {
            _fseeki64(fp, 0, SEEK_END);
            const long long size = _ftelli64(fp);
            _fseeki64(fp, 0, SEEK_SET);

            SendResponseHead(s, 200, res.contentType, size, res.headers);

            std::vector<char> chunk(64 * 1024);
            long long remain = size;
            while (remain > 0)
            {
                size_t want = static_cast<size_t>(std::min<long long>(static_cast<long long>(chunk.size()), remain));
                size_t got = fread(chunk.data(), 1, want, fp);
                if (got == 0) break;
                if (!SendAll(s, chunk.data(), got)) break;
                remain -= static_cast<long long>(got);
            }
            fclose(fp);
            return;
        }
    }

    SendResponseHead(s, res.status, res.contentType, static_cast<long long>(res.body.size()), res.headers);
    if (!res.body.empty()) SendAll(s, res.body.data(), res.body.size());
}
