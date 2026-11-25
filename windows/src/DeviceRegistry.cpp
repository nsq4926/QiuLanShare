#include "DeviceRegistry.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "Common.h"
#include "TransferHub.h"

#pragma comment(lib, "ole32.lib")

namespace
{
    void AppendLogLine(const std::wstring& path, const std::string& line)
    {
        if (path.empty()) return;
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"ab") != 0 || fp == nullptr) return;
        const std::string text = line + "\n";
        fwrite(text.data(), 1, text.size(), fp);
        fclose(fp);
    }
}

namespace
{
    const size_t kMaxPeers        = 256;   // 坏文件防御：别让设备表无限膨胀
    const size_t kMaxIpsPerPeer   = 16;
    const size_t kMaxMacHistory   = 8;

    bool SameStringList(const std::vector<std::string>& a, const std::vector<std::string>& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (a[i] != b[i]) return false;
        }
        return true;
    }
}

std::string NewDeviceId()
{
    // 随机、持久、与硬件无关。
    // 这里刻意用 GUID 而不是 MAC：换网卡 / Windows 随机 MAC / 克隆镜像都会让
    // MAC 派生出来的 ID 变化或碰撞，而 deviceId 一变，对端就把它当成另一台设备。
    GUID g{};
    if (::CoCreateGuid(&g) == S_OK)
    {
        char buf[32] = { 0 };
        sprintf_s(buf, "%08lx%04x",
                  static_cast<unsigned long>(g.Data1),
                  static_cast<unsigned int>(g.Data2));
        return std::string("pc-") + buf;
    }
    // GUID 都拿不到（几乎不可能）：用时间 + 进程 ID 拼一个，仍然保证"首次生成"语义
    char buf[32] = { 0 };
    sprintf_s(buf, "%08lx%04x",
              static_cast<unsigned long>(::GetTickCount64() & 0xFFFFFFFFu),
              static_cast<unsigned int>(::GetCurrentProcessId() & 0xFFFFu));
    return std::string("pc-") + buf;
}

std::string MakeConvId(const std::string& a, const std::string& b)
{
    // 只由两方 deviceId 排序后哈希得出，**不含 IP**：
    // 换网卡 / 换 IP 之后仍然只有一个会话、一份未读（docs/06 §P0-15）。
    const std::string& lo = (a <= b) ? a : b;
    const std::string& hi = (a <= b) ? b : a;

    unsigned long long h = 1469598103934665603ULL;   // FNV-1a 64
    const std::string joined = lo + '\x1f' + hi;
    for (size_t i = 0; i < joined.size(); ++i)
    {
        h ^= static_cast<unsigned char>(joined[i]);
        h *= 1099511628211ULL;
    }
    char buf[32] = { 0 };
    sprintf_s(buf, "%016llx", h);
    return std::string("c-") + buf;
}

const char* DeviceStateToString(DeviceState state)
{
    switch (state)
    {
    case DeviceState::Online:          return "online";
    case DeviceState::OnlineNoInbound: return "online_no_inbound";
    case DeviceState::Unreachable:     return "unreachable";
    case DeviceState::Discovered:      return "discovered";
    case DeviceState::Offline:         return "offline";
    }
    return "offline";
}

int DeviceStateRank(DeviceState state)
{
    switch (state)
    {
    case DeviceState::Online:          return 0;
    case DeviceState::OnlineNoInbound: return 1;
    case DeviceState::Unreachable:     return 2;
    case DeviceState::Discovered:      return 3;
    case DeviceState::Offline:         return 4;
    }
    return 4;
}

DeviceRegistry& DeviceRegistry::Instance()
{
    static DeviceRegistry s_instance;
    return s_instance;
}

bool DeviceRegistry::Init(const std::wstring& dataDir, const std::string& instanceName)
{
    std::lock_guard<std::mutex> lk(m_mutex);

    if (m_inited) return true;
    if (dataDir.empty()) return false;

    m_dataDir = dataDir;

    // 实例名进文件名（TMLS_INSTANCE），只保留可安全出现在文件名里的字符。
    std::wstring suffix;
    for (size_t i = 0; i < instanceName.size() && suffix.size() < 32; ++i)
    {
        const char c = instanceName[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        suffix.push_back(ok ? static_cast<wchar_t>(c) : L'_');
    }

    std::wstring leaf = L"lanshare_devices";
    if (!suffix.empty() && suffix != L"default")
    {
        leaf += L".";
        leaf += suffix;
    }
    leaf += L".json";

    m_path = tml::JoinPath(dataDir, leaf);
    m_logPath = tml::JoinPath(dataDir, L"lanshare.log");

    if (!tml::EnsureDir(dataDir))
    {
        // 数据目录建不出来 = **设备身份存不下去**。
        // 此时绝不能报"初始化成功"：deviceId 每次启动都会重新生成，
        // 对端每次都看到一台新设备、旧会话全部断裂 —— 而且不报错、不告警。
        // 正确做法是**明确失败**，让上层报 503，把"没起来"和"附近没设备"分开。
        m_persistReason = "data dir not creatable: " + tml::WideToUtf8(dataDir);
        AppendLogLine(m_logPath, m_persistReason);
        return false;
    }

    LoadLocked();

    if (m_self.deviceId.empty())
    {
        m_self.deviceId = NewDeviceId();
        {
            const std::string reason = m_regenReason.empty() ? "missing" : m_regenReason;
            AppendLogLine(m_logPath, "deviceId regenerated: old=" + m_regenOldId +
                                     " new=" + m_self.deviceId + " reason=" + reason);
        }
        m_inited = true;
        if (!SaveLocked())     // 首次生成立刻落盘：下一次启动必须还是这个 ID
        {
            // 存不下去 = 身份保不住。同样不能报成功。
            m_persistReason = "device table not writable: " + tml::WideToUtf8(m_path);
            AppendLogLine(m_logPath, m_persistReason);
            m_inited = false;
            return false;
        }
        return true;
    }

    m_inited = true;
    return true;
}

bool DeviceRegistry::IsInited() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_inited;
}

