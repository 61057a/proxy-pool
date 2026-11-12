#include "server.h"
#include "common.h"

#include <ws2tcpip.h>
#include <thread>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace pp {

static bool sendAll(SOCKET s, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = ::send(s, data + off, (int)(len - off), 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

static void sendSimple(SOCKET s, int code, const char* reason, const std::string& body) {
    char head[512];
    snprintf(head, sizeof(head),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: text/plain; charset=utf-8\r\n"
             "Content-Length: %u\r\n"
             "Connection: close\r\n\r\n",
             code, reason, (unsigned)body.size());
    sendAll(s, head, strlen(head));
    sendAll(s, body.data(), body.size());
}

// 一直读到请求头结束，返回读到的全部字节（可能含请求体开头）
static bool readHeaders(SOCKET s, std::string& out, size_t maxBytes) {
    char buf[4096];
    out.clear();
    while (out.size() < maxBytes) {
        int n = recv(s, buf, (int)sizeof(buf), 0);
        if (n <= 0) return false;
        out.append(buf, (size_t)n);
        if (out.find("\r\n\r\n") != std::string::npos) return true;
    }
    return false;
}

static void pump(SOCKET from, SOCKET to) {
    std::vector<char> buf(32768);
    for (;;) {
        int n = recv(from, &buf[0], (int)buf.size(), 0);
        if (n <= 0) break;
        if (!sendAll(to, &buf[0], (size_t)n)) break;
    }
    shutdown(to, SD_SEND);
}

static void relay(SOCKET a, SOCKET b) {
    std::thread t(pump, a, b);
    pump(b, a);
    t.join();
}

static void sendControlResponse(SOCKET s, const std::string& body) {
    char head[256];
    snprintf(head, sizeof(head),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/plain; charset=utf-8\r\n"
             "Content-Length: %u\r\n"
             "Cache-Control: no-store\r\n"
             "Connection: close\r\n\r\n",
             (unsigned)body.size());
    sendAll(s, head, strlen(head));
    sendAll(s, body.data(), body.size());
}

// 从 "/rotate?dead=1.2.3.4:8080&ttl=3600" 里取参数；不做 URL 解码，
// 控制接口只接受 ASCII 节点 key 和数字。
static std::string queryParam(const std::string& target, const char* name) {
    size_t q = target.find('?');
    if (q == std::string::npos) return std::string();
    std::string query = target.substr(q + 1);
    std::string key(name);
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        std::string pair = (amp == std::string::npos)
                               ? query.substr(pos)
                               : query.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key)
            return pair.substr(eq + 1);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return std::string();
}

// 节点 key 只允许 ip/域名 + 端口，避免把控制参数原样写进日志或状态
static bool saneKey(const std::string& key) {
    if (key.empty() || key.size() > 128) return false;
    for (size_t i = 0; i < key.size(); ++i) {
        char c = key[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') || c == '.' || c == ':' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

ProxyServer::ProxyServer(ProxyPool& pool, const std::string& bindHost, int port,
                         int connectTimeoutMs, int controlPort)
    : pool_(pool), bindHost_(bindHost), port_(port), connectTimeoutMs_(connectTimeoutMs),
      stopping_(false), listen_(INVALID_SOCKET), served_(0), failed_(0),
      ctlPort_(controlPort), ctlListen_(INVALID_SOCKET), ctlStopping_(false) {}

ProxyServer::~ProxyServer() { stop(); }

bool ProxyServer::start(std::string& err) {
    listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_ == INVALID_SOCKET) {
        err = "socket() 失败 (错误码 " + std::to_string((int)WSAGetLastError()) + ")";
        return false;
    }

    BOOL exclusive = TRUE;
    setsockopt(listen_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               (const char*)&exclusive, sizeof(exclusive));

    sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port_);
    if (bindHost_.empty() || bindHost_ == "0.0.0.0" || bindHost_ == "*")
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else
        addr.sin_addr.s_addr = inet_addr(bindHost_.c_str());
    if (addr.sin_addr.s_addr == INADDR_NONE && bindHost_ != "255.255.255.255") {
        addrinfo hints;
        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_INET;
        addrinfo* res = NULL;
        if (getaddrinfo(bindHost_.c_str(), NULL, &hints, &res) == 0 && res) {
            addr.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        } else {
            err = "无法解析绑定地址: " + bindHost_;
            closesocket(listen_);
            listen_ = INVALID_SOCKET;
            return false;
        }
    }

    if (bind(listen_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        int e = WSAGetLastError();
        err = "绑定 " + bindHost_ + ":" + std::to_string(port_) + " 失败";
        err += (e == WSAEADDRINUSE) ? "（端口被占用）" : "（错误码 " + std::to_string(e) + "）";
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
        return false;
    }

    if (listen(listen_, SOMAXCONN) == SOCKET_ERROR) {
        err = "listen() 失败";
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
        return false;
    }

    // 控制接口只绑本机，和代理端口分开，避免外部机器直接操作固定节点
    if (ctlPort_ > 0 && ctlPort_ != port_) {
        SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (c != INVALID_SOCKET) {
            BOOL exclusive = TRUE;
            setsockopt(c, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       (const char*)&exclusive, sizeof(exclusive));

            sockaddr_in ca;
            ZeroMemory(&ca, sizeof(ca));
            ca.sin_family = AF_INET;
            ca.sin_port = htons((u_short)ctlPort_);
            ca.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            if (bind(c, (sockaddr*)&ca, sizeof(ca)) == 0 &&
                listen(c, SOMAXCONN) != SOCKET_ERROR) {
                ctlListen_ = c;
                std::thread(&ProxyServer::runControl, this).detach();
                logf(LV_INFO, "控制接口 http://127.0.0.1:%d/status", ctlPort_);
            } else {
                logf(LV_WARN, "控制接口 %d 启动失败，固定出口 / 自动换出口不可用", ctlPort_);
                closesocket(c);
            }
        }
    }
    return true;
}

void ProxyServer::stop() {
    stopping_ = true;
    if (listen_ != INVALID_SOCKET) {
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
    }
    ctlStopping_ = true;
    if (ctlListen_ != INVALID_SOCKET) {
        closesocket(ctlListen_);
        ctlListen_ = INVALID_SOCKET;
    }
}

void ProxyServer::runUntilStopped() {
    while (!stopping_) {
        sockaddr_in peer;
        int plen = (int)sizeof(peer);
        SOCKET c = accept(listen_, (sockaddr*)&peer, &plen);
        if (c == INVALID_SOCKET) {
            if (stopping_) break;
            continue;
        }
        std::thread(&ProxyServer::handleClient, this, c).detach();
    }
}

void ProxyServer::handleClient(SOCKET client) {
    SockHolder ch;
    ch.s = client;
    setIoTimeouts(client, 45000);

    std::string head;
    if (!readHeaders(client, head, 64 * 1024)) {
        logf(LV_DEBUG, "客户端请求头读取失败/超时");
        return;
    }

    size_t lineEnd = head.find("\r\n");
    if (lineEnd == std::string::npos) lineEnd = head.size();
    std::string reqLine = head.substr(0, lineEnd);

    size_t sp1 = reqLine.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos
                                            : reqLine.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        sendSimple(client, 400, "Bad Request", "bad request line\n");
        return;
    }

    std::string method = reqLine.substr(0, sp1);
    std::string target = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);
    bool isConnect = (method == "CONNECT");

    std::string dstHost;
    unsigned short dstPort = 0;
    if (isConnect) {
        if (!splitHostPort(target, dstHost, dstPort)) {
            sendSimple(client, 400, "Bad Request", "bad CONNECT target\n");
            return;
        }
    } else {
        std::string path;
        if (!splitHttpUrl(target, dstHost, dstPort, path)) {
            sendSimple(client, 501, "Not Implemented",
                       "only explicit HTTP proxy (http:// absolute-URI) and CONNECT are supported\n");
            return;
        }
    }

    std::string lastErr = "no upstream proxy";
    for (int attempt = 0; attempt < 3; ++attempt) {
        Proxy up;
        if (!pool_.next(up)) {
            ++failed_;
            sendSimple(client, 503, "Service Unavailable",
                       "proxy pool is empty or all upstreams are dead, retry in a moment\n");
            return;
        }

        SOCKET ups = INVALID_SOCKET;
        std::string err;
        if (!tcpConnect(up.host, up.port, connectTimeoutMs_, ups, err)) {
            pool_.reportFailure(up.key());
            lastErr = err;
            ++failed_;
            continue;
        }
        SockHolder uh;
        uh.s = ups;
        setIoTimeouts(ups, 45000);

        if (isConnect) {
            std::string req = "CONNECT " + target + " HTTP/1.1\r\n";
            req += "Host: " + target + "\r\n";
            req += "Proxy-Connection: keep-alive\r\n\r\n";
            if (!sendAll(ups, req.data(), req.size())) {
                pool_.reportFailure(up.key());
                lastErr = "上游代理发送 CONNECT 失败";
                continue;
            }

            std::string resp;
            if (!readHeaders(ups, resp, 32 * 1024)) {
                pool_.reportFailure(up.key());
                lastErr = "上游代理无响应";
                continue;
            }

            size_t e = resp.find("\r\n");
            std::string statusLine = resp.substr(0, e);
            if (statusLine.find(" 200") == std::string::npos) {
                pool_.reportFailure(up.key());
                lastErr = "上游返回: " + statusLine;
                logf(LV_DEBUG, "代理 %s 隧道建立失败: %s", up.key().c_str(), statusLine.c_str());
                continue;
            }

            const char* established =
                "HTTP/1.1 200 Connection Established\r\n"
                "Proxy-Agent: ProxyPool\r\n\r\n";
            if (!sendAll(client, established, strlen(established))) return;
        } else {
            // 明文 HTTP：请求头是绝对 URI 形式，HTTP 代理可直接接受，原样转发
            if (!sendAll(ups, head.data(), head.size())) {
                pool_.reportFailure(up.key());
                lastErr = "转发请求失败";
                continue;
            }
        }

        pool_.reportSuccess(up.key());
        ++served_;
        logStatsIfNeeded();
        logf(LV_DEBUG, "%s %s -> %s", method.c_str(), target.c_str(), up.key().c_str());

        relay(client, ups);
        return;
    }

    ++failed_;
    sendSimple(client, 502, "Bad Gateway", "all upstream proxies failed: " + lastErr + "\n");
}

void ProxyServer::logStatsIfNeeded() {
    unsigned long long n = served_.load();
    if (n % 50 == 0) {
        std::string s = std::to_string(n);
        std::string f = std::to_string(failed_.load());
        logf(LV_INFO, "已转发 %s 条请求，失败 %s 条，池内可用 %u 个",
             s.c_str(), f.c_str(), (unsigned)pool_.size());
    }
}

void ProxyServer::runControl() {
    while (!ctlStopping_) {
        SOCKET c = accept(ctlListen_, NULL, NULL);
        if (c == INVALID_SOCKET) {
            if (ctlStopping_) break;
            continue;
        }
        std::thread(&ProxyServer::handleControl, this, c).detach();
    }
}

void ProxyServer::handleControl(SOCKET client) {
    SockHolder holder;
    holder.s = client;
    setIoTimeouts(client, 3000);

    std::string head;
    if (!readHeaders(client, head, 16 * 1024)) return;

    size_t lineEnd = head.find("\r\n");
    std::string reqLine = head.substr(0, lineEnd);
    size_t sp1 = reqLine.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos
                                            : reqLine.find(' ', sp1 + 1);
    std::string target;
    if (sp1 != std::string::npos && sp2 != std::string::npos)
        target = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);

    std::string body = controlResponse(target);
    sendControlResponse(client, body);
}

