#include "TransferHub.h"
#include "HostBridge.h"
#include "Common.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <random>
#include <sstream>

namespace
{
    bool ReadTextFile(const std::wstring& path, std::string& out)
    {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || fp == nullptr) return false;
        ::fseek(fp, 0, SEEK_END);
        const long n = ::ftell(fp);
        ::fseek(fp, 0, SEEK_SET);
        out.resize(n > 0 ? static_cast<size_t>(n) : 0);
        if (n > 0) ::fread(&out[0], 1, static_cast<size_t>(n), fp);
        ::fclose(fp);
        return true;
    }

    bool WriteTextFile(const std::wstring& path, const std::string& text)
    {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || fp == nullptr) return false;
        const bool ok = ::fwrite(text.data(), 1, text.size(), fp) == text.size();
        ::fclose(fp);
        return ok;
    }

    // 未读口径（唯一实现，三处统计都走它）：
    //   只算"收到的"、"还标着未读的"、且**不是系统消息**。
    // 自己发的（dir=out）不可能是未读；status=failed 只出现在出站消息上，天然被排除。
    bool IsUnreadCountable(const ShareMessage& m)
    {
        return m.unread && m.dir == "in" && m.kind != "system";
    }
}

TransferHub& TransferHub::Instance()
{
    // 故意用 new 且不释放：避免 DLL 卸载时后台线程访问已析构的对象
    static TransferHub* inst = new TransferHub();
    return *inst;
}

void TransferHub::Resume()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_stopping = false;
}

void TransferHub::Init()
{
    // 无参版本保持旧行为；实际启动走 InstanceLock 给出的 ini / 数据目录
    Init(tml::JoinPath(tml::GetModuleDir(), L"lanshare.ini"), tml::GetModuleDir());
}

void TransferHub::Init(const std::wstring& iniPath, const std::wstring& dataDir, bool dataDirExplicit)
{
    if (m_inited) return;

    m_iniPath = iniPath.empty() ? tml::JoinPath(tml::GetModuleDir(), L"lanshare.ini") : iniPath;
    m_dataDir = dataDir.empty() ? tml::GetModuleDir() : dataDir;

    // 无参 Init 路径不经过 InstanceLock，这里也要挡一次 BOM（见 Common::StripIniBom）
    tml::StripIniBom(m_iniPath);
    tml::NormalizeIniToUnicode(m_iniPath);   // 中文路径必须走 UTF-16，见 Common

    wchar_t buf[1024] = { 0 };
    ::GetPrivateProfileStringW(L"server", L"port", L"8765", buf, 1024, m_iniPath.c_str());
    int port = _wtoi(buf);
    if (port > 0 && port < 65536) m_port = static_cast<unsigned short>(port);

    ::GetPrivateProfileStringW(L"server", L"dir", L"", buf, 1024, m_iniPath.c_str());
    if (buf[0] != L'\0')
    {
        m_shareDir = buf;
    }
    else if (dataDirExplicit)
    {
        // TMLS_DATA_DIR 显式指定（测试用法）：默认保存目录也跟着走，
        // 否则两个测试实例会把收到的文件落进同一个文件夹，
        // 看起来像"收到的文件莫名其妙多了一份"。
        //
        // 注意判据是"**显式指定**"，不是"dataDir != 模块目录"——
        // T07b 之后默认数据目录也变成了 %LOCALAPPDATA%\...，
        // 用旧判据会把附件目录也挪到那儿去（附件是给用户的，不该被程序决定）。
        m_shareDir = m_dataDir;
    }
    else
    {
        m_shareDir = tml::DefaultShareDir();
    }

    // 首次运行时生成一份配置文件，方便用户改端口/目录
    if (::GetFileAttributesW(m_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        wchar_t portStr[16] = { 0 };
        _itow_s(static_cast<int>(m_port), portStr, 16, 10);
        ::WritePrivateProfileStringW(L"server", L"port", portStr, m_iniPath.c_str());
        ::WritePrivateProfileStringW(L"server", L"dir", L"", m_iniPath.c_str());
    }

    // 历史保留天数：0 或空 = 永不自动删除
    ::GetPrivateProfileStringW(L"history", L"days", L"0", buf, 1024, m_iniPath.c_str());
    m_historyDays = _wtoi(buf);
    if (m_historyDays < 0) m_historyDays = 0;

    // 文件直接存到保存目录，不再建 recv/send/tmp 子文件夹
    tml::EnsureDir(m_shareDir);

    // ip 与 mac 必须来自**同一次**网卡枚举：两次调用之间网卡热插拔，就会拿到
    // "真网卡 IP + 虚拟网卡 MAC"的拼装组合，对端会误报"这台设备换了网卡"。
    {
        const tml::LocalIdentity ident = tml::GetLocalIdentity();
        m_ip = ident.ip;
        m_mac = ident.mac;
    }

    m_deviceName = tml::WideToUtf8(tml::GetHostName());
    m_historyPath = tml::JoinPath(m_dataDir, L"lanshare_history.jsonl");
    m_conflictPath = tml::JoinPath(m_dataDir, L"lanshare_conflicts.log");
    m_convPath = tml::JoinPath(m_dataDir, L"lanshare_convs.json");
    m_transfersPath = tml::JoinPath(m_dataDir, L"lanshare_transfers.jsonl");

    LoadHistory();
    LoadTransfers();
    LoadConvRevs();
    if (m_historyDays > 0) PruneHistory(m_historyDays);
    m_inited = true;
}

void TransferHub::Shutdown()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_stopping = true;
    }
    m_cv.notify_all();
}

unsigned short TransferHub::Port() const
{
    // 实际端口优先：端口递进后 m_port 仍是最初的"意图端口"，
    // 用它上报会让对端连到错误端口上（表现为"发现到了但连不上"）
    const unsigned short effective = m_runtimePort.load();
    return effective != 0 ? effective : m_port;
}

void TransferHub::SetRuntimePort(unsigned short port)
{
    m_runtimePort.store(port);
}
std::wstring TransferHub::ShareDir() { return m_shareDir; }
std::string TransferHub::LocalIp() { return m_ip; }
std::string TransferHub::DeviceName() { return m_deviceName; }
std::wstring TransferHub::IniPath() { return m_iniPath; }
std::wstring TransferHub::DataDir() { return m_dataDir; }

// 协议代次。/api/hello 与 UDP announce 的 v 字段必须同源，否则对端无法判断
// 该用哪套字段解析（docs/05 里 announce 用的是 v:2）。
const char* TransferHub::ApiVersion() { return "2"; }

std::string TransferHub::MacAddress()
{
    // Init 时已经和 m_ip 从同一次枚举里取好了；这里只读缓存。
    // 网卡变化属于"重启或重新初始化"才处理的事，不能每次请求各查一遍。
    return m_mac;
}

void TransferHub::AttachRegistry(DeviceRegistry* registry)
{
    m_registry = registry;
}

DeviceRegistry& TransferHub::Registry()
{
    // 刻意不在这里惰性 Init：Init 需要 dataDir + 实例名两个参数，猜错一个就会
    // 生成"第二个身份文件"，那比暂时没有设备表更糟。Plugin 会在任何 HTTP 请求
    // 之前完成 Init，这里只负责把实例给出去。
    return (m_registry != nullptr) ? *m_registry : DeviceRegistry::Instance();
}

std::string TransferHub::DeviceId()
{
    // T02：deviceId 一律来自设备表（首次运行随机生成、写进 lanshare_devices.json，
    // 之后只读）。**不由 MAC 派生**：换网卡 / 随机 MAC / USB 网卡热插拔都会让 ID 变，
    // 对端就把它当成一台新设备；克隆镜像还会共用 MAC 造成 ID 碰撞。
    std::string id = Registry().DeviceId();
    if (!id.empty())
    {
        m_deviceId = id;
        return id;
    }

    // 兜底：设备表读写全失败（只读介质 / 磁盘满 / dataDir 不可写）。
    // 这里**仍然不用 MAC**——用一个进程内稳定的哈希，至少不会因为换网卡而在
    // 运行期变成"另一台设备"。真正的修复是让设备表可写，日志里会体现初始化失败。
    if (!m_deviceId.empty()) return m_deviceId;

    const std::string host = tml::WideToUtf8(tml::GetHostName());
    unsigned int h = 2166136261u;
    for (size_t i = 0; i < host.size(); ++i)
    {
        h ^= static_cast<unsigned char>(host[i]);
        h *= 16777619u;
    }
    char buf[16] = { 0 };
    sprintf_s(buf, "%08x", h);
    m_deviceId = std::string("pc-") + buf;
    return m_deviceId;
}
std::string TransferHub::SelfDeviceId()
{
    return DeviceId();
}

std::string TransferHub::MakeMsgId(const std::string& deviceId, long long epoch, long long seq)
{
    return deviceId + ":" + std::to_string(epoch) + ":" + std::to_string(seq);
}