std::string DeviceRegistry::PersistReason() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_persistReason;
}

std::wstring DeviceRegistry::Path() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_path;
}

std::wstring DeviceRegistry::DataDir() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_dataDir;
}

SelfDevice DeviceRegistry::Self() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_self;      // 值拷贝 = 快照
}

std::string DeviceRegistry::DeviceId() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_self.deviceId;
}

bool DeviceRegistry::UpdateSelf(const std::string& name,
                                const std::string& mac,
                                const std::vector<std::string>& ips,
                                int filteredNicCount,
                                int keptNicCount)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_inited) return false;

    bool changed = false;

    // 本机名字以主机名为准：改名（改主机名）后重启应当显示新名字。
    if (!name.empty() && m_self.name != name) { m_self.name = name; changed = true; }
    if (!mac.empty() && m_self.mac != mac)    { m_self.mac = mac;   changed = true; }
    if (!SameStringList(m_self.ips, ips))     { m_self.ips = ips;   changed = true; }
    if (m_self.filteredNicCount != filteredNicCount)
    {
        m_self.filteredNicCount = filteredNicCount;
        changed = true;
    }
    if (m_self.keptNicCount != keptNicCount)
    {
        m_self.keptNicCount = keptNicCount;
        changed = true;
    }

    if (!changed) return true;
    return SaveLocked();
}

long long DeviceRegistry::Epoch() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_self.epoch > 0 ? m_self.epoch : 1;
}

long long DeviceRegistry::CurrentSeq() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_self.seq;
}

long long DeviceRegistry::NextSeq()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_inited) return 0;

    ++m_self.seq;

    // 落盘失败时序号只在内存里前进（本进程内仍然不重复），重启会回退——
    // 这是磁盘不可写时的既成事实，只能靠 /api/hello 的 apiVersion 之外的日志发现。
    SaveLocked();
    return m_self.seq;
}

std::vector<PeerDevice> DeviceRegistry::Peers() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<PeerDevice> out;
    out.reserve(m_peers.size());
    for (size_t i = 0; i < m_peers.size(); ++i)
    {
        if (m_peers[i].ephemeral) continue;   // 双保险：文件里的条目永远不该是 ephemeral
        out.push_back(m_peers[i]);
    }
    return out;
}

bool DeviceRegistry::FindPeer(const std::string& deviceId, PeerDevice& out) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (size_t i = 0; i < m_peers.size(); ++i)
    {
        if (m_peers[i].deviceId == deviceId) { out = m_peers[i]; return true; }
    }
    for (size_t i = 0; i < m_ephemeral.size(); ++i)
    {
        if (m_ephemeral[i].deviceId == deviceId) { out = m_ephemeral[i]; return true; }
    }
    return false;
}

