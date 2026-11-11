#pragma once

#include <winsock2.h>
#include <string>

namespace pp {

void netInit();
void netShutdown();

void setIoTimeouts(SOCKET s, int ms);

// 带超时的 TCP 连接（非阻塞 connect + select）
bool tcpConnect(const std::string& host, unsigned short port, int timeoutMs,
                SOCKET& out, std::string& err);

struct SockHolder {
    SOCKET s;
    SockHolder() : s(INVALID_SOCKET) {}
    ~SockHolder() {
        if (s != INVALID_SOCKET) closesocket(s);
    }

private:
    SockHolder(const SockHolder&);
    SockHolder& operator=(const SockHolder&);
};

} // namespace pp