std::string TransferHub::MakeLegacyKey(const std::string& dev)
{
    // dev 缺失时统一并到同一个 legacy:unknown，否则每来一个不带名字的老端
    // 就会堆出一串无名设备会话。
    // **幂等**：已经带前缀的直接返回。不幂等的话，调用链上多走一次就会变成
    // "legacy:legacy:xxx"，同一个对端在两个地方算出两个不同的键。
    if (dev.rfind("legacy:", 0) == 0) return dev;
    return dev.empty() ? std::string("legacy:unknown") : ("legacy:" + dev);
}

std::string TransferHub::ResolvePeerDeviceId(const std::string& nameOrId)
{
    if (nameOrId.empty()) return std::string("unknown:anonymous");

    DeviceRegistry& reg = Registry();
    PeerDevice peer;
    if (reg.FindPeer(nameOrId, peer)) return peer.deviceId;

    // 老客户端只报设备名（`/api/messages?dev=<名字>`）。按名字/别名找一次。
    const std::vector<PeerDevice> peers = reg.Peers();
    for (size_t i = 0; i < peers.size(); ++i)
    {
        if (peers[i].name == nameOrId || peers[i].alias == nameOrId) return peers[i].deviceId;
    }

    // 还没登记过：给一个稳定占位，**不返回空串**。
    // 空串会让 convId 退化成"自己和自己的会话"，两条不同来源的消息会串到一起。
    return std::string("unknown:") + nameOrId;
}

bool TransferHub::AssignIdentityLocked(ShareMessage& m, const std::string& peerDeviceId,
                                       const std::string& msgIdIn, bool outgoing)
{
    const std::string self = DeviceId();
    // 对端键的唯一解析点：AddFile 的目录、这里的 from/to/convId 都走它
    const std::string peer = PeerKeyLocked(peerDeviceId, m.dev);

    if (m.peerTs == 0) m.peerTs = m.time;
    if (m.peerTs == 0) m.peerTs = tml::NowMs();
    m.recvTs = tml::NowMs();

    // 从 "<dev>:<epoch>:<seq>" 里取出后两段；重发时沿用原 msgId，序号不能重算
    auto parseTail = [&m](const std::string& text)
    {
        const size_t last = text.find_last_of(':');
        if (last == std::string::npos) return;
        const size_t prev = text.find_last_of(':', last - 1);
        if (prev == std::string::npos || prev + 1 >= last) return;
        m.epoch = _strtoi64(text.substr(prev + 1, last - prev - 1).c_str(), nullptr, 10);
        m.seq = _strtoi64(text.substr(last + 1).c_str(), nullptr, 10);
    };

    if (outgoing)
    {
        m.from = self;
        m.to = peer;
        m.status = "sent";
        if (!msgIdIn.empty())
        {
            m.msgId = msgIdIn;
            parseTail(msgIdIn);
        }
        else
        {
            DeviceRegistry& reg = Registry();
            m.epoch = reg.Epoch();
            m.seq = reg.NextSeq();              // 全局单调、按 epoch 递增，分配即落盘
            m.msgId = MakeMsgId(self, m.epoch, m.seq);
        }
    }
    else
    {
        m.from = peer;
        m.to = self;
        m.status.clear();
        if (!msgIdIn.empty())
        {
            m.msgId = msgIdIn;
            parseTail(msgIdIn);
        }
        else
        {
            // 老客户端不带 msgId：用全局 id 推导。
            // 形态 "legacy:" 开头，与真实 msgId（"<deviceId>:<epoch>:<seq>"）不可能碰撞。
            // **注意 peer 本身可能已经是 "legacy:xxx"**（legacy 会话的键就长这样），
            // 无脑再拼一次会得到 "legacy:legacy:xxx:0:1" —— 与历史行的推导规则不一致，
            // 同一条消息在"入库"和"重启恢复"两条路上会得到两个不同的 msgId。
            const std::string marker = (peer.rfind("legacy:", 0) == 0) ? std::string() : std::string("legacy:");
            m.epoch = 0;
            m.seq = m.id;
            m.msgId = marker + peer + ":0:" + std::to_string(m.id);
        }
    }
    m.convId = MakeConvId(m.from, m.to);

    // 幂等：同一个 msgId 只留一条，保留先到的
    const std::map<std::string, long long>::const_iterator it = m_msgIdToId.find(m.msgId);
    if (it != m_msgIdToId.end())
    {
        for (size_t i = 0; i < m_messages.size(); ++i)
        {
            if (m_messages[i].id != it->second) continue;
            const ShareMessage& kept = m_messages[i];
            if (kept.text != m.text || kept.fileId != m.fileId || kept.fileName != m.fileName)
            {
                LogConflictLocked(m.msgId, kept, m);   // 记日志，不抛异常
            }
            break;
        }
        m.id = it->second;      // 返回已有的 id，调用方据此知道"没新建"
        return false;
    }
    return true;
}

void TransferHub::LogConflictLocked(const std::string& msgId, const ShareMessage& kept,
                                    const ShareMessage& dropped)
{
    if (m_conflictPath.empty()) return;

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, m_conflictPath.c_str(), L"ab") != 0 || fp == nullptr) return;

    std::ostringstream os;
    os << "{\"ts\":" << tml::NowMs()
       << ",\"msgId\":\"" << tml::JsonEscape(msgId) << "\""
       << ",\"keptId\":" << kept.id
       << ",\"keptText\":\"" << tml::JsonEscape(kept.text.substr(0, 200)) << "\""
       << ",\"keptFile\":\"" << tml::JsonEscape(kept.fileName) << "\""
       << ",\"droppedText\":\"" << tml::JsonEscape(dropped.text.substr(0, 200)) << "\""
       << ",\"droppedFile\":\"" << tml::JsonEscape(dropped.fileName) << "\"}\n";
    const std::string line = os.str();
    fwrite(line.data(), 1, line.size(), fp);
    fclose(fp);
}

void TransferHub::IndexMessageLocked(const ShareMessage& m)
{
    if (!m.msgId.empty()) m_msgIdToId[m.msgId] = m.id;
    // 消息一出现就把 conv 登记进来：清空后会话仍可见才有据可依，
    // 且从未聊过的设备不会凭空出现在列表里（没有消息就永远不会登记）。
    // 名字取**此刻**的 m.dev：这是它最后一次能确定的时候。
    EnsureConvLocked(m.convId, (m.dir == "out") ? m.to : m.from, m.dev);
}

void TransferHub::TrimLocked()
{
    // 只保留最近 500 条；被挤出去的必须同时从去重索引里删掉，
    // 否则索引会越长越大，而且会指向一条已经不在列表里的 id。
    while (m_messages.size() > 500)
    {
        const ShareMessage& gone = m_messages.front();
        const std::map<std::string, long long>::iterator it = m_msgIdToId.find(gone.msgId);
        if (it != m_msgIdToId.end() && it->second == gone.id) m_msgIdToId.erase(it);
        m_messages.pop_front();
    }
}

std::wstring TransferHub::HistoryPath() { return m_historyPath; }



int TransferHub::HistoryDays()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_historyDays;
}

void TransferHub::SetHistoryDays(int days)
{
    if (days < 0) days = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_historyDays = days;
    }
    wchar_t buf[16] = { 0 };
    _itow_s(days, buf, 16, 10);
    ::WritePrivateProfileStringW(L"history", L"days", buf, m_iniPath.c_str());

    if (days > 0) PruneHistory(days);
}

int TransferHub::PruneHistory(int days)
{
    if (days <= 0) return 0;

    const long long cutoff = tml::NowMs() - static_cast<long long>(days) * 24LL * 3600LL * 1000LL;
    int removed = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const size_t before = m_messages.size();
        std::deque<ShareMessage> keep;
        for (const auto& m : m_messages)
        {
            if (m.time >= cutoff) keep.push_back(m);
        }
        removed = static_cast<int>(before - keep.size());
        m_messages.swap(keep);
        // 被清掉的消息，同时把 fileId → 路径 的映射也删除（文件本身留在磁盘上不动）
        for (auto it = m_filePaths.begin(); it != m_filePaths.end();)
        {
            bool stillUsed = false;
            for (const auto& m : m_messages)
            {
                if (m.fileId == it->first) { stillUsed = true; break; }
            }
            if (stillUsed) ++it; else it = m_filePaths.erase(it);
        }
        if (removed > 0) RewriteHistory();
    }
    return removed;
}