bool DeviceRegistry::UpsertPeer(const PeerDevice& peer, std::vector<std::string>* mergedOut)
{
    if (peer.deviceId.empty()) return false;

    std::lock_guard<std::mutex> lk(m_mutex);

    // 网页身份（ephemeral）只进内存：它不能被写进设备表，也不能出现在 /api/devices 里
    std::vector<PeerDevice>& bucket = peer.ephemeral ? m_ephemeral : m_peers;
    std::string pendingAlias;   // 被并掉的旧身份上，用户起过的别名

    // ---- 同一台机器换了 deviceId（重装 / 换数据目录 / 清过配置）----
    // 旧条目会以"离线设备"的身份永远留在列表里，用户看到的就是"同一个设备显示好几项"。
    // MAC 是"同一台机器"最可靠的证据，拿它把死掉的身份并掉。
    //
    // **只有对方处于离线时才并**：17b 要求"同机两实例在对端眼里显示两台设备"，
    // 那种情况下两条都在线、MAC 也相同 —— 无条件按 MAC 合并会把 17b 直接做废。
    if (!peer.ephemeral && !peer.mac.empty() && peer.mac != m_self.mac)
    {
        for (size_t i = 0; i < bucket.size(); )
        {
            const PeerDevice& cur = bucket[i];
            if (cur.deviceId == peer.deviceId || cur.mac != peer.mac || cur.state == "online")
            {
                ++i;
                continue;
            }
            // 用户给旧身份起过的别名要跟到新身份上，否则改一次名就要再改一次
            if (!cur.alias.empty()) pendingAlias = cur.alias;
            if (mergedOut != nullptr) mergedOut->push_back(cur.deviceId);
            bucket.erase(bucket.begin() + i);
        }
    }

    for (size_t i = 0; i < bucket.size(); ++i)
    {
        PeerDevice& cur = bucket[i];
        if (cur.deviceId != peer.deviceId) continue;

        // 合并而不是覆盖：设备表是"累积"语义，单次 announce 只带一部分字段
        if (!peer.name.empty()) cur.name = peer.name;
        if (!peer.platform.empty()) cur.platform = peer.platform;
        if (!peer.mac.empty() && cur.mac != peer.mac)
        {
            if (!cur.mac.empty() && std::find(cur.macHistory.begin(), cur.macHistory.end(), cur.mac) == cur.macHistory.end())
            {
                cur.macHistory.push_back(cur.mac);
                if (cur.macHistory.size() > kMaxMacHistory) cur.macHistory.erase(cur.macHistory.begin());
            }
            cur.mac = peer.mac;
        }
        if (peer.port != 0) cur.port = peer.port;
        if (!peer.role.empty()) cur.role = peer.role;
        if (!peer.state.empty()) cur.state = peer.state;
        if (!peer.apiVersion.empty()) cur.apiVersion = peer.apiVersion;
        if (peer.resumable) cur.resumable = true;
        cur.filteredNicCount = peer.filteredNicCount;   // announce 是权威，0 也是合法值
        cur.acceptsInbound = peer.acceptsInbound;
        cur.paired = peer.paired;
        if (!peer.convId.empty()) cur.convId = peer.convId;
        if (peer.firstSeen != 0 && cur.firstSeen == 0) cur.firstSeen = peer.firstSeen;
        if (peer.lastSeen > cur.lastSeen) cur.lastSeen = peer.lastSeen;
        if (!peer.alias.empty()) cur.alias = peer.alias;   // 别名只由本机显式设置覆盖
        else if (cur.alias.empty() && !pendingAlias.empty()) cur.alias = pendingAlias;
        if (cur.convId.empty() && !m_self.deviceId.empty())
            cur.convId = MakeConvId(m_self.deviceId, cur.deviceId);

        for (size_t k = 0; k < peer.ips.size(); ++k)
        {
            bool found = false;
            for (size_t j = 0; j < cur.ips.size(); ++j)
            {
                if (cur.ips[j].ip != peer.ips[k].ip) continue;
                if (peer.ips[k].lastOkAt > cur.ips[j].lastOkAt) cur.ips[j].lastOkAt = peer.ips[k].lastOkAt;
                found = true;
                break;
            }
            if (!found && cur.ips.size() < kMaxIpsPerPeer) cur.ips.push_back(peer.ips[k]);
        }

        if (peer.ephemeral) return true;
        return SaveLocked();
    }

    if (bucket.size() >= kMaxPeers) return false;

    PeerDevice fresh = peer;
    if (fresh.alias.empty() && !pendingAlias.empty()) fresh.alias = pendingAlias;
    if (fresh.firstSeen == 0) fresh.firstSeen = tml::NowMs();
    if (fresh.convId.empty() && !m_self.deviceId.empty())
        fresh.convId = MakeConvId(m_self.deviceId, fresh.deviceId);
    if (fresh.ips.size() > kMaxIpsPerPeer) fresh.ips.resize(kMaxIpsPerPeer);
    if (fresh.macHistory.size() > kMaxMacHistory) fresh.macHistory.resize(kMaxMacHistory);
    bucket.push_back(fresh);

    if (peer.ephemeral) return true;
    return SaveLocked();
}

int DeviceRegistry::DedupeByMac(std::vector<std::string>* removedOut)
{
    std::lock_guard<std::mutex> lk(m_mutex);

    // mac -> 该 mac 里 lastSeen 最大的那条下标（保留者）
    std::map<std::string, size_t> keeper;
    for (size_t i = 0; i < m_peers.size(); ++i)
    {
        const PeerDevice& p = m_peers[i];
        if (p.mac.empty() || p.mac == m_self.mac) continue;
        const std::map<std::string, size_t>::iterator it = keeper.find(p.mac);
        if (it == keeper.end() || p.lastSeen > m_peers[it->second].lastSeen) keeper[p.mac] = i;
    }
    if (keeper.empty()) return 0;

    int removed = 0;
    std::vector<PeerDevice> keep;
    keep.reserve(m_peers.size());
    for (size_t i = 0; i < m_peers.size(); ++i)
    {
        const PeerDevice& p = m_peers[i];
        if (p.mac.empty() || p.mac == m_self.mac) { keep.push_back(p); continue; }

        const std::map<std::string, size_t>::const_iterator it = keeper.find(p.mac);
        if (it != keeper.end() && it->second != i && p.state != "online")
        {
            if (removedOut != nullptr) removedOut->push_back(p.deviceId);
            ++removed;
            continue;
        }
        keep.push_back(p);
    }
    if (removed == 0) return 0;

    m_peers.swap(keep);
    SaveLocked();
    return removed;
}

bool DeviceRegistry::TouchPeer(const std::string& deviceId, const std::string& ip,
                               unsigned short port, long long now)
{
    if (deviceId.empty()) return false;

    std::lock_guard<std::mutex> lk(m_mutex);

    for (size_t i = 0; i < m_peers.size(); ++i)
    {
        PeerDevice& cur = m_peers[i];
        if (cur.deviceId != deviceId) continue;

        if (port != 0) cur.port = port;
        if (now > cur.lastSeen) cur.lastSeen = now;
        cur.state = "online";
        if (!ip.empty())
        {
            bool found = false;
            for (size_t j = 0; j < cur.ips.size(); ++j)
            {
                if (cur.ips[j].ip != ip) continue;
                if (now > cur.ips[j].lastOkAt) cur.ips[j].lastOkAt = now;
                found = true;
                break;
            }
            if (!found && cur.ips.size() < kMaxIpsPerPeer)
            {
                DeviceEndpoint ep;
                ep.ip = ip;
                ep.lastOkAt = now;
                cur.ips.push_back(ep);
            }
        }
        return SaveLocked();
    }

    // 还没登记过：不在这里凭空造一条（缺 name/platform 的半成品设备会在列表里显形）。
    // 调用方应先 UpsertPeer 再 TouchPeer。
    return false;
}

