#include "server.h"
#include "common.h"

#include <ws2tcpip.h>
#include <thread>
#include <vector>
#include <cstring>
#include <cstdio>

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

ProxyServer::ProxyServer(ProxyPool& pool, const std::string& bindHost, int port,
                         int connectTimeoutMs)
    : pool_(pool), bindHost_(bindHost), port_(port), connectTimeoutMs_(connectTimeoutMs),
      stopping_(false), listen_(INVALID_SOCKET), served_(0), failed_(0) {}

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
    return true;
}

void ProxyServer::stop() {
    stopping_ = true;
    if (listen_ != INVALID_SOCKET) {
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
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

} // namespace pp
