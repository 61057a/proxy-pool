#include "net.h"
#include "common.h"

#include <ws2tcpip.h>
#include <chrono>
#include <cstdio>

namespace pp {

void netInit() {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
}

void netShutdown() { WSACleanup(); }

void setIoTimeouts(SOCKET s, int ms) {
    DWORD v = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&v, sizeof(v));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&v, sizeof(v));
}

bool tcpConnect(const std::string& host, unsigned short port, int timeoutMs,
                SOCKET& out, std::string& err) {
    out = INVALID_SOCKET;

    struct addrinfo hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    struct addrinfo* res = NULL;
    int rc = getaddrinfo(host.c_str(), portstr, &hints, &res);
    if (rc != 0 || !res) {
        err = "DNS 解析失败: " + host;
        return false;
    }

    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    for (struct addrinfo* p = res; p; p = p->ai_next) {
        SOCKET s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);

        int r = ::connect(s, p->ai_addr, (int)p->ai_addrlen);
        bool ok = (r == 0);

        if (!ok) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINVAL) {
                fd_set wf, ef;
                FD_ZERO(&wf);
                FD_SET(s, &wf);
                FD_ZERO(&ef);
                FD_SET(s, &ef);

                long long remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now()).count();
                if (remain < 0) remain = 0;

                timeval tv;
                tv.tv_sec = (long)(remain / 1000);
                tv.tv_usec = (long)((remain % 1000) * 1000);

                int sel = select(0, NULL, &wf, &ef, &tv);
                if (sel > 0 && FD_ISSET(s, &wf)) {
                    int soerr = 0;
                    int len = (int)sizeof(soerr);
                    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len);
                    ok = (soerr == 0);
                }
            }
        }

        u_long blk = 0;
        ioctlsocket(s, FIONBIO, &blk);

        if (ok) {
            setIoTimeouts(s, timeoutMs);
            freeaddrinfo(res);
            out = s;
            return true;
        }

        closesocket(s);
        if (std::chrono::steady_clock::now() >= deadline) break;
    }

    freeaddrinfo(res);
    err = "连接失败: " + host + ":" + std::to_string((int)port);
    return false;
}

} // namespace pp
