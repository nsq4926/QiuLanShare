#pragma once

#include <string>
#include <vector>

// 单实例锁 + 端口候选的唯一决策点。
//
// 为什么需要它：插件是 DLL，由宿主（TrafficMonitor）加载。同一台机器上完全可能
// 同时存在多份宿主（正式版 + 便携版 + 调试版）。它们会抢同一个端口，更麻烦的是
// 会同时写同一份设备表 / 历史 —— 后者的表现是两个实例互相"串台"，
// 看起来像同步 bug，实际是数据目录没隔离。
//
// 真实落盘文件名（别按记忆写，按这里）：
//   <dataDir>\lanshare_devices[.<instance>].json   设备表（T02 起）
//   <dataDir>\lanshare_history.jsonl               消息历史
//   <dataDir>\lanshare_conflicts.log               同 msgId 内容冲突（T03 起）
//   <dataDir>\lanshare.<instance>.lock             实例文件锁
//
// 规则：只有第一个实例监听；其余只作客户端，不监听、不抢端口。
struct InstanceLockResult
{
    bool           primary = false;    // true = 本实例应监听；false = 只作客户端
    std::string    instanceName;       // TMLS_INSTANCE，缺省 "default"
    std::wstring   iniPath;            // TMLS_INI 或 <模块目录>\lanshare.ini
    std::wstring   dataDir;            // TMLS_DATA_DIR 显式指定，或 %LOCALAPPDATA%\TM-LAN-Share[\\<实例>]
    bool           dataDirExplicit = false;  // true = TMLS_DATA_DIR 指定的（测试用法，不做迁移）
    unsigned short basePort = 8765;    // ini 的 [server] port 或 TMLS_PORT
    bool           portFixed = false;  // TMLS_PORT 显式指定 >0：只试这一个，不递进
    std::wstring   lockName;           // 实际使用的互斥名（排障用）
    std::wstring   reason;             // 非 primary 的原因 / 降级说明
};

class InstanceLock
{
public:
    static InstanceLock& Instance();

    // 取锁。可重复调用，内部缓存结果。
    // TMLS_ALLOW_MULTI=1 时强制 primary（仅供测试脚本，生产路径不得依赖）。
    InstanceLockResult Acquire();

    // 候选端口：basePort → basePort+1 → basePort+2（最多三个）。
    // portFixed 为真时只有一个候选。
    std::vector<unsigned short> PortCandidates() const;

    // 实例名（TMLS_INSTANCE）。/api/hello 会上报，多实例测试时靠它区分"是谁在应答"。
    std::string InstanceName() const;

    unsigned short EffectivePort() const;
    void           SetEffectivePort(unsigned short port);

    // 把实际监听端口写回 ini 的 [server] last_port。
    //
    // 故意不覆盖 [server] port：那是用户配置的"意图"。覆盖它会让临时占用变成
    // 永久迁移——8765 被别的程序占用一次，之后就再也回不到 8765 了。
    // last_port 只供任务栏/日志显示；对端一律以 /api/hello 的 port 为准。
    bool WriteDisplayPort(unsigned short port);

    void Release();

private:
    InstanceLock() = default;

    bool AcquireMutex(std::wstring& outName, std::wstring& outReason);
    bool AcquireFileLock(const std::wstring& instanceName, std::wstring& outReason);
    unsigned short ReadIniPort(const std::wstring& iniPath) const;

    bool               m_acquired = false;
    InstanceLockResult m_result;
    unsigned short     m_effectivePort = 0;

    // 故意用 void* 承载 HANDLE，避免头文件把 windows.h 传播给所有包含者
    void* m_mutex = nullptr;
    void* m_lockFile = nullptr;
};
