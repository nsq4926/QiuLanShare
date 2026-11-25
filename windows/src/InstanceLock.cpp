#include "InstanceLock.h"
#include "Common.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    // T07b：把数据目录从"TrafficMonitor 的 plugins 目录"搬到
    // "%LOCALAPPDATA%\TM-LAN-Share"。搬的是**文件本身**，所以 deviceId 逐字符不变。
    //
    // 三条硬规则：
    //   1. **复制**而不是移动 —— 旧文件保留（用户可能回退版本，用户自己也会清理它，
    //      我们不能替他删）；
    //   2. 任何一步失败 → **整体回滚**（删掉本次拷过去的）并退回旧目录继续用：
    //      宁可位置不对，也不能因为挪不动而丢掉身份；
    //   3. 新位置已有同名文件时**不覆盖** —— 那说明已经迁过了，重复迁会把新数据覆盖掉。
    bool MigrateDataDir(const std::wstring& oldDir, const std::wstring& newDir, const std::wstring& instanceName)
    {
        if (oldDir.empty() || newDir.empty()) return false;
        if (::_wcsicmp(oldDir.c_str(), newDir.c_str()) == 0) return true;
        if (!tml::EnsureDir(newDir)) return false;

        std::vector<std::wstring> names;
        names.push_back(L"lanshare_devices.json");
        if (!instanceName.empty() && instanceName != L"default")
            names.push_back(L"lanshare_devices." + instanceName + L".json");
        names.push_back(L"lanshare_convs.json");
        names.push_back(L"lanshare_history.jsonl");
        names.push_back(L"lanshare_conflicts.log");
        names.push_back(L"lanshare.log");

        std::vector<std::wstring> copied;
        bool ok = true;
        for (size_t i = 0; i < names.size() && ok; ++i)
        {
            const std::wstring src = tml::JoinPath(oldDir, names[i]);
            const std::wstring dst = tml::JoinPath(newDir, names[i]);
            if (!tml::FileExists(src)) continue;      // 旧位置没有 → 不用搬
            if (tml::FileExists(dst)) continue;        // 新位置已有 → 不覆盖
            if (!tml::CopyFileTo(src, dst)) { ok = false; break; }
            copied.push_back(dst);
        }
        if (!ok)
        {
            for (size_t i = 0; i < copied.size(); ++i) ::DeleteFileW(copied[i].c_str());
            return false;
        }
        if (!copied.empty())
        {
            tml::AppendLogLine(tml::JoinPath(newDir, L"lanshare.log"),
                                "data dir migrated from " + tml::WideToUtf8(oldDir) +
                                " (" + std::to_string(copied.size()) + " file(s)); old files kept");
        }
        return true;
    }

    // 读环境变量；不存在或过长都返回空
    std::wstring ReadEnv(const wchar_t* name)
    {
        wchar_t buf[1024] = { 0 };
        const DWORD n = ::GetEnvironmentVariableW(name, buf, static_cast<DWORD>(_countof(buf)));
        if (n == 0 || n >= _countof(buf)) return std::wstring();
        return std::wstring(buf);
    }

    // Global\ 需要 SeCreateGlobalPrivilege。TrafficMonitor 通常以管理员运行，
    // 但便携版/调试版未必，所以名字前缀要能退化（见 AcquireMutex）。
    std::wstring BuildMutexName(const wchar_t* scope, const std::wstring& instance)
    {
        std::wstring name(scope);
        name += L"TMLanShare";

        // instance == "default" 时与旧版名字完全一致，保证向后兼容
        if (!instance.empty() && instance != L"default")
        {
            name += L"_";
            name += instance;
        }
        return name;
    }
}

InstanceLock& InstanceLock::Instance()
{
    // 与插件对象同样故意不释放：锁的生命周期就是进程生命周期
    static InstanceLock* inst = new InstanceLock();
    return *inst;
}

unsigned short InstanceLock::ReadIniPort(const std::wstring& iniPath) const
{
    wchar_t buf[64] = { 0 };
    ::GetPrivateProfileStringW(L"server", L"port", L"8765", buf, static_cast<DWORD>(_countof(buf)), iniPath.c_str());
    const int port = _wtoi(buf);
    if (port > 0 && port < 65536) return static_cast<unsigned short>(port);
    return 8765;
}

