#pragma once

#include "net.h"
#include "pool.h"

#include <string>
#include <atomic>
#include <map>
#include <mutex>

namespace pp {

// 本地 HTTP 代理服务器：
//   - CONNECT  -> 隧道转发（HTTPS）
//   - 绝对 URI -> 明文 HTTP 转发
// 每条连接从代理池里取一个上游，失败自动换下一个。
//
// 另开一个 127.0.0.1 控制接口（默认代理端口+1），给启动器这类外部程序用：
//   GET /status   查看固定节点 / 池大小 / 转发计数
//   GET /fixed    固定到一个可用节点（OpenCode 启动时用，保证出口 IP 不变）
//   GET /rotate   换到下一个节点，旧节点按 ttl 秒记入“额度用尽”名单
//   GET /auto     恢复自动轮换
class ProxyServer {
public:
    ProxyServer(ProxyPool& pool, const std::string& bindHost, int port, int connectTimeoutMs,
                int controlPort = 0);
    ~ProxyServer();

    bool start(std::string& err);
    void runUntilStopped();
    void stop();

    unsigned long long served() const { return served_.load(); }
    unsigned long long failed() const { return failed_.load(); }
    bool listening() const { return listen_ != INVALID_SOCKET; }
    int controlPort() const { return ctlPort_; }

private:
    void handleClient(SOCKET client);
    void logStatsIfNeeded();

    void runControl();
    void handleControl(SOCKET client);
    std::string controlResponse(const std::string& target);
    void pruneQuotaDeadLocked();
    bool chooseFixedLocked(std::string& out);
    bool chooseRotateLocked(const std::string& dead, int ttlSec, std::string& out);

    ProxyPool& pool_;
    std::string bindHost_;
    int port_;
    int connectTimeoutMs_;
    std::atomic<bool> stopping_;
    SOCKET listen_;
    std::atomic<unsigned long long> served_;
    std::atomic<unsigned long long> failed_;

    // 控制接口
    int ctlPort_;
    SOCKET ctlListen_;
    std::atomic<bool> ctlStopping_;
    std::mutex ctlMu_;
    std::map<std::string, long long> quotaDead_;  // 节点 key -> 额度恢复时间(ms)
};

} // namespace pp
