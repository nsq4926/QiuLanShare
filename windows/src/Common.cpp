#include "Common.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shlobj.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <algorithm>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "ws2_32.lib")

// 命名空间刻意不叫 tm：<ctime> 会把 C 的 struct tm 引入全局作用域，
// 与之同名会报 C2757「具有该名称的符号已存在」。
namespace tml
{
    // ================= 模块句柄 =================
    static void* g_selfModule = nullptr;

    void SetSelfModule(void* hModule) { g_selfModule = hModule; }
    void* GetSelfModule() { return g_selfModule; }

    // ================= 编码 =================
    std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return std::string();
        int len = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        if (len <= 0) return std::string();
        std::string out(static_cast<size_t>(len), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], len, nullptr, nullptr);
        return out;
    }

    std::wstring Utf8ToWide(const std::string& s)
    {
        if (s.empty()) return std::wstring();
        int len = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        if (len <= 0) return std::wstring();
        std::wstring out(static_cast<size_t>(len), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], len);
        return out;
    }

    // ================= 极简 JSON =================
    static size_t SkipWs(const std::string& s, size_t i)
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
        return i;
    }

    static void AppendCodePointUtf8(std::string& out, unsigned int cp)
    {
        if (cp <= 0x7F)
        {
            out += static_cast<char>(cp);
        }
        else if (cp <= 0x7FF)
        {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp <= 0xFFFF)
        {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    static int HexVal(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    static bool ParseJsonString(const std::string& s, size_t& i, std::string& out)
    {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size())
        {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e)
            {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
            {
                if (i + 4 > s.size()) return false;
                unsigned int cp = 0;
                for (int k = 0; k < 4; ++k)
                {
                    int h = HexVal(s[i + k]);
                    if (h < 0) return false;
                    cp = (cp << 4) | static_cast<unsigned int>(h);
                }
                i += 4;
                // 代理对
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u')
                {
                    unsigned int low = 0;
                    bool ok = true;
                    for (int k = 0; k < 4; ++k)
                    {
                        int h = HexVal(s[i + 2 + k]);
                        if (h < 0) { ok = false; break; }
                        low = (low << 4) | static_cast<unsigned int>(h);
                    }
                    if (ok && low >= 0xDC00 && low <= 0xDFFF)
                    {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                    }
                }
                AppendCodePointUtf8(out, cp);
                break;
            }
            default: out += e; break;
            }
        }
        return false;
    }

    static bool SkipJsonValue(const std::string& s, size_t& i)
    {
        i = SkipWs(s, i);
        if (i >= s.size()) return false;
        char c = s[i];
        if (c == '"')
        {
            std::string tmp;
            return ParseJsonString(s, i, tmp);
        }
        if (c == '{' || c == '[')
        {
            char open = c;
            char close = (c == '{') ? '}' : ']';
            int depth = 0;
            bool inStr = false;
            while (i < s.size())
            {
                char d = s[i];
                if (inStr)
                {
                    if (d == '\\') { ++i; }
                    else if (d == '"') { inStr = false; }
                }
                else
                {
                    if (d == '"') inStr = true;
                    else if (d == open) ++depth;
                    else if (d == close)
                    {
                        --depth;
                        if (depth == 0) { ++i; return true; }
                    }
                }
                ++i;
            }
            return false;
        }
        while (i < s.size() && std::strchr(",}] \t\r\n", s[i]) == nullptr) ++i;
        return true;
    }

    std::string JsonEscape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 16);
        for (unsigned char c : s)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) out += ' ';
                else out += static_cast<char>(c);
            }
        }
        return out;
    }

    bool JsonGetString(const std::string& json, const std::string& key, std::string& out)
    {
        const std::string pat = "\"" + key + "\"";
        size_t pos = 0;
        while ((pos = json.find(pat, pos)) != std::string::npos)
        {
            size_t i = pos + pat.size();
            i = SkipWs(json, i);
            if (i < json.size() && json[i] == ':')
            {
                ++i;
                i = SkipWs(json, i);
                if (i < json.size() && json[i] == '"') return ParseJsonString(json, i, out);
            }
            pos += pat.size();
        }
        return false;
    }

    bool JsonGetInt(const std::string& json, const std::string& key, long long& out)
    {
        const std::string pat = "\"" + key + "\"";
        size_t pos = 0;
        while ((pos = json.find(pat, pos)) != std::string::npos)
        {
            size_t i = pos + pat.size();
            i = SkipWs(json, i);
            if (i < json.size() && json[i] == ':')
            {
                ++i;
                i = SkipWs(json, i);
                bool neg = false;
                if (i < json.size() && (json[i] == '-' || json[i] == '+'))
                {
                    neg = (json[i] == '-');
                    ++i;
                }
                long long v = 0;
                bool any = false;
                while (i < json.size() && json[i] >= '0' && json[i] <= '9')
                {
                    v = v * 10 + (json[i] - '0');
                    ++i;
                    any = true;
                }
                if (any)
                {
                    out = neg ? -v : v;
                    return true;
                }
            }
            pos += pat.size();
        }
        return false;
    }

    // ================= 递归下降 JSON 解析 =================
    // 只用在本项目自己写出去的文件（设备表）里，所以不做防御式过度设计：
    // 出错就返回 false，由调用方决定"坏文件"的语义，绝不"尽力猜一半"。
    namespace
    {
        struct JsonCursor
        {
            const std::string& s;
            size_t i = 0;
            int    depth = 0;
            explicit JsonCursor(const std::string& text) : s(text) {}
        };

        void SkipSpace(JsonCursor& c)
        {
            while (c.i < c.s.size())
            {
                const char ch = c.s[c.i];
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { ++c.i; continue; }
                break;
            }
        }

        bool ParseValue(JsonCursor& c, JsonValue& out);

        bool ParseString(JsonCursor& c, std::string& out)
        {
            if (c.i >= c.s.size() || c.s[c.i] != '"') return false;
            ++c.i;
            out.clear();
            while (c.i < c.s.size())
            {
                const char ch = c.s[c.i++];
                if (ch == '"') return true;
                if (ch != '\\')
                {
                    out.push_back(ch);
                    continue;
                }
                if (c.i >= c.s.size()) return false;
                const char esc = c.s[c.i++];
                switch (esc)
                {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u':
                {
                    // 只处理 BMP 内的转义，够设备名（中文属于 BMP）
                    if (c.i + 4 > c.s.size()) return false;
                    unsigned int cp = 0;
                    for (int k = 0; k < 4; ++k)
                    {
                        const char h = c.s[c.i++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9')      cp |= static_cast<unsigned int>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned int>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned int>(h - 'A' + 10);
                        else return false;
                    }
                    if (cp < 0x80)
                    {
                        out.push_back(static_cast<char>(cp));
                    }
                    else if (cp < 0x800)
                    {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    else
                    {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return false;
                }
            }
            return false;
        }

        bool ParseNumber(JsonCursor& c, long long& out)
        {
            bool neg = false;
            if (c.i < c.s.size() && (c.s[c.i] == '-' || c.s[c.i] == '+'))
            {
                neg = (c.s[c.i] == '-');
                ++c.i;
            }
            if (c.i >= c.s.size() || c.s[c.i] < '0' || c.s[c.i] > '9') return false;

            long long v = 0;
            while (c.i < c.s.size() && c.s[c.i] >= '0' && c.s[c.i] <= '9')
            {
                v = v * 10 + (c.s[c.i] - '0');
                ++c.i;
            }
            // 小数/指数：本项目 schema 里没有小数，截断整数部分即可，但要吃掉尾巴
            if (c.i < c.s.size() && c.s[c.i] == '.')
            {
                ++c.i;
                while (c.i < c.s.size() && c.s[c.i] >= '0' && c.s[c.i] <= '9') ++c.i;
            }
            if (c.i < c.s.size() && (c.s[c.i] == 'e' || c.s[c.i] == 'E'))
            {
                ++c.i;
                if (c.i < c.s.size() && (c.s[c.i] == '-' || c.s[c.i] == '+')) ++c.i;
                while (c.i < c.s.size() && c.s[c.i] >= '0' && c.s[c.i] <= '9') ++c.i;
            }
            out = neg ? -v : v;
            return true;
        }

        bool MatchLiteral(JsonCursor& c, const char* lit)
        {
            const size_t n = ::strlen(lit);
            if (c.i + n > c.s.size()) return false;
            if (c.s.compare(c.i, n, lit) != 0) return false;
            c.i += n;
            return true;
        }

        bool ParseValue(JsonCursor& c, JsonValue& out)
        {
            // 嵌套深度上限：防止坏文件把我们递归到栈溢出
            if (c.depth > 32) return false;
            SkipSpace(c);
            if (c.i >= c.s.size()) return false;

            const char ch = c.s[c.i];
            if (ch == '"')
            {
                out.kind = JsonValue::String;
                return ParseString(c, out.text);
            }
            if (ch == '{')
            {
                out.kind = JsonValue::Object;
                ++c.i;
                ++c.depth;
                SkipSpace(c);
                if (c.i < c.s.size() && c.s[c.i] == '}') { ++c.i; --c.depth; return true; }
                for (;;)
                {
                    SkipSpace(c);
                    std::string key;
                    if (!ParseString(c, key)) return false;
                    SkipSpace(c);
                    if (c.i >= c.s.size() || c.s[c.i] != ':') return false;
                    ++c.i;
                    JsonValue child;
                    if (!ParseValue(c, child)) return false;
                    out.members.push_back(std::make_pair(key, child));
                    SkipSpace(c);
                    if (c.i < c.s.size() && c.s[c.i] == ',') { ++c.i; continue; }
                    if (c.i < c.s.size() && c.s[c.i] == '}') { ++c.i; --c.depth; return true; }
                    return false;
                }
            }
            if (ch == '[')
            {
                out.kind = JsonValue::Array;
                ++c.i;
                ++c.depth;
                SkipSpace(c);
                if (c.i < c.s.size() && c.s[c.i] == ']') { ++c.i; --c.depth; return true; }
                for (;;)
                {
                    JsonValue child;
                    if (!ParseValue(c, child)) return false;
                    out.items.push_back(child);
                    SkipSpace(c);
                    if (c.i < c.s.size() && c.s[c.i] == ',') { ++c.i; continue; }
                    if (c.i < c.s.size() && c.s[c.i] == ']') { ++c.i; --c.depth; return true; }
                    return false;
                }
            }
            if (ch == 't')
            {
                if (!MatchLiteral(c, "true")) return false;
                out.kind = JsonValue::Bool;
                out.boolean = true;
                return true;
            }
            if (ch == 'f')
            {
                if (!MatchLiteral(c, "false")) return false;
                out.kind = JsonValue::Bool;
                out.boolean = false;
                return true;
            }
            if (ch == 'n')
            {
                if (!MatchLiteral(c, "null")) return false;
                out.kind = JsonValue::Null;
                return true;
            }
            out.kind = JsonValue::Number;
            return ParseNumber(c, out.number);
        }
    }

    const JsonValue* JsonValue::Find(const std::string& key) const
    {
        if (kind != Object) return nullptr;
        for (size_t i = 0; i < members.size(); ++i)
        {
            if (members[i].first == key) return &members[i].second;
        }
        return nullptr;
    }

    std::string JsonValue::GetString(const std::string& key, const std::string& def) const
    {
        const JsonValue* v = Find(key);
        if (v == nullptr || v->kind != String) return def;
        return v->text;
    }

    long long JsonValue::GetInt(const std::string& key, long long def) const
    {
        const JsonValue* v = Find(key);
        if (v == nullptr || v->kind != Number) return def;
        return v->number;
    }

    bool JsonValue::GetBool(const std::string& key, bool def) const
    {
        const JsonValue* v = Find(key);
        if (v == nullptr || v->kind != Bool) return def;
        return v->boolean;
    }

    bool JsonParse(const std::string& text, JsonValue& out)
    {
        // 容忍 UTF-8 BOM：设备表可能被人用记事本/Set-Content 打开改过，
        // 带 BOM 时首字符不是 '{'，解析会直接失败 → 设备表被当成坏文件重生成、
        // deviceId 变新 —— 一个 BOM 就能让"身份突然变了"，不值得。
        size_t begin = 0;
        if (text.size() >= 3 &&
            static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
        {
            begin = 3;
        }

        JsonCursor c(text);
        c.i = begin;
        out = JsonValue();
        if (!ParseValue(c, out)) return false;
        SkipSpace(c);
        return c.i >= text.size();
    }

    // ================= URL =================
    std::string UrlDecode(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i)
        {
            char c = s[i];
            if (c == '+')
            {
                out += ' ';
            }
            else if (c == '%' && i + 2 < s.size())
            {
                int h1 = HexVal(s[i + 1]);
                int h2 = HexVal(s[i + 2]);
                if (h1 >= 0 && h2 >= 0)
                {
                    out += static_cast<char>((h1 << 4) | h2);
                    i += 2;
                }
                else
                {
                    out += c;
                }
            }
            else
            {
                out += c;
            }
        }
        return out;
    }

    std::string UrlEncode(const std::string& s)
    {
        static const char* kHex = "0123456789ABCDEF";
        std::string out;
        out.reserve(s.size() + 8);
        for (size_t i = 0; i < s.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            const bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                               (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.' || c == '~';
            if (plain)
            {
                out.push_back(static_cast<char>(c));
                continue;
            }
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
        return out;
    }

    std::string QueryParam(const std::string& query, const std::string& key)
    {
        size_t pos = 0;
        while (pos <= query.size())
        {
            size_t amp = query.find('&', pos);
            std::string pair = query.substr(pos, (amp == std::string::npos) ? std::string::npos : amp - pos);
            size_t eq = pair.find('=');
            if (eq != std::string::npos)
            {
                if (pair.substr(0, eq) == key) return UrlDecode(pair.substr(eq + 1));
            }
            else if (pair == key)
            {
                return std::string();
            }
            if (amp == std::string::npos) break;
            pos = amp + 1;
        }
        return std::string();
    }

    // ================= 网络 =================
    namespace
    {
        std::string ToLowerAscii(const std::string& s)
        {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i)
            {
                char c = s[i];
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                out.push_back(c);
            }
            return out;
        }

        bool StartsWithText(const std::string& hay, const char* prefix)
        {
            const size_t n = ::strlen(prefix);
            if (hay.size() < n) return false;
            return hay.compare(0, n, prefix) == 0;
        }

        // 已知虚拟网卡名（8 项，与 tools/precheck-bridge.ps1、docs/05 §四 是同一张表）。
        // 匹配分档同样是硬要求：Description 用子串（驱动写死），Name 只用前缀（用户可改）。
        // Name 若也做子串，用户把有线网卡改名成 "Virtual Lab NIC" 就会被静默误杀，
        // 症状只是"少一个 IP"，在列表里看不出来。
        bool LooksLikeVirtualNic(const std::string& name, const std::string& description)
        {
            static const char* kTokens[] = {
                "vethernet", "docker", "wsl", "vmware",
                "virtualbox", "hyper-v", "tap-windows", "npcap loopback"
            };
            const std::string desc = ToLowerAscii(description);
            const std::string nm = ToLowerAscii(name);

            for (size_t i = 0; i < sizeof(kTokens) / sizeof(kTokens[0]); ++i)
            {
                if (!desc.empty() && desc.find(kTokens[i]) != std::string::npos) return true;
            }
            for (size_t i = 0; i < sizeof(kTokens) / sizeof(kTokens[0]); ++i)
            {
                if (!nm.empty() && StartsWithText(nm, kTokens[i])) return true;
            }
            return false;
        }

        // 四步过滤后的集合为空时的兜底实现（旧的"有网关优先"扫法）。
        // 保留它只为不改变"极端情况下也能报个地址"的既有行为；
        // 正常机器根本走不到这里，因为 GetUsableIPv4Nics() 会先给出结果。
        std::string GetLanIPv4Unfiltered()
        {
            std::string fallback;
        ULONG bufSize = 16 * 1024;
        std::vector<BYTE> buffer;

        for (int attempt = 0; attempt < 3; ++attempt)
        {
            buffer.resize(bufSize);
            ULONG size = bufSize;
            ULONG ret = ::GetAdaptersAddresses(AF_INET,
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                nullptr,
                reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()),
                &size);
            if (ret == ERROR_BUFFER_OVERFLOW)
            {
                bufSize = size + 4096;
                continue;
            }
            if (ret != NO_ERROR) break;

            PIP_ADAPTER_ADDRESSES addr = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
            for (; addr != nullptr; addr = addr->Next)
            {
                if (addr->OperStatus != IfOperStatusUp) continue;
                if (addr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

                for (PIP_ADAPTER_UNICAST_ADDRESS uni = addr->FirstUnicastAddress; uni != nullptr; uni = uni->Next)
                {
                    if (uni->Address.lpSockaddr == nullptr) continue;
                    if (uni->Address.lpSockaddr->sa_family != AF_INET) continue;

                    char ip[64] = { 0 };
                    sockaddr_in* sa = reinterpret_cast<sockaddr_in*>(uni->Address.lpSockaddr);
                    ::InetNtopA(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                    if (ip[0] == '\0') continue;
                    if (std::strcmp(ip, "127.0.0.1") == 0) continue;
                    if (std::strncmp(ip, "169.254.", 8) == 0) continue;

                    if (addr->FirstGatewayAddress != nullptr)
                        return std::string(ip);

                    if (fallback.empty()) fallback = ip;
                }
            }
            break;
        }

        if (!fallback.empty()) return fallback;
        return std::string("127.0.0.1");
    }
    }   // namespace

    NicFilterResult GetNicFilter()
    {
        // 计数单位 = **适配器条目**，在下面这层遍历里数出来的。
        // 为什么不能按名字或按地址数：① 按地址数会把"一张卡两个 IP"算成两个接口，
        // 唯一可用护栏因此不触发，唯一的地址会被删掉；② 按 Name 会把"两张同名网卡"
        // 算成一个，护栏误触发、ips[] 少一条；而且 Name 在中文系统上就是"以太网"，
        // 拿它做键本身就是脆的。③ 返回值 nics 也**不能**用来数接口数（一行一个地址）。
        struct AdapterInfo
        {
            std::string name;
            std::string description;
            std::string mac;
            unsigned int ifType = 0;
            bool matched = false;       // 名字命中已知虚拟网卡
            bool hasGateway = false;
            std::vector<std::string> ips;
            std::vector<int>         prefixes;   // 与 ips 一一对应的前缀长度
        };

        std::vector<AdapterInfo> adapters;
        ULONG bufSize = 16 * 1024;
        std::vector<BYTE> buffer;

        for (int attempt = 0; attempt < 3; ++attempt)
        {
            buffer.resize(bufSize);
            ULONG size = bufSize;
            const ULONG ret = ::GetAdaptersAddresses(AF_INET,
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                nullptr,
                reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()),
                &size);
            if (ret == ERROR_BUFFER_OVERFLOW)
            {
                bufSize = size + 4096;
                continue;
            }
            if (ret != NO_ERROR) break;

            for (PIP_ADAPTER_ADDRESSES addr = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
                 addr != nullptr; addr = addr->Next)
            {
                // 步骤 0：接口本身得是"起来"的，回环直接不看
                if (addr->OperStatus != IfOperStatusUp) continue;
                if (addr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

                AdapterInfo info;
                info.ifType = addr->IfType;
                if (addr->FriendlyName != nullptr) info.name = WideToUtf8(addr->FriendlyName);
                if (addr->Description != nullptr) info.description = WideToUtf8(addr->Description);
                info.hasGateway = (addr->FirstGatewayAddress != nullptr);
                info.matched = LooksLikeVirtualNic(info.name, info.description);

                if (addr->PhysicalAddressLength == 6)
                {
                    char mac[32] = { 0 };
                    sprintf_s(mac, "%02X:%02X:%02X:%02X:%02X:%02X",
                              addr->PhysicalAddress[0], addr->PhysicalAddress[1],
                              addr->PhysicalAddress[2], addr->PhysicalAddress[3],
                              addr->PhysicalAddress[4], addr->PhysicalAddress[5]);
                    info.mac = mac;
                }

                // 步骤 1：有效 IPv4 —— 排掉 127/8、169.254/16、0.0.0.0
                for (PIP_ADAPTER_UNICAST_ADDRESS uni = addr->FirstUnicastAddress; uni != nullptr; uni = uni->Next)
                {
                    if (uni->Address.lpSockaddr == nullptr) continue;
                    if (uni->Address.lpSockaddr->sa_family != AF_INET) continue;

                    char ip[64] = { 0 };
                    const sockaddr_in* sa = reinterpret_cast<const sockaddr_in*>(uni->Address.lpSockaddr);
                    if (::InetNtopA(AF_INET, &sa->sin_addr, ip, sizeof(ip)) == nullptr) continue;
                    if (ip[0] == '\0') continue;
                    if (std::strcmp(ip, "0.0.0.0") == 0) continue;
                    if (std::strncmp(ip, "127.", 4) == 0) continue;
                    if (std::strncmp(ip, "169.254.", 8) == 0) continue;

                    info.ips.push_back(ip);
                    info.prefixes.push_back(static_cast<int>(uni->OnLinkPrefixLength));
                }

                if (!info.ips.empty()) adapters.push_back(info);
            }
            break;
        }

        // 护栏要在"排除之前"判定，所以先把通过了步骤 1 的接口数清点出来。
        // 判据是"去掉这一个之后还有没有别的可用 IPv4 接口"，不是"结果集大小 == 1"：
        // 一张网卡可以挂多个 IPv4，按地址条数判会把同一张卡当两张卡。
        int step2AdapterCount = 0;
        for (size_t i = 0; i < adapters.size(); ++i)
        {
            const unsigned int t = adapters[i].ifType;
            if (t == IF_TYPE_ETHERNET_CSMACD || t == IF_TYPE_IEEE80211) ++step2AdapterCount;
        }

        NicFilterResult out;
        std::vector<Ipv4Nic>& result = out.nics;
        for (size_t i = 0; i < adapters.size(); ++i)
        {
            const AdapterInfo& a = adapters[i];

            // 步骤 2：只看真以太网 / 真 Wi-Fi。这是数值判据，和预验脚本同一套
            // （GetAdaptersAddresses 的 IfType 与 .NET 的 NetworkInterfaceType 同源）。
            // 计数口径：被步骤 2 排掉的**要计入 filteredNicCount**——
            // filter 的语义是"被过滤掉了几张网卡"，不是"还剩几张"。
            if (a.ifType != IF_TYPE_ETHERNET_CSMACD && a.ifType != IF_TYPE_IEEE80211)
            {
                ++out.filteredNicCount;
                continue;
            }

            bool keep = true;
            bool guardrailKept = false;
            if (a.matched)
            {
                // 步骤 3：已知虚拟网卡名。但"最后一张能用的网卡"绝不排除——
                // 桥接宿主上那条唯一的有线网卡被误杀，症状是"发不出去且不知道为什么"。
                if (step2AdapterCount <= 1)
                {
                    keep = true;
                    guardrailKept = true;   // 救回来的算"保留"，不计入 filtered
                }
                else
                {
                    keep = false;
                }
            }
            if (!keep)
            {
                ++out.filteredNicCount;
                continue;
            }

            ++out.keptNicCount;

            for (size_t k = 0; k < a.ips.size(); ++k)
            {
                Ipv4Nic nic;
                nic.ip = a.ips[k];
                nic.mac = a.mac;
                nic.ifType = a.ifType;
                nic.hasGateway = a.hasGateway;
                nic.prefixLen = (k < a.prefixes.size()) ? a.prefixes[k] : 32;
                nic.guardrailKept = guardrailKept;
                nic.name = a.name;
                nic.description = a.description;
                result.push_back(nic);
            }
        }

        // 有网关的排前面：它才是"这台机器对外的那条链路"
        std::stable_partition(result.begin(), result.end(),
                              [](const Ipv4Nic& n) { return n.hasGateway; });
        return out;
    }

    LocalIdentity GetLocalIdentity()
    {
        LocalIdentity id;
        const NicFilterResult f = GetNicFilter();
        id.filteredNicCount = f.filteredNicCount;
        id.keptNicCount = f.keptNicCount;

        for (size_t i = 0; i < f.nics.size(); ++i) id.ips.push_back(f.nics[i].ip);

        if (!f.nics.empty())
        {
            // ip 与 mac 固定从**同一个** nics[0] 取。分两次查会出现"真网卡 IP +
            // 虚拟网卡 MAC"的拼装组合，对端按 macHistory 会误报"这台设备换了网卡"。
            id.ip = f.nics[0].ip;
            id.mac = f.nics[0].mac;
            return id;
        }

        // 过滤后为空（只剩回环 / 只有 APIPA / 驱动未就绪）：退回旧扫法，
        // 但仍然不"编"地址——只有回环时对外等于"没有地址"。
        id.ip = GetLanIPv4Unfiltered();
        if (id.ip == "127.0.0.1") id.ip.clear();
        return id;
    }

    std::string GetLanIPv4()
    {
        // 兼容入口。要用 ip + mac + 统计的调用方请直接用 GetLocalIdentity()：
        // 分别调用会把网卡枚举做两遍，两次之间热插拔就会拿到不一致的组合。
        return GetLocalIdentity().ip;
    }

    bool EnsureWinsock()
    {
        // 只初始化一次，且全程不配对 WSACleanup。
        // 背景（T01）：winsock 的初始化原本只写在 HttpServer::Start() 里，而"第二实例"
        // 只作客户端、从不调用 Start，导致它一 socket() 就拿到 10093
        // （WSANOTINITIALISED）。这里补一次进程级初始化，两个角色都覆盖。
        static std::atomic<bool> s_inited(false);
        static std::atomic<bool> s_ok(false);
        if (s_inited.load()) return s_ok.load();

        WSADATA wsa{};
        const int rc = ::WSAStartup(MAKEWORD(2, 2), &wsa);
        s_ok.store(rc == 0);
        s_inited.store(true);
        return rc == 0;
    }

    std::wstring GetHostName()
    {
        wchar_t buf[256] = { 0 };
        DWORD size = 256;
        if (::GetComputerNameW(buf, &size)) return std::wstring(buf, size);
        return L"Windows";
    }

    // 与 GetLanIPv4() 同一张网卡的 MAC。
    //
    // 用途：/api/hello 的 mac 字段、以及 deviceId 的稳定来源。必须取"报出 LAN IP
    // 的那张网卡"，否则多网卡机器（有线 + Wi-Fi + 虚拟网卡）会报出随机的 MAC，
    // 对端会把它当成两台设备。
    std::string GetMacAddress()
    {
        // mac 必须与 ip 出自同一张网卡：GetLocalIdentity() 一次枚举同时给出两者，
        // 分别查会拿到"真网卡 IP + 虚拟网卡 MAC"的拼装组合。
        const LocalIdentity id = GetLocalIdentity();
        if (!id.mac.empty()) return id.mac;

        const std::string wantIp = id.ip;
        std::string fallback;
        ULONG bufSize = 16 * 1024;
        std::vector<BYTE> buffer;

        for (int attempt = 0; attempt < 3; ++attempt)
        {
            buffer.resize(bufSize);
            ULONG size = bufSize;
            const ULONG ret = ::GetAdaptersAddresses(AF_INET,
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                nullptr,
                reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()),
                &size);
            if (ret == ERROR_BUFFER_OVERFLOW)
            {
                bufSize = size + 4096;
                continue;
            }
            if (ret != NO_ERROR) break;

            PIP_ADAPTER_ADDRESSES addr = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
            for (; addr != nullptr; addr = addr->Next)
            {
                if (addr->OperStatus != IfOperStatusUp) continue;
                if (addr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
                if (addr->PhysicalAddressLength != 6) continue;

                char mac[32] = { 0 };
                sprintf_s(mac, "%02X:%02X:%02X:%02X:%02X:%02X",
                          addr->PhysicalAddress[0], addr->PhysicalAddress[1],
                          addr->PhysicalAddress[2], addr->PhysicalAddress[3],
                          addr->PhysicalAddress[4], addr->PhysicalAddress[5]);

                bool match = false;
                for (PIP_ADAPTER_UNICAST_ADDRESS uni = addr->FirstUnicastAddress; uni != nullptr; uni = uni->Next)
                {
                    if (uni->Address.lpSockaddr == nullptr) continue;
                    if (uni->Address.lpSockaddr->sa_family != AF_INET) continue;

                    char ip[64] = { 0 };
                    sockaddr_in* sa = reinterpret_cast<sockaddr_in*>(uni->Address.lpSockaddr);
                    ::InetNtopA(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                    if (wantIp == ip)
                    {
                        match = true;
                        break;
                    }
                }

                if (match) return std::string(mac);
                if (fallback.empty()) fallback = mac;
            }
            break;
        }

        return fallback;
    }

    // ================= 路径 =================
    std::wstring GetModuleDir()
    {
        wchar_t buf[MAX_PATH] = { 0 };
        DWORD n = ::GetModuleFileNameW(static_cast<HMODULE>(g_selfModule), buf, MAX_PATH);
        if (n == 0) return std::wstring();
        std::wstring path(buf, n);
        size_t pos = path.find_last_of(L"\\/");
        if (pos == std::wstring::npos) return std::wstring();
        return path.substr(0, pos);
    }

    std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
    {
        if (a.empty()) return b;
        if (b.empty()) return a;
        std::wstring out = a;
        if (out.back() != L'\\' && out.back() != L'/') out += L'\\';
        out += b;
        return out;
    }

    std::wstring DefaultDataDir(const std::wstring& instanceName)
    {
        // 数据目录跟插件走：<TrafficMonitor>\plugins\LanShare。
        // （2026-11-15 按用户要求从 %LOCALAPPDATA% 改回插件目录；旧的迁移仍然保留，
        //   见 InstanceLock 里的多来源迁移——换位置必须搬身份，不能重建。）
        std::wstring base = JoinPath(GetModuleDir(), L"LanShare");
        if (!instanceName.empty() && instanceName != L"default")
            base = JoinPath(base, instanceName);
        return base;
    }

    std::wstring LegacyDataDir(const std::wstring& instanceName)
    {
        // 上一版默认位置。改默认目录时它必须仍然能被找到并搬过来，
        // 否则升级一次就"设备表消失 → deviceId 重建 → 对端眼里变新设备"。
        wchar_t* local = nullptr;
        std::wstring base;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local != nullptr)
        {
            base = local;
            ::CoTaskMemFree(local);
        }
        if (base.empty()) return std::wstring();
        base = JoinPath(base, L"TM-LAN-Share");
        if (!instanceName.empty() && instanceName != L"default")
            base = JoinPath(base, instanceName);
        return base;
    }

    bool CopyFileTo(const std::wstring& src, const std::wstring& dst)
    {
        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) return false;
        return true;
    }

    bool FileExists(const std::wstring& path)
    {
        return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    void AppendLogLine(const std::wstring& path, const std::string& line)
    {
        if (path.empty()) return;
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"ab") != 0 || fp == nullptr) return;
        const std::string text = line + "\n";
        fwrite(text.data(), 1, text.size(), fp);
        fclose(fp);
    }

    void StripIniBom(const std::wstring& path)
    {
        if (path.empty()) return;
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || fp == nullptr) return;

        unsigned char head[3] = { 0, 0, 0 };
        const size_t n = fread(head, 1, 3, fp);
        if (n < 3 || head[0] != 0xEF || head[1] != 0xBB || head[2] != 0xBF)
        {
            fclose(fp);
            return;     // 没有 BOM，什么都不做
        }

        std::string rest;
        char buf[8192];
        size_t m = 0;
        while ((m = fread(buf, 1, sizeof(buf), fp)) > 0) rest.append(buf, m);
        fclose(fp);

        FILE* out = nullptr;
        if (_wfopen_s(&out, path.c_str(), L"wb") != 0 || out == nullptr) return;
        fwrite(rest.data(), 1, rest.size(), out);
        fclose(out);
    }

    // 粗判 UTF-8：遇到非法序列就当 ANSI(GBK)。只用来决定 ini 怎么解码，不求严格。
    static bool LooksLikeUtf8(const std::string& s)
    {
        size_t i = 0;
        while (i < s.size())
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            size_t extra = 0;
            if (c < 0x80) extra = 0;
            else if ((c & 0xE0) == 0xC0) extra = 1;
            else if ((c & 0xF0) == 0xE0) extra = 2;
            else if ((c & 0xF8) == 0xF0) extra = 3;
            else return false;

            if (i + extra >= s.size()) return false;
            for (size_t k = 1; k <= extra; ++k)
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
            i += extra + 1;
        }
        return true;
    }

    void NormalizeIniToUnicode(const std::wstring& path)
    {
        if (path.empty()) return;

        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || fp == nullptr) return;
        std::string raw;
        char buf[8192];
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) raw.append(buf, n);
        fclose(fp);
        if (raw.empty()) return;

        // 已经是 UTF-16LE(BOM FF FE) 就不用动
        if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF &&
            static_cast<unsigned char>(raw[1]) == 0xFE) return;

        size_t start = 0;
        bool utf8 = false;
        if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
            static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF)
        {
            start = 3;
            utf8 = true;
        }

        std::string body = raw.substr(start);
        std::wstring wide;
        if (utf8 || LooksLikeUtf8(body))
        {
            wide = Utf8ToWide(body);
        }
        else
        {
            // 老文件是 ANSI(GBK)：先按本机代码页解
            const int len = ::MultiByteToWideChar(CP_ACP, 0, body.c_str(),
                                                  static_cast<int>(body.size()), nullptr, 0);
            if (len > 0)
            {
                wide.resize(static_cast<size_t>(len));
                ::MultiByteToWideChar(CP_ACP, 0, body.c_str(), static_cast<int>(body.size()),
                                      &wide[0], len);
            }
        }
        if (wide.empty()) return;

        const unsigned short bom = 0xFEFF;
        FILE* out = nullptr;
        if (_wfopen_s(&out, path.c_str(), L"wb") != 0 || out == nullptr) return;
        fwrite(&bom, 1, 2, out);
        fwrite(wide.data(), sizeof(wchar_t), wide.size(), out);
        fclose(out);
    }

    void MigrateLegacyDataFiles(const std::wstring& dir)
    {
        if (dir.empty()) return;
        const std::wstring pattern = JoinPath(dir, L"lanshare*");

        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;

        std::vector<std::wstring> copied;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring name = fd.cFileName;
            if (name.compare(0, 12, L"lanshare") != 0) continue;   // 13 字符，前缀 12 已唯一

            std::wstring target = L"lanshare" + name.substr(12);
            const std::wstring dst = JoinPath(dir, target);
            if (::GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) continue;  // 已迁过，不覆盖
            if (CopyFileTo(JoinPath(dir, name), dst)) copied.push_back(target);
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);

        if (!copied.empty())
        {
            std::string line = "renamed legacy data files:";
            for (size_t i = 0; i < copied.size(); ++i) line += " " + WideToUtf8(copied[i]);
            AppendLogLine(JoinPath(dir, L"lanshare.log"), line);
        }
    }

    std::wstring PickFolder()
    {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const bool needUninit = SUCCEEDED(hr);

        // ---- 把它顶到前台 ----
        // 这个对话框是我们在**后台线程**里弹的：SetForegroundWindow 只认"当前前台进程"
        // 或"刚收到过用户输入"的进程，所以不加这两步，窗口只会出现在任务栏，
        // 用户得自己点一下才看得见（实测就是这样）。
        //   1) AllowSetForegroundWindow：本进程自己允许自己抢前台；
        //   2) 建一个隐形且 TOPMOST 的 owner 窗口挂在对话框上：
        //      有 owner 的窗口不会被无 owner 的窗口压住，置顶属性再把整条链顶到最前。
        ::AllowSetForegroundWindow(static_cast<DWORD>(-1));   // ASFW_ANY
        ::AllowSetForegroundWindow(::GetCurrentProcessId());

        HWND owner = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"STATIC", L"",
                                       WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                       ::GetModuleHandleW(nullptr), nullptr);
        if (owner != nullptr)
        {
            ::ShowWindow(owner, SW_SHOWNA);
            ::SetWindowPos(owner, HWND_TOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ::SetForegroundWindow(owner);
        }

        BROWSEINFOW bi{};
        // owner 建不出来就退回 nullptr —— 至少对话框还能出来，只是可能要自己点任务栏
        bi.hwndOwner = owner;
        bi.lpszTitle = L"\u9009\u62e9\u6587\u4ef6\u4fdd\u5b58\u76ee\u5f55";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;

        std::wstring result;
        LPITEMIDLIST pidl = ::SHBrowseForFolderW(&bi);
        if (pidl != nullptr)
        {
            wchar_t path[MAX_PATH] = { 0 };
            if (::SHGetPathFromIDListW(pidl, path)) result = path;
            ::CoTaskMemFree(pidl);
        }

        if (owner != nullptr) ::DestroyWindow(owner);

        if (needUninit) ::CoUninitialize();
        return result;
    }

    std::wstring DefaultShareDir()
    {
        // 用户要求：收到的文件默认落到系统「下载」目录（再按设备分子文件夹）。
        wchar_t* dl = nullptr;
        std::wstring base;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &dl)) && dl != nullptr)
        {
            base = dl;
            ::CoTaskMemFree(dl);
        }
        if (base.empty())
        {
            // 极少数环境没有 Downloads（组策略重定向失败等）→ 退回文档目录
            wchar_t* docs = nullptr;
            if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs != nullptr)
            {
                base = docs;
                ::CoTaskMemFree(docs);
            }
        }
        if (base.empty()) base = GetModuleDir();
        return base;
    }

    bool EnsureDir(const std::wstring& dir)
    {
        if (dir.empty()) return false;
        DWORD attr = ::GetFileAttributesW(dir.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;

        size_t pos = dir.find_last_of(L"\\/");
        if (pos != std::wstring::npos && pos > 1)
        {
            EnsureDir(dir.substr(0, pos));
        }
        ::CreateDirectoryW(dir.c_str(), nullptr);
        return ::GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    bool ReadWholeFile(const std::wstring& path, std::string& out)
    {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || fp == nullptr) return false;

        out.clear();
        char buf[64 * 1024];
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        {
            out.append(buf, n);
        }
        fclose(fp);
        return true;
    }

    bool WriteWholeFile(const std::wstring& path, const std::string& data)
    {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || fp == nullptr) return false;
        size_t written = data.empty() ? 0 : fwrite(data.data(), 1, data.size(), fp);
        fclose(fp);
        return written == data.size();
    }

    bool WriteWholeFileAtomic(const std::wstring& path, const std::string& data)
    {
        if (path.empty()) return false;

        const std::wstring tmp = path + L".tmp";

        HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;

        bool ok = true;
        size_t written = 0;
        while (written < data.size())
        {
            const DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - written, 1u << 20));
            DWORD wrote = 0;
            if (!::WriteFile(h, data.data() + written, want, &wrote, nullptr) || wrote == 0)
            {
                ok = false;
                break;
            }
            written += wrote;
        }
        if (ok) ok = (::FlushFileBuffers(h) != FALSE);
        ::CloseHandle(h);

        if (!ok)
        {
            ::DeleteFileW(tmp.c_str());
            return false;
        }

        // 原子替换：MOVEFILE_REPLACE_EXISTING 保证对端永远看不到"写了一半"的设备表；
        // WRITE_THROUGH 保证这次替换本身也是落盘的。
        if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            ::DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    }

    bool IsSameSubnet(const std::string& a, const std::string& b, int prefixLen)
    {
        if (prefixLen < 0 || prefixLen > 32) return false;

        IN_ADDR x{}, y{};
        if (::InetPtonA(AF_INET, a.c_str(), &x) != 1) return false;
        if (::InetPtonA(AF_INET, b.c_str(), &y) != 1) return false;

        const unsigned int mask = (prefixLen == 0) ? 0u : (0xFFFFFFFFu << (32 - prefixLen));
        return ((::ntohl(x.S_un.S_addr) & mask) == (::ntohl(y.S_un.S_addr) & mask));
    }

    bool SubnetBroadcast(const std::string& ip, int prefixLen, std::string& out)
    {
        out.clear();

        // /31 是点对点、/32 是单地址，**它们没有广播地址**。
        // 这里返回 false 而不是"算个值出来"：公式在 /31、/32 上会给出一个看起来
        // 合法的地址（比如 x.x.x.1），调用方会以为算过了、照发不误，而发现是间歇性的。
        if (prefixLen < 0 || prefixLen > 30) return false;

        IN_ADDR a{};
        if (::InetPtonA(AF_INET, ip.c_str(), &a) != 1) return false;

        const unsigned int host = ::ntohl(a.S_un.S_addr);
        const unsigned int mask = (prefixLen == 0) ? 0u : (0xFFFFFFFFu << (32 - prefixLen));
        const unsigned int bcast = host | ~mask;

        IN_ADDR b{};
        b.S_un.S_addr = ::htonl(bcast);
        char text[64] = { 0 };
        if (::InetNtopA(AF_INET, &b, text, sizeof(text)) == nullptr) return false;

        out = text;
        return true;
    }

    // ================= 最小 HTTP 客户端 =================
    namespace
    {
        bool SetSockTimeout(SOCKET s, int timeoutMs)
        {
            DWORD t = static_cast<DWORD>(timeoutMs > 0 ? timeoutMs : 1);
            if (::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t)) != 0) return false;
            if (::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t)) != 0) return false;
            return true;
        }

        // 带超时的 connect：非阻塞 + select。
        // 为什么不用阻塞 connect：阻塞态下的超时由系统决定（20 秒起），隧道地址的
        // 典型失败形态就是"一直不回"，会让发送端卡住很久，正好复现"偶发很慢"。
        bool ConnectWithTimeout(SOCKET s, const sockaddr_in& addr, int timeoutMs, int& outErr)
        {
            u_long nonBlocking = 1;
            if (::ioctlsocket(s, FIONBIO, &nonBlocking) != 0)
            {
                outErr = ::WSAGetLastError();
                return false;
            }

            const int rc = ::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
            if (rc == 0) { outErr = 0; }
            else
            {
                outErr = ::WSAGetLastError();
                if (outErr != WSAEWOULDBLOCK) return false;

                fd_set wset;
                FD_ZERO(&wset);
                FD_SET(s, &wset);
                fd_set eset;
                FD_ZERO(&eset);
                FD_SET(s, &eset);

                timeval tv{};
                tv.tv_sec = timeoutMs / 1000;
                tv.tv_usec = (timeoutMs % 1000) * 1000;

                const int sel = ::select(0, nullptr, &wset, &eset, &tv);
                if (sel == 0) { outErr = WSAETIMEDOUT; return false; }
                if (sel == SOCKET_ERROR) { outErr = ::WSAGetLastError(); return false; }

                int soErr = 0;
                int len = sizeof(soErr);
                if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len) != 0)
                {
                    outErr = ::WSAGetLastError();
                    return false;
                }
                if (soErr != 0) { outErr = soErr; return false; }
            }

            nonBlocking = 0;
            ::ioctlsocket(s, FIONBIO, &nonBlocking);
            return true;
        }
    }

    bool HttpPerform(const HttpClientRequest& req, HttpClientResponse& res)
    {
        res = HttpClientResponse();

        if (req.host.empty() || req.port == 0)
        {
            res.errorKind = "connect_fail";
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = ::htons(req.port);
        if (::InetPtonA(AF_INET, req.host.c_str(), &addr.sin_addr) != 1)
        {
            res.errorKind = "connect_fail";
            return false;
        }

        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
        {
            res.errorKind = "connect_fail";
            res.lastError = ::WSAGetLastError();
            return false;
        }

        // 源端口固定为 0（由系统分配）。
        // **绝不能 bind 到本机监听端口**：那会变成"自己连自己"——连接成功、对端收到 0 条，
        // 而发送端以为自己发出去了。T01 的 SO_REUSEADDR 是同一类假象。
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = 0;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
        {
            res.errorKind = "connect_fail";
            res.lastError = ::WSAGetLastError();
            ::closesocket(s);
            return false;
        }

        SetSockTimeout(s, req.timeoutMs);

        int connErr = 0;
        if (!ConnectWithTimeout(s, addr, req.timeoutMs, connErr))
        {
            res.errorKind = (connErr == WSAETIMEDOUT) ? "timeout" : "connect_fail";
            res.lastError = connErr;
            ::closesocket(s);
            return false;
        }

        // ---- 组请求头 ----
        std::string head;
        head += req.method + " " + (req.path.empty() ? std::string("/") : req.path) + " HTTP/1.1\r\n";
        head += "Host: " + req.host + ":" + std::to_string(req.port) + "\r\n";
        head += "Connection: close\r\n";
        head += "Accept: application/json\r\n";
        if (!req.contentType.empty()) head += "Content-Type: " + req.contentType + "\r\n";

        long long payload = static_cast<long long>(req.body.size());
        if (!req.bodyFile.empty()) payload = req.bodySize;
        if (payload > 0 || req.method == "POST")
        {
            head += "Content-Length: " + std::to_string(payload) + "\r\n";
        }
        for (size_t i = 0; i < req.headers.size(); ++i)
        {
            head += req.headers[i].first + ": " + req.headers[i].second + "\r\n";
        }
        head += "\r\n";

        auto sendAll = [&](const char* data, int len) -> bool
        {
            int sent = 0;
            while (sent < len)
            {
                const int n = ::send(s, data + sent, len - sent, 0);
                if (n <= 0)
                {
                    res.errorKind = "io";
                    res.lastError = ::WSAGetLastError();
                    return false;
                }
                sent += n;
            }
            return true;
        };

        if (!sendAll(head.data(), static_cast<int>(head.size())))
        {
            ::closesocket(s);
            return false;
        }

        // ---- body ----
        if (!req.bodyFile.empty())
        {
            HANDLE f = ::CreateFileW(req.bodyFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE)
            {
                res.errorKind = "io";
                res.lastError = static_cast<int>(::GetLastError());
                ::closesocket(s);
                return false;
            }
            // 分片上传：从 bodyOffset 开始，只发 bodySize 个字节
            if (req.bodyOffset > 0)
            {
                LARGE_INTEGER li{};
                li.QuadPart = req.bodyOffset;
                ::SetFilePointerEx(f, li, nullptr, FILE_BEGIN);
            }
            std::vector<char> chunk(64 * 1024);
            DWORD read = 0;
            bool ok = true;
            long long remaining = req.bodySize > 0 ? req.bodySize : -1;
            while (remaining != 0 &&
                   ::ReadFile(f, chunk.data(),
                              static_cast<DWORD>(remaining > 0 && remaining < static_cast<long long>(chunk.size())
                                                     ? remaining : static_cast<long long>(chunk.size())),
                              &read, nullptr) && read > 0)
            {
                if (!sendAll(chunk.data(), static_cast<int>(read))) { ok = false; break; }
                if (remaining > 0) remaining -= static_cast<long long>(read);
            }
            ::CloseHandle(f);
            if (!ok) { ::closesocket(s); return false; }
        }
        else if (!req.body.empty())
        {
            if (!sendAll(req.body.data(), static_cast<int>(req.body.size())))
            {
                ::closesocket(s);
                return false;
            }
        }

        // ---- 读响应 ----
        // 不能"读到对端关连接为止"：对端可能保持 keep-alive，那会一直等到超时，
        // 于是明明拿到了 200 也被判成失败（这正是第一次实测 A→B 全 FAIL 的原因）。
        // 正确做法是解析出 Content-Length，凑满就停。
        std::string raw;
        char buf[8192];
        size_t headerEnd = std::string::npos;
        long long contentLen = -1;
        for (;;)
        {
            const int n = ::recv(s, buf, sizeof(buf), 0);
            if (n == 0) break;
            if (n < 0)
            {
                const int err = ::WSAGetLastError();
                res.errorKind = (err == WSAETIMEDOUT) ? "timeout" : "io";
                res.lastError = err;
                ::closesocket(s);
                return false;
            }
            raw.append(buf, static_cast<size_t>(n));
            if (raw.size() > 8 * 1024 * 1024) break;   // 响应体上限，防止对端发疯

            if (headerEnd == std::string::npos)
            {
                headerEnd = raw.find("\r\n\r\n");
                if (headerEnd != std::string::npos)
                {
                    std::string head = raw.substr(0, headerEnd);
                    for (size_t i = 0; i < head.size(); ++i)
                    {
                        if (head[i] >= 'A' && head[i] <= 'Z') head[i] = static_cast<char>(head[i] - 'A' + 'a');
                    }
                    const size_t cl = head.find("\r\ncontent-length:");
                    if (cl != std::string::npos)
                    {
                        const size_t v = cl + 17;
                        const size_t e = head.find("\r\n", v);
                        contentLen = _strtoi64(head.substr(v, (e == std::string::npos) ? std::string::npos : e - v).c_str(), nullptr, 10);
                    }
                }
            }
            if (headerEnd != std::string::npos)
            {
                if (contentLen < 0) break;    // 没有 body（chunked 本项目不用）
                const long long have = static_cast<long long>(raw.size() - headerEnd - 4);
                if (have >= contentLen) break;
            }
        }
        ::closesocket(s);

        const size_t headEnd = raw.find("\r\n\r\n");
        if (headEnd == std::string::npos)
        {
            res.errorKind = "io";
            return false;
        }

        // 状态行："HTTP/1.1 200 OK"
        const size_t eol = raw.find("\r\n");
        if (eol == std::string::npos) { res.errorKind = "io"; return false; }
        const size_t sp = raw.find(' ', 0);
        if (sp != std::string::npos && sp < eol)
        {
            res.status = atoi(raw.substr(sp + 1, 4).c_str());
        }

        res.body = raw.substr(headEnd + 4);
        res.ok = (res.status > 0);
        return res.ok;
    }

    // ================= 杂项 =================
    long long NowMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    std::string FormatSize(long long bytes)
    {
        char buf[64];
        if (bytes < 1024)
            std::snprintf(buf, sizeof(buf), "%lld B", bytes);
        else if (bytes < 1024LL * 1024)
            std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
        else if (bytes < 1024LL * 1024 * 1024)
            std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
        else
            std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        return std::string(buf);
    }
}
