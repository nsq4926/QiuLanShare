#pragma once

#include <atomic>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <mutex>
#include <condition_variable>

#include "DeviceRegistry.h"

// 一条互通消息（文字 / 图片 / 文件）
struct ShareMessage
{
    long long   id = 0;         // 全局单调 id。老客户端与老历史都靠它，**不能动**
    std::string msgId;          // "<fromDeviceId>:<epoch>:<seq>"；legacy 行 "legacy:<dev>:0:<id>"
    std::string convId;         // 两方 deviceId 排序哈希，不含 IP（换网卡不产生新会话）
    std::string from;           // 发送方 deviceId
    std::string to;             // 接收方 deviceId
    long long   epoch = 0;      // 发送方代次；legacy 行为 0
    long long   seq = 0;        // 发送方在该会话内的序号；legacy 行用全局 id
    long long   peerTs = 0;     // 发送方时间戳。**排序键用它**，不用本机收到的时间
    long long   recvTs = 0;     // 本机收到/发出的时间，只用于诊断
    std::string status;         // 出站 "sent" / "failed"；入站为空

    std::string dir;        // "in" = 手机发来的, "out" = PC 发出的
    std::string kind;       // "text" / "image" / "file"
    std::string text;
    std::string fileId;
    std::string fileName;   // 磁盘上的**基名**（可能因重名被加了 " (2)"）
    // 相对保存目录的路径，含设备子目录（形如 "pc-fd39aef785e6/a.jpg"）。
    // **必须落盘**：fileName 只有基名，重启后靠它才能找回子目录里的文件；
    // 缺这个字段时历史恢复会把路径拼错，表现为"重启后图片打不开、但文件明明在"。
    std::string fileRel;
    std::string mime;
    std::string dev;        // 发送方设备名（手机端上报的完整设备名，PC 自己发的是本机名）
    long long   fileSize = 0;
    long long   time = 0;
    bool        unread = false;
};

// 拉取条件。sinceGlobal 是给老客户端的（全局 id），peer + sinceEpoch/sinceSeq 是
// 新协议的 per-peer 水位。两套可以同时给，取并集里的"该对端"消息。
struct MessageQuery
{
    long long   sinceGlobal = 0;
    std::string peerDeviceId;   // 空 = 不过滤（老行为）
    long long   sinceEpoch = -1;
    long long   sinceSeq = -1;
    int         waitSeconds = 0;
    // T04：legacy 通道**不下发 kind=system**。老端会把它当 dir=in 的普通消息：
    // 未读 +1、还进本地历史，用户会看到一条自己没发过的"空白气泡"。
    bool        legacy = false;
};

// 全局的“互通中心”：消息队列 + 文件落盘 + 未读计数
// 所有成员函数均可跨线程调用。
class TransferHub
{
public:
    static TransferHub& Instance();

    void Init();
    // 由 InstanceLock 决定 ini 与数据根目录后再初始化（支持 TMLS_INI / TMLS_DATA_DIR 覆盖）
    void Init(const std::wstring& iniPath, const std::wstring& dataDir, bool dataDirExplicit = false);
    void Shutdown();
    void Resume();   // 重新启动服务时清除停止标志

    bool IsInited() const { return m_inited; }

    // 实际监听的端口。端口递进（8765→8766→8767）落地后由 Plugin 回填，
    // 此后 /api/hello、任务栏、复制地址一律以它为准——绝不能再用 ini 里那个"意图端口"。
    unsigned short Port() const;
    void           SetRuntimePort(unsigned short port);

    std::wstring   ShareDir();
    std::string    LocalIp();
    std::string    DeviceName();
    std::wstring   IniPath();
    std::wstring   DataDir();

    // 身份：T01 先给出稳定值供 /api/hello 上报；持久化与完整格式由 T02 DeviceRegistry 接管
    std::string    DeviceId();
    std::string    MacAddress();
    static const char* ApiVersion();

    // 设备表的注入点：默认用全局单例，测试/多实例场景可以替换。
    // 必须在 Init() 之后、任何 DeviceId() 调用之前接上。
    void           AttachRegistry(DeviceRegistry* registry);
    DeviceRegistry& Registry();

    // ---- 历史记录（落盘在插件目录，重启不丢） ----
    std::wstring HistoryPath();
    int  HistoryDays();                 // 0 = 不自动删除
    void SetHistoryDays(int days);      // 写回 ini 并立刻按新规则清理
    void ClearHistory();                // 手动清空（内存 + 文件）
    // 会话统计（/api/devices 与 /api/conversations 必须同源，别各自算一遍）
    struct ConvStat { int unread = 0; int histRev = 0; };
    ConvStat ConvStatOf(const std::string& convId) const;
    int HistRevOf(const std::string& convId) const;

