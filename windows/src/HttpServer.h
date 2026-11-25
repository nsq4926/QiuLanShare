#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

struct HttpRequest
{
    std::string method;
    std::string path;      // 不含 query
    std::string query;
    std::map<std::string, std::string> headers;   // key 已转小写
    std::string body;
    std::uintptr_t sock = 0;
};

struct HttpResponse
{
    int status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    std::wstring sendFile;   // 非空则流式发送该文件
};

// 极简 HTTP/1.1 服务：每个连接一个线程，仅监听局域网，无鉴权。
class HttpServer
{
public:
    static HttpServer& Instance();

    bool Start(unsigned short port);
    void Stop();
    bool IsRunning() const { return m_running.load(); }
    std::string LastError();

private:
    HttpServer() = default;
    void AcceptLoop();
    void HandleClient(std::uintptr_t sockRaw);
    void StreamUpload(std::uintptr_t sockRaw, const HttpRequest& req, long long contentLength);

    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_stopping{ false };
    std::uintptr_t    m_listenSock = 0;
    std::thread       m_acceptThread;
    std::string       m_lastError;
    std::mutex        m_errorMutex;

    std::mutex                     m_clientsMutex;
    std::vector<std::uintptr_t>     m_clients;

    // 每个连接一个线程，必须有上限：客户端异常重连时不能把宿主进程的线程数打爆
    std::atomic<int>                m_connCount{ 0 };
};
