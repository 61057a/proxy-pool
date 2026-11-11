#pragma once

#include <string>

namespace pp {

struct Proxy {
    std::string host;
    unsigned short port;
    int latencyMs;
    int fails;

    Proxy() : port(0), latencyMs(0), fails(0) {}

    std::string key() const { return host + ":" + std::to_string((int)port); }
};

enum LogLevel { LV_DEBUG = 0, LV_INFO, LV_WARN, LV_ERROR };

void setVerbose(bool v);
void logf(LogLevel lv, const char* fmt, ...);

// 输出一行/一段文本：控制台走 WriteConsoleW，重定向走原始 UTF-8 字节
void out(const std::string& s);
void outf(const char* fmt, ...);

std::string trim(const std::string& s);
std::string toLower(const std::string& s);
long long nowMs();

// 解析 "1.2.3.4:8080" / "http://1.2.3.4:8080" / "socks5://u:p@host:1080" / 带尾随垃圾的文本行
bool parseProxyLine(const std::string& raw, std::string& host, unsigned short& port);

// "example.com:443" -> host/port
bool splitHostPort(const std::string& s, std::string& host, unsigned short& port);

// "http://example.com:8080/path" -> host/port/path（只接受 http，供校验使用）
bool splitHttpUrl(const std::string& url, std::string& host, unsigned short& port, std::string& path);

} // namespace pp