    // 只把某个会话的未读清零（POST /api/messages/read）。
    // 传 convId 或对端 deviceId 都可以；返回清零的条数。
    int  MarkRead(const std::string& convOrPeer);
    // 只清某个会话的历史，返回删掉的条数（T04：legacy 会话允许删除）
    int  ClearConversation(const std::string& convId);
    // 由对端标识（peerDeviceId，如 "legacy:<设备名>"）反查它的 convId。
    // 为什么要反查：ClearConversation 按 **convId**（哈希）匹配，而调用方
    // （旧客户端）手里只有"设备名"——两者不是同一个键。**早期版本直接把
    // legacy:<dev> 当 convId 传进去，removed 恒为 0，且不报错。**
    std::string ConvIdOfPeer(const std::string& peerDeviceId) const;
    /** 由 convId 反查对端标识（清空会话时用来一并清掉它的传输记录） */
    std::string PeerOfConv(const std::string& convId) const;
    int  PruneHistory(int days);        // 删掉早于 days 天的消息，返回删掉条数

    // ---- 文件保存目录（默认目录，直接落盘，不建子文件夹） ----
    void SetSaveDir(const std::wstring& dir);   // 写回 ini 并立即生效

    // ---- 对端设备名 ----
    std::string PeerName();
    void SetPeerName(const std::string& name);

    // ---- 正在进行的传输（给网页显示 大小/进度/速度） ----
    long long BeginTransfer(const std::string& name, long long total, const std::string& dir,
                            const std::string& peer = std::string(),
                            const std::string& xid = std::string());
    void UpdateTransfer(long long id, long long received);
    /** 结束一条：**保留**记录并写终态（ok=false → failed），不再 erase */
    void EndTransfer(long long id, bool ok = true, const std::string& error = std::string());
    /** 取消一条在传任务（返回 false = 没有这条或已结束） */
    bool CancelTransfer(long long id);
    /** 删除一条记录（在传的先取消再删） */
    bool RemoveTransfer(long long id);
    /** 分片上传的取消标记：收片循环每片问一次 */
    bool IsTransferCancelled(const std::string& xid);
    /** 清空某个会话的传输记录（"清空会话"时调用） */
    int  DropTransfersForPeer(const std::string& peerId);
    /** 清空全部传输记录（"清空全部历史"时调用） */
    int  DropAllTransfers();
    std::string TransfersJson();
    /** 启动时把上次的传输记录读回来（跨重启保留） */
    void LoadTransfers();

    // ---- 出站两步（T05）----
    // 先分配身份（msgId/epoch/seq，seq 落盘），再决定投递结果，最后一次性入库。
    // 为什么不分两次写：如果先按 sent 写进历史、失败后再改成 failed，文件要重写一遍，
    // 而且进程在两步之间崩溃会留下一条"看起来已发送"的假记录。
    bool BeginOutbound(const std::string& peerDeviceId, ShareMessage& msg);
    long long FinishOutbound(ShareMessage msg, const std::string& status);   // 返回库内 id
    bool FindByMsgId(const std::string& msgId, ShareMessage& out) const;
    // 只给文件用：AddFile 已经落盘并入库，投递失败时补一次状态修正
    bool SetMessageStatus(const std::string& msgId, const std::string& status);

    long long   AddText(const std::string& text, const std::string& dir, const std::string& dev,
                        const std::string& peerDeviceId = std::string(),
                        const std::string& msgIdIn = std::string(),
                        std::string* outMsgId = nullptr);
    ShareMessage AddFile(const std::wstring& tempPath,
                         const std::string& fileName,
                         const std::string& mime,
                         const std::string& dir,
                         const std::string& dev,
                         const std::string& peerDeviceId = std::string(),
                         const std::string& msgIdIn = std::string());
    void        DiscardFile(const std::wstring& tempPath);