bool DeviceRegistry::SetAlias(const std::string& deviceId, const std::string& alias)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (size_t i = 0; i < m_peers.size(); ++i)
    {
        if (m_peers[i].deviceId != deviceId) continue;
        if (m_peers[i].alias == alias) return true;
        m_peers[i].alias = alias;
        return SaveLocked();
    }
    return false;
}

bool DeviceRegistry::KeepPeerIps(const std::string& deviceId, const std::vector<std::string>& keep)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (size_t i = 0; i < m_peers.size(); ++i)
    {
        PeerDevice& p = m_peers[i];
        if (p.deviceId != deviceId) continue;

        std::vector<DeviceEndpoint> next;
        for (size_t k = 0; k < p.ips.size(); ++k)
        {
            for (size_t j = 0; j < keep.size(); ++j)
            {
                if (p.ips[k].ip != keep[j]) continue;
                next.push_back(p.ips[k]);   // 保留原 lastOkAt
                break;
            }
        }
        if (next.size() == p.ips.size()) return true;   // 无变化，不写盘
        p.ips.swap(next);
        return SaveLocked();
    }
    return false;
}

bool DeviceRegistry::MarkPeerReachable(const std::string& deviceId, long long at)
{
    if (deviceId.empty()) return false;
    std::lock_guard<std::mutex> lk(m_mutex);
    for (size_t i = 0; i < m_peers.size(); ++i)
    {
        if (m_peers[i].deviceId != deviceId) continue;
        if (at > m_peers[i].lastReachableAt) m_peers[i].lastReachableAt = at;
        m_reachableInProc[m_peers[i].deviceId] = at;   // state 判定只看它
        // 可达是最强证据：顺带把 lastSeen 一起推新，免得两个字段自相矛盾
        if (at > m_peers[i].lastSeen) m_peers[i].lastSeen = at;
        return SaveLocked();
    }
    return false;
}

bool DeviceRegistry::IdentityRegenerated() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return !m_regenReason.empty();
}

std::string DeviceRegistry::RegenReason() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_regenReason;
}

bool DeviceRegistry::MarkPeerState(const std::string& deviceId, const std::string& state)
{
    if (state.empty()) return false;
    std::lock_guard<std::mutex> lk(m_mutex);
    for (size_t i = 0; i < m_peers.size(); ++i)
    {
        if (m_peers[i].deviceId != deviceId) continue;
        if (m_peers[i].state == state) return true;
        m_peers[i].state = state;
        return SaveLocked();
    }
    return false;
}

bool DeviceRegistry::RemovePeer(const std::string& deviceId)
{
    std::lock_guard<std::mutex> lk(m_mutex);

    for (size_t i = 0; i < m_ephemeral.size(); ++i)
    {
        if (m_ephemeral[i].deviceId == deviceId) { m_ephemeral.erase(m_ephemeral.begin() + i); return true; }
    }
    for (size_t i = 0; i < m_peers.size(); ++i)
    {
        if (m_peers[i].deviceId != deviceId) continue;

        // 还活着的设备删掉后必须压制自动发现一小会儿，否则它的下一次 announce
        // 立刻把它加回来（离线设备本来就不会 announce，压不压都一样）。
        const bool alive = (m_peers[i].state != "offline");
        m_peers.erase(m_peers.begin() + i);
        const bool ok = SaveLocked();
        if (alive) m_ignoreUntil[deviceId] = tml::NowMs() + 60000;
        return ok;
    }
    return false;
}

void DeviceRegistry::IgnoreDiscovery(const std::string& deviceId, long long ms)
{
    if (deviceId.empty()) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    m_ignoreUntil[deviceId] = tml::NowMs() + (ms > 0 ? ms : 0);
}

bool DeviceRegistry::IsDiscoveryIgnored(const std::string& deviceId) const
{
    if (deviceId.empty()) return false;
    std::lock_guard<std::mutex> lk(m_mutex);
    const std::map<std::string, long long>::const_iterator it = m_ignoreUntil.find(deviceId);
    if (it == m_ignoreUntil.end()) return false;
    if (tml::NowMs() >= it->second) { m_ignoreUntil.erase(it); return false; }   // 过期即解除
    return true;
}

std::string DeviceRegistry::AliasForKey(const std::string& key) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const std::map<std::string, std::string>::const_iterator it = m_aliases.find(key);
    return (it == m_aliases.end()) ? std::string() : it->second;
}

bool DeviceRegistry::SetAliasForKey(const std::string& key, const std::string& alias)
{
    if (key.empty()) return false;

    std::lock_guard<std::mutex> lk(m_mutex);
    const std::map<std::string, std::string>::iterator it = m_aliases.find(key);

    if (alias.empty())
    {
        if (it == m_aliases.end()) return true;
        m_aliases.erase(it);
        return SaveLocked();
    }
    if (it != m_aliases.end() && it->second == alias) return true;
    m_aliases[key] = alias;
    return SaveLocked();
}

