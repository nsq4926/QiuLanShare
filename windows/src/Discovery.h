#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 局域网自动发现（T06）。
//
// 三个通道并行，缺一个就会在特定网络下"看不见对方"：
//   1) 受限广播 255.255.255.255
//   2) **每张网卡各自的子网定向广播**（双网卡/多网段只发 255.255.255.255 会漏）
//   3) 组播 239.255.42.99（部分 AP 会丢广播但放过组播）
// 另有一个 TCP /24 兜底扫描，用来判别"AP 隔离"还是"防火墙"——两者症状相同、
// 处置完全相反，不能靠猜。
//
// 两条不能省的设计约束：
//   * announce 必须带**全量 ips[]**：同一台机器每张网卡各发一次，对端按 deviceId
//     合并，ips[] 取并集；否则双网卡机器会在对端变成两台。
//   * ips[] 要有 TTL：没再 announce 的地址必须摘掉，否则拔了网线那个地址会永远留着，
//     对端按 ①②③ 选地址时会一直去连一个不存在的地址。
class Discovery
{
public:
    static Discovery& Instance();

    // listenPort = 本机实际监听的 HTTP 端口（announce 里要报它，不能报 ini 里的意图值）
    // iniPath    = 配置来源；发现策略（主动发现/被发现/周期）存在它的 [discovery] 段
    void Start(unsigned short listenPort, const std::wstring& dataDir,
               const std::wstring& iniPath = std::wstring());
    void Stop();
    bool IsRunning() const { return m_running.load(); }

    // 发现策略（设置页三个开关的唯一生效点，别再在别处派生第二份判断）
    //   active       = 主动发现：周期广播 + 登记别人
    //   discoverable = 被发现：自报家门 + 回应别人（announce 里的 acceptsInbound）
    //   intervalMs   = 广播周期
    void SetOptions(bool active, bool discoverable, int intervalMs);
    bool ActiveDiscover() const { return m_active.load(); }
    bool Discoverable() const { return m_discoverable.load(); }
    int  IntervalMs() const { return m_intervalMs.load(); }
    // 选项 JSON：/api/hello 与 /api/discovery 共用，避免两处各写一份口径
    std::string OptionsJson() const;
    bool SaveOptions() const;      // 写回 ini（失败返回 false，调用方要如实回 ok:false）
    void LoadOptions();            // 启动时从 ini 读一次

    // 触发一次立即 announce，并返回已发现设备（JSON 数组，不含 ephemeral）
    std::string DiscoverJson();

    // 手动添加设备（兜底通道）：直接问 http://<ip>:<port>/api/hello，把对端登记进设备表。
    // 为什么必须有这条：AP 隔离 / 跨网段时广播与组播都收不到，自动发现彻底失效，
    // 而 PRD 要求此时仍能靠"扫码或手输 IP"连上——扫码是鸿蒙端的事，手输 IP 就在这里。
    // 失败原因写进 err：connect_fail / bad_hello / not_lanshare / self / ip_required
    bool AddPeerByAddress(const std::string& ip, unsigned short port, std::string& err,
                          std::string& deviceIdOut);

    // 子网广播的逐档自检报告（/8 /16 /22 /24 /25 /30 必须等于标准值，/31 /32 必须失败）。
    // 只在 TMLS_SELFTEST=1 时由 Start() 写盘，不影响生产路径。
    static std::string BroadcastSelfTest();

private:
    Discovery() = default;
    Discovery(const Discovery&) = delete;
    Discovery& operator=(const Discovery&) = delete;

    void Loop();
    void AnnounceOnce();
    void HandlePacket(const char* data, int len, const std::string& fromIp);
    void TickPrune();          // TTL 退役 + 连续无 announce 的状态降级
    void ProbeReachablePeers();   // 周期 TCP 健康探测（/api/hello），无副作用
    std::string BuildPayload() const;

    std::atomic<bool>  m_running{ false };
    std::thread        m_thread;
    std::uintptr_t     m_sock = 0;
    unsigned short     m_port = 0;
    std::wstring       m_dataDir;
    std::wstring       m_iniPath;

    // 发现策略：UI 可改，运行期只在这里生效
    std::atomic<bool>  m_active{ true };
    std::atomic<bool>  m_discoverable{ true };
    std::atomic<int>   m_intervalMs{ 3000 };

    // deviceId -> (ip -> 最近一次收到 announce 的时间)。只在内存里，不落盘：
    // 重启后第一轮 announce 会重新填满，落盘反而会带来"过期的活跃度"。
    mutable std::mutex m_mutex;
    std::map<std::string, std::map<std::string, long long>> m_announced;
    std::map<std::string, int> m_missTicks;   // 连续多少个 tick 没收到它的 announce
};
