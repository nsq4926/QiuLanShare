#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

// 设备表（docs/06 §P0-5、docs/05 §四）。
//
// 设计要点，都不是可选项：
//  1. deviceId 是主键，**首次运行随机生成并落盘，之后只读**。绝不由 MAC 派生：
//     换网卡 / 随机 MAC / USB 网卡热插拔都会让 ID 变，对端眼里就成了"一台新设备"，
//     会话与历史全断；克隆镜像还会共用 MAC 造成 ID 碰撞，两台设备互相顶掉。
//     MAC 只作展示与辅助匹配（mac 字段 + macHistory）。
//  2. 落盘用 tmp + 原子替换：半截写坏的设备表比丢掉更糟——下次解析失败，
//     deviceId 重新生成，而对端已经记住了旧 ID。
//  3. ephemeral=true 的条目（网页身份）只存在于内存，**永不写进文件**。
//  4. 读路径一律返回快照（值拷贝），绝不让调用方在加锁状态下"读的时候顺便改"。
//     任务栏 UI 线程和 HTTP 线程会同时读它。
struct DeviceEndpoint
{
    std::string ip;
    long long   lastOkAt = 0;   // 这个 IP 最近一次成功通信的时间（ms）
};

struct PeerDevice
{
    std::string deviceId;
    std::string name;
    std::string alias;          // 用户改的显示名，空则用 name
    std::string platform;       // "harmony" / "windows" / "web"
    std::string mac;
    std::vector<std::string>  macHistory;
    std::vector<DeviceEndpoint> ips;
    unsigned short port = 0;
    std::string role = "peer";      // peer / server / client
    std::string state = "offline";  // online / offline / ...
    bool acceptsInbound = true;
    bool ephemeral = false;         // true = 只进内存，不落盘、不下发
    long long lastSeen = 0;
    // **HTTP 往返成功**的时刻（我连上它，或它连上我）。与 lastSeen 语义不同，不能合并：
    // lastSeen 只说明"收到过它的报文"，announce 一直发而连不上的设备 lastSeen 也会很新。
    // 四态判定必须用这个字段，否则"显示在线却发不出去"。
    // 冷启动一律视为 0（不拿磁盘上的旧值判在线）：否则开机 30 秒内一堆早就断开的
    // 设备会显示成在线，而这正是用户开机第一眼看到的东西。
    long long lastReachableAt = 0;
    long long firstSeen = 0;
    std::string apiVersion;
    // 对端支持分片/续传（announce 或 hello 带 resumable:true）。
    // 落盘是必须的：发送时才知道该按 256KB 切还是整份发。
    bool resumable = false;
    // 对端自报"被网卡过滤排除的接口数"（仅排障展示，不参与状态判定）
    int filteredNicCount = 0;
    bool paired = false;
    std::string convId;

    std::string DisplayName() const { return alias.empty() ? name : alias; }
};

// 五态。**枚举值只在这里定义一次**，JSON 序列化与内部比较都走 ToString()。
// 理由：`online_no_inbound` 与 `online_no_accept` 差 6 个字母，散写一次编译照过、
// 返回照常，只是那个值永远匹配不上——断言里就是一条永远红的断言，而永红的断言
// 会被当成"功能还没做"长期挂着，没人去修。
enum class DeviceState
{
    Online = 0,
    OnlineNoInbound = 1,
    Unreachable = 2,
    Discovered = 3,
    Offline = 4,
};

const char* DeviceStateToString(DeviceState state);
// 排序优先级：online > online_no_inbound > unreachable > discovered > offline
int DeviceStateRank(DeviceState state);

struct SelfDevice
{
    std::string deviceId;
    std::string name;
    std::string mac;
    std::vector<std::string> ips;
    int filteredNicCount = 0;   // 被步骤 2/3 排除掉的接口数（见 Common.h 的口径说明）
    int keptNicCount = 0;

