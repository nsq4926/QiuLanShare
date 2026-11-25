#include "Discovery.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "Common.h"
#include "DeviceRegistry.h"
#include "PeerClient.h"
#include "TransferHub.h"

#pragma comment(lib, "ws2_32.lib")

namespace
{
    const unsigned short kDiscoverPort = 8766;
    const char* kMulticastIp = "239.255.42.99";

    const int kPruneIntervalMs    = 5000;   // TTL / 降级检查周期
    const long long kIpTtlMs      = 5 * 60 * 1000;   // ips[] 的 TTL（docs/06 第六节）
    const int kOfflineMissTicks   = 3;      // 连续 3 次没收到 → 状态降级
    const int kProbeIntervalMs    = 5000;    // TCP 健康探测周期
    const long long kProbeWindowMs = 120000; // 只探 lastSeen 还在窗口内的对端

    bool WriteText(const std::wstring& path, const std::string& text)
    {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || fp == nullptr) return false;
        fwrite(text.data(), 1, text.size(), fp);
        fclose(fp);
        return true;
    }
}

Discovery& Discovery::Instance()
{
    static Discovery s_instance;
    return s_instance;
}

std::string Discovery::BroadcastSelfTest()
{
    // 逐档断言。这份表来自"按 4 bit 切字节算掩码"那个真实事故：
    // /24 会算成 15.15.15.15、广播地址成 250.244.240.241，不崩不报错，
    // 只表现为"某个网段就是不灵"。
    struct Case { const char* ip; int prefix; const char* expect; };
    static const Case kCases[] = {
        { "10.1.2.3",        8,  "10.255.255.255" },
        { "172.16.5.6",     16,  "172.16.255.255" },
        { "192.168.8.5",    22,  "192.168.11.255" },
        { "192.168.8.5",    24,  "192.168.8.255"  },
        { "192.168.8.5",    25,  "192.168.8.127"  },
        { "192.168.8.5",    30,  "192.168.8.7"    },
    };

    std::string report = "subnet broadcast self-test\n";
    int pass = 0;
    int total = 0;

    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i)
    {
        std::string got;
        const bool ok = tml::SubnetBroadcast(kCases[i].ip, kCases[i].prefix, got);
        const bool match = ok && (got == kCases[i].expect);
        ++total;
        if (match) ++pass;
        char line[256] = { 0 };
        sprintf_s(line, "  %-14s /%-2d -> %-16s expect %-16s %s\n",
                  kCases[i].ip, kCases[i].prefix, ok ? got.c_str() : "(none)",
                  kCases[i].expect, match ? "PASS" : "FAIL");
        report += line;
    }

    // /31、/32 必须"没有广播地址"——公式在这里会给出一个看起来合法的值
    static const int kNoBroadcast[] = { 31, 32 };
    for (size_t i = 0; i < sizeof(kNoBroadcast) / sizeof(kNoBroadcast[0]); ++i)
    {
        std::string got;
        const bool ok = tml::SubnetBroadcast("192.168.8.5", kNoBroadcast[i], got);
        ++total;
        if (!ok) ++pass;
        char line[256] = { 0 };
        sprintf_s(line, "  %-14s /%-2d -> %-16s expect (none)         %s\n",
                  "192.168.8.5", kNoBroadcast[i], ok ? got.c_str() : "(none)",
                  ok ? "FAIL" : "PASS");
        report += line;
    }

    char tail[96] = { 0 };
    sprintf_s(tail, "RESULT %d/%d\n", pass, total);
    report += tail;
    return report;
}