// ---------------------------------------------------------------- 序列化

std::string DeviceRegistry::ToJsonLocked() const
{
    std::string out;
    out.reserve(1024);
    out += "{\n  \"version\": 1,\n  \"self\": {";
    out += "\"deviceId\":\"" + tml::JsonEscape(m_self.deviceId) + "\"";
    out += ",\"name\":\"" + tml::JsonEscape(m_self.name) + "\"";
    out += ",\"mac\":\"" + tml::JsonEscape(m_self.mac) + "\"";
    out += ",\"ips\":[";
    for (size_t i = 0; i < m_self.ips.size(); ++i)
    {
        if (i > 0) out += ",";
        out += "\"" + tml::JsonEscape(m_self.ips[i]) + "\"";
    }
    out += "]";
    out += ",\"filteredNicCount\":" + std::to_string(m_self.filteredNicCount);
    out += ",\"keptNicCount\":" + std::to_string(m_self.keptNicCount);
    // epoch / seq 必须落盘：重启后 seq 归零会让接收方水位永久卡在旧值
    out += ",\"epoch\":" + std::to_string(m_self.epoch > 0 ? m_self.epoch : 1);
    out += ",\"seq\":" + std::to_string(m_self.seq);
    out += "},\n  \"peers\": [";
    if (!m_peers.empty()) out += "\n";
    for (size_t i = 0; i < m_peers.size(); ++i)
    {
        const PeerDevice& p = m_peers[i];
        out += "    {\"deviceId\":\"" + tml::JsonEscape(p.deviceId) + "\"";
        out += ",\"name\":\"" + tml::JsonEscape(p.name) + "\"";
        out += ",\"alias\":\"" + tml::JsonEscape(p.alias) + "\"";
        out += ",\"platform\":\"" + tml::JsonEscape(p.platform) + "\"";
        out += ",\"mac\":\"" + tml::JsonEscape(p.mac) + "\"";
        out += ",\"macHistory\":[";
        for (size_t k = 0; k < p.macHistory.size(); ++k)
        {
            if (k > 0) out += ",";
            out += "\"" + tml::JsonEscape(p.macHistory[k]) + "\"";
        }
        out += "],\"ips\":[";
        for (size_t k = 0; k < p.ips.size(); ++k)
        {
            if (k > 0) out += ",";
            out += "{\"ip\":\"" + tml::JsonEscape(p.ips[k].ip) + "\",\"lastOkAt\":" +
                   std::to_string(p.ips[k].lastOkAt) + "}";
        }
        out += "]";
        out += ",\"port\":" + std::to_string(p.port);
        out += ",\"role\":\"" + tml::JsonEscape(p.role) + "\"";
        out += ",\"state\":\"" + tml::JsonEscape(p.state) + "\"";
        out += std::string(",\"acceptsInbound\":") + (p.acceptsInbound ? "true" : "false");
        out += ",\"ephemeral\":false";                       // 文件里永远是 false
        out += ",\"lastSeen\":" + std::to_string(p.lastSeen);
        out += ",\"lastReachableAt\":" + std::to_string(p.lastReachableAt);
        out += ",\"firstSeen\":" + std::to_string(p.firstSeen);
        out += ",\"apiVersion\":\"" + tml::JsonEscape(p.apiVersion) + "\"";
        out += std::string(",\"resumable\":") + (p.resumable ? "true" : "false");
        out += ",\"filteredNicCount\":" + std::to_string(p.filteredNicCount);
        out += std::string(",\"paired\":") + (p.paired ? "true" : "false");
        out += ",\"convId\":\"" + tml::JsonEscape(p.convId) + "\"}";
        if (i + 1 < m_peers.size()) out += ",";
        out += "\n";
    }
    out += "  ],\n  \"aliases\": {";
    {
        bool first = true;
        for (std::map<std::string, std::string>::const_iterator it = m_aliases.begin();
             it != m_aliases.end(); ++it)
        {
            if (!first) out += ",";
            first = false;
            out += "\"" + tml::JsonEscape(it->first) + "\":\"" + tml::JsonEscape(it->second) + "\"";
        }
    }
    out += "}\n}\n";
    return out;
}

