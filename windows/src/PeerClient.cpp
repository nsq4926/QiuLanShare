#include "PeerClient.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "Common.h"
#include "DeviceRegistry.h"
#include "TransferHub.h"

namespace
{
    // lastOkAt 超过这个时间就不再认为它"新"。5 分钟与 docs/06 第六节的
    // "未过期"口径一致：太短会把刚换过的地址丢掉，太长会让已经失效的地址一直排第一。
    const long long kFreshWindowMs = 5 * 60 * 1000;

    struct Candidate
    {
        std::string ip;
        int         score = 0;
    };

    // 目标是"取前两个各试一次"，所以排序规则必须稳定且可解释：
    //   ① lastOkAt 未过期（最新优先） ② 与本机同网段 ③ ips[] 原本的顺序
    std::vector<std::string> OrderCandidates(const PeerDevice& peer)
    {
        const long long now = tml::NowMs();
        const tml::NicFilterResult mine = tml::GetNicFilter();

        std::vector<Candidate> list;
        list.reserve(peer.ips.size());
        for (size_t i = 0; i < peer.ips.size(); ++i)
        {
            Candidate c;
            c.ip = peer.ips[i].ip;

            if (peer.ips[i].lastOkAt > 0 && (now - peer.ips[i].lastOkAt) <= kFreshWindowMs)
                c.score += 4;

            for (size_t k = 0; k < mine.nics.size(); ++k)
            {
                if (tml::IsSameSubnet(peer.ips[i].ip, mine.nics[k].ip, mine.nics[k].prefixLen))
                {
                    c.score += 2;
                    break;
                }
            }
            list.push_back(c);
        }

        // stable_sort：同分时保持 ips[] 原顺序（第 ③ 条）
        std::stable_sort(list.begin(), list.end(),
                         [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

        std::vector<std::string> out;
        out.reserve(list.size());
        for (size_t i = 0; i < list.size(); ++i) out.push_back(list[i].ip);
        return out;
    }

    // 一次投递尝试：只发一个 IP。
    /** 分片上传的续传标识：同名规则三端一致（字母数字，无路径字符） */
    std::string MakeXid()
    {
        static std::mt19937 rng{ std::random_device{}() };
        char buf[40] = { 0 };
        sprintf_s(buf, "pc%llx%04x", static_cast<unsigned long long>(tml::NowMs()),
                  static_cast<unsigned>(rng() & 0xFFFF));
        return std::string(buf);
    }

    tml::HttpClientResponse TryOnce(const std::string& ip, unsigned short port,
                                    const std::string& path, const std::string& contentType,
                                    const std::string& body, const std::wstring& bodyFile,
                                    long long bodySize, const std::string& selfDeviceId,
                                    int timeoutMs, long long bodyOffset = 0)
    {
        tml::HttpClientRequest req;
        req.host = ip;
        req.port = port;
        req.method = "POST";
        req.path = path;
        req.contentType = contentType;
        req.body = body;
        req.bodyFile = bodyFile;
        req.bodySize = bodySize;
        req.bodyOffset = bodyOffset;
        req.timeoutMs = timeoutMs;
        // v2 身份：对端据此判定"这是带 deviceId 的新客户端"，落进它自己的会话
        req.headers.push_back(std::make_pair(std::string("X-Device-Id"), selfDeviceId));

        tml::HttpClientResponse res;
        tml::HttpPerform(req, res);
        return res;
    }

    bool ResponseOk(const tml::HttpClientResponse& res)
    {
        if (!res.ok || res.status != 200) return false;
        tml::JsonValue v;
        if (!tml::JsonParse(res.body, v)) return false;
        return v.GetBool("ok", false);
    }
}

const char* PeerClient::Unreachable() { return "对方当前不可达"; }
const char* PeerClient::Closed() { return "对方已关闭接收"; }

PeerClient& PeerClient::Instance()
{
    static PeerClient s_instance;
    return s_instance;
}

PeerSendResult PeerClient::ProbeHello(const PeerDevice& peer)
{
    PeerSendResult out;

    DeviceRegistry& reg = DeviceRegistry::Instance();
    const unsigned short port = (peer.port != 0) ? peer.port : static_cast<unsigned short>(8765);
    const std::vector<std::string> candidates = OrderCandidates(peer);
    if (candidates.empty())
    {
        out.reason = Unreachable();
        return out;
    }

    const std::string selfId = TransferHub::Instance().DeviceId();

    // 只探第一个候选：这是每 5 秒一次的健康检查，不是投递；两个候选都试会让
    // 一个失联设备每轮多花一个超时的时间，且毫无收益。
    ++out.attempts;
    tml::HttpClientRequest req;
    req.host = candidates[0];
    req.port = port;
    req.method = "GET";
    req.path = "/api/hello";
    req.timeoutMs = 2000;
    req.headers.push_back(std::make_pair(std::string("X-Device-Id"), selfId));

    tml::HttpClientResponse res;
    tml::HttpPerform(req, res);

    bool ok = false;
    if (res.ok && res.status == 200)
    {
        tml::JsonValue v;
        if (tml::JsonParse(res.body, v))
        {
            // **连上 ≠ 连的是我们对端**：端口可能被别的程序占着（端口递进就是为了这个）。
            // 所以必须核对回来的 deviceId，不一致就当作探测失败。
            ok = (v.GetString("deviceId", std::string()) == peer.deviceId);
        }
    }

    if (!ok)
    {
        // 失败只记诊断信息：**不清 zero lastReachableAt**。
        // 清零会让"暂时连不上（unreachable）"永远无法出现，等于再造一个恒假状态。
        out.reason = Unreachable();
        out.lastError = res.ok ? res.status : res.lastError;
        out.detail = std::string(res.ok ? "http" : res.errorKind) + "/code" +
                     std::to_string(out.lastError) + "/" + candidates[0];
        return out;
    }

    out.ok = true;
    out.ipUsed = candidates[0];
    const long long now = tml::NowMs();
    // 成功：同时推 lastReachableAt 与该 IP 的 lastOkAt（后者决定 ips[] 排序）
    reg.TouchPeer(peer.deviceId, candidates[0], port, now);
    reg.MarkPeerReachable(peer.deviceId, now);
    return out;
}

PeerSendResult PeerClient::SendText(const std::string& peerDeviceId,
                                    const std::string& text,
                                    const std::string& msgId)
{
    PeerSendResult out;

    DeviceRegistry& reg = DeviceRegistry::Instance();
    PeerDevice peer;
    if (!reg.FindPeer(peerDeviceId, peer))
    {
        // 设备表里没有它，就没有端口也没有 IP，连"试一次"都无从谈起
        out.reason = Unreachable();
        return out;
    }

    // 对方显式声明"不接收"：这是唯一能给出第二种原因的情形。
    // 其余一切失败（超时、拒绝、HTTP 非 200、响应 ok=false）都归"不可达"——
    // 不猜，因为猜错会让用户去关对方的接收开关，而真正的问题是网不通。
    if (!peer.acceptsInbound)
    {
        out.reason = Closed();
        return out;
    }

    // 目标端口取设备表：端口递进（8765→8766→8767）之后对端可能不在 8765
    const unsigned short port = (peer.port != 0) ? peer.port : static_cast<unsigned short>(8765);

    const std::vector<std::string> candidates = OrderCandidates(peer);
    if (candidates.empty())
    {
        out.reason = Unreachable();
        return out;
    }

    const std::string selfId = TransferHub::Instance().DeviceId();
    const std::string selfName = TransferHub::Instance().DeviceName();

    std::string body = "{\"text\":\"" + tml::JsonEscape(text) + "\"";
    body += ",\"dir\":\"in\"";
    body += ",\"dev\":\"" + tml::JsonEscape(selfName) + "\"";
    body += ",\"peer\":\"" + tml::JsonEscape(selfId) + "\"";   // 对端据此知道"对面是谁"
    if (!msgId.empty()) body += ",\"msgId\":\"" + tml::JsonEscape(msgId) + "\"";
    body += "}";

    // 取前两个候选，各试一次：不是队列、不落 pending、不自动第三次
    const size_t attempts = (std::min)(candidates.size(), static_cast<size_t>(2));
    for (size_t i = 0; i < attempts; ++i)
    {
        ++out.attempts;
        const tml::HttpClientResponse res = TryOnce(candidates[i], port, "/api/message",
                                                    "application/json", body, std::wstring(), 0,
                                                    selfId, 4000);
        if (ResponseOk(res))
        {
            out.ok = true;
            out.ipUsed = candidates[i];
            // 成功才回写 lastOkAt。失败不回写、也不清零：
            // 一次抖动不该把曾经正确的地址丢掉。
            reg.TouchPeer(peerDeviceId, candidates[i], port, tml::NowMs());
            // 投递成功 = 一次成功的 HTTP 往返：四态判定看 lastReachableAt，不看 lastSeen
            reg.MarkPeerReachable(peerDeviceId, tml::NowMs());
            return out;
        }
        out.lastError = res.ok ? res.status : res.lastError;
        out.detail = std::string("try") + std::to_string(i) + ":" +
                     (res.ok ? ("http" + std::to_string(res.status)) : res.errorKind) +
                     "/code" + std::to_string(res.lastError) + "/" + candidates[i];
    }

    out.reason = Unreachable();
    return out;
}

PeerSendResult PeerClient::SendFile(const std::string& peerDeviceId,
                                    const std::wstring& filePath,
                                    const std::string& fileName,
                                    const std::string& mime,
                                    const std::string& msgId)
{
    PeerSendResult out;

    DeviceRegistry& reg = DeviceRegistry::Instance();
    PeerDevice peer;
    if (!reg.FindPeer(peerDeviceId, peer))
    {
        out.reason = Unreachable();
        return out;
    }
    if (!peer.acceptsInbound)
    {
        out.reason = Closed();
        return out;
    }

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard, &fad))
    {
        out.reason = Unreachable();
        return out;
    }
    const long long size = (static_cast<long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;

    const unsigned short port = (peer.port != 0) ? peer.port : static_cast<unsigned short>(8765);
    const std::vector<std::string> candidates = OrderCandidates(peer);
    if (candidates.empty())
    {
        out.reason = Unreachable();
        return out;
    }

    const std::string selfId = TransferHub::Instance().DeviceId();
    const std::string selfName = TransferHub::Instance().DeviceName();

    std::string path = "/api/upload?name=" + tml::UrlEncode(fileName) +
                       "&mime=" + tml::UrlEncode(mime.empty() ? std::string("application/octet-stream") : mime) +
                       "&dir=in&dev=" + tml::UrlEncode(selfName) +
                       "&peer=" + tml::UrlEncode(selfId);
    if (!msgId.empty()) path += "&msgId=" + tml::UrlEncode(msgId);

    // 分片：对端支持续传时按 1 MB 发。
    // 为什么必须分片：整份发时手机端要把它整个收进内存（ArkTS 还要在主线程上处理），
    // 10 MB 就开始卡、上百 MB 直接被 FreezeDetector 杀掉（"Win 发文件把鸿蒙端搞崩"）。
    if (peer.resumable && size > 0)
    {
        const size_t n = (std::min)(candidates.size(), static_cast<size_t>(2));
        for (size_t i = 0; i < n; ++i)
        {
            ++out.attempts;
            const std::string xid = MakeXid();
            long long off = 0;
            int fails = 0;
            bool allOk = true;
            while (off < size)
            {
                const long long len = (std::min<long long>)(1024LL * 1024LL, size - off);
                const std::string cp = path + "&xid=" + tml::UrlEncode(xid) +
                                       "&offset=" + std::to_string(off) +
                                       "&total=" + std::to_string(size);
                const tml::HttpClientResponse r = TryOnce(candidates[i], port, cp,
                                                          "application/octet-stream", std::string(),
                                                          filePath, len, selfId, 30000, off);
                if (!(r.ok && r.status == 200))
                {
                    // 只有失败才记日志：正常分片一片一行会把日志冲掉
                    tml::AppendLogLine(tml::JoinPath(TransferHub::Instance().DataDir(), L"lanshare.log"),
                                       "chunk fail off=" + std::to_string(off) + " ip=" + candidates[i] +
                                       " ok=" + (r.ok ? "1" : "0") + " st=" + std::to_string(r.status) +
                                       " err=" + r.errorKind + "/" + std::to_string(r.lastError));
                }
                if (r.ok && r.status == 200)
                {
                    off += len;
                    fails = 0;
                    continue;
                }
                if (r.ok && r.status == 409)
                {
                    // 服务端回报真实进度：从那儿继续，不整份重来
                    tml::JsonValue v;
                    if (tml::JsonParse(r.body, v))
                    {
                        const long long got = v.GetInt("received", -1);
                        if (got >= 0 && got <= size) { off = got; fails = 0; continue; }
                    }
                }
                ++fails;
                if (fails > 6) { allOk = false; break; }
                ::Sleep(200);
            }
            if (allOk)
            {
                out.ok = true;
                out.ipUsed = candidates[i];
                reg.TouchPeer(peerDeviceId, candidates[i], port, tml::NowMs());
                reg.MarkPeerReachable(peerDeviceId, tml::NowMs());
                return out;
            }
        }
        out.reason = Unreachable();
        return out;
    }

    const size_t attempts = (std::min)(candidates.size(), static_cast<size_t>(2));
    for (size_t i = 0; i < attempts; ++i)
    {
        ++out.attempts;
        const tml::HttpClientResponse res = TryOnce(candidates[i], port, path,
                                                    "application/octet-stream", std::string(),
                                                    filePath, size, selfId, 30000);
        if (ResponseOk(res))
        {
            out.ok = true;
            out.ipUsed = candidates[i];
            reg.TouchPeer(peerDeviceId, candidates[i], port, tml::NowMs());
            // 投递成功 = 一次成功的 HTTP 往返：四态判定看 lastReachableAt，不看 lastSeen
            reg.MarkPeerReachable(peerDeviceId, tml::NowMs());
            return out;
        }
        out.lastError = res.ok ? res.status : res.lastError;
        out.detail = std::string("try") + std::to_string(i) + ":" +
                     (res.ok ? ("http" + std::to_string(res.status)) : res.errorKind) +
                     "/code" + std::to_string(res.lastError) + "/" + candidates[i];
    }

    out.reason = Unreachable();
    return out;
}