InstanceLockResult InstanceLock::Acquire()
{
    if (m_acquired) return m_result;

    // ---------------- 环境变量（T08 多实例测试的地基） ----------------
    std::wstring envInstance = ReadEnv(L"TMLS_INSTANCE");
    if (envInstance.empty()) envInstance = L"default";

    const std::wstring envIni = ReadEnv(L"TMLS_INI");
    const std::wstring envData = ReadEnv(L"TMLS_DATA_DIR");
    const std::wstring envPort = ReadEnv(L"TMLS_PORT");
    const std::wstring envMulti = ReadEnv(L"TMLS_ALLOW_MULTI");

    m_result.instanceName = tml::WideToUtf8(envInstance);
    m_result.iniPath = envIni.empty() ? tml::JoinPath(tml::GetModuleDir(), L"lanshare.ini") : envIni;

    // 插件改名（tm_lan_share.ini → lanshare.ini）时把老配置搬过来，
    // 否则用户之前设的端口/保存目录会"莫名其妙回到默认"。
    if (::GetFileAttributesW(m_result.iniPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        const std::wstring oldIni = tml::JoinPath(tml::GetModuleDir(), L"tm_lan_share.ini");
        if (::GetFileAttributesW(oldIni.c_str()) != INVALID_FILE_ATTRIBUTES)
            tml::CopyFileTo(oldIni, m_result.iniPath);
    }

    // ---------------- 数据目录（T07b）----------------
        // 数据目录：默认落在 <TrafficMonitor>\plugins\LanShare（2026-11-15 按用户要求改）。
        // 之前落过两个位置，**两个都必须能搬**：
        //   - %LOCALAPPDATA%\TM-LAN-Share（上一版默认）
        //   - plugins 目录本身（最早一版）
        // 换目录不搬身份 = deviceId 重建 = 对端眼里变成新设备、旧会话全断，
        // 这是这类改动里唯一不能出错的一步。
        if (!envData.empty())
        {
            // 测试/高级用法：原样使用，不迁移、不解释。
            m_result.dataDir = envData;
            m_result.dataDirExplicit = true;
        }
        else
        {
            const std::wstring newDir = tml::DefaultDataDir(envInstance);
            const std::wstring prevDir = tml::LegacyDataDir(envInstance);
            const std::wstring origDir = tml::GetModuleDir();

            bool moved = false;
            if (!prevDir.empty()) moved = MigrateDataDir(prevDir, newDir, envInstance);
            if (!moved) moved = MigrateDataDir(origDir, newDir, envInstance);

            // 搬不动（新位置不可写）就退回上一版的位置继续用：宁可位置不对，不能丢身份。
            m_result.dataDir = moved ? newDir : (prevDir.empty() ? origDir : prevDir);
        }

    // 数据目录可能还不存在（测试脚本常指向临时目录）
    tml::EnsureDir(m_result.dataDir);

    // 老文件名的数据（tm_lan_share*）→ lanshare*：先复制再让后面的加载走新名字。
    // 不做这一步，设备表"消失" → deviceId 重建 → 对端眼里变新设备、旧会话全断。
    tml::MigrateLegacyDataFiles(m_result.dataDir);

    // ini：剥 BOM 之后统一转成 UTF-16LE。**中文路径必须这么做**——
    // UTF-8 的 `E:\临时` 会被 GetPrivateProfileStringW 按 GBK 读成 `E:\涓存々`，
    // 保存目录于是指向一个不存在的路径，且不报任何错。
    tml::StripIniBom(m_result.iniPath);
    tml::NormalizeIniToUnicode(m_result.iniPath);

    // ---------------- 端口 ----------------
    unsigned short basePort = ReadIniPort(m_result.iniPath);
    bool portFixed = false;
    if (!envPort.empty())
    {
        const int v = _wtoi(envPort.c_str());
        if (v > 0 && v < 65536)
        {
            basePort = static_cast<unsigned short>(v);
            portFixed = true;
        }
        // TMLS_PORT=0 → 明确要求"走递进"，基准端口仍取 ini
    }
    m_result.basePort = basePort;
    m_result.portFixed = portFixed;

    // ---------------- 测试后门（仅测试脚本可用） ----------------
    // 必须在取锁之前判断：测试要的正是"两个实例同时存在"。
    if (envMulti == L"1")
    {
        m_result.primary = true;
        m_result.lockName = L"(skipped: TMLS_ALLOW_MULTI=1)";
        m_result.reason = L"TMLS_ALLOW_MULTI=1，已跳过单实例锁（仅测试）";
        m_acquired = true;
        return m_result;
    }

    // ---------------- 互斥 + 文件锁双保险 ----------------
    std::wstring mutexName;
    std::wstring mutexReason;
    const bool mutexOk = AcquireMutex(mutexName, mutexReason);
    m_result.lockName = mutexName;

    bool fileOk = false;
    std::wstring fileReason = L"未尝试（互斥已失败）";
    if (mutexOk)
    {
        fileOk = AcquireFileLock(envInstance, fileReason);
    }

    if (mutexOk && fileOk)
    {
        m_result.primary = true;
        m_result.reason = mutexReason;   // 可能是"Global\ 不可用，已退为 Local\"
    }
    else
    {
        m_result.primary = false;
        m_result.reason = mutexOk ? fileReason : mutexReason;
        Release();   // 别留着半个锁：否则真正的第二实例会以为自己不是第一
    }

    m_acquired = true;
    return m_result;
}

bool InstanceLock::AcquireMutex(std::wstring& outName, std::wstring& outReason)
{
    const std::wstring instance = tml::Utf8ToWide(m_result.instanceName);

    const std::wstring globalName = BuildMutexName(L"Global\\", instance);
    HANDLE h = ::CreateMutexW(nullptr, TRUE, globalName.c_str());
    DWORD err = (h == nullptr) ? ::GetLastError() : ERROR_SUCCESS;

    if (h == nullptr && (err == ERROR_ACCESS_DENIED || err == ERROR_PRIVILEGE_NOT_HELD))
    {
        // 非管理员拿不到 Global\ 命名空间：退到 Local\，跨 Session 由文件锁兜住
        const std::wstring localName = BuildMutexName(L"Local\\", instance);
        h = ::CreateMutexW(nullptr, TRUE, localName.c_str());
        err = (h == nullptr) ? ::GetLastError() : ERROR_SUCCESS;
        if (h != nullptr)
        {
            outName = localName;
            outReason = L"Global\\ 命名空间不可用（非管理员？），已退为 Local\\ + 文件锁";
            if (err == ERROR_ALREADY_EXISTS)
            {
                ::CloseHandle(h);
                outReason = L"另一实例已持有互斥 " + localName;
                return false;
            }
            m_mutex = h;
            return true;
        }
    }

    if (h == nullptr)
    {
        outName = globalName;
        outReason = L"CreateMutex 失败，错误码 " + std::to_wstring(err);
        return false;
    }

    outName = globalName;
    if (err == ERROR_ALREADY_EXISTS)
    {
        ::CloseHandle(h);
        outReason = L"另一实例已持有互斥 " + globalName;
        return false;
    }

    m_mutex = h;
    outReason = L"已取得互斥 " + globalName;
    return true;
}

bool InstanceLock::AcquireFileLock(const std::wstring& instanceName, std::wstring& outReason)
{
    // 文件名带实例名：不同 TMLS_INSTANCE 属于"不同实例"，必须能同时运行（T08 依赖）。
    std::wstring fileName = L"lanshare.";
    fileName += instanceName;
    fileName += L".lock";

    const std::wstring path = tml::JoinPath(m_result.dataDir, fileName);

    // 共享模式 0 = 独占。第二个进程会拿到 ERROR_SHARING_VIOLATION。
    // 进程崩溃时系统自动释放，不会留下需要人工清理的死锁文件。
    HANDLE h = ::CreateFileW(path.c_str(),
                             GENERIC_READ | GENERIC_WRITE,
                             0,
                             nullptr,
                             OPEN_ALWAYS,
                             FILE_ATTRIBUTE_HIDDEN,
                             nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        const DWORD err = ::GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION)
            outReason = L"锁文件已被另一实例独占：" + path;
        else
            outReason = L"打开锁文件失败（错误码 " + std::to_wstring(err) + L"）：" + path;
        return false;
    }

    m_lockFile = h;
    outReason = L"已取得文件锁 " + path;
    return true;
}