bool DeviceRegistry::FromJson(const std::string& text)
{
    tml::JsonValue root;
    if (!tml::JsonParse(text, root)) return false;
    if (root.kind != tml::JsonValue::Object) return false;

    const tml::JsonValue* self = root.Find("self");
    if (self != nullptr && self->kind == tml::JsonValue::Object)
    {
        m_self.deviceId = self->GetString("deviceId", std::string());
        m_self.name = self->GetString("name", std::string());
        m_self.mac = self->GetString("mac", std::string());
        m_self.filteredNicCount = static_cast<int>(self->GetInt("filteredNicCount", 0));
        m_self.keptNicCount = static_cast<int>(self->GetInt("keptNicCount", 0));
        m_self.epoch = self->GetInt("epoch", 1);
        if (m_self.epoch <= 0) m_self.epoch = 1;
        m_self.seq = self->GetInt("seq", 0);
        if (m_self.seq < 0) m_self.seq = 0;

        const tml::JsonValue* ips = self->Find("ips");
        if (ips != nullptr && ips->kind == tml::JsonValue::Array)
        {
            for (size_t i = 0; i < ips->items.size(); ++i)
            {
                if (ips->items[i].kind == tml::JsonValue::String)
                    m_self.ips.push_back(ips->items[i].text);
            }
        }
    }

    const tml::JsonValue* peers = root.Find("peers");
    if (peers != nullptr && peers->kind == tml::JsonValue::Array)
    {
        for (size_t i = 0; i < peers->items.size() && m_peers.size() < kMaxPeers; ++i)
        {
            const tml::JsonValue& pv = peers->items[i];
            if (pv.kind != tml::JsonValue::Object) continue;

            PeerDevice p;
            p.deviceId = pv.GetString("deviceId", std::string());
            if (p.deviceId.empty()) continue;
            p.name = pv.GetString("name", std::string());
            p.alias = pv.GetString("alias", std::string());
            p.platform = pv.GetString("platform", std::string());
            p.mac = pv.GetString("mac", std::string());
            p.port = static_cast<unsigned short>(pv.GetInt("port", 0));
            p.role = pv.GetString("role", "peer");
            p.state = pv.GetString("state", "offline");
            p.acceptsInbound = pv.GetBool("acceptsInbound", true);
            p.ephemeral = false;                   // 文件里的东西一律当持久条目
            p.lastSeen = pv.GetInt("lastSeen", 0);
            // 冷启动必须视为 0：磁盘上的旧值只用于诊断，绝不用来判"在线"
            p.lastReachableAt = 0;
            p.firstSeen = pv.GetInt("firstSeen", 0);
            p.apiVersion = pv.GetString("apiVersion", std::string());
            p.resumable = pv.GetBool("resumable", false);
            p.filteredNicCount = static_cast<int>(pv.GetInt("filteredNicCount", 0));
            p.paired = pv.GetBool("paired", false);
            p.convId = pv.GetString("convId", std::string());

            const tml::JsonValue* mh = pv.Find("macHistory");
            if (mh != nullptr && mh->kind == tml::JsonValue::Array)
            {
                for (size_t k = 0; k < mh->items.size() && p.macHistory.size() < kMaxMacHistory; ++k)
                {
                    if (mh->items[k].kind == tml::JsonValue::String)
                        p.macHistory.push_back(mh->items[k].text);
                }
            }

            const tml::JsonValue* ips = pv.Find("ips");
            if (ips != nullptr && ips->kind == tml::JsonValue::Array)
            {
                for (size_t k = 0; k < ips->items.size() && p.ips.size() < kMaxIpsPerPeer; ++k)
                {
                    const tml::JsonValue& iv = ips->items[k];
                    if (iv.kind != tml::JsonValue::Object) continue;
                    DeviceEndpoint ep;
                    ep.ip = iv.GetString("ip", std::string());
                    ep.lastOkAt = iv.GetInt("lastOkAt", 0);
                    if (!ep.ip.empty()) p.ips.push_back(ep);
                }
            }

            m_peers.push_back(p);
        }
    }

    // 会话级别名（legacy）：单独一段，不参与设备判活
    const tml::JsonValue* aliases = root.Find("aliases");
    if (aliases != nullptr && aliases->kind == tml::JsonValue::Object)
    {
        for (size_t i = 0; i < aliases->members.size(); ++i)
        {
            const tml::JsonValue& v = aliases->members[i].second;
            if (v.kind != tml::JsonValue::String || v.text.empty()) continue;
            m_aliases[aliases->members[i].first] = v.text;
        }
    }
    return true;
}

bool DeviceRegistry::LoadLocked()
{
    m_self = SelfDevice();
    m_peers.clear();
    m_ephemeral.clear();
    m_aliases.clear();

    std::string text;
    if (m_path.empty()) return false;
    if (!tml::ReadWholeFile(m_path, text)) return false;   // 首次运行：没有文件是正常情况
    if (text.empty()) return false;

    if (!FromJson(text))
    {
        // 坏文件：留证据、重新开始，**不静默当成空表**。
        const std::wstring bad = m_path + L".bad";
        ::MoveFileExW(m_path.c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

        // 身份是最不该丢的东西：格式坏了不代表身份也要换。
        // 只从坏文件里抢救 deviceId 这一个字段，其余全部重来。
        std::string salvaged;
        tml::JsonGetString(text, "deviceId", salvaged);

        m_self = SelfDevice();
        m_peers.clear();

        if (!salvaged.empty())
        {
            m_self.deviceId = salvaged;    // 身份保住，对端不会把它当成新设备
            AppendLogLine(m_logPath, "device table corrupted: " + tml::WideToUtf8(m_path) +
                                     " reason=parse; deviceId salvaged=" + salvaged);
            return false;
        }

        // 连 deviceId 都读不出来，只能重建 —— 这件事必须可见，不能静默
        m_regenReason = "parse";
        AppendLogLine(m_logPath, "device table corrupted: " + tml::WideToUtf8(m_path) +
                                 " reason=parse; deviceId NOT recoverable, will regenerate");
        return false;
    }
    return true;
}

bool DeviceRegistry::SaveLocked()
{
    if (!m_inited && m_self.deviceId.empty()) return false;
    if (m_path.empty()) return false;

    return tml::WriteWholeFileAtomic(m_path, ToJsonLocked());
}

namespace
{
    // 五态边界。**ONLINE_TTL < STALE_TTL 是不变式**：两者相等或倒置时
    // `unreachable` 恒为假（因为 lastReachableAt <= lastSeen 恒成立），
    // 设备会从"在线"直接跳"离线"，中间态静默消失，而所有单条状态断言仍全绿。
    constexpr long long kOnlineTtl = 30000;
    constexpr long long kStaleTtl  = 120000;
    static_assert(kOnlineTtl < kStaleTtl, "ONLINE_TTL must be < STALE_TTL");

    const long long kIpFreshMs = 5 * 60 * 1000;
}

long long DeviceRegistry::OnlineTtlMs() { return kOnlineTtl; }
long long DeviceRegistry::StaleTtlMs()  { return kStaleTtl; }

void DeviceRegistry::BumpEpoch()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_self.epoch = (m_self.epoch > 0 ? m_self.epoch : 1) + 1;
    SaveLocked();     // 必须落盘：只在内存里的话，重启后 epoch 回到 1
}

