#pragma once

#include "net.h"
#include "pool.h"

#include <string>
#include <atomic>

namespace pp {

// 本地 HTTP 代理服务器：
//   - CONNECT  -> 隧道转发（HTTPS）
//   - 绝对 URI -> 明文 HTTP 转发
// 每条连接从代理池里取一个上游，失败自动换下一个。
class ProxyServer {
public:
    ProxyServer(ProxyPool& pool, const std::string& bindHost, int port, int connectTimeoutMs);
    ~ProxyServer();

    bool start(std::string& err);
    void runUntilStopped();
    void stop();

    unsigned long long served() const { return served_.load(); }
    unsigned long long failed() const { return failed_.load(); }
    bool listening() const { return listen_ != INVALID_SOCKET; }

private:
    void handleClient(SOCKET client);
    void logStatsIfNeeded();

    ProxyPool& pool_;
    std::string bindHost_;
    int port_;
    int connectTimeoutMs_;
    std::atomic<bool> stopping_;
    SOCKET listen_;
    std::atomic<unsigned long long> served_;
    std::atomic<unsigned long long> failed_;
};

} // namespace pp