void Discovery::Start(unsigned short listenPort, const std::wstring& dataDir,
                      const std::wstring& iniPath)
{
    if (m_running.exchange(true)) return;

    m_port = listenPort;
    m_dataDir = dataDir;
    m_iniPath = iniPath;
    // 策略必须在第一次 announce 之前装好，否则启动瞬间会按默认值广播一次，
    // 用户明明关了「被发现」，对端却在开机那几秒里看见它。
    LoadOptions();

    if (!tml::EnsureWinsock())
    {
        m_running.store(false);
        return;
    }

    // 自检：只在显式打开时写盘，生产路径不受影响
    {
        char flag[8] = { 0 };
        if (::GetEnvironmentVariableA("TMLS_SELFTEST", flag, sizeof(flag)) > 0 && dataDir.size() > 0)
        {
            WriteText(tml::JoinPath(dataDir, L"lanshare_selftest.log"), BroadcastSelfTest());
        }
    }

    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
    {
        m_running.store(false);
        return;
    }

    // UDP 这里**要** SO_REUSEADDR：同机双实例（5b 形态）必须都能收广播，
    // 这与 TCP 监听端口的情形正好相反——TCP 上它会造成"自己连自己"的假象（见 HttpServer）。
    BOOL reuse = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(kDiscoverPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        ::closesocket(s);
        m_running.store(false);
        return;
    }

    BOOL on = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));

    // 加入组播组：部分 AP 会丢广播但放过组播
    ip_mreq mreq{};
    ::InetPtonA(AF_INET, kMulticastIp, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    ::setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));

    DWORD rcvTimeout = 1000;   // 让线程能定期醒来做 announce / TTL
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcvTimeout), sizeof(rcvTimeout));

    m_sock = static_cast<std::uintptr_t>(s);
    m_thread = std::thread(&Discovery::Loop, this);
}

void Discovery::Stop()
{
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();

    if (m_sock != 0)
    {
        ::closesocket(static_cast<SOCKET>(m_sock));
        m_sock = 0;
    }
}

std::string Discovery::BuildPayload() const
{
    TransferHub& hub = TransferHub::Instance();
    const tml::NicFilterResult nics = tml::GetNicFilter();

    std::string out = "{\"v\":2,\"app\":\"TMLS\"";
    out += ",\"deviceId\":\"" + tml::JsonEscape(hub.DeviceId()) + "\"";
    out += ",\"name\":\"" + tml::JsonEscape(hub.DeviceName()) + "\"";
    out += ",\"port\":" + std::to_string(hub.Port());
    out += ",\"mac\":\"" + tml::JsonEscape(hub.MacAddress()) + "\"";

    // ips[] 必须全量：每张网卡各发一次 announce，对端按 deviceId 合并取并集
    out += ",\"ips\":[";
    for (size_t i = 0; i < nics.nics.size(); ++i)
    {
        if (i > 0) out += ",";
        out += "\"" + tml::JsonEscape(nics.nics[i].ip) + "\"";
    }
    out += "]";

    // acceptsInbound 每次必带（docs/06 P0-10：值必须稳定、不能一次有一次没有）。
    // 它现在由「被发现」开关决定 —— 关掉后既不自报，也不出现在别人的列表里。
    out += std::string(",\"acceptsInbound\":") + (m_discoverable.load() ? "true" : "false");
    out += ",\"filteredNicCount\":" + std::to_string(nics.filteredNicCount);
    // 分片续传能力：手机端看到这个才敢按 256KB 切片发（老 PC 端没有它，只能整份发）
    out += ",\"resumable\":true";
    out += ",\"apiVersion\":\"";
    out += TransferHub::ApiVersion();
    out += "\",\"platform\":\"windows\"}";
    return out;
}

std::string Discovery::OptionsJson() const
{
    // 只给"成员列表"，不带花括号：调用方按 {"ok":true,<这里>} 拼。
    // （返回整对象时外面再包一层就成了 {"ok":true,{"active":...}} —— 不是合法 JSON，
    //   网页 JSON.parse 直接抛，表现为"开关点了没反应"。）
    std::string out = std::string("\"active\":") + (m_active.load() ? "true" : "false");
    out += std::string(",\"discoverable\":") + (m_discoverable.load() ? "true" : "false");
    out += ",\"seconds\":" + std::to_string(m_intervalMs.load() / 1000);
    return out;
}

void Discovery::LoadOptions()
{
    if (m_iniPath.empty()) return;

    wchar_t buf[32] = { 0 };
    ::GetPrivateProfileStringW(L"discovery", L"active", L"1", buf, _countof(buf), m_iniPath.c_str());
    m_active.store(std::wstring(buf) != L"0");

    ::GetPrivateProfileStringW(L"discovery", L"acceptsInbound", L"1", buf, _countof(buf), m_iniPath.c_str());
    m_discoverable.store(std::wstring(buf) != L"0");

    ::GetPrivateProfileStringW(L"discovery", L"seconds", L"3", buf, _countof(buf), m_iniPath.c_str());
    int sec = ::_wtoi(buf);
    if (sec < 3) sec = 3;
    if (sec > 300) sec = 300;
    m_intervalMs.store(sec * 1000);
}

