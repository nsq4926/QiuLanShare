#pragma once

#include <string>
#include <vector>
#include <cstdint>

// 命名空间刻意不叫 tm：<ctime> 会把 C 的 struct tm 引入全局作用域，
// 与之同名会报 C2757「具有该名称的符号已存在」。
namespace tml
{
    // ---------- 本 DLL 模块句柄（由 DllMain 传入） ----------
    void  SetSelfModule(void* hModule);
    void* GetSelfModule();

    // ---------- 编码 ----------
    std::string WideToUtf8(const std::wstring& w);
    std::wstring Utf8ToWide(const std::string& s);

    // ---------- 极简 JSON（只做本项目需要的读写） ----------
    std::string JsonEscape(const std::string& s);
    bool JsonGetString(const std::string& json, const std::string& key, std::string& out);
    bool JsonGetInt(const std::string& json, const std::string& key, long long& out);

    // 递归下降的最小 JSON 解析器。
    // 为什么需要它：设备表要读回"数组套对象"（peers[] / ips[]），子串扫描那套只够读
    // 扁平的单层键值，遇到嵌套就会把花括号/方括号当普通字符读错。
    // 只支持本项目用到的类型；小数按整数截断（我们的 schema 里没有小数）。
    struct JsonValue
    {
        enum Kind { Null, Bool, Number, String, Array, Object };

        Kind        kind = Null;
        bool        boolean = false;
        long long   number = 0;
        std::string text;                                      // String
        std::vector<JsonValue> items;                          // Array
        std::vector<std::pair<std::string, JsonValue>> members; // Object

        const JsonValue* Find(const std::string& key) const;
        std::string      GetString(const std::string& key, const std::string& def = std::string()) const;
        long long        GetInt(const std::string& key, long long def = 0) const;
        bool             GetBool(const std::string& key, bool def = false) const;
    };
    // 解析失败返回 false（调用方必须自己决定"坏文件"怎么处理，不许静默当成空表）
    bool JsonParse(const std::string& text, JsonValue& out);

    // ---------- URL ----------
    std::string UrlDecode(const std::string& s);
    std::string QueryParam(const std::string& query, const std::string& key);
    std::string UrlEncode(const std::string& s);

    // ---------- 网络 ----------
    std::string GetLanIPv4();
    // 与 GetLanIPv4() 同一张网卡的 MAC，形如 "AA:BB:CC:DD:EE:FF"；取不到返回空串
    std::string GetMacAddress();

    // 一张"可用"的 IPv4 网卡（四步过滤之后的结果）。
    // 这是产品侧与预验脚本共用的同一套判据：① Up 且 IPv4 有效（排除 127/8、
    // 169.254/16、0.0.0.0）② IfType ∈ {6,71} ③ 已知虚拟网卡名排除 ④ 唯一可用护栏。
    struct Ipv4Nic
    {
        std::string  ip;
        std::string  mac;          // AA:BB:CC:DD:EE:FF，取不到为空串
        unsigned int ifType = 0;   // 6 = Ethernet, 71 = Wireless80211
        int          prefixLen = 32;   // 子网前缀长度（同网段判定要用）
        bool         hasGateway = false;
        bool         guardrailKept = false;   // 名字像虚拟网卡，但它是本机唯一的可用 IPv4
        std::string  name;         // FriendlyName（用户可改）
        std::string  description;  // 驱动写死的描述
    };
    // 一次枚举的完整结果。ip / mac / 统计必须**同源**：分两次调用的话，两次之间
    // 网卡热插拔就会出现"真网卡的 IP + 虚拟网卡的 MAC"这种拼装组合，而对端会据此
    // 误报"这台设备换了网卡"。
    struct NicFilterResult
    {
        // ⚠ **不要拿这个列表的长度当"接口数"**：同一个适配器可以有多个 IPv4，
        //   在这里就会出现多行。按列表长度计数会退化成"按地址条数"，进而让
        //   唯一可用护栏判断错误（一张卡的两个 IP 被当成两张卡 → 护栏不触发）。
        //   接口数只看 keptNicCount / filteredNicCount，它们是在适配器遍历里数的。
        std::vector<Ipv4Nic> nics;      // 过滤后的可用接口，有网关者优先
        // **被过滤掉了几张网卡**（不是"剩下几张"！）
        //   只统计被步骤 2（IfType 不对）与步骤 3（虚拟网卡名）排除的接口；
        //   步骤 1（没有可用 IPv4，例如拔了网线只剩 169.254）**不计入**，否则数字虚高；
        //   护栏 guardrailKept 救回来算"保留"，同样不计入。
        // 名字叫 filtered、语义就必须是"被过滤"，反过来的话"带 VPN 的机器 count >= 1"
        // 这类断言会永真——永真的断言不会报红，只会让人以为验过了。
        int filteredNicCount = 0;
        int keptNicCount = 0;           // 保留下来参与广播的接口数（含护栏救回的）
    };
    NicFilterResult GetNicFilter();

    // 本机对外的身份三件套，一次枚举得出
    struct LocalIdentity
    {
        std::string ip;                 // 对外的 LAN 地址；取不到为空串
        std::string mac;                // 与 ip 出自同一张网卡
        std::vector<std::string> ips;   // 过滤后集合里的全部地址
        int filteredNicCount = 0;
        int keptNicCount = 0;
    };
    LocalIdentity GetLocalIdentity();