void ProxyServer::pruneQuotaDeadLocked() {
    long long now = nowMs();
    for (std::map<std::string, long long>::iterator it = quotaDead_.begin();
         it != quotaDead_.end();) {
        if (it->second <= now)
            quotaDead_.erase(it++);
        else
            ++it;
    }
}

bool ProxyServer::chooseFixedLocked(std::string& out) {
    pruneQuotaDeadLocked();
    std::vector<Proxy> list = pool_.snapshot();
    std::string cur = pool_.pinned();

    // 已经固定在一个健康节点上就不要动，避免重复调用时出口 IP 乱跳
    if (!cur.empty()) {
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i].key() == cur && list[i].fails < 3 &&
                quotaDead_.find(cur) == quotaDead_.end()) {
                out = cur;
                return true;
            }
        }
    }

    // install() 已按延迟排序，取第一个健康的、没被标记额度用尽的
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].fails < 3 && quotaDead_.find(list[i].key()) == quotaDead_.end()) {
            out = list[i].key();
            pool_.pin(out);
            logf(LV_INFO, "控制接口固定出口 %s", out.c_str());
            return true;
        }
    }
    return false;
}

bool ProxyServer::chooseRotateLocked(const std::string& dead, int ttlSec, std::string& out) {
    pruneQuotaDeadLocked();
    if (!dead.empty()) {
        quotaDead_[dead] = nowMs() + (long long)ttlSec * 1000;
        logf(LV_INFO, "节点 %s 额度用尽，%d 秒内不再使用", dead.c_str(), ttlSec);
    }

    std::vector<Proxy> list = pool_.snapshot();
    std::string cur = pool_.pinned();

    size_t start = 0;
    if (!cur.empty()) {
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i].key() == cur) {
                start = i + 1;
                break;
            }
        }
    }

    size_t n = list.size();
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (start + i) % n;
        const std::string& key = list[idx].key();
        // 绕一圈回到自己不算“换出口”
        if (!cur.empty() && key == cur) continue;
        if (list[idx].fails < 3 && quotaDead_.find(key) == quotaDead_.end()) {
            out = key;
            pool_.pin(out);
            logf(LV_INFO, "控制接口切换出口 %s", out.c_str());
            return true;
        }
    }

    if (!cur.empty()) out = cur;
    return false;
}