bool Discovery::SaveOptions() const
{
    if (m_iniPath.empty()) return false;
    // ini 一律 UTF-16（WritePrivateProfileStringW），与 docs/01 第七节第 8 条一致
    const BOOL a = ::WritePrivateProfileStringW(L"discovery", L"active",
        m_active.load() ? L"1" : L"0", m_iniPath.c_str());
    const BOOL b = ::WritePrivateProfileStringW(L"discovery", L"acceptsInbound",
        m_discoverable.load() ? L"1" : L"0", m_iniPath.c_str());
    wchar_t num[16] = { 0 };
    swprintf_s(num, L"%d", m_intervalMs.load() / 1000);
    const BOOL c = ::WritePrivateProfileStringW(L"discovery", L"seconds", num, m_iniPath.c_str());
    return a && b && c;
}

void Discovery::SetOptions(bool active, bool discoverable, int intervalMs)
{
    if (intervalMs < 3000) intervalMs = 3000;
    if (intervalMs > 300000) intervalMs = 300000;

    const bool wasAnnouncing = m_active.load() && m_discoverable.load();
    m_active.store(active);
    m_discoverable.store(discoverable);
    m_intervalMs.store(intervalMs);

    // 刚打开（或改了周期）就立刻发一次：用户点了开关应当马上有效，而不是等一个周期
    if (!wasAnnouncing && active && discoverable) AnnounceOnce();
}