    // ---- 删除设备（用户显式动作，M1 之后新增）----
    // 与"清空会话"的区别：清空只清记录、保留会话；删除是把设备/会话/目录一起清掉。
    // 目录必须先给用户看清楚（路径 + 文件数 + 总大小）再删，所以拆成"查"和"删"两步。
    bool RemoveConversation(const std::string& convId);          // 连同会话条目一起移除
    // 对端换了 deviceId（重装/清配置）时，旧身份留下的会话要跟着清掉，
    // 否则设备表里合并了、列表里还会多出几行同名会话。
    void DropPeerHistory(const std::string& peerId);
    bool DeviceFolderInfo(const std::string& peerId, const std::string& displayName,
                          std::wstring& pathOut, int& fileCount, long long& bytesOut);
    bool DeviceFolderRemove(const std::string& peerId, const std::string& displayName, int& removedFiles);
    std::wstring ShareDirPath() const { return m_shareDir; }

    // ---- 消息模型（T03）----
    static std::string MakeMsgId(const std::string& deviceId, long long epoch, long long seq);
    // legacy 会话键的**唯一产出点**。
    //
    // 为什么要收在一个函数里：这个键曾经在 HttpServer 和 LoadHistory 里各拼了一次，
    // 一处拼成 legacy:<dev>、另一处拼成 unknown:<dev>，后果是**老客户端升级后拉自己
    // 那段历史一条都拉不到，而且不报错**。跨模块共享的键只许有一处产出。
    static std::string MakeLegacyKey(const std::string& dev);
    // 名字 -> deviceId。设备表里查不到时返回稳定占位（"unknown:<name>"），
    // 绝不返回空串：空串会让 convId 退化成"自己和自己的会话"。
    std::string ResolvePeerDeviceId(const std::string& nameOrId);
    std::string SelfDeviceId();
    // 按 per-peer 水位 / 全局 id 拉取；结果按 (peerTs, seq) 排序
    std::vector<ShareMessage> QueryMessages(const MessageQuery& q);
    // 会话列表（convId / deviceId / displayName / 最后一条时间 / unread / histRev 预留）
    std::string ConversationsJson();

    // 长轮询：阻塞最多 waitSeconds 秒，返回 id > since 的全部消息（老语义，保留）
    std::vector<ShareMessage> WaitMessages(long long since, int waitSeconds);

    std::wstring FilePathById(const std::string& fileId);
    bool         FileInfoById(const std::string& fileId, std::string& name, std::string& mime);

    int  UnreadCount();
    void ClearUnread();

    // 给主程序 UI 线程用的非阻塞版本：抢不到锁就返回 false，
    // 调用方沿用上一次的值。绝对不能让 UI 线程等锁——等锁会连累任务栏重绘。
    bool TryUnreadCount(int& out);

    static std::string ToJson(const ShareMessage& m);

private:
    TransferHub() = default;
    std::wstring NewFileId();
    std::wstring SafeFileName(const std::string& name);
    // 第 index 个候选路径：0 = 原名，1 = "name (2)"，2 = "name (3)"……
    static std::wstring CandidatePath(const std::wstring& dir, const std::wstring& safeName, int index);
    // 对端键 → 保存目录下的子目录名，形如 "SANQIUVM-pc-c31db3428a2f"。
    // 带显示名是为了让用户在资源管理器里认得出是谁；deviceId 后缀保证唯一且改名后仍能找回。
    static std::wstring DeviceFolderName(const std::string& peerId, const std::string& displayName);
    // 解析该对端应该用哪个子目录：**已经存在的就沿用**（否则改名会留下一个孤儿目录，
    // 用户看到的旧文件突然"消失"）。兼容早期只有 deviceId 的目录名。
    static std::wstring ResolveDeviceFolder(const std::wstring& shareDir, const std::string& peerId,
                                            const std::string& displayName);
    // 在 dir 里找与 tempPath 内容完全相同（尺寸 + 64 位哈希）的文件，命中返回其路径。
    // 用户要的是"同一个文件只留一份"：命中就不要落第二份，也不要加 (2)。
    bool FindIdenticalFile(const std::wstring& dir, const std::wstring& tempPath, std::wstring& foundOut);
    // 把临时文件**原子地**放进 dir：目标已存在就让这次调用失败并换下一个序号，
    // 不做"先查存在再改名"——那中间有窗口，两端同时发同名文件时必丢一份。
    bool PlaceFileAtomic(const std::wstring& tempPath, const std::wstring& dir,
                         const std::string& name, std::wstring& outPath);
    // 显式 peer 优先，否则按设备名解析。**文件目录与会话键必须走同一个函数**，
    // 否则会出现"消息进了 A 会话、附件落在 B 目录"。
    std::string PeerKeyLocked(const std::string& explicitPeer, const std::string& devName);
    void AppendHistory(const ShareMessage& m);          // 追加一行 JSON
    void RewriteHistory();                              // 按当前 m_messages 重写整个文件
    void LoadHistory();                                 // 启动时读回
    void LoadConvRevs();                                // histRev 落盘：重启后接着涨
    void SaveConvRevsLocked();
    void EnsureConvLocked(const std::string& convId, const std::string& peerId = std::string(),
                          const std::string& lastKnownName = std::string());
    static bool ParseHistoryLine(const std::string& line, ShareMessage& out);

