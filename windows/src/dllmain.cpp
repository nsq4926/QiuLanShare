#include "Plugin.h"
#include "HttpServer.h"
#include "Common.h"

#include <windows.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:
        tml::SetSelfModule(hModule);
        ::DisableThreadLibraryCalls(hModule);
        // 刻意不在这里取单实例锁（InstanceLock）。
        //
        // DllMain 运行在加载器锁内，而在里面做 CreateMutex / CreateFile 会引入
        // 额外的句柄创建与文件系统调用：一旦触发加载新 DLL 或等待 I/O，
        // 就可能与持锁线程互相等死，表现是宿主启动即卡住。
        //
        // "取锁失败 → 只作客户端、不监听" 这条语义实现在
        // CLanSharePlugin::EnsureStarted()（由 OnInitialize 调用），
        // 那里已经在正常线程上下文，可以安全地做锁与端口判定。
        break;

    case DLL_PROCESS_DETACH:
        // 这里**什么都不能做**。
        // DllMain 运行在加载器锁内，此时调用 WSACleanup / 等待工作线程
        // （哪怕是 join 一下 accept 线程）都可能和正在收发数据的线程互相等死，
        // 表现就是主程序退不掉、任务栏窗口卡住，看起来像 explorer 无响应。
        // 进程即将销毁，套接字和线程由系统回收即可。
        //
        // 需要"干净关服"的场景（比如插件热重载），调 HttpServer::Stop() 的代码
        // 必须放在 DllMain 之外执行。
        (void)lpReserved;
        break;

    default:
        break;
    }
    return TRUE;
}

// TrafficMonitor 唯一要求的导出函数
extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    return &CLanSharePlugin::Instance();
}