void TransferHub::ClearHistory()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        // 清全部：**所有已知会话各 +1**，且 epoch 只 +1 一次（由调用方
        // DeviceRegistry 负责 bump epoch，避免每 conv 各 +1 造成多次纪元跳变）
        for (std::map<std::string, ConvMeta>::iterator it = m_convs.begin(); it != m_convs.end(); ++it)
            ++it->second.rev;
        SaveConvRevsLocked();
        m_messages.clear();
        m_filePaths.clear();
        m_msgIdToId.clear();     // 索引必须跟着清，否则去重会指向已经不在列表里的 id
        RewriteHistory();   // 文件里一行不剩（等价于清空）
    }
    m_cv.notify_all();

    // 清全部 → epoch **只 +1 一次**（不是每个 conv 各 +1：那会让对端看到多次
    // 纪元跳变、反复重置水位）。放在锁外调，避免同时持有两把锁。
    Registry().BumpEpoch();
}

std::string TransferHub::ConvIdOfPeer(const std::string& peerDeviceId) const
{
    if (peerDeviceId.empty()) return std::string();
    std::lock_guard<std::mutex> lk(m_mutex);
    for (std::map<std::string, ConvMeta>::const_iterator it = m_convs.begin(); it != m_convs.end(); ++it)
    {
        if (it->second.peerId == peerDeviceId) return it->first;
    }
    return std::string();
}

std::string TransferHub::PeerOfConv(const std::string& convId) const
{
    if (convId.empty()) return std::string();
    std::lock_guard<std::mutex> lk(m_mutex);
    std::map<std::string, ConvMeta>::const_iterator it = m_convs.find(convId);
    return it == m_convs.end() ? std::string() : it->second.peerId;
}

int TransferHub::ClearConversation(const std::string& convId)
{
    if (convId.empty()) return 0;

    int removed = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);

        std::deque<ShareMessage> keep;
        for (const auto& m : m_messages)
        {
            if (m.convId != convId)
            {
                keep.push_back(m);
                continue;
            }
            const std::map<std::string, long long>::iterator it = m_msgIdToId.find(m.msgId);
            if (it != m_msgIdToId.end() && it->second == m.id) m_msgIdToId.erase(it);
            ++removed;
        }
        if (removed == 0) return 0;

        m_messages.swap(keep);
        ++m_convs[convId].rev;        // 该会话历史变了：客户端据此全量重拉
        SaveConvRevsLocked();          // 与消息一起落盘：重启后接着涨，不回 0
        RewriteHistory();
    }
    m_cv.notify_all();

    // 清空 → 换纪次（epoch +1）：清空后 seq 归零，而对端水位停在旧值，
    // 新消息会被永久跳过且不报错。epoch 必须一起落盘，否则重启后回到 1，
    // 对端会当成旧纪元继续用老水位。
    // 放在锁外调：registry 有自己的锁，不必要求同时持有两把。
    if (removed > 0) Registry().BumpEpoch();
    return removed;
}

// ---------------- 传输进度（供网页显示大小/百分比/速度） ----------------

void TransferHub::SaveTransfersLocked()
{
    if (m_transfersPath.empty()) return;
    std::string out;
    for (const auto& kv : m_transfers)
    {
        const TransferInfo& t = kv.second;
        out += "{\"id\":" + std::to_string(kv.first)
            + ",\"name\":\"" + tml::JsonEscape(t.name) + "\""
            + ",\"dir\":\"" + tml::JsonEscape(t.dir) + "\""
            + ",\"total\":" + std::to_string(t.total)
            + ",\"received\":" + std::to_string(t.received)
            + ",\"status\":\"" + tml::JsonEscape(t.status) + "\""
            + ",\"peer\":\"" + tml::JsonEscape(t.peer) + "\""
            + ",\"xid\":\"" + tml::JsonEscape(t.xid) + "\""
            + ",\"started\":" + std::to_string(t.started)
            + ",\"ended\":" + std::to_string(t.ended)
            + ",\"error\":\"" + tml::JsonEscape(t.error) + "\"}\n";
    }
    if (!WriteTextFile(m_transfersPath, out))
    {
        tml::AppendLogLine(tml::JoinPath(m_dataDir, L"lanshare.log"), "save transfers failed");
    }
}

void TransferHub::LoadTransfers()
{
    if (m_transfersPath.empty()) return;
    std::string text;
    if (!ReadTextFile(m_transfersPath, text) || text.empty()) return;

    std::lock_guard<std::mutex> lk(m_trMutex);
    m_transfers.clear();
    size_t pos = 0;
    long long maxId = 0;
    while (pos < text.size())
    {
        const size_t nl = text.find('\n', pos);
        const std::string line = text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
        pos = (nl == std::string::npos) ? text.size() : nl + 1;
        if (line.empty()) continue;
        tml::JsonValue v;
        if (!tml::JsonParse(line, v) || v.kind != tml::JsonValue::Object) continue;
        const long long id = v.GetInt("id", 0);
        if (id <= 0) continue;
        TransferInfo t;
        t.name = v.GetString("name", std::string());
        t.dir = v.GetString("dir", "in");
        t.total = v.GetInt("total", 0);
        t.received = v.GetInt("received", 0);
        t.status = v.GetString("status", "done");
        t.peer = v.GetString("peer", std::string());
        t.xid = v.GetString("xid", std::string());
        t.started = v.GetInt("started", 0);
        t.ended = v.GetInt("ended", 0);
        t.error = v.GetString("error", std::string());
        if (t.status == "active")
        {
            t.status = "failed";
            t.error = "interrupted";
            t.ended = tml::NowMs();
        }
        m_transfers[id] = t;
        if (id > maxId) maxId = id;
    }
    m_nextTransferId = maxId + 1;
}

long long TransferHub::BeginTransfer(const std::string& name, long long total, const std::string& dir,
                                     const std::string& peer, const std::string& xid)
{
    std::lock_guard<std::mutex> lk(m_trMutex);
    // 同一个 xid = 同一次传输的后续分片：**复用同一条记录**。
    // 以前每片都新建一条，界面上一个文件会变成 10 行（而且都把 received 当"已完成"显示）。
    if (!xid.empty())
    {
        for (auto& kv : m_transfers)
        {
            if (kv.second.xid == xid && kv.second.status != "cancelled")
            {
                kv.second.name = name;
                kv.second.dir = dir;
                if (total > 0) kv.second.total = total;
                if (!peer.empty()) kv.second.peer = peer;
                return kv.first;
            }
        }
    }
    const long long id = m_nextTransferId++;
    m_nextTransferId = id + 1;
    TransferInfo info;
    info.name = name;
    info.dir = dir;
    info.total = total;
    info.received = 0;
    info.started = tml::NowMs();
    info.status = "active";
    info.peer = peer;
    info.xid = xid;
    m_transfers[id] = info;
    SaveTransfersLocked();
    return id;
}

void TransferHub::UpdateTransfer(long long id, long long received)
{
    std::lock_guard<std::mutex> lk(m_trMutex);
    auto it = m_transfers.find(id);
    if (it != m_transfers.end() && it->second.status == "active")
    {
        it->second.received = received;
    }
}

void TransferHub::EndTransfer(long long id, bool ok, const std::string& error)
{
    std::lock_guard<std::mutex> lk(m_trMutex);
    auto it = m_transfers.find(id);
    if (it == m_transfers.end()) return;
    // 已取消的不再被"失败/完成"覆盖
    if (it->second.status == "cancelled") return;
    it->second.status = ok ? "done" : "failed";
    it->second.error = error;
    it->second.ended = tml::NowMs();
    if (ok && it->second.total > 0) it->second.received = it->second.total;
    SaveTransfersLocked();
}

bool TransferHub::CancelTransfer(long long id)
{
    std::lock_guard<std::mutex> lk(m_trMutex);
    auto it = m_transfers.find(id);
    if (it == m_transfers.end() || it->second.status != "active") return false;
    it->second.status = "cancelled";
    it->second.ended = tml::NowMs();
    SaveTransfersLocked();
    return true;
}

bool TransferHub::RemoveTransfer(long long id)
{
    std::lock_guard<std::mutex> lk(m_trMutex);
    auto it = m_transfers.find(id);
    if (it == m_transfers.end()) return false;
    it->second.status = "cancelled";   // 在传的先让发送/接收循环停
    m_transfers.erase(it);
    SaveTransfersLocked();
    return true;
}

bool TransferHub::IsTransferCancelled(const std::string& xid)
{
    if (xid.empty()) return false;
    std::lock_guard<std::mutex> lk(m_trMutex);
    for (const auto& kv : m_transfers)
    {
        if (kv.second.xid == xid) return kv.second.status == "cancelled";
    }
    // 记录被删掉（RemoveTransfer）也算取消：否则收片循环会一直写那条已经没人看的记录
    return false;
}

int TransferHub::DropTransfersForPeer(const std::string& peerId)
{
    if (peerId.empty()) return 0;
    std::lock_guard<std::mutex> lk(m_trMutex);
    int n = 0;
    for (auto it = m_transfers.begin(); it != m_transfers.end(); )
    {
        if (it->second.peer == peerId) { it = m_transfers.erase(it); ++n; }
        else ++it;
    }
    if (n > 0) SaveTransfersLocked();
    return n;
}