    // 给消息补上身份（msgId/epoch/seq/convId/from/to/status）。
    // 调用方已持有 m_mutex。返回 false = 同 msgId 已存在（幂等，不重复入库）。
    bool AssignIdentityLocked(ShareMessage& m, const std::string& peerDeviceId,
                              const std::string& msgIdIn, bool outgoing);
    void IndexMessageLocked(const ShareMessage& m);
    void TrimLocked();
    void LogConflictLocked(const std::string& msgId, const ShareMessage& kept, const ShareMessage& dropped);

    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;

    std::deque<ShareMessage>            m_messages;
    std::map<std::string, std::wstring> m_filePaths;   // fileId -> 绝对路径
    // 内容去重用的哈希缓存：绝对路径 -> (尺寸, 最后写时间, 哈希)。
    // 带 size+mtime 是为了失效：文件被外部改过就必须重算，否则会复用错文件。
    struct FileHashEntry { long long size = 0; long long writeTime = 0; std::string hash; };
    std::map<std::string, FileHashEntry> m_hashCache;
    long long m_nextId = 1;

    unsigned short m_port = 8765;          // ini / TMLS_PORT 给出的基准端口（"意图"）
    std::atomic<unsigned short> m_runtimePort{ 0 };  // 实际绑定成功的端口；0 = 还没定
    std::wstring   m_shareDir;
    std::wstring   m_iniPath;
    std::wstring   m_dataDir;              // devices.json / history.jsonl 的根（TMLS_DATA_DIR）
    std::wstring   m_historyPath;
    std::string    m_ip;
    std::string    m_deviceName;
    std::string    m_deviceId;             // 仅作"设备表不可用"时的兜底缓存
    std::string    m_mac;                  // 惰性计算并缓存
    DeviceRegistry* m_registry = nullptr;  // 空 = 用 DeviceRegistry::Instance()
    std::string    m_peerName;      // 最近一次连上来的手机设备名
    std::map<std::string, long long> m_msgIdToId;   // msgId -> 全局 id（去重唯一索引）
    // per-conv 历史版本号（M1 内存态；bump 见 docs/07 T07：清该会话/清全部/按天裁剪）。
    // **同一个 conv 在 /api/devices、/api/conversations、/api/messages 必须同源**，
    // 所以只存在这里，由三处共同读取。
    // convId -> {peerDeviceId, lastKnownName, rev}。
    // convId 是两方 deviceId 的排序哈希，**反推不回来**，所以对端身份必须存；
    // lastKnownName 取"消息入库那一刻"的显示名：设备表后来改名或删了设备，
    // 会话行仍能显示一个名字，而不是变成"未知设备"让人以为对话坏了。
    struct ConvMeta
    {
        std::string peerId;
        std::string lastKnownName;
        int         rev = 0;
    };
    std::map<std::string, ConvMeta> m_convs;
    std::wstring m_convPath;      // per-conv histRev 的落盘文件
    std::wstring   m_conflictPath;  // 同 msgId 内容不一致时写这里，不抛异常
    int            m_historyDays = 0;   // 0 = 不自动删除
    bool           m_inited = false;
    bool           m_stopping = false;

    // 传输进度用独立互斥量：千万别和 m_mutex 混用，
    // 否则每 64KB 更新一次进度会跟 UI 线程抢锁。
    struct TransferInfo
    {
        std::string name;
        std::string dir;
        long long   total = 0;
        long long   received = 0;
        long long   started = 0;
        // 记录要留着（用户要能回看"传过什么"），所以必须有终态而不是结束时 erase
        std::string status = "active";   // active / done / failed / cancelled
        std::string peer;                // 对端 deviceId（清空会话时按它整批清）
        std::string xid;                 // 分片上传的续传标识（取消时按它找）
        std::string error;
        long long   ended = 0;
    };
    std::mutex                     m_trMutex;
    std::map<long long, TransferInfo> m_transfers;
    long long                      m_nextTransferId = 1;
    std::wstring                   m_transfersPath;

    /** 落盘（调用方必须已持 m_trMutex） */
    void SaveTransfersLocked();
};
