// 核心服务装配：与插件 CLanSharePlugin::EnsureStarted() 同一套顺序。
// 这里的每一条顺序约束都来自插件踩过的坑，改动前先读 Plugin.cpp 的注释。
#include "App.h"
#include "../src/TransferHub.h"
#include "../src/HttpServer.h"
#include "../src/InstanceLock.h"
#include "../src/Discovery.h"
#include "../src/DeviceRegistry.h"
#include "../src/Common.h"

#include <string>
#include <vector>

namespace
{
    // exe 绿色版：数据目录显式锁到 exe 同级 LanShare\。
    // 通过环境变量传给 InstanceLock，它认 TMLS_DATA_DIR，且 explicit 模式**不做迁移**
    // ——这是对的：exe 是全新形态，不该去搬插件的数据（那会搅乱插件的 deviceId）。
    bool SetPortableDataDir()
    {
        const std::wstring dataDir = tml::JoinPath(tml::GetModuleDir(), L"LanShare");
        if (!tml::EnsureDir(dataDir)) return false;
        return ::SetEnvironmentVariableW(L"TMLS_DATA_DIR", dataDir.c_str()) != FALSE;
    }

    // 写日志到数据目录，绿色版排障用（静默失败是本项目最主要的 bug 形态）
    void Log(const std::string& line)
    {
        const std::wstring path = tml::JoinPath(tml::GetModuleDir(), L"LanShare\\lanshare-app.log");
        tml::AppendLogLine(path, line);
    }
}

unsigned short AppStartCore()
{
    // SetSelfModule 必须在任何 GetModuleDir() 之前：exe 的模块句柄就是 hInstance，
    // 由 WinMain 传入。核心内部所有相对路径都基于它。
    // （在 WinMain 里已调用，这里不重复。）

    if (!SetPortableDataDir())
    {
        Log("failed to set portable data dir");
        return 0;
    }

    tml::EnsureWinsock();

    InstanceLock& lock = InstanceLock::Instance();
    const InstanceLockResult lr = lock.Acquire();
    if (!lr.primary)
    {
        // 已有实例（插件或另一个 exe）在监听。exe 不抢占、不做"纯客户端"降级——
        // 那套语义是给宿主里多个 TrafficMonitor 进程用的；这里直接告知用户。
        Log("not primary: " + tml::WideToUtf8(lr.reason));
        return 0;
    }

    TransferHub& hub = TransferHub::Instance();
    DeviceRegistry& registry = DeviceRegistry::Instance();
    hub.AttachRegistry(&registry);
    registry.Init(lr.dataDir, lock.InstanceName());

    if (!hub.IsInited()) hub.Init(lr.iniPath, lr.dataDir, lr.dataDirExplicit);

    const tml::LocalIdentity ident = tml::GetLocalIdentity();
    registry.UpdateSelf(hub.DeviceName(), ident.mac, ident.ips,
                        ident.filteredNicCount, ident.keptNicCount);

    // 换过 deviceId 的旧身份按 MAC 合并（与插件同构）
    {
        std::vector<std::string> stale;
        const int merged = registry.DedupeByMac(&stale);
        for (size_t i = 0; i < stale.size(); ++i) hub.DropPeerHistory(stale[i]);
        if (merged > 0) Log("dedupe by mac: " + std::to_string(merged));
    }

    const std::vector<unsigned short> candidates = lock.PortCandidates();
    unsigned short bound = 0;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (HttpServer::Instance().Start(candidates[i]))
        {
            bound = candidates[i];
            break;
        }
    }
    if (bound == 0)
    {
        Log("no port available: " + HttpServer::Instance().LastError());
        return 0;
    }

    hub.SetRuntimePort(bound);
    Discovery::Instance().Start(bound, lr.dataDir);
    lock.SetEffectivePort(bound);
    lock.WriteDisplayPort(bound);

    Log("core started on port " + std::to_string(bound));
    return bound;
}

void AppStopCore()
{
    // **必须幂等**：AppQuit() 与 wWinMain 正常退出路径都会调它，
    // 重复调用会二次 Release() 单实例锁 —— 锁被提前释放，
    // 下一个实例就能抢到锁，两个实例同时监听同一端口。
    // 本轮新增了"第二实例提前转发后退出"的路径，更容易踩到。
    static bool stopped = false;
    if (stopped) return;
    stopped = true;

    // 先停发现（不再 announce），再停 HTTP（不再收请求），最后释放锁。
    // 顺序反了会出现"端口已关但还在广播自己"的窗口。
    Discovery::Instance().Stop();
    HttpServer::Instance().Stop();
    InstanceLock::Instance().Release();
    TransferHub::Instance().Shutdown();
    Log("core stopped");
}