int TransferHub::DropAllTransfers()
{
    std::lock_guard<std::mutex> lk(m_trMutex);
    const int n = static_cast<int>(m_transfers.size());
    m_transfers.clear();
    if (n > 0) SaveTransfersLocked();
    return n;
}

std::string TransferHub::TransfersJson()
{
    std::lock_guard<std::mutex> lk(m_trMutex);
    const long long now = tml::NowMs();

    // 新的在上（最近一次传输置顶）
    std::vector<std::pair<long long, const TransferInfo*>> ordered;
    ordered.reserve(m_transfers.size());
    for (const auto& kv : m_transfers) ordered.push_back(std::make_pair(kv.first, &kv.second));
    std::sort(ordered.begin(), ordered.end(),
              [](const std::pair<long long, const TransferInfo*>& a,
                 const std::pair<long long, const TransferInfo*>& b)
              { return a.second->started > b.second->started; });

    std::ostringstream os;
    os << "{\"ok\":true,\"items\":[";
    bool first = true;
    int emitted = 0;
    for (const auto& e : ordered)
    {
        if (emitted >= 200) break;   // 只保护内存/带宽，不影响磁盘文件
        const TransferInfo& t = *e.second;
        const long long elapsed = (t.status == "active" ? now : (t.ended > 0 ? t.ended : now)) - t.started;
        const long long speed = (t.status == "active" && elapsed > 200 && t.received > 0)
                              ? (t.received * 1000LL / elapsed) : 0;
        // 终态给**平均速度**（整趟 total / 耗时），界面上"传完显示 0"是错的
        const long long avg = (t.status != "active" && elapsed > 200 && t.total > 0)
                              ? (t.total * 1000LL / elapsed) : 0;
        if (!first) os << ",";
        first = false;
        ++emitted;
        os << "{\"id\":" << e.first
           << ",\"name\":\"" << tml::JsonEscape(t.name) << "\""
           << ",\"dir\":\"" << tml::JsonEscape(t.dir) << "\""
           << ",\"total\":" << t.total
           << ",\"received\":" << t.received
           << ",\"status\":\"" << tml::JsonEscape(t.status) << "\""
           << ",\"peer\":\"" << tml::JsonEscape(t.peer) << "\""
           << ",\"xid\":\"" << tml::JsonEscape(t.xid) << "\""
           << ",\"speed\":" << speed
           << ",\"avg\":" << avg
           << ",\"started\":" << t.started
           << ",\"ended\":" << t.ended << "}";
    }
    os << "]}";
    return os.str();
}

void TransferHub::SetSaveDir(const std::wstring& dir)
{
    if (dir.empty()) return;
    ::WritePrivateProfileStringW(L"server", L"dir", dir.c_str(), m_iniPath.c_str());
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_shareDir = dir;
    }
    tml::EnsureDir(dir);
}

std::string TransferHub::PeerName()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_peerName;
}

void TransferHub::SetPeerName(const std::string& name)
{
    if (name.empty()) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    m_peerName = name;
}

// ---------------- 历史记录落盘 ----------------

void TransferHub::AppendHistory(const ShareMessage& m)
{
    if (m_historyPath.empty()) return;

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, m_historyPath.c_str(), L"ab") != 0 || fp == nullptr) return;

    const std::string line = ToJson(m) + "\n";
    fwrite(line.data(), 1, line.size(), fp);
    fclose(fp);
}

void TransferHub::RewriteHistory()
{
    if (m_historyPath.empty()) return;

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, m_historyPath.c_str(), L"wb") != 0 || fp == nullptr) return;

    for (const auto& m : m_messages)
    {
        const std::string line = ToJson(m) + "\n";
        fwrite(line.data(), 1, line.size(), fp);
    }
    fclose(fp);
}

bool TransferHub::ParseHistoryLine(const std::string& line, ShareMessage& out)
{
    if (line.find("\"id\"") == std::string::npos) return false;

    long long id = 0;
    if (!tml::JsonGetInt(line, "id", id)) return false;

    out = ShareMessage();
    out.id = id;
    tml::JsonGetString(line, "dir", out.dir);
    tml::JsonGetString(line, "kind", out.kind);
    tml::JsonGetString(line, "text", out.text);
    tml::JsonGetString(line, "fileId", out.fileId);
    tml::JsonGetString(line, "fileName", out.fileName);
    tml::JsonGetString(line, "fileRel", out.fileRel);
    tml::JsonGetString(line, "mime", out.mime);
    tml::JsonGetString(line, "dev", out.dev);
    tml::JsonGetInt(line, "fileSize", out.fileSize);
    tml::JsonGetInt(line, "time", out.time);
    out.unread = false;   // 历史消息一律算已读，避免重启后未读数爆表
    if (out.dir != "out") out.dir = "in";

    // T03 字段。老历史行没有它们：**只读推导，绝不回写**。
    tml::JsonGetString(line, "msgId", out.msgId);
    tml::JsonGetString(line, "convId", out.convId);
    tml::JsonGetString(line, "from", out.from);
    tml::JsonGetString(line, "to", out.to);
    tml::JsonGetString(line, "status", out.status);
    tml::JsonGetInt(line, "epoch", out.epoch);
    tml::JsonGetInt(line, "seq", out.seq);
    tml::JsonGetInt(line, "peerTs", out.peerTs);
    tml::JsonGetInt(line, "recvTs", out.recvTs);
    if (out.peerTs == 0) out.peerTs = out.time;
    if (out.recvTs == 0) out.recvTs = out.time;
    return true;
}

void TransferHub::LoadConvRevs()
{
    if (m_convPath.empty()) return;
    std::string text;
    if (!tml::ReadWholeFile(m_convPath, text) || text.empty()) return;

    tml::JsonValue root;
    if (!tml::JsonParse(text, root) || root.kind != tml::JsonValue::Object) return;
    for (size_t i = 0; i < root.members.size(); ++i)
    {
        const tml::JsonValue& v = root.members[i].second;
        ConvMeta meta;
        if (v.kind == tml::JsonValue::Number)
        {
            // 老结构 {"c-x": 1}：只有 rev。**绝不能因为升级把已有 rev 丢掉**——
            // rev 一掉，客户端"只认变大"的判断就永久失效，表现为再也不重拉。
            if (v.number < 0) continue;
            meta.rev = static_cast<int>(v.number);
        }
        else if (v.kind == tml::JsonValue::Object)
        {
            meta.peerId = v.GetString("peerDeviceId", std::string());
            meta.lastKnownName = v.GetString("lastKnownName", std::string());
            meta.rev = static_cast<int>(v.GetInt("rev", 0));
            if (meta.rev < 0) meta.rev = 0;
        }
        else
        {
            continue;
        }
        m_convs[root.members[i].first] = meta;
    }
}

void TransferHub::SaveConvRevsLocked()
{
    if (m_convPath.empty()) return;
    std::string out = "{";
    bool first = true;
    for (std::map<std::string, ConvMeta>::const_iterator it = m_convs.begin(); it != m_convs.end(); ++it)
    {
        if (!first) out += ",";
        first = false;
        out += "\"" + tml::JsonEscape(it->first) + "\":{";
        out += "\"peerDeviceId\":\"" + tml::JsonEscape(it->second.peerId) + "\"";
        out += ",\"lastKnownName\":\"" + tml::JsonEscape(it->second.lastKnownName) + "\"";
        out += ",\"rev\":" + std::to_string(it->second.rev);
        out += "}";
    }
    out += "}";
    // 原子写：写坏了客户端会看到 rev 回退，而"只认变大"的客户端就再也不重拉了
    tml::WriteWholeFileAtomic(m_convPath, out);
}

void TransferHub::EnsureConvLocked(const std::string& convId, const std::string& peerId,
                                  const std::string& lastKnownName)
{
    if (convId.empty()) return;

    ConvMeta& meta = m_convs[convId];    // 不存在则从 rev=0 开始，之后只增不减
    bool changed = false;
    if (!peerId.empty() && meta.peerId != peerId) { meta.peerId = peerId; changed = true; }
    if (!lastKnownName.empty() && meta.lastKnownName != lastKnownName)
    {
        meta.lastKnownName = lastKnownName;
        changed = true;
    }
    if (changed) SaveConvRevsLocked();
}

