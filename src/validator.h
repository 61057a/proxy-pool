#pragma once

#include <string>

namespace pp {

struct CheckResult {
    bool ok;
    int latencyMs;
    std::string exitIp;
    std::string error;

    CheckResult() : ok(false), latencyMs(0) {}
};

// 通过代理访问 checkHost/checkPath，返回出口 IP；同时用来测延迟
CheckResult checkProxy(const std::string& host, unsigned short port, int timeoutMs,
                       const std::string& checkHost, unsigned short checkPort,
                       const std::string& checkPath);

} // namespace pp
