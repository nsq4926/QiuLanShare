#pragma once

#include <string>

// 主动连接对端的薄客户端（T05）。
//
// Windows 到这一步之前只能"被连"：手机上点发送 → PC 收到。要变成双向节点，
// 必须有"PC 主动发起"的路径，而这条路径的每个细节都能安静地出错：
//   * 源端口误绑监听端口 → 自己连自己：发送成功、对端 0 条；
//   * 目标端口写死 8765 → 对端端口递进到 8766 之后全军覆没；
//   * 只把 ECONNREFUSED 当失败 → 隧道地址的失败形态是"一直不回"，发送端卡很久；
//   * 失败后自动重试 → 变成隐形队列，用户看不到"未发送"，也永远不会重发。
struct PeerSendResult
{
    bool        ok = false;
    std::string reason;      // 失败时只有两种取值，见下面两个常量
    std::string ipUsed;      // 成功时实际连通的 IP
    int         attempts = 0;
    int         lastError = 0;   // 原始 WSA / HTTP 状态，仅供日志
    // 诊断串（kind/code/status/ip）。**不参与对外那两种原因的语义**，只用于排障：
    // 没有它，"不可达"到底是超时、被拒还是响应解析失败，现场看不出来。
    std::string detail;
};

class PeerClient
{
public:
    static PeerClient& Instance();

    // 两条、且只有两条失败原因。不给第三个模糊说法：
    // 用户要能据此决定"换个网试试"还是"让对方把接收打开"。
    static const char* Unreachable();   // "对方当前不可达"
    static const char* Closed();        // "对方已关闭接收"

    // 投递一条文字。成功后回写该 IP 的 lastOkAt。
    // 周期健康探测（T07 补）：GET /api/hello，带 X-Device-Id。
    // 与消息投递的区别是**不产生任何副作用**（不写消息、不推进 seq、不记 conflicts），
    // 而且 acceptsInbound=false 的对端同样会被探测到——它控制"收不收消息"，
    // 不控制"能不能被看见"。
    PeerSendResult ProbeHello(const struct PeerDevice& peer);

    PeerSendResult SendText(const std::string& peerDeviceId,
                            const std::string& text,
                            const std::string& msgId);

    // 投递一个已落盘的文件（流式发送，不整体读进内存）。
    PeerSendResult SendFile(const std::string& peerDeviceId,
                            const std::wstring& filePath,
                            const std::string& fileName,
                            const std::string& mime,
                            const std::string& msgId);
};