void TransferHub::LoadHistory()
{
    if (m_historyPath.empty()) return;

    std::string raw;
    if (!tml::ReadWholeFile(m_historyPath, raw) || raw.empty()) return;

    size_t pos = 0;
    while (pos < raw.size())
    {
        size_t e = raw.find('\n', pos);
        if (e == std::string::npos) e = raw.size();
        const std::string line = raw.substr(pos, e - pos);
        pos = e + 1;
        if (line.size() < 8) continue;

        ShareMessage m;
        if (!ParseHistoryLine(line, m)) continue;

        // 老历史行（没有 msgId/convId/epoch）：**只读推导，绝不回写文件**。
        // 方向由 dir 决定：out = 本端发出，in = 对端发来。
        if (m.msgId.empty())
        {
            const std::string legacyKey = MakeLegacyKey(m.dev);   // 唯一产出点，别在这里再拼一次
            m.epoch = 0;
            m.seq = m.id;
            m.msgId = legacyKey + ":0:" + std::to_string(m.id);
            if (m.from.empty()) m.from = (m.dir == "out") ? SelfDeviceId() : legacyKey;
            if (m.to.empty()) m.to = (m.dir == "out") ? legacyKey : SelfDeviceId();
        }
        if (m.convId.empty()) m.convId = MakeConvId(m.from, m.to);
        if (m.peerTs == 0) m.peerTs = m.time;

        m_messages.push_back(m);
        IndexMessageLocked(m);
        if (m.id >= m_nextId) m_nextId = m.id + 1;
        if (!m.fileId.empty() && !m.fileName.empty())
        {
            // P0-19 之后文件在 <saveDir>\<设备子目录>\ 下：优先用 fileRel 还原。
            // 老历史行没有 fileRel（升级前文件都平铺在 saveDir 根），退回基名拼接——
            // **两条路都要留着**，否则升级一次，所有旧图片都打不开。
            m_filePaths[m.fileId] = m.fileRel.empty()
                ? tml::JoinPath(m_shareDir, tml::Utf8ToWide(m.fileName))
                : tml::JoinPath(m_shareDir, tml::Utf8ToWide(m.fileRel));
        }
    }

    TrimLocked();
}

std::wstring TransferHub::CandidatePath(const std::wstring& dir, const std::wstring& safeName, int index)
{
    if (index <= 0) return tml::JoinPath(dir, safeName);

    // 重名就在名字后面加 (2)、(3)……扩展名保持原样
    std::wstring stem = safeName;
    std::wstring ext;
    const size_t dot = safeName.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0)
    {
        stem = safeName.substr(0, dot);
        ext = safeName.substr(dot);
    }

    wchar_t suffix[24] = { 0 };
    swprintf_s(suffix, L" (%d)", index + 1);
    return tml::JoinPath(dir, stem + suffix + ext);
}

bool TransferHub::PlaceFileAtomic(const std::wstring& tempPath, const std::wstring& dir,
                                  const std::string& name, std::wstring& outPath)
{
    const std::wstring safe = SafeFileName(name.empty() ? std::string("file") : name);

    for (int i = 0; i < 1000; ++i)
    {
        const std::wstring candidate = CandidatePath(dir, safe, i);

        // 关键是**不带 MOVEFILE_REPLACE_EXISTING**：目标已存在就让这次调用失败。
        // 老写法是"先 GetFileAttributes 判断不存在 → 再带 REPLACE 改名"，
        // 两个动作之间必然有窗口，而 REPLACE 又恰好把窗口里的冲突抹平了：
        // 两端同时发同名文件时，后到的会把先到的**覆盖**掉——用户看到"只收到一份"。
        if (::MoveFileExW(tempPath.c_str(), candidate.c_str(), 0))
        {
            outPath = candidate;
            return true;
        }

        const DWORD err = ::GetLastError();
        if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) continue;   // 换下一个序号重试

        if (err == ERROR_NOT_SAME_DEVICE)
        {
            // 跨卷兜底：bFailIfExists=TRUE 保持同样的"不覆盖"语义
            if (::CopyFileW(tempPath.c_str(), candidate.c_str(), TRUE))
            {
                ::DeleteFileW(tempPath.c_str());
                outPath = candidate;
                return true;
            }
            const DWORD e2 = ::GetLastError();
            if (e2 == ERROR_ALREADY_EXISTS || e2 == ERROR_FILE_EXISTS) continue;
        }

        return false;   // 权限不足 / 磁盘满 / 路径过长：不再空转 999 次
    }
    return false;
}

// 目录名清洗。**单独抽出来**是因为 ResolveDeviceFolder 要用同一套规则去比对已有目录，
// 两处规则一旦不一致，就会"找不到自己上次建的目录"，然后每发一次多一个文件夹。
static std::string SanitizeFolderPart(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        // 只挡真正进不了文件名的那几个 + 控制字符。UTF-8 多字节(>=0x80)原样保留，
        // 否则中文设备名会被截成半个字符，目录名变成乱码。
        if (u < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            out += '_';
        else
            out += c;
    }

    // Windows 会静默吃掉结尾的点和空格："xxx." 与 "xxx" 是同一个目录。
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();

    // 超长截断，注意别把 UTF-8 多字节切成半个字符
    if (out.size() > 48)
    {
        size_t cut = 48;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
    }
    return out;
}

std::wstring TransferHub::DeviceFolderName(const std::string& peerId, const std::string& displayName)
{
    const std::string key = peerId.empty() ? std::string("unknown:anonymous") : peerId;
    const bool isLegacy = (key.rfind("legacy:", 0) == 0);

    // 目录名的"身份"部分：legacy 会话的名字本身就是身份，再拼一遍 legacy:<名> 只是噪音
    std::string idPart = isLegacy ? std::string("legacy") : SanitizeFolderPart(key);
    std::string namePart = SanitizeFolderPart(displayName);
    if (namePart.empty()) namePart = isLegacy ? key.substr(7) : "设备";
    if (namePart.empty()) namePart = "设备";

    std::string folder = namePart + "-" + idPart;

    // 名字可能被截断成空；也挡一下把整个目录名弄成设备保留名的情况
    if (folder.empty()) folder = "unknown";
    static const char* const kReserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    for (const char* r : kReserved)
    {
        if (_stricmp(folder.c_str(), r) == 0) { folder.insert(folder.begin(), '_'); break; }
    }

    return tml::Utf8ToWide(folder);
}

std::wstring TransferHub::ResolveDeviceFolder(const std::wstring& shareDir, const std::string& peerId,
                                              const std::string& displayName)
{
    const std::wstring want = DeviceFolderName(peerId, displayName);

    // 先看想要的名字在不在
    const std::wstring wantPath = tml::JoinPath(shareDir, want);
    if (::GetFileAttributesW(wantPath.c_str()) != INVALID_FILE_ATTRIBUTES) return want;

    // 不在就按"身份"找已有目录：改名之后（SANQIU-pc-xx → 新名-pc-xx）必须复用旧目录，
    // 否则用户改一次名，之前收的文件就"消失"了（实际是留在旧目录里没人指向）。
    const std::string key = peerId.empty() ? std::string("unknown:anonymous") : peerId;
    const std::string idPart = (key.rfind("legacy:", 0) == 0) ? std::string("legacy") : SanitizeFolderPart(key);
    const std::string suffix = "-" + idPart;
    const std::string pureId = SanitizeFolderPart(key);      // 早期版本只有 deviceId 的目录名

    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = tml::JoinPath(shareDir, L"*");
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            if (fd.cFileName[0] == L'.') continue;
            const std::string name = tml::WideToUtf8(fd.cFileName);
            const bool sameIdentity =
                (name == pureId) ||
                (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0);
            if (sameIdentity)
            {
                ::FindClose(h);
                return fd.cFileName;
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }

    return want;
}

// 文件内容哈希：FNV-1a 64（够快、无依赖；配合"尺寸必须相等"之后碰撞概率可忽略）
static bool HashFileInternal(const std::wstring& path, long long& sizeOut, long long& writeOut, std::string& hashOut)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    sizeOut = (static_cast<long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    writeOut = (static_cast<long long>(fad.ftLastWriteTime.dwHighDateTime) << 32) | fad.ftLastWriteTime.dwLowDateTime;

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || fp == nullptr) return false;

    unsigned long long h = 1469598103934665603ULL;   // FNV offset basis
    char buf[64 * 1024];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    {
        for (size_t i = 0; i < n; ++i)
        {
            h ^= static_cast<unsigned char>(buf[i]);
            h *= 1099511628211ULL;                    // FNV prime
        }
    }
    fclose(fp);

    char hex[32] = { 0 };
    sprintf_s(hex, "%016llx", h);
    hashOut = hex;
    return true;
}