DeviceState DeviceRegistry::ComputeState(const PeerDevice& peer, long long lastReachableInProc, long long now){
    // 按序命中即停。reachable 用**进程内**的值：冷启动时它对所有设备都是 0，
    // 否则重启后 30 秒内一堆早就断开的设备会显示成"在线"——那是用户重启后的第一屏。
    const long long reachable = lastReachableInProc;

    if (reachable > 0 && (now - reachable) <= kOnlineTtl)
    {
        return peer.acceptsInbound ? DeviceState::Online : DeviceState::OnlineNoInbound;
    }

    if (now - peer.lastSeen <= kStaleTtl)
    {
        // ③④ 必须是一个 if/else：两个独立 if 会让同一设备同时命中两个状态，
        // 而"五值计数之和 == 设备总数"的断言会因此变成 2 个计数重复。
        // 两种"不可达"不能合并：unreachable 用户在本机重试有意义（刚还在，可能抖动），
        // discovered 重试没意义，要去对方那边看插件/防火墙。
        if (reachable > 0) return DeviceState::Unreachable;
        return DeviceState::Discovered;
    }
    return DeviceState::Offline;
}

std::string DeviceRegistry::DevicesJson(long long now, int limit, bool& outInited) const
{
    // 先在锁内拷一份快照，**出锁之后**再查 TransferHub 统计：
    // TransferHub 内部会回调 DeviceId()（取 registry 锁），持锁交叉会死锁。
    std::vector<PeerDevice> peers;
    std::string selfId;
    std::map<std::string, long long> reachableInProc;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        outInited = m_inited;
        if (!m_inited) return std::string("{}");
        selfId = m_self.deviceId;
        reachableInProc = m_reachableInProc;
        for (size_t i = 0; i < m_peers.size(); ++i)
        {
            if (m_peers[i].ephemeral) continue;
            peers.push_back(m_peers[i]);
        }
    }

    struct Row
    {
        std::string json;
        DeviceState  state = DeviceState::Offline;
        long long    lastSeen = 0;
        std::string  deviceId;
    };
    std::vector<Row> rows;
    rows.reserve(peers.size());

    const tml::NicFilterResult mine = tml::GetNicFilter();
    TransferHub& hub = TransferHub::Instance();

    for (size_t i = 0; i < peers.size(); ++i)
    {
        const PeerDevice& p = peers[i];

        long long reachable = 0;
        const std::map<std::string, long long>::const_iterator rit = reachableInProc.find(p.deviceId);
        if (rit != reachableInProc.end()) reachable = rit->second;

        const DeviceState state = ComputeState(p, reachable, now);

        // ips[]：先剔 TTL 过期项，再按 ①lastOkAt 未过期 ②同网段 ③原序 排好。
        // lastOkAt==0 表示"还没用过"，不能按 TTL 剔除——否则刚发现的设备 ips 会是空的。
        std::vector<DeviceEndpoint> ips;
        for (size_t k = 0; k < p.ips.size(); ++k)
        {
            if (p.ips[k].ip.empty()) continue;
            if (p.ips[k].lastOkAt > 0 && (now - p.ips[k].lastOkAt) > kIpFreshMs) continue;
            ips.push_back(p.ips[k]);
        }
        {
            std::vector<std::pair<int, size_t>> order;
            order.reserve(ips.size());
            for (size_t k = 0; k < ips.size(); ++k)
            {
                int score = 0;
                if (ips[k].lastOkAt > 0 && (now - ips[k].lastOkAt) <= kIpFreshMs) score += 4;
                for (size_t m = 0; m < mine.nics.size(); ++m)
                {
                    if (tml::IsSameSubnet(ips[k].ip, mine.nics[m].ip, mine.nics[m].prefixLen)) { score += 2; break; }
                }
                order.push_back(std::make_pair(score, k));
            }
            // 稳定：同分保持原顺序（ips[] 原序 = 第 ③ 条）
            std::stable_sort(order.begin(), order.end(),
                             [](const std::pair<int, size_t>& a, const std::pair<int, size_t>& b)
                             { return a.first > b.first; });
            std::vector<DeviceEndpoint> sorted;
            sorted.reserve(order.size());
            for (size_t k = 0; k < order.size(); ++k) sorted.push_back(ips[order[k].second]);
            ips.swap(sorted);
        }

        // 显示名：aliases[会话键] > 设备表 alias > name > deviceId 前 8 位
        std::string display = p.name;
        if (!p.alias.empty()) display = p.alias;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            const std::map<std::string, std::string>::const_iterator ai = m_aliases.find(p.deviceId);
            if (ai != m_aliases.end() && !ai->second.empty()) display = ai->second;
        }
        if (display.empty()) display = (p.deviceId.size() > 8) ? p.deviceId.substr(0, 8) : p.deviceId;

        std::string convId = p.convId;
        if (convId.empty()) convId = MakeConvId(selfId, p.deviceId);
        const TransferHub::ConvStat st = hub.ConvStatOf(convId);

        std::string json;
        json.reserve(512);
        json += "{\"deviceId\":\"" + tml::JsonEscape(p.deviceId) + "\"";
        json += ",\"displayName\":\"" + tml::JsonEscape(display) + "\"";
        json += ",\"name\":\"" + tml::JsonEscape(p.name) + "\"";
        json += ",\"alias\":\"" + tml::JsonEscape(p.alias) + "\"";
        json += ",\"platform\":\"" + tml::JsonEscape(p.platform) + "\"";
        json += ",\"mac\":\"" + tml::JsonEscape(p.mac) + "\"";
        json += ",\"port\":" + std::to_string(p.port);                 // 数字，不是字符串
        json += std::string(",\"state\":\"") + DeviceStateToString(state) + "\"";
        json += std::string(",\"acceptsInbound\":") + (p.acceptsInbound ? "true" : "false");
        json += ",\"lastSeen\":" + std::to_string(p.lastSeen);
        json += ",\"lastReachableAt\":" + std::to_string(p.lastReachableAt);   // 仅供展示
        json += ",\"ips\":[";
        for (size_t k = 0; k < ips.size(); ++k)
        {
            if (k > 0) json += ",";
            json += "{\"ip\":\"" + tml::JsonEscape(ips[k].ip) + "\",\"lastOkAt\":" +
                    std::to_string(ips[k].lastOkAt) + "}";
        }
        json += "]";
        json += ",\"convId\":\"" + tml::JsonEscape(convId) + "\"";
        json += ",\"unread\":" + std::to_string(st.unread);
        json += ",\"histRev\":" + std::to_string(st.histRev);
        json += ",\"apiVersion\":\"" + tml::JsonEscape(p.apiVersion) + "\"";
        json += ",\"filteredNicCount\":" + std::to_string(p.filteredNicCount);   // 对端自报
        json += "}";

        Row row;
        row.json = json;
        row.state = state;
        row.lastSeen = p.lastSeen;
        row.deviceId = p.deviceId;
        rows.push_back(row);
    }

    // 排序：state 优先级 → lastSeen 倒序 → deviceId 字典序（稳定兜底）
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b)
    {
        const int ra = DeviceStateRank(a.state);
        const int rb = DeviceStateRank(b.state);
        if (ra != rb) return ra < rb;
        if (a.lastSeen != b.lastSeen) return a.lastSeen > b.lastSeen;
        return a.deviceId < b.deviceId;
    });

    const int total = static_cast<int>(rows.size());
    const size_t keep = (limit > 0 && static_cast<size_t>(limit) < rows.size())
                        ? static_cast<size_t>(limit) : rows.size();
    const bool truncated = (keep < rows.size());

    std::string out;
    out.reserve(1024 + keep * 400);
    out += "{\"ok\":true,\"now\":" + std::to_string(now);
    out += ",\"total\":" + std::to_string(total);
    out += std::string(",\"truncated\":") + (truncated ? "true" : "false");
    out += ",\"devices\":[";
    for (size_t i = 0; i < keep; ++i)
    {
        if (i > 0) out += ",";
        out += rows[i].json;
    }
    out += "]}";
    return out;
}

