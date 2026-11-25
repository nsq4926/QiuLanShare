#pragma once

// 宿主能力桥：把「exe 客户端独有」的能力从共享核心里剥出来。
//
// 为什么需要：HttpServer / index.html 是 TrafficMonitor 插件和 LanShare.exe
// 共用的。开机自启、系统通知这类能力只对独立 exe 有意义。若直接在核心里
// 写死，插件进程会跟着读写注册表、弹气泡。
//
// 约定：插件宿主**不注册**任何 hook，所有函数都是空实现 —— 插件行为与
// 本文件出现之前逐字节一致。exe 在 AppStartCore() 之前注册。

#include <functional>
#include <string>

namespace tml
{
namespace host
{
    struct IncomingNotice
    {
        std::string kind;      // text / image / file
        std::string dev;       // 发送方显示名
        std::string preview;   // 预览文本（文件/图片为 "[文件] xxx" 形式）
        std::string convId;    // 所属会话。通知点击后要能**定位到这条消息所在会话**
    };

    struct Hooks
    {
        // 收到一条**入站新消息**时触发（重复提交不触发）
        std::function<void(const IncomingNotice&)> onIncoming;
        // 查询开机自启。返回 false = 当前宿主不支持该能力
        std::function<bool(bool& enabled)> getAutostart;
        // 设置开机自启。返回是否成功
        std::function<bool(bool enable)> setAutostart;
    };

    // 函数内 static：跨 DLL / exe 各自一份，无需导出符号，插件与 exe 互不干扰。
    inline Hooks& GetHooks()
    {
        static Hooks h;
        return h;
    }

    inline void NotifyIncoming(const IncomingNotice& n)
    {
        const auto& f = GetHooks().onIncoming;
        if (f) f(n);
    }
}
}