bool TransferHub::FindIdenticalFile(const std::wstring& dir, const std::wstring& tempPath, std::wstring& foundOut)
{
    long long newSize = 0, newWrite = 0;
    std::string newHash;
    if (!HashFileInternal(tempPath, newSize, newWrite, newHash)) return false;

    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = tml::JoinPath(dir, L"*");
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool hit = false;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (fd.cFileName[0] == L'.') continue;
        // 本插件自己的临时文件不算候选（它可能正在被写）
        if (wcsncmp(fd.cFileName, L"~up_", 4) == 0) continue;

        const long long size = (static_cast<long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        if (size != newSize) continue;      // 尺寸不同直接跳过，省掉读盘

        const std::wstring full = tml::JoinPath(dir, fd.cFileName);
        const std::string keyPath = tml::WideToUtf8(full);
        const long long writeTime = (static_cast<long long>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                                    fd.ftLastWriteTime.dwLowDateTime;

        std::string hash;
        std::map<std::string, FileHashEntry>::iterator it = m_hashCache.find(keyPath);
        if (it != m_hashCache.end() && it->second.size == size && it->second.writeTime == writeTime)
        {
            hash = it->second.hash;
        }
        else
        {
            long long s2 = 0, w2 = 0;
            if (!HashFileInternal(full, s2, w2, hash)) continue;
            FileHashEntry e;
            e.size = s2; e.writeTime = w2; e.hash = hash;
            m_hashCache[keyPath] = e;
        }

        if (hash == newHash)
        {
            foundOut = full;
            hit = true;
            break;
        }
    } while (::FindNextFileW(h, &fd));

    ::FindClose(h);
    return hit;
}

std::string TransferHub::PeerKeyLocked(const std::string& explicitPeer, const std::string& devName)
{
    if (!explicitPeer.empty()) return explicitPeer;

    const std::string peer = ResolvePeerDeviceId(devName);
    // ResolvePeerDeviceId 对空名字返回 "unknown:anonymous"，这里只是兜底
    return peer.empty() ? (std::string("unknown:") + devName) : peer;
}

std::wstring TransferHub::NewFileId()
{
    static std::mt19937 rng{ std::random_device{}() };
    unsigned int r = rng() & 0xFFFF;
    wchar_t buf[64] = { 0 };
    swprintf_s(buf, L"f_%lld_%04x", tml::NowMs(), r);
    return std::wstring(buf);
}

std::wstring TransferHub::SafeFileName(const std::string& name)
{
    std::string base = name;
    size_t pos = base.find_last_of("/\\");
    if (pos != std::string::npos) base = base.substr(pos + 1);

    std::string cleaned;
    for (char c : base)
    {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 0x20)
            cleaned += '_';
        else
            cleaned += c;
    }
    if (cleaned.empty()) cleaned = "file";

    // 超长文件名截断，注意不能把 UTF-8 多字节字符切成两半
    if (cleaned.size() > 120)
    {
        size_t cut = 120;
        while (cut > 0 && (static_cast<unsigned char>(cleaned[cut]) & 0xC0) == 0x80) --cut;
        cleaned.resize(cut);
    }
    return tml::Utf8ToWide(cleaned);
}

bool TransferHub::BeginOutbound(const std::string& peerDeviceId, ShareMessage& msg)
{
    std::lock_guard<std::mutex> lk(m_mutex);

    msg.dir = "out";
    msg.unread = false;
    if (msg.time == 0) msg.time = tml::NowMs();
    if (msg.dev.empty()) msg.dev = m_deviceName;

    // 幂等：同 msgId 已存在 → 返回 false，msg.id 已被改回已有那条的 id
    if (!AssignIdentityLocked(msg, peerDeviceId, std::string(), true)) return false;
    return true;
}

long long TransferHub::FinishOutbound(ShareMessage msg, const std::string& status)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        // 只有两种状态，而且必须由调用方给出真实结果——默认值不设 "sent"，
        // 免得"忘了传"变成"看起来发出去了"。
        msg.status = (status == "sent") ? "sent" : "failed";
        msg.id = m_nextId++;
        m_messages.push_back(msg);
        IndexMessageLocked(msg);
        TrimLocked();
        AppendHistory(msg);
    }
    m_cv.notify_all();
    return msg.id;
}

bool TransferHub::FindByMsgId(const std::string& msgId, ShareMessage& out) const
{
    if (msgId.empty()) return false;
    std::lock_guard<std::mutex> lk(m_mutex);
    const std::map<std::string, long long>::const_iterator it = m_msgIdToId.find(msgId);
    if (it == m_msgIdToId.end()) return false;
    for (size_t i = 0; i < m_messages.size(); ++i)
    {
        if (m_messages[i].id != it->second) continue;
        out = m_messages[i];
        return true;
    }
    return false;
}

bool TransferHub::SetMessageStatus(const std::string& msgId, const std::string& status)
{
    if (msgId.empty()) return false;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const std::map<std::string, long long>::const_iterator it = m_msgIdToId.find(msgId);
        if (it == m_msgIdToId.end()) return false;
        for (size_t i = 0; i < m_messages.size(); ++i)
        {
            if (m_messages[i].id != it->second) continue;
            const std::string next = (status == "sent") ? "sent" : "failed";
            if (m_messages[i].status == next) return true;
            m_messages[i].status = next;
            changed = true;
            break;
        }
        if (!changed) return false;
        RewriteHistory();
    }
    m_cv.notify_all();
    return true;
}

long long TransferHub::AddText(const std::string& text, const std::string& dir, const std::string& dev,
                               const std::string& peerDeviceId, const std::string& msgIdIn,
                               std::string* outMsgId)
{
    ShareMessage msg;
    bool created = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        msg.id = m_nextId;               // 先借一个号；重复时会被改回已有的 id
        msg.dir = (dir == "out") ? "out" : "in";
        msg.kind = "text";
        msg.text = text;
        msg.time = tml::NowMs();
        msg.unread = (msg.dir == "in");
        msg.dev = !dev.empty() ? dev : (msg.dir == "out" ? m_deviceName : m_peerName);
        if (msg.dev.empty()) msg.dev = (msg.dir == "out") ? "本机" : "手机";

        const bool outgoing = (msg.dir == "out");
        if (AssignIdentityLocked(msg, peerDeviceId, msgIdIn, outgoing))
        {
            ++m_nextId;
            m_messages.push_back(msg);
            IndexMessageLocked(msg);
            TrimLocked();
            AppendHistory(msg);
            created = true;
        }
    }
    if (outMsgId != nullptr) *outMsgId = msg.msgId;
    if (created)
    {
        m_cv.notify_all();     // 只在真的新增时唤醒，重复提交不该惊动长轮询
        // 宿主动作通知（exe 托盘闪烁/系统气泡）。插件宿主未注册 hook = 空操作。
        // 只在入站、非 system 消息触发：出站是自己发的，system 是内部提示，都不该打扰。
        if (msg.dir == "in" && msg.kind != "system" && !msg.text.empty())
        {
            tml::host::IncomingNotice n;
            n.kind = msg.kind;
            n.dev = msg.dev;
            // 单行预览：换行折叠成空格，避免气泡里出现多行怪排版
            n.preview = msg.text;
            for (char& c : n.preview) if (c == '\r' || c == '\n') c = ' ';
            n.convId = msg.convId;
            tml::host::NotifyIncoming(n);
        }
    }
    return msg.id;
}