    // ---- 消息模型（T03）----
    // epoch：本端代次，从 1 开始；清空本地历史时可 +1，让对端能识别"这是新一代的号"。
    // seq  ：**本端全局单调、按 epoch 递增**。
    //
    // 为什么不是"每个对端各自从 1 开始"：msgId = "<deviceId>:<epoch>:<seq>" 是全局去重键。
    // 若 seq 按对端独立自增，同一台机器的两个会话都会产生 ":1:1" —— 实测后果是
    // **第二条消息被当成重复直接丢掉，而且不报错**（一台机器同时和两台设备通信就会撞）。
    // 每位接收方只需记住"这个对端我见过的最大的 seq"，中间的空号不影响水位判定。
    //
    // ⚠ 两条配套约束：
    //   1) **任何人不得用"seq 是否连续"检测丢包** —— 全局递增必然跳号（1、3、7），
    //      按连续性推导只会得到假丢包。
    //   2) 已知代价：对端能从跳号幅度**推断本端给别的设备发了多少条消息**。
    //      LAN 工具接受这个泄漏，但它是可预期的取舍，不是疏忽。
    long long epoch = 1;
    long long seq = 0;
};

class DeviceRegistry
{
public:
    static DeviceRegistry& Instance();

    // 载入 <dataDir>\lanshare_devices[.<instance>].json；文件不存在或
    // self.deviceId 为空时生成新的随机 ID 并立刻落盘。可重复调用（幂等）。
    //
    // 为什么要按实例分文件：docs 的 17b 要求"同一台机器跑两个实例（5a 形态）时，
    // 对端必须显示两台设备"。若两个实例共用一份 devices.json，它们会报同一个
    // deviceId，对端必然合并成一台 —— 测试会"跑绿但什么都没验"，比失败更危险。
    // 实例名缺省（default）时仍用不带后缀的文件名，保持单一实例的常识预期。
    bool Init(const std::wstring& dataDir, const std::string& instanceName = std::string());
    bool IsInited() const;
    // 初始化失败的原因（空 = 没失败）。供 503 响应说明"为什么没起来"。
    std::string PersistReason() const;
    std::wstring Path() const;
    std::wstring DataDir() const;

    // ---- 本机身份 ----
    SelfDevice  Self() const;
    std::string DeviceId() const;        // 未初始化时返回空串，调用方负责兜底
    bool UpdateSelf(const std::string& name,
                    const std::string& mac,
                    const std::vector<std::string>& ips,
                    int filteredNicCount,
                    int keptNicCount);   // 内容有变化才落盘

    // ---- 消息序号（T03）----
    long long Epoch() const;

    // 本端代次 +1（并落盘）。
    // 契约：清空会话 → 该 conv 的 histRev +1 **且** epoch +1；清全部 → 各 conv +1
    // 而 epoch **只 +1 一次**；按天裁剪 → 只 bump rev，**不动 epoch**。
    //
    // 为什么清空要换纪次：清空后 seq 归零，而对端的水位停在旧值，
    // 新消息会被永久跳过（且不报错）。换 epoch 是让对端重置水位的唯一手段。
    //
    // 注意：epoch 必须**持久化** —— 只存在内存里的话，重启后回到 1，
    // 对端看到"纪元变小"会当成旧纪元而继续用老水位。
    void BumpEpoch();
    long long CurrentSeq() const;
    // 分配下一个 seq **并立刻落盘**（write-through）。
    // 宁可每条消息多一次原子写，也不能接受"重启后 seq 归零"这种静默丢消息：
    // 接收方水位已到 20，新消息从 1 开始就**永久拉不到**，而且不会报任何错。
    long long NextSeq();

    // ---- 对端 ----
    std::vector<PeerDevice> Peers() const;                       // 不含 ephemeral
    bool FindPeer(const std::string& deviceId, PeerDevice& out) const;
    // 按 deviceId 合并；ephemeral 只进内存。
    // mergedOut 非空时，把「因 MAC 相同而被并掉的旧 deviceId」回传，调用方据此清掉
    // 那些已经死掉的身份留下的会话 —— 否则列表里仍会多出几行同名设备。
    bool UpsertPeer(const PeerDevice& peer, std::vector<std::string>* mergedOut = nullptr);
    bool TouchPeer(const std::string& deviceId, const std::string& ip,
                   unsigned short port, long long now);          // 记一次成功通信
    bool SetAlias(const std::string& deviceId, const std::string& alias);
    bool RemovePeer(const std::string& deviceId);
    // 删除设备后要"安静"一段时间：对端还在周期 announce，不压制的话删完立刻又被
    // 自动发现加回来（用户看到的就是"删了还在"）。只压自动发现，不挡消息与手动添加。
    void IgnoreDiscovery(const std::string& deviceId, long long ms);
    bool IsDiscoveryIgnored(const std::string& deviceId) const;
    // 同一台机器换了 deviceId 后，旧身份会以"离线设备"永久留在表里，用户看到的就是
    // "一个设备显示好几项"。这里按 MAC 把死掉的身份并掉（同一 MAC 只留 lastSeen 最新的；
    // **在线的一律不动**，否则同机双实例（17b）会被并成一台）。
    // removedOut 回传被并掉的 deviceId，调用方据此清掉它们的会话。
    int DedupeByMac(std::vector<std::string>* removedOut = nullptr);
    // 发现层用：把该对端的 ips[] 收敛到 keep 这几个地址（TTL 退役），保留已有 lastOkAt
    bool KeepPeerIps(const std::string& deviceId, const std::vector<std::string>& keep);
    bool MarkPeerState(const std::string& deviceId, const std::string& state);
    // HTTP 往返成功时调用（PeerClient 成功返回 / 收到对端请求）
    bool MarkPeerReachable(const std::string& deviceId, long long at);