    // 进程级 winsock 初始化，只做一次，成功返回 true。
    // 不配对 WSACleanup：进程退出由系统回收，避免把宿主的引用计数减到 0。
    bool EnsureWinsock();
    std::wstring GetHostName();

    // 同网段判定（按前缀长度逐位比较，不是"前三段相同"那种近似）
    bool IsSameSubnet(const std::string& a, const std::string& b, int prefixLen);

    // 子网定向广播地址。
    // **返回 false 表示"这个前缀长度没有广播地址"**（/31 点对点、/32 单地址），
    // 调用方必须据此跳过该接口，不能"用公式算出来什么就发什么"——
    // 在 /31、/32 上公式会退化成一个看起来合法的地址，然后照发，谁也不会报错。
    bool SubnetBroadcast(const std::string& ip, int prefixLen, std::string& out);

    // ---------- 最小 HTTP 客户端（T05，不引第三方库） ----------
    // 只做本项目需要的事：一次请求、一次性读完响应、明确区分"连不上"与"连上但被拒"。
    struct HttpClientRequest
    {
        std::string  host;
        unsigned short port = 0;
        std::string  method = "GET";
        std::string  path = "/";
        std::string  contentType;
        std::string  body;          // 内存 body；与 bodyFile 二选一
        std::wstring bodyFile;      // 非空则把该文件流式发出去（大文件不整读进内存）
        long long    bodySize = 0;  // bodyFile 的长度（分片时 = 这一片的长度）
        // 从 bodyFile 的哪个偏移开始发：分片上传用同一个文件反复发不同段
        long long    bodyOffset = 0;
        int          timeoutMs = 4000;
        // 每个请求都带上的额外头（如 X-Device-Id）
        std::vector<std::pair<std::string, std::string>> headers;
    };

    struct HttpClientResponse
    {
        bool        ok = false;     // 拿到了完整的 HTTP 响应（不代表 200）
        int         status = 0;
        std::string body;
        // 失败原因分两类，调用方只能据此二选一：
        //   "timeout"      连接/读写超时
        //   "connect_fail" 连不上（拒绝、无路由、DNS 失败）
        //   "io"           连上之后收发中断
        std::string errorKind;
        int         lastError = 0;  // 原始 WSA 错误码，仅供日志
    };
    bool HttpPerform(const HttpClientRequest& req, HttpClientResponse& res);

    // ---------- 路径 ----------
    std::wstring GetModuleDir();
    std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
    std::wstring DefaultShareDir();
    // 数据目录（身份、会话、历史、冲突日志）——与"保存目录"（用户收到的附件）分开。
    // 为什么分开：保存目录是**用户的**，随时可能被清空；而设备表里存的是
    // **deviceId**，它一旦没了就会重新生成，对端随即把这个人当成新设备。
    // 数据目录放到 %LOCALAPPDATA% 下，也避开"TrafficMonitor 升级/重装会清掉 plugins 目录"。
    // instanceName 非 default 时再分子目录：不同实例各有一份历史，互不干扰。
    std::wstring DefaultDataDir(const std::wstring& instanceName);
    // ini 若被存成 UTF-8 with BOM，GetPrivateProfileStringW 读不到第一个 [section]，
    // **整份配置静默失效**（端口、保存目录全按默认走，一个错都不报）。读取前先去掉 BOM。
    void StripIniBom(const std::wstring& path);
    // 把 ini 统一成 UTF-16LE(带 BOM)。**中文路径必须这么做**：
    // GetPrivateProfileStringW 对 UTF-8 文件按 ANSI(GBK) 解读，`E:\临时` 会被读成
    // `E:\涓存々`，于是保存目录指向一个根本不存在的路径——而且不报任何错。
    void NormalizeIniToUnicode(const std::wstring& path);
    // 老的文件名（lanshare*）→ 新名字（lanshare*）。复制，不删旧文件。
    // 插件改名时必须先跑这个，否则设备表"消失"→ deviceId 重建 → 对端眼里变新设备。
    void MigrateLegacyDataFiles(const std::wstring& dir);
    // 弹系统目录选择器（STA）。返回空 = 用户取消。调用方自己保证不在 UI 线程上阻塞主程序。
    std::wstring PickFolder();    // 上一版默认数据目录（%LOCALAPPDATA%\TM-LAN-Share）。改默认目录时必须仍能找到它并搬过来。
    std::wstring LegacyDataDir(const std::wstring& instanceName);
    bool EnsureDir(const std::wstring& dir);
    bool CopyFileTo(const std::wstring& src, const std::wstring& dst);
    bool FileExists(const std::wstring& path);
    void AppendLogLine(const std::wstring& path, const std::string& line);
    bool ReadWholeFile(const std::wstring& path, std::string& out);
    bool WriteWholeFile(const std::wstring& path, const std::string& data);
    // 先写 <path>.tmp 再 MoveFileEx 原子替换。
    // 设备表讲究这个：断电/崩溃时留下"写了一半"的 JSON，比整个丢掉更糟——
    // 下次启动会解析失败，设备身份就断了，而对端已经记住了旧 ID。
    bool WriteWholeFileAtomic(const std::wstring& path, const std::string& data);

    // ---------- 杂项 ----------
    long long NowMs();
    std::string FormatSize(long long bytes);
}