ShareMessage TransferHub::AddFile(const std::wstring& tempPath,
                                  const std::string& fileName,
                                  const std::string& mime,
                                  const std::string& dir,
                                  const std::string& dev,
                                  const std::string& peerDeviceId,
                                  const std::string& msgIdIn)
{
    ShareMessage msg;
    std::wstring finalPath;
    bool created = false;

    const bool outgoing = (dir == "out");

    {
        std::lock_guard<std::mutex> lk(m_mutex);

        const std::wstring fileId = NewFileId();

        msg.id = m_nextId;               // 先借号；重复提交时会被改回已有的 id
        msg.dir = outgoing ? "out" : "in";
        // dev 必须在解析对端键**之前**定下来：AssignIdentityLocked 用的就是它
        msg.dev = !dev.empty() ? dev : (outgoing ? m_deviceName : m_peerName);
        if (msg.dev.empty()) msg.dev = outgoing ? "本机" : "手机";

        // P0-19：文件按设备子目录保存。目录键与会话键同源（PeerKeyLocked），
        // 并把解析结果**回填**给 AssignIdentityLocked，避免两处各算一次。
        const std::string peerKey = PeerKeyLocked(peerDeviceId, msg.dev);

        // 目录名要用**真实设备名**（不是用户改的别名）：别名随时会改，目录名跟着变
        // 就会留下一堆孤儿目录。别名只影响界面显示，见网页端的 `别名 (真实名)`。
        std::string peerName;
        {
            PeerDevice pd;
            if (DeviceRegistry::Instance().FindPeer(peerKey, pd)) peerName = pd.name;
        }
        if (peerName.empty())
        {
            if (peerKey.rfind("legacy:", 0) == 0) peerName = peerKey.substr(7);
            // 不用 msg.dev 兜底：它可能是"手机"/"本机"这类占位词，拿它命名目录只会让人更糊涂
            else peerName = "未知设备";
        }

        const std::wstring devFolder = ResolveDeviceFolder(m_shareDir, peerKey, peerName);
        const std::wstring devDir = tml::JoinPath(m_shareDir, devFolder);
        tml::EnsureDir(devDir);

        // 内容去重：同一个文件传两次只留一份（用户要的行为）。
        // 命中就删掉刚上传的临时文件、直接复用已有路径，**不加 (2)**。
        // 只有"同名但内容不同"才走改名分支。
        bool placedNew = false;
        if (FindIdenticalFile(devDir, tempPath, finalPath))
        {
            ::DeleteFileW(tempPath.c_str());
        }
        // 同名不覆盖靠**一次原子的改名**，不靠"先查存在"（见 PlaceFileAtomic）
        else if (PlaceFileAtomic(tempPath, devDir,
                                 fileName.empty() ? std::string("file") : fileName, finalPath))
        {
            placedNew = true;
        }
        else
        {
            // 落盘失败就别入库：一条"文件在但路径是空的"消息，用户点开就是白板
            ::DeleteFileW(tempPath.c_str());
            return ShareMessage();
        }

        msg.fileId = tml::WideToUtf8(fileId);
        // fileName 记录"磁盘上真实的文件名"（可能因重名被加了 (2)），
        // fileRel 记住"相对保存目录的完整位置"，历史恢复靠它
        {
            const size_t slash = finalPath.find_last_of(L"\\/");
            const std::wstring realName = (slash == std::wstring::npos) ? finalPath : finalPath.substr(slash + 1);
            msg.fileName = tml::WideToUtf8(realName);
            msg.fileRel = tml::WideToUtf8(devFolder + L"\\" + realName);
        }
        if (msg.fileName.empty()) msg.fileName = "file";
        msg.mime = mime.empty() ? "application/octet-stream" : mime;
        msg.time = tml::NowMs();
        msg.unread = !outgoing;

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (::GetFileAttributesExW(finalPath.c_str(), GetFileExInfoStandard, &fad))
            msg.fileSize = (static_cast<long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;

        msg.kind = (msg.mime.rfind("image/", 0) == 0) ? "image" : "file";

        if (AssignIdentityLocked(msg, peerKey, msgIdIn, outgoing))
        {
            ++m_nextId;
            m_filePaths[msg.fileId] = finalPath;
            m_messages.push_back(msg);
            IndexMessageLocked(msg);
            TrimLocked();
            AppendHistory(msg);
            created = true;
        }
        else
        {
            // 同一个 msgId 重复上传：刚落到磁盘的那份多余副本要删掉，
            // 否则重发一次就多一个文件，而消息列表里只有一条。
            // **只在这次真的新建了文件时才删**：内容去重命中的是别人还在用的那份，
            // 删了会让先前那条消息的附件凭空消失。
            if (placedNew) ::DeleteFileW(finalPath.c_str());
        }
    }

    if (created)
    {
        m_cv.notify_all();
        if (!outgoing)
        {
            tml::host::IncomingNotice n;
            n.kind = msg.kind;
            n.dev = msg.dev;
            n.preview = std::string(msg.kind == "image" ? "[图片] " : "[文件] ") + msg.fileName;
            n.convId = msg.convId;
            tml::host::NotifyIncoming(n);
        }
    }
    return msg;
}

void TransferHub::DiscardFile(const std::wstring& tempPath)
{
    ::DeleteFileW(tempPath.c_str());
}

bool TransferHub::RemoveConversation(const std::string& convId)
{
    if (convId.empty()) return false;
    int removed = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);

        std::deque<ShareMessage> keep;
        for (const auto& m : m_messages)
        {
            if (m.convId != convId) { keep.push_back(m); continue; }
            const std::map<std::string, long long>::iterator it = m_msgIdToId.find(m.msgId);
            if (it != m_msgIdToId.end() && it->second == m.id) m_msgIdToId.erase(it);
            if (!m.fileId.empty()) m_filePaths.erase(m.fileId);
            ++removed;
        }
        m_messages.swap(keep);

        // 会话条目也要一起删：只删消息的话，它会以 count=0 的形式继续留在列表里，
        // 用户点了"删除设备"却发现那行还在，只会以为没生效。
        m_convs.erase(convId);
        SaveConvRevsLocked();
        RewriteHistory();
    }
    m_cv.notify_all();
    return true;
}

void TransferHub::DropPeerHistory(const std::string& peerId)
{
    if (peerId.empty()) return;
    const std::string convId = ConvIdOfPeer(peerId);
    if (!convId.empty()) RemoveConversation(convId);
}