    // ---- 身份重建（BOM/坏文件导致时）必须可见 ----
    bool        IdentityRegenerated() const;
    std::string RegenReason() const;    // "parse" / "missing" / ""

    // ---- 会话级别名（T04）----
    // 只给"没有 deviceId 的对端"（legacy 会话）用：键是**会话键**（如 legacy:OldPhone），
    // 单独存成一张映射表，**不建任何设备条目**。
    // 为什么不建成设备条目：它没有心跳，四态判定会把它永久算成"离线"，而且
    // /api/devices 还得额外过滤——等于往设备表里塞了一个永远不可达的伪设备。
    std::string AliasForKey(const std::string& key) const;
    bool        SetAliasForKey(const std::string& key, const std::string& alias);

    // 给 /api/devices 用的 JSON（不含 ephemeral）。T03 接上端点时直接用。
    std::string PeersJson() const;
    // /api/devices 与 /api/discover **共用**这一个函数（一个函数，不是两份代码）
    // 设备表未初始化 → 返回 false，调用方给 503 not_initialized
    std::string DevicesJson(long long now, int limit, bool& outInited) const;
    // 五态判定（服务端唯一计算点）
    static DeviceState ComputeState(const PeerDevice& peer, long long lastReachableInProc, long long now);

    // 五态判定的两个时间窗（毫秒），**只读**。
    // 暴露它们是为了让验收脚本能读到"运行期实际用的常量"：
    // 脚本自己写死 30000/120000 再比大小，那条断言与实现无关、永远通过；
    // 而这两个值一旦被改错（比如被配成相等），`unreachable` 会恒为假、
    // 中间态静默消失，而所有单条状态断言仍然全绿。
    static long long OnlineTtlMs();
    static long long StaleTtlMs();

private:
    DeviceRegistry() = default;
    DeviceRegistry(const DeviceRegistry&) = delete;
    DeviceRegistry& operator=(const DeviceRegistry&) = delete;

    bool LoadLocked();
    bool SaveLocked();
    std::string ToJsonLocked() const;
    bool        FromJson(const std::string& text);


    mutable std::mutex m_mutex;
    SelfDevice         m_self;
    std::vector<PeerDevice> m_peers;
    std::vector<PeerDevice> m_ephemeral;   // 只在内存里
    std::map<std::string, std::string> m_aliases;   // 会话键 -> 显示名（legacy 用）
    mutable std::map<std::string, long long> m_ignoreUntil;   // deviceId -> 压制到什么时候（ms）
    // **本进程内**成功往返过的时刻。冷启动一律没有记录 → 视为 0，
    // 持久化的 lastReachableAt 只用于排障展示，不参与 state 判定
    std::map<std::string, long long> m_reachableInProc;
    std::wstring m_path;
    std::wstring m_logPath;
    std::string  m_regenReason;
    // 身份**存不下去**时的原因（数据目录不可写 / 设备表不可写）。
    // 这种情况下绝不能"初始化成功"：deviceId 每次启动都会重新生成，
    // 对端每次都看到一台新设备、旧会话全断，而且不报错。
    std::string  m_persistReason;
    std::string  m_regenOldId;
    std::wstring m_dataDir;
    bool m_inited = false;
};

// 生成 "pc-" + 12 位随机 hex（UUIDv4 的前 12 位）。首次运行用，之后只读。
std::string NewDeviceId();

// 两个 deviceId 排序后哈希得到 convId（不含 IP）。换网卡 / 换 IP 不产生新会话。
std::string MakeConvId(const std::string& a, const std::string& b);