std::vector<unsigned short> InstanceLock::PortCandidates() const
{
    std::vector<unsigned short> out;

    const unsigned short base = (m_result.basePort == 0) ? 8765 : m_result.basePort;
    out.push_back(base);

    // TMLS_PORT 显式指定端口时不递进：测试脚本要的是"我要这个端口"，不是"随便给一个"
    if (!m_result.portFixed)
    {
        for (unsigned int i = 1; i <= 2; ++i)
        {
            const unsigned int next = static_cast<unsigned int>(base) + i;
            if (next <= 65535) out.push_back(static_cast<unsigned short>(next));
        }
    }
    return out;
}

unsigned short InstanceLock::EffectivePort() const
{
    return m_effectivePort;
}

std::string InstanceLock::InstanceName() const
{
    if (!m_result.instanceName.empty()) return m_result.instanceName;
    return std::string("default");
}

void InstanceLock::SetEffectivePort(unsigned short port)
{
    m_effectivePort = port;
}

bool InstanceLock::WriteDisplayPort(unsigned short port)
{
    if (!m_acquired || !m_result.primary || m_result.iniPath.empty()) return false;

    wchar_t buf[16] = { 0 };
    _itow_s(static_cast<int>(port), buf, _countof(buf), 10);

    const BOOL ok = ::WritePrivateProfileStringW(L"server", L"last_port", buf, m_result.iniPath.c_str());

    // 实例名也记一笔：排障时能直接看出"是谁在监听这个端口"
    const std::wstring instanceW = tml::Utf8ToWide(m_result.instanceName);
    ::WritePrivateProfileStringW(L"server", L"instance", instanceW.c_str(), m_result.iniPath.c_str());

    return ok != FALSE;
}

void InstanceLock::Release()
{
    if (m_lockFile != nullptr)
    {
        ::CloseHandle(static_cast<HANDLE>(m_lockFile));
        m_lockFile = nullptr;
    }
    if (m_mutex != nullptr)
    {
        // 先释放所有权再关闭：CreateMutex 时已 RequestOwnership，
        // 不 Release 直接 Close 虽然系统也会回收，但会留下"放弃"语义，排障时难看
        ::ReleaseMutex(static_cast<HANDLE>(m_mutex));
        ::CloseHandle(static_cast<HANDLE>(m_mutex));
        m_mutex = nullptr;
    }
    m_result.primary = false;
}