std::string ProxyServer::controlResponse(const std::string& target) {
    size_t q = target.find('?');
    std::string path = (q == std::string::npos) ? target : target.substr(0, q);

    std::ostringstream ss;
    std::lock_guard<std::mutex> lk(ctlMu_);

    if (path == "/status") {
        std::string pinned = pool_.pinned();
        ss << "ok=1\n"
           << "mode=" << (pinned.empty() ? "auto" : "fixed") << "\n"
           << "pinned=" << pinned << "\n"
           << "size=" << (unsigned)pool_.size() << "\n"
           << "total=" << (unsigned)pool_.total() << "\n"
           << "served=" << served_.load() << "\n"
           << "failed=" << failed_.load() << "\n"
           << "quota_dead=" << (unsigned)quotaDead_.size() << "\n";
        return ss.str();
    }

    if (path == "/fixed") {
        std::string key;
        if (chooseFixedLocked(key))
            ss << "ok=1\npinned=" << key << "\n";
        else
            ss << "ok=0\npinned=\nreason=no healthy node\n";
        return ss.str();
    }

    if (path == "/rotate") {
        std::string dead = queryParam(target, "dead");
        if (!saneKey(dead)) dead.clear();
        int ttlSec = std::atoi(queryParam(target, "ttl").c_str());
        if (ttlSec < 60) ttlSec = 3600;
        if (ttlSec > 86400 * 7) ttlSec = 86400 * 7;

        std::string key;
        if (chooseRotateLocked(dead, ttlSec, key))
            ss << "ok=1\npinned=" << key << "\n";
        else
            ss << "ok=0\npinned=" << key << "\nreason=no alternative node\n";
        return ss.str();
    }

    if (path == "/auto") {
        pool_.pin("");
        ss << "ok=1\nmode=auto\n";
        logf(LV_INFO, "控制接口恢复自动轮换");
        return ss.str();
    }

    ss << "ok=0\nreason=unknown endpoint\n";
    return ss.str();
}

} // namespace pp