void Discovery::AnnounceOnce()
{
    const SOCKET s = static_cast<SOCKET>(m_sock);
    if (s == INVALID_SOCKET || m_sock == 0) return;
    // 「被发现」关掉就不再自报家门：这是它唯一语义，别在这里再叠别的条件。
    // 「主动发现」只控制**周期**广播与登记（见 Loop），手动「立即搜寻」照样发一次。
    if (!m_discoverable.load()) return;

    const std::string payload = BuildPayload();
    const tml::NicFilterResult nics = tml::GetNicFilter();

    std::vector<std::string> targets;
    targets.push_back("255.255.255.255");
    targets.push_back(kMulticastIp);

    for (size_t i = 0; i < nics.nics.size(); ++i)
    {
        std::string bcast;
        // /31、/32 没有广播地址：这里跳过，而不是"用公式算出来什么就发什么"
        if (!tml::SubnetBroadcast(nics.nics[i].ip, nics.nics[i].prefixLen, bcast)) continue;
        if (std::find(targets.begin(), targets.end(), bcast) == targets.end()) targets.push_back(bcast);
    }

    for (size_t i = 0; i < targets.size(); ++i)
    {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = ::htons(kDiscoverPort);
        if (::InetPtonA(AF_INET, targets[i].c_str(), &to.sin_addr) != 1) continue;
        ::sendto(s, payload.data(), static_cast<int>(payload.size()), 0,
                 reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }
}

void Discovery::HandlePacket(const char* data, int len, const std::string& fromIp)
{
    if (len <= 0 || len > 4096) return;

    tml::JsonValue v;
    if (!tml::JsonParse(std::string(data, static_cast<size_t>(len)), v)) return;

    const std::string deviceId = v.GetString("deviceId", std::string());
    if (deviceId.empty()) return;                              // 老协议没有身份，不入列
    if (deviceId == TransferHub::Instance().DeviceId()) return; // 自己的广播

    // 「主动发现」关掉后不再登记新设备（但仍要听 socket：否则「被发现」的回应收不到）
    if (!m_active.load()) return;

    // 刚被用户删掉的设备：压制期内**不再自动发现**（否则下一轮 announce 立刻加回来）。
    // 只挡发现这一条路 —— messages / 手动添加照旧，删除不是拉黑。
    if (DeviceRegistry::Instance().IsDiscoveryIgnored(deviceId)) return;

    const long long now = tml::NowMs();

    PeerDevice peer;
    peer.deviceId = deviceId;
    peer.name = v.GetString("name", std::string());
    peer.platform = v.GetString("platform", "windows");
    peer.mac = v.GetString("mac", std::string());
    peer.port = static_cast<unsigned short>(v.GetInt("port", 0));
    peer.apiVersion = v.GetString("apiVersion", std::string());
    peer.acceptsInbound = v.GetBool("acceptsInbound", true);   // 缺省按"接收"处理
    peer.resumable = v.GetBool("resumable", false);
    peer.filteredNicCount = static_cast<int>(v.GetInt("filteredNicCount", 0));
    peer.state = "online";
    peer.lastSeen = now;

    const tml::JsonValue* ips = v.Find("ips");
    std::map<std::string, long long>& seen = m_announced[deviceId];
    if (ips != nullptr && ips->kind == tml::JsonValue::Array)
    {
        for (size_t i = 0; i < ips->items.size(); ++i)
        {
            if (ips->items[i].kind != tml::JsonValue::String) continue;
            DeviceEndpoint ep;
            ep.ip = ips->items[i].text;
            if (ep.ip.empty()) continue;
            peer.ips.push_back(ep);
            seen[ep.ip] = now;
        }
    }
    if (peer.ips.empty() && !fromIp.empty())
    {
        // 对端没报 ips[]（或报了空）：至少把"从哪个地址来的"记下来，
        // 否则这条记录没法被主动连接。
        DeviceEndpoint ep;
        ep.ip = fromIp;
        peer.ips.push_back(ep);
        seen[fromIp] = now;
    }

    m_missTicks[deviceId] = 0;
    // 同一台机器换了 deviceId 时，旧身份会被并掉；它留下的会话也要一起清，
    // 否则设备表只剩一项、列表里却还挂着几行同名会话。
    std::vector<std::string> merged;
    DeviceRegistry::Instance().UpsertPeer(peer, &merged);
    for (size_t i = 0; i < merged.size(); ++i) TransferHub::Instance().DropPeerHistory(merged[i]);
}

void Discovery::TickPrune()
{
    const long long now = tml::NowMs();

    std::vector<std::string> ids;
    for (std::map<std::string, std::map<std::string, long long>>::const_iterator it = m_announced.begin();
         it != m_announced.end(); ++it)
    {
        ids.push_back(it->first);
    }

    for (size_t i = 0; i < ids.size(); ++i)
    {
        const std::string& id = ids[i];

        // ips[] TTL：没再 announce 的地址摘掉，否则拔了网线它还在，对端会一直连它
        std::vector<std::string> keep;
        std::map<std::string, long long>& seen = m_announced[id];
        for (std::map<std::string, long long>::iterator it = seen.begin(); it != seen.end(); )
        {
            if (now - it->second > kIpTtlMs) it = seen.erase(it);
            else { keep.push_back(it->first); ++it; }
        }
        DeviceRegistry::Instance().KeepPeerIps(id, keep);

        // 连续 kOfflineMissTicks 个周期没收到 → 状态降级（去抖在 T07 做细）
        int& miss = m_missTicks[id];
        ++miss;
        DeviceRegistry::Instance().MarkPeerState(id, miss >= kOfflineMissTicks ? "offline" : "online");
    }
}

void Discovery::ProbeReachablePeers()
{
    // 先拷快照、出锁再发请求：**发网络请求时绝不持锁**。
    // 持 registry 锁去 connect，回包处理又要更新 registry —— 就是前面踩过的
    // "持锁重入"的第三种形态，而且是跨网络的，比前两种更容易卡死。
    const long long now = tml::NowMs();
    const std::vector<PeerDevice> peers = DeviceRegistry::Instance().Peers();

    for (size_t i = 0; i < peers.size(); ++i)
    {
        if (now - peers[i].lastSeen > kProbeWindowMs) continue;   // 窗口外（离线）不探
        // 探测本身走 /api/hello，不产生消息、不推进 seq、不写 conflicts
        PeerClient::Instance().ProbeHello(peers[i]);
    }
}

void Discovery::Loop()
{
    long long lastAnnounce = 0;
    long long lastPrune = 0;
    long long lastProbe = 0;

    while (m_running.load())
    {
        const long long now = tml::NowMs();
        // 周期可配（设置页 3/5/10/30/60 秒）；「主动发现」关掉就不发
        if (m_active.load() && now - lastAnnounce >= m_intervalMs.load())
        {
            AnnounceOnce();
            lastAnnounce = now;
        }
        if (now - lastPrune >= kPruneIntervalMs)
        {
            TickPrune();
            lastPrune = now;
        }
        if (now - lastProbe >= kProbeIntervalMs)
        {
            ProbeReachablePeers();
            lastProbe = now;
        }

        char buf[2048];
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int n = ::recvfrom(static_cast<SOCKET>(m_sock), buf, sizeof(buf), 0,
                                 reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n <= 0) continue;   // 超时就回到循环顶部做 announce，不忙等

        char fromIp[64] = { 0 };
        ::InetNtopA(AF_INET, &from.sin_addr, fromIp, sizeof(fromIp));

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            HandlePacket(buf, n, fromIp);
        }
    }
}