std::string DeviceRegistry::PeersJson() const
{
    std::lock_guard<std::mutex> lk(m_mutex);

    std::string out = "[";
    size_t emitted = 0;
    for (size_t i = 0; i < m_peers.size(); ++i)
    {
        const PeerDevice& p = m_peers[i];
        if (p.ephemeral) continue;
        if (emitted > 0) out += ",";
        out += "{\"deviceId\":\"" + tml::JsonEscape(p.deviceId) + "\"";
        out += ",\"name\":\"" + tml::JsonEscape(p.name) + "\"";
        out += ",\"alias\":\"" + tml::JsonEscape(p.alias) + "\"";
        out += ",\"displayName\":\"" + tml::JsonEscape(p.DisplayName()) + "\"";
        out += ",\"platform\":\"" + tml::JsonEscape(p.platform) + "\"";
        out += ",\"mac\":\"" + tml::JsonEscape(p.mac) + "\"";
        out += ",\"port\":" + std::to_string(p.port);
        out += ",\"state\":\"" + tml::JsonEscape(p.state) + "\"";
        out += std::string(",\"acceptsInbound\":") + (p.acceptsInbound ? "true" : "false");
        out += ",\"lastSeen\":" + std::to_string(p.lastSeen);
        out += ",\"lastReachableAt\":" + std::to_string(p.lastReachableAt);
        out += ",\"apiVersion\":\"" + tml::JsonEscape(p.apiVersion) + "\"";
        out += ",\"filteredNicCount\":" + std::to_string(p.filteredNicCount);
        out += ",\"convId\":\"" + tml::JsonEscape(p.convId) + "\"";
        out += ",\"ips\":[";
        for (size_t k = 0; k < p.ips.size(); ++k)
        {
            if (k > 0) out += ",";
            out += "{\"ip\":\"" + tml::JsonEscape(p.ips[k].ip) + "\",\"lastOkAt\":" +
                   std::to_string(p.ips[k].lastOkAt) + "}";
        }
        out += "]}";
        ++emitted;
    }
    out += "]";
    return out;
}
