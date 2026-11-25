#pragma once

#include <string>

namespace tml
{
    // Windows 防火墙入站放行：**检测 + 一键放行，不做静默自动加**。
    //
    // 为什么要做：插件监听 TCP 8765(占用则递进 8766/8767)、发现用 UDP 8766，
    // 而 Windows 防火墙默认拦入站。放行与否的后果是：
    //   收不到 announce（发现不到人）+ 对端主动发来的消息被拒（对方显示我们离线），
    // 而**出站默认允许** —— 于是表现为"我能发给他、他发不了我"这种最难解释的不对称。
    //
    // 三条不可协商的规矩（详见 docs/05 §3.5）：
    //   1. **不静默加规则**：必须用户点击确认后才写。
    //   2. **不改、不删用户已有规则**：重复执行只补缺失的那条。
    //   3. **非管理员不尝试提权**：只展示手动步骤，不弹 UAC。
    namespace Firewall
    {
        // 规则名与端口的**唯一出处**（docs/05 §3.5）。别处一律"照抄本处"，不得改写：
        // 用户是靠这两个字符串在防火墙里搜前缀、自查、手动清理的。
        extern const wchar_t* const kTcpRuleName;   // TrafficMonitorLanShare-TCP-In
        extern const wchar_t* const kUdpRuleName;   // TrafficMonitorLanShare-UDP-In
        extern const int kTcpPortLow;               // 8765
        extern const int kTcpPortHigh;              // 8767（= 现有端口递进阶梯）
        extern const int kUdpPort;                  // 8766（发现）

        struct Status
        {
            bool        blocked = false;      // 疑似入站被拦（**配置证据**：找不到任何允许入站的规则）
            bool        isAdmin = false;      // 当前进程是否管理员
            bool        ruleExists = false;  // 是否有允许入站的规则（我们的端口规则，或针对本程序的规则）
            bool        selfProbeOk = true;   // 自检（连本机 LAN IP:port）结果。**仅供参考**：
                                             // Windows 上本机回环通常绕过防火墙过滤，
                                             // 实测"没有任何放行规则时自检仍然通过"，
                                             // 所以它**不能作为主判据**。
            int         port = 0;             // 实际监听端口（从调用方传入，不硬编码）
            std::string lanIp;                // 用于自检的 LAN 地址
            std::string reason;               // 判定依据的简短说明（排障用）
        };

        // 当前进程是否以管理员身份运行。
        bool IsAdmin();

        // 判据 A（主动自检）：向**本机 LAN IPv4**（不是 127.0.0.1 —— loopback 通常豁免过滤）
        // 的监听端口做一次 TCP connect。
        // 连不上 => 本机入站被拦的**直接证据**；连得上 => 入站通。
        // 注意：同机连自己的 LAN IP 在部分 Windows 配置下会绕过过滤，
        // 所以这个信号**要配合 HasAllowRule 一起看**，两者一致才提示。
        bool ProbeSelfInbound(const std::string& lanIp, int port, int timeoutMs);

        // 我们那两条放行规则是否已经存在（netsh 查询）。
        bool HasAllowRule();

        // 一键放行：加缺失的那条规则。**已存在则跳过**（不动已有规则）。
        // 非管理员返回 false 并在 detail 里说明。
        // 返回值：true = 至少补上了一条或本来就已存在；detail 说明实际做了什么。
        bool AddAllowRules(std::string& detail);

        // 一次性判定：自检 + 规则查询，合成 Status。
        Status Inspect(const std::string& lanIp, int port);

        // 提示节流（防误报三闸之 ③）：**同一网络 24h 内最多一次**；
        // 用户选"不再提示"则永久记住。
        // 记在**插件自己的配置**里，不记设备表——它是**网络属性**，不是设备属性
        // （换一台设备不会让"这个网络该提示一次"重新开始）。
        bool ShouldPrompt(const std::wstring& iniPath, const std::string& networkKey, long long nowMs);
        void MarkPrompted(const std::wstring& iniPath, const std::string& networkKey, long long nowMs);
        void SetNeverPrompt(const std::wstring& iniPath);
    }
}
