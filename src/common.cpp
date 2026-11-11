#include "common.h"

#include <windows.h>
#include <io.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <chrono>
#include <vector>

namespace pp {

static std::mutex g_logMu;
static bool g_verbose = false;

void setVerbose(bool v) { g_verbose = v; }

static void writeOut(const char* s, size_t n) {
    static int isConsole = -1;
    if (isConsole < 0) isConsole = _isatty(_fileno(stdout)) ? 1 : 0;

    if (isConsole && n > 0) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, NULL, 0);
        if (wlen > 0) {
            std::vector<wchar_t> w((size_t)wlen);
            MultiByteToWideChar(CP_UTF8, 0, s, (int)n, &w[0], wlen);
            DWORD written = 0;
            WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), &w[0], (DWORD)wlen, &written, NULL);
            return;
        }
    }
    std::fwrite(s, 1, n, stdout);
    std::fflush(stdout);
}

void out(const std::string& s) { writeOut(s.data(), s.size()); }

void outf(const char* fmt, ...) {
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';
    out(std::string(msg));
}

static const char* levelTag(LogLevel lv) {
    switch (lv) {
        case LV_DEBUG: return "debug";
        case LV_INFO:  return "info";
        case LV_WARN:  return "warn";
        case LV_ERROR: return "error";
    }
    return "?";
}

void logf(LogLevel lv, const char* fmt, ...) {
    if (lv == LV_DEBUG && !g_verbose) return;

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';

    time_t t = time(NULL);
    struct tm tmv;
    struct tm* pt = localtime(&t);
    if (pt) tmv = *pt;
    else std::memset(&tmv, 0, sizeof(tmv));

    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);

    char line[2200];
    snprintf(line, sizeof(line), "[%s] %-5s %s\n", ts, levelTag(lv), msg);

    std::lock_guard<std::mutex> lk(g_logMu);
    out(std::string(line));
}

long long nowMs() {
    using namespace std::chrono;
    return (long long)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string toLower(const std::string& s) {
    std::string r = s;
    for (size_t i = 0; i < r.size(); ++i)
        r[i] = (char)std::tolower((unsigned char)r[i]);
    return r;
}

bool splitHostPort(const std::string& s, std::string& host, unsigned short& port) {
    std::string t = trim(s);
    size_t colon = t.rfind(':');
    if (colon == std::string::npos) return false;
    host = t.substr(0, colon);
    std::string ps = t.substr(colon + 1);
    if (host.empty() || ps.empty()) return false;
    int p = std::atoi(ps.c_str());
    if (p <= 0 || p > 65535) return false;
    port = (unsigned short)p;
    return true;
}

bool parseProxyLine(const std::string& raw, std::string& host, unsigned short& port) {
    std::string s = trim(raw);

    // UTF-8 BOM
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF &&
        (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s = s.substr(3);

    if (s.empty()) return false;
    if (s[0] == '#' || s[0] == '/' || s[0] == ';') return false;
    if (s.find('{') != std::string::npos || s.find('<') != std::string::npos ||
        s.find('"') != std::string::npos)
        return false;  // JSON / HTML 之类的行

    size_t sp = s.find_first_of(" \t\r\n,;|");
    if (sp != std::string::npos) s = s.substr(0, sp);

    size_t scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);

    size_t at = s.find('@');
    if (at != std::string::npos) s = s.substr(at + 1);

    size_t slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);

    // 兼容 "ip:port" 与 "ip:port:国家/地区"
    size_t c1 = s.find(':');
    if (c1 == std::string::npos) return false;
    size_t c2 = s.find(':', c1 + 1);

    host = s.substr(0, c1);
    std::string ps = (c2 == std::string::npos) ? s.substr(c1 + 1)
                                               : s.substr(c1 + 1, c2 - c1 - 1);
    if (host.empty() || ps.empty() || host.size() > 253) return false;

    for (size_t i = 0; i < host.size(); ++i) {
        unsigned char c = (unsigned char)host[i];
        if (!(std::isalnum(c) || c == '.' || c == '-'))
            return false;
    }
    for (size_t i = 0; i < ps.size(); ++i)
        if (!std::isdigit((unsigned char)ps[i])) return false;

    int p = std::atoi(ps.c_str());
    if (p <= 0 || p > 65535) return false;
    port = (unsigned short)p;
    return true;
}

bool splitHttpUrl(const std::string& url, std::string& host, unsigned short& port, std::string& path) {
    std::string u = trim(url);
    std::string low = toLower(u);
    if (low.compare(0, 7, "http://") != 0) return false;

    std::string rest = u.substr(7);
    size_t slash = rest.find('/');
    std::string hp = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    if (hp.empty()) return false;

    size_t colon = hp.rfind(':');
    if (colon != std::string::npos) {
        host = hp.substr(0, colon);
        int p = std::atoi(hp.c_str() + colon + 1);
        if (p <= 0 || p > 65535) return false;
        port = (unsigned short)p;
    } else {
        host = hp;
        port = 80;
    }
    return !host.empty();
}

} // namespace pp