bool TransferHub::DeviceFolderInfo(const std::string& peerId, const std::string& displayName,
                                   std::wstring& pathOut, int& fileCount, long long& bytesOut)
{
    fileCount = 0;
    bytesOut = 0;
    const std::wstring folder = ResolveDeviceFolder(m_shareDir, peerId, displayName);
    pathOut = tml::JoinPath(m_shareDir, folder);
    if (::GetFileAttributesW(pathOut.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = tml::JoinPath(pathOut, L"*");
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    do
    {
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ++fileCount;
        bytesOut += (static_cast<long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return true;
}

bool TransferHub::DeviceFolderRemove(const std::string& peerId, const std::string& displayName,
                                     int& removedFiles)
{
    removedFiles = 0;
    const std::wstring folder = ResolveDeviceFolder(m_shareDir, peerId, displayName);
    const std::wstring dir = tml::JoinPath(m_shareDir, folder);
    if (::GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = tml::JoinPath(dir, L"*");
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.cFileName[0] == L'.') continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (::DeleteFileW(tml::JoinPath(dir, fd.cFileName).c_str())) ++removedFiles;
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    // 只删自己建的这一层；里面如果还有子目录（用户自己放进去的），留着更安全
    ::RemoveDirectoryW(dir.c_str());
    return true;
}

std::vector<ShareMessage> TransferHub::WaitMessages(long long since, int waitSeconds)
{
    MessageQuery q;
    q.sinceGlobal = since;
    q.waitSeconds = waitSeconds;
    return QueryMessages(q);
}

std::vector<ShareMessage> TransferHub::QueryMessages(const MessageQuery& q)
{
    std::vector<ShareMessage> result;
    std::unique_lock<std::mutex> lk(m_mutex);

    auto collect = [&]()
    {
        result.clear();
        for (const auto& m : m_messages)
        {
            // legacy 通道不下发系统消息（老端会当成一条 dir=in 的普通消息：
            // 未读 +1、进本地历史、界面上是个没有内容的空气泡）
            if (q.legacy && m.kind == "system") continue;

            // legacy 通道只看 "*:legacy:*" 这类会话（对端没有 deviceId）。
            // 不按具体 dev 过滤的原因见 HttpServer：PC 管理页发消息时不带 dev，
            // 按 dev 过滤会让"电脑网页发给手机"的消息在手机上消失。
            if (q.legacy)
            {
                const bool legacyConv = (m.from.rfind("legacy:", 0) == 0) || (m.to.rfind("legacy:", 0) == 0);
                if (!legacyConv) continue;
            }

            // 全局 id 水位（老客户端）：只用 since
            if (q.sinceGlobal > 0 && m.id <= q.sinceGlobal) continue;

            if (!q.peerDeviceId.empty())
            {
                // 会话过滤：本端发出的 to == peer，对端发来的 from == peer
                const bool inConv = (m.from == q.peerDeviceId || m.to == q.peerDeviceId);
                if (!inConv) continue;

                // per-peer 水位：epoch 更大的都算新；同 epoch 比 seq
                if (q.sinceEpoch >= 0)
                {
                    if (m.epoch < q.sinceEpoch) continue;
                    if (m.epoch == q.sinceEpoch && m.seq <= q.sinceSeq) continue;
                }
            }
            result.push_back(m);
        }

        // 排序键用 (peerTs, seq)，**不能用 recvTs**：补拉多条时 recvTs 全是"现在"，
        // 顺序会乱，而 peerTs 是发送方自己的时间，才反映真实先后。
        std::stable_sort(result.begin(), result.end(),
                         [](const ShareMessage& a, const ShareMessage& b)
                         {
                             if (a.peerTs != b.peerTs) return a.peerTs < b.peerTs;
                             if (a.seq != b.seq) return a.seq < b.seq;
                             return a.id < b.id;
                         });
        return !result.empty();
    };

    if (collect()) return result;
    if (q.waitSeconds <= 0 || m_stopping) return result;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(q.waitSeconds);
    while (!m_stopping)
    {
        if (std::chrono::steady_clock::now() >= deadline) break;
        m_cv.wait_for(lk, std::chrono::milliseconds(500));
        if (collect()) break;
    }
    return result;
}

std::string TransferHub::ConversationsJson()
{
    std::lock_guard<std::mutex> lk(m_mutex);

    // 会话 = convId。按最后一条消息的 (peerTs) 倒序，列表里最近的排前面。
    struct Conv
    {
        std::string convId;
        std::string peerId;
        std::string lastKnownName;
        long long   lastTs = 0;
        long long   lastId = 0;
        int         unread = 0;
        int         count = 0;
    };
    std::vector<Conv> convs;

    // 先按"曾经出现过消息的 conv"建骨架（m_convs 的键）：
    // 已清空的会话必须仍在列表里，否则客户端无法区分"我清空了"和"服务端把数据弄丢了"——
    // 这两种情况用户的反应完全不同，而 histRev 的作用正是"让客户端知道缓存过期了"。
    for (std::map<std::string, ConvMeta>::const_iterator it = m_convs.begin(); it != m_convs.end(); ++it)
    {
        Conv c;
        c.convId = it->first;
        c.peerId = it->second.peerId;      // 清空后仍能说出对端是谁
        c.lastKnownName = it->second.lastKnownName;
        convs.push_back(c);
    }

    for (size_t i = 0; i < m_messages.size(); ++i)
    {
        const ShareMessage& m = m_messages[i];
        const std::string peerId = (m.dir == "out") ? m.to : m.from;

        Conv* hit = nullptr;
        for (size_t k = 0; k < convs.size(); ++k)
        {
            if (convs[k].convId != m.convId) continue;
            hit = &convs[k];
            break;
        }
        if (hit == nullptr)
        {
            Conv c;
            c.convId = m.convId;
            c.peerId = peerId;
            convs.push_back(c);
            hit = &convs.back();
        }
        if (hit->peerId.empty()) hit->peerId = peerId;
        if (!m.dev.empty()) hit->lastKnownName = m.dev;   // 以最新一条消息的名字为准
        if (m.peerTs >= hit->lastTs)
        {
            hit->lastTs = m.peerTs;
            hit->lastId = m.id;
            hit->peerId = peerId;    // 以最后一条消息的对端为准
        }
        if (IsUnreadCountable(m)) ++hit->unread;
        ++hit->count;
    }

    std::stable_sort(convs.begin(), convs.end(),
                     [](const Conv& a, const Conv& b) { return a.lastTs > b.lastTs; });

    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < convs.size(); ++i)
    {
        if (i > 0) os << ",";

        std::string display = convs[i].peerId;
        // 解析顺序：会话级别名（legacy 专用 aliases）> 设备表 alias > name >
        // **lastKnownName** > 原始标识。lastKnownName 是"消息入库那一刻"的名字，
        // 设备表后来改名/删设备时靠它兜底，否则会话行会变成"未知设备"。
        const std::string convAlias = Registry().AliasForKey(convs[i].peerId);
        PeerDevice peer;
        if (!convAlias.empty())
        {
            display = convAlias;
        }
        else if (Registry().FindPeer(convs[i].peerId, peer))
        {
            display = peer.DisplayName();
        }
        else if (!convs[i].lastKnownName.empty())
        {
            display = convs[i].lastKnownName;
        }
        else if (convs[i].peerId.rfind("legacy:", 0) == 0)
        {
            // legacy 会话的设备名就是去掉前缀那一段；别把内部键直接显示给用户看
            display = convs[i].peerId.substr(7);
        }
        else if (display.empty()) 
        {
            display = "未知设备";
        }

        os << "{\"convId\":\"" << tml::JsonEscape(convs[i].convId) << "\""
           << ",\"deviceId\":\"" << tml::JsonEscape(convs[i].peerId) << "\""
           << ",\"displayName\":\"" << tml::JsonEscape(display) << "\""
           << ",\"lastTs\":" << convs[i].lastTs
           << ",\"lastMessageId\":" << convs[i].lastId
           << ",\"count\":" << convs[i].count
           << ",\"unread\":" << convs[i].unread
           // T04：legacy 会话（对端没有 deviceId）标记出来，网页端据此显示"来自旧版本"
           << ",\"legacy\":" << ((convs[i].peerId.rfind("legacy:", 0) == 0) ? "true" : "false")
           // histRev 与 /api/devices 同源（同一份 m_convs）。
           // 这里**直接读成员**，不要调 HistRevOf()：本函数已经持有 m_mutex，
           // 再调那个会重入同一把非递归锁 → 死锁/进程终止（实测就是这么崩的）。
           << ",\"histRev\":" << ((m_convs.find(convs[i].convId) == m_convs.end())
                                      ? 0 : m_convs.find(convs[i].convId)->second.rev) << "}";
    }
    os << "]";
    return os.str();
}

std::wstring TransferHub::FilePathById(const std::string& fileId)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_filePaths.find(fileId);
    if (it != m_filePaths.end()) return it->second;
    return std::wstring();
}

bool TransferHub::FileInfoById(const std::string& fileId, std::string& name, std::string& mime)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto it = m_messages.rbegin(); it != m_messages.rend(); ++it)
    {
        if (it->fileId == fileId)
        {
            name = it->fileName;
            mime = it->mime;
            return true;
        }
    }
    return false;
}

int TransferHub::UnreadCount()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    int n = 0;
    for (const auto& m : m_messages)
    {
        if (IsUnreadCountable(m)) ++n;
    }
    return n;
}

TransferHub::ConvStat TransferHub::ConvStatOf(const std::string& convId) const
{
    ConvStat st;
    if (convId.empty()) return st;
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& m : m_messages)
    {
        if (m.convId != convId) continue;
        if (IsUnreadCountable(m)) ++st.unread;
    }
    const std::map<std::string, ConvMeta>::const_iterator it = m_convs.find(convId);
    st.histRev = (it == m_convs.end()) ? 0 : it->second.rev;
    return st;
}

int TransferHub::HistRevOf(const std::string& convId) const
{
    if (convId.empty()) return 0;
    std::lock_guard<std::mutex> lk(m_mutex);
    const std::map<std::string, ConvMeta>::const_iterator it = m_convs.find(convId);
    return (it == m_convs.end()) ? 0 : it->second.rev;
}

int TransferHub::MarkRead(const std::string& convOrPeer)
{
    if (convOrPeer.empty()) return 0;

    int cleared = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto& m : m_messages)
        {
            if (!IsUnreadCountable(m)) continue;
            // 两种入参都收：convId 直接命中；否则当成对端 deviceId
            const bool hit = (m.convId == convOrPeer) ||
                             (m.from == convOrPeer) || (m.to == convOrPeer);
            if (!hit) continue;
            m.unread = false;
            ++cleared;
        }
        if (cleared > 0) RewriteHistory();   // 未读标记要能扛住重启
    }
    if (cleared > 0) m_cv.notify_all();
    return cleared;
}

void TransferHub::ClearUnread()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& m : m_messages) m.unread = false;
}

bool TransferHub::TryUnreadCount(int& out)
{
    std::unique_lock<std::mutex> lk(m_mutex, std::try_to_lock);
    if (!lk.owns_lock()) return false;

    int n = 0;
    for (const auto& m : m_messages)
    {
        if (IsUnreadCountable(m)) ++n;
    }
    out = n;
    return true;
}

std::string TransferHub::ToJson(const ShareMessage& m)
{
    std::ostringstream os;
    os << "{";
    os << "\"id\":" << m.id << ",";
    os << "\"dir\":\"" << tml::JsonEscape(m.dir) << "\",";
    os << "\"kind\":\"" << tml::JsonEscape(m.kind) << "\",";
    os << "\"text\":\"" << tml::JsonEscape(m.text) << "\",";
    os << "\"fileId\":\"" << tml::JsonEscape(m.fileId) << "\",";
    os << "\"fileName\":\"" << tml::JsonEscape(m.fileName) << "\",";
    // P0-19：文件按设备子目录保存，基名不足以定位文件。**这一行是重启后
    // 图片/文件还能打开的唯一依据**，去掉就退化成"列表里有、点开 404"。
    os << "\"fileRel\":\"" << tml::JsonEscape(m.fileRel) << "\",";
    os << "\"mime\":\"" << tml::JsonEscape(m.mime) << "\",";
    os << "\"dev\":\"" << tml::JsonEscape(m.dev) << "\",";
    os << "\"fileSize\":" << m.fileSize << ",";
    os << "\"time\":" << m.time;
    // ---- T03 追加字段（老客户端只读它认识的键，多余字段会被忽略）----
    os << ",\"msgId\":\"" << tml::JsonEscape(m.msgId) << "\"";
    os << ",\"convId\":\"" << tml::JsonEscape(m.convId) << "\"";
    os << ",\"from\":\"" << tml::JsonEscape(m.from) << "\"";
    os << ",\"to\":\"" << tml::JsonEscape(m.to) << "\"";
    os << ",\"epoch\":" << m.epoch;
    os << ",\"seq\":" << m.seq;
    os << ",\"peerTs\":" << m.peerTs;
    os << ",\"recvTs\":" << m.recvTs;
    os << ",\"status\":\"" << tml::JsonEscape(m.status) << "\"";
    os << ",\"unread\":" << (m.unread ? "true" : "false");
    os << "}";
    return os.str();
}