std::string Discovery::DiscoverJson()
{
    // 立即广播一次，让"刚打开界面"不必等一个 announce 周期
    if (m_running.load()) AnnounceOnce();
    return DeviceRegistry::Instance().PeersJson();
}

bool Discovery::AddPeerByAddress(const std::string& ip, unsigned short port, std::string& err,
                                 std::string& deviceIdOut)
{
    deviceIdOut.clear();
    if (ip.empty())
    {
        err = "ip_required";
        return false;
    }
    if (port == 0) port = 8765;

    const std::string selfId = TransferHub::Instance().DeviceId();

    // 探测走 /api/hello：不产生消息、不推进 seq、不写 conflicts
    tml::HttpClientRequest req;
    req.host = ip;
    req.port = port;
    req.method = "GET";
    req.path = "/api/hello";
    req.timeoutMs = 2500;
    req.headers.push_back(std::make_pair(std::string("X-Device-Id"), selfId));

    tml::HttpClientResponse res;
    tml::HttpPerform(req, res);
    if (!res.ok || res.status != 200)
    {
        err = "connect_fail";
        return false;
    }

    tml::JsonValue v;
    if (!tml::JsonParse(res.body, v))
    {
        err = "bad_hello";
        return false;
    }

    // **端口可能被别的程序占着**（端口递进就是为这个），所以必须核对回来的
    // 是不是本插件：没有 deviceId 的一律不登记，否则会凭空造出一条"永远连不上"的设备。
    const std::string deviceId = v.GetString("deviceId", std::string());
    if (deviceId.empty())
    {
        err = "not_lanshare";
        return false;
    }
    if (deviceId == selfId)
    {
        err = "self";
        return false;
    }

    const long long now = tml::NowMs();

    PeerDevice peer;
    peer.deviceId = deviceId;
    peer.name = v.GetString("name", std::string());
    peer.platform = v.GetString("platform", "windows");
    peer.mac = v.GetString("mac", std::string());
    // 用对端自报的端口，不用户输的那个：端口递进后它对外的真实端口可能不是 8765
    const unsigned short realPort = static_cast<unsigned short>(v.GetInt("port", port));
    peer.port = realPort != 0 ? realPort : port;
    peer.apiVersion = v.GetString("apiVersion", std::string());
    peer.acceptsInbound = v.GetBool("acceptsInbound", true);   // hello 不带这个字段时按"接收"处理
    peer.resumable = v.GetBool("resumable", false);
    peer.filteredNicCount = static_cast<int>(v.GetInt("filteredNicCount", 0));
    peer.state = "online";
    peer.lastSeen = now;

    DeviceEndpoint ep;
    ep.ip = ip;
    ep.lastOkAt = now;              // 刚刚真的往返成功过，不是"还没用过"
    peer.ips.push_back(ep);

    // 对端自报的地址（hello 的 ip 字段）一并登记：用户可能填的是它的另一张网卡，
    // 只留一个地址会让下一次投递失败。
    const std::string reported = v.GetString("ip", std::string());
    if (!reported.empty() && reported != ip)
    {
        DeviceEndpoint ep2;
        ep2.ip = reported;
        peer.ips.push_back(ep2);
    }

    DeviceRegistry& reg = DeviceRegistry::Instance();
    if (!reg.UpsertPeer(peer))
    {
        err = "persist_failed";
        return false;
    }
    reg.MarkPeerReachable(deviceId, now);

    deviceIdOut = deviceId;
    err.clear();
    return true;
}
