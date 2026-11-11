#include "validator.h"
#include "net.h"
#include "common.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>

namespace pp {

static bool sendAllSock(SOCKET s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        int n = ::send(s, data.data() + off, (int)(data.size() - off), 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

static std::string dechunk(const std::string& in) {
    std::string out;
    size_t pos = 0;
    while (pos < in.size()) {
        size_t eol = in.find("\r\n", pos);
        if (eol == std::string::npos) break;
        std::string lenStr = in.substr(pos, eol - pos);
        size_t semi = lenStr.find(';');
        if (semi != std::string::npos) lenStr = lenStr.substr(0, semi);
        long len = strtol(lenStr.c_str(), NULL, 16);
        if (len <= 0) break;
        pos = eol + 2;
        if (pos + (size_t)len > in.size()) { out.append(in, pos, std::string::npos); break; }
        out.append(in, pos, (size_t)len);
        pos += (size_t)len;
        if (in.compare(pos, 2, "\r\n") == 0) pos += 2;
    }
    return out;
}

static bool looksLikeIp(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    bool hasDot = false;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '.') { hasDot = true; continue; }
        if (c == ':') continue;
        if (!std::isxdigit(c)) return false;
    }
    return hasDot || s.find(':') != std::string::npos;
}

CheckResult checkProxy(const std::string& host, unsigned short port, int timeoutMs,
                       const std::string& checkHost, unsigned short checkPort,
                       const std::string& checkPath) {
    CheckResult r;
    long long t0 = nowMs();

    SOCKET raw = INVALID_SOCKET;
    std::string err;
    if (!tcpConnect(host, port, timeoutMs, raw, err)) {
        r.error = err;
        return r;
    }
    SockHolder holder;
    holder.s = raw;

    std::string hostHdr = checkHost;
    if (checkPort != 80) hostHdr += ":" + std::to_string((int)checkPort);

    std::string req;
    req += "GET http://" + hostHdr + checkPath + " HTTP/1.1\r\n";
    req += "Host: " + hostHdr + "\r\n";
    req += "User-Agent: ProxyPool/1.0\r\n";
    req += "Accept: */*\r\n";
    req += "Connection: close\r\n\r\n";

    if (!sendAllSock(holder.s, req)) {
        r.error = "发送请求失败";
        return r;
    }

    std::string resp;
    char buf[8192];
    const long long deadline = t0 + timeoutMs * 2;
    while (resp.size() < 256u * 1024u) {
        int n = recv(holder.s, buf, (int)sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, (size_t)n);
        if (nowMs() > deadline) break;
    }

    if (resp.empty()) {
        r.error = "无响应";
        return r;
    }

    size_t he = resp.find("\r\n\r\n");
    if (he == std::string::npos) {
        r.error = "响应头不完整";
        return r;
    }

    std::string headers = resp.substr(0, he);
    std::string body = resp.substr(he + 4);

    size_t sp = headers.find(' ');
    int code = (sp == std::string::npos) ? 0 : std::atoi(headers.c_str() + sp + 1);
    if (code != 200) {
        r.error = "HTTP " + std::to_string(code);
        return r;
    }

    if (toLower(headers).find("transfer-encoding: chunked") != std::string::npos)
        body = dechunk(body);

    body = trim(body);
    if (!looksLikeIp(body)) {
        r.error = "响应不是 IP 明文";
        return r;
    }

    r.ok = true;
    r.exitIp = body;
    r.latencyMs = (int)(nowMs() - t0);
    return r;
}

} // namespace pp
