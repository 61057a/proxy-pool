#include "app.h"
#include "net.h"
#include "fetcher.h"
#include "validator.h"

#include <windows.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <random>
#include <set>
#include <thread>

namespace pp {

std::atomic<bool> g_stop(false);
RefreshStatus g_refresh;

RefreshStatus::RefreshStatus() : isRunning(false), done(0), total(0), found(0) {}

void RefreshStatus::begin() {
    isRunning = true;
    done = 0;
    total = 0;
    found = 0;
    setStage("准备中");
}

void RefreshStatus::setStage(const std::string& s) {
    std::lock_guard<std::mutex> lk(mu_);
    stage_ = s;
}

void RefreshStatus::setTotal(int n) { total = n; }

void RefreshStatus::tick(bool ok) {
    done.fetch_add(1);
    if (ok) found.fetch_add(1);
}

void RefreshStatus::end() { isRunning = false; }

std::string RefreshStatus::stage() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stage_;
}

Options::Options()
    : port(8080), threads(64), timeoutMs(3000), refreshSec(600), maxProxies(100), minPool(10),
      host("127.0.0.1"), sourcesFile("sources.txt"), checkUrl("http://icanhazip.com/"),
      once(false), verbose(false), noValidate(false) {}

namespace {

const char* kDefaultSources[] = {
    "https://cdn.jsdelivr.net/gh/TheSpeedX/PROXY-List@master/http.txt",
    "https://cdn.jsdelivr.net/gh/monosans/proxy-list@main/proxies/http.txt",
    "https://cdn.jsdelivr.net/gh/proxifly/free-proxy-list@main/proxies/protocols/http/data.txt",
    "https://cdn.jsdelivr.net/gh/zloi-user/hideip.me@main/http.txt",
    NULL};

// 首选校验地址命中太少时自动换这个
const char* kFallbackCheckUrl = "http://ip.3322.net/";

struct RefreshGuard {
    ~RefreshGuard() { g_refresh.end(); }
};

std::set<std::string> fetchAll(const std::vector<std::string>& urls, int timeoutMs) {
    std::mutex mu;
    std::set<std::string> uniq;
    std::vector<std::thread> ts;

    for (size_t k = 0; k < urls.size(); ++k) {
        std::string url = urls[k];
        ts.push_back(std::thread([&, url]() {
            std::string body, err;
            if (!httpGet(url, body, err, timeoutMs)) {
                logf(LV_WARN, "抓取失败  %s  (%s)", url.c_str(), err.c_str());
                return;
            }
            std::set<std::string> local;
            size_t pos = 0;
            while (pos <= body.size()) {
                size_t eol = body.find('\n', pos);
                if (eol == std::string::npos) eol = body.size();
                std::string line = body.substr(pos, eol - pos);
                std::string h;
                unsigned short p = 0;
                if (parseProxyLine(line, h, p)) local.insert(h + ":" + std::to_string((int)p));
                if (eol >= body.size()) break;
                pos = eol + 1;
            }
            if (local.empty()) {
                logf(LV_WARN, "来源无有效条目  %s", url.c_str());
                return;
            }
            {
                std::lock_guard<std::mutex> lk(mu);
                for (std::set<std::string>::iterator it = local.begin(); it != local.end(); ++it)
                    uniq.insert(*it);
            }
            logf(LV_INFO, "来源 %-56s +%u 条", url.c_str(), (unsigned)local.size());
        }));
    }
    for (size_t i = 0; i < ts.size(); ++i) ts[i].join();
    return uniq;
}

std::vector<Proxy> validateAll(const std::vector<std::string>& keys, const Options& o,
                               const std::string& checkHost, unsigned short checkPort,
                               const std::string& checkPath,
                               const std::vector<std::string>& ownIps, const char* label) {
    std::vector<Proxy> good;
    std::mutex mu;
    std::atomic<size_t> idx(0);
    std::atomic<int> goodCount(0);

    g_refresh.setStage(label);
    g_refresh.setTotal((int)keys.size());
    g_refresh.done = 0;
    g_refresh.found = 0;

    std::vector<std::thread> ts;
    for (int w = 0; w < o.threads; ++w) {
        ts.push_back(std::thread([&]() {
            for (;;) {
                if (g_stop) break;
                size_t i = idx.fetch_add(1);
                if (i >= keys.size()) break;

                std::string host;
                unsigned short port = 0;
                if (!splitHostPort(keys[i], host, port)) {
                    g_refresh.tick(false);
                    continue;
                }

                bool ok = false;
                int latency = 0;

                if (o.noValidate) {
                    SOCKET s = INVALID_SOCKET;
                    std::string e;
                    long long t0 = nowMs();
                    if (tcpConnect(host, port, o.timeoutMs, s, e)) {
                        closesocket(s);
                        ok = true;
                        latency = (int)(nowMs() - t0);
                    }
                } else {
                    CheckResult r =
                        checkProxy(host, port, o.timeoutMs, checkHost, checkPort, checkPath);
                    ok = r.ok;
                    latency = r.latencyMs;
                    // 返回的出口 IP 就是本机公网 IP，说明请求根本没被转发
                    // （典型情况是 CDN 边缘节点直接应答了回显站）
                    if (ok) {
                        for (size_t k = 0; k < ownIps.size(); ++k) {
                            if (r.exitIp == ownIps[k]) {
                                ok = false;
                                logf(LV_DEBUG, "假代理 %s:%u （返回本机 IP）", host.c_str(),
                                     (unsigned)port);
                                break;
                            }
                        }
                    }
                    if (!ok && !r.error.empty())
                        logf(LV_DEBUG, "失效 %s:%u （%s）", host.c_str(), (unsigned)port,
                             r.error.c_str());
                }

                if (ok) {
                    Proxy p;
                    p.host = host;
                    p.port = port;
                    p.latencyMs = latency;
                    std::lock_guard<std::mutex> lk(mu);
                    good.push_back(p);
                    goodCount.fetch_add(1);
                }

                g_refresh.tick(ok);
                int d = g_refresh.done.load();
                if (d % 500 == 0)
                    logf(LV_INFO, "[%s] 进度 %d/%u，通过 %d", label, d, (unsigned)keys.size(),
                         goodCount.load());
            }
        }));
    }
    for (size_t i = 0; i < ts.size(); ++i) ts[i].join();
    return good;
}

// 取本机公网 IP：用于识别"直连回显站"的假代理
std::vector<std::string> probeOwnIps() {
    std::vector<std::string> ownIps;
    const char* probes[] = {"http://ipv4.icanhazip.com/", "http://ip.3322.net/", NULL};
    for (int i = 0; probes[i]; ++i) {
        std::string ip, e;
        if (!httpGet(probes[i], ip, e, 8000)) continue;
        ip = trim(ip);
        if (ip.empty() || ip.size() > 64) continue;
        bool dup = false;
        for (size_t k = 0; k < ownIps.size(); ++k)
            if (ownIps[k] == ip) dup = true;
        if (!dup) ownIps.push_back(ip);
    }
    return ownIps;
}

} // namespace

void printUsage() {
    out(
        "\n代理池启动工具 (proxypool)\n"
        "从公开源抓取免费 HTTP 代理，校验后在本机开一个可轮换的 HTTP 代理。\n\n"
        "用法:\n"
        "  proxypool [选项]          命令行模式\n"
        "  proxypool-gui.exe         图形界面（可手动选择节点）\n\n"
        "选项（两种模式通用）:\n"
        "  --port N        本地监听端口，默认 8080\n"
        "  --host IP       绑定地址，默认 127.0.0.1（局域网共享用 0.0.0.0）\n"
        "  --sources FILE  代理源列表文件，默认 exe 同目录 sources.txt\n"
        "  --threads N     校验并发数，默认 64\n"
        "  --timeout MS    单个代理的连接/校验超时，默认 3000\n"
        "  --refresh SEC   刷新间隔秒数，默认 600\n"
        "  --max N         池中最多保留的代理数，默认 100\n"
        "  --min N         池中低于该数量时提前刷新，默认 10\n"
        "  --check URL     校验用的回显地址，默认 http://icanhazip.com/\n"
        "  --no-validate   只做 TCP 连通性测试（更快，但可用率低）\n"
        "  --once          只刷新一次并打印结果，不启动代理服务\n"
        "  -v, --verbose   输出调试日志\n"
        "  -h, --help      显示本帮助\n\n"
        "示例:\n"
        "  proxypool --port 8080\n"
        "  curl -x http://127.0.0.1:8080 https://api.ipify.org\n\n");
}

bool parseArgv(const std::vector<std::string>& args, Options& o, bool& help, std::string& err) {
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        bool hasVal = (i + 1 < args.size());

        if (a == "-h" || a == "--help") { help = true; return true; }
        else if (a == "--port") { if (!hasVal) { err = "--port 缺少参数"; return false; } o.port = std::atoi(args[++i].c_str()); }
        else if (a == "--host") { if (!hasVal) { err = "--host 缺少参数"; return false; } o.host = args[++i]; }
        else if (a == "--sources") { if (!hasVal) { err = "--sources 缺少参数"; return false; } o.sourcesFile = args[++i]; }
        else if (a == "--threads") { if (!hasVal) { err = "--threads 缺少参数"; return false; } o.threads = std::atoi(args[++i].c_str()); }
        else if (a == "--timeout") { if (!hasVal) { err = "--timeout 缺少参数"; return false; } o.timeoutMs = std::atoi(args[++i].c_str()); }
        else if (a == "--refresh") { if (!hasVal) { err = "--refresh 缺少参数"; return false; } o.refreshSec = std::atoi(args[++i].c_str()); }
        else if (a == "--max") { if (!hasVal) { err = "--max 缺少参数"; return false; } o.maxProxies = std::atoi(args[++i].c_str()); }
        else if (a == "--min") { if (!hasVal) { err = "--min 缺少参数"; return false; } o.minPool = std::atoi(args[++i].c_str()); }
        else if (a == "--check") { if (!hasVal) { err = "--check 缺少参数"; return false; } o.checkUrl = args[++i]; }
        else if (a == "--once") o.once = true;
        else if (a == "-v" || a == "--verbose") o.verbose = true;
        else if (a == "--no-validate") o.noValidate = true;
        else { err = "未知参数: " + a; return false; }
    }

    if (o.port <= 0 || o.port > 65535) { err = "端口不合法"; return false; }
    if (o.threads < 1 || o.threads > 512) { err = "线程数应在 1..512"; return false; }
    if (o.timeoutMs < 200 || o.timeoutMs > 60000) { err = "超时应在 200..60000 ms"; return false; }
    if (o.maxProxies < 1) o.maxProxies = 1;
    if (o.refreshSec < 30) o.refreshSec = 30;
    return true;
}

bool parseArgs(int argc, char** argv, Options& o, bool& help, std::string& err) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(argv[i]);
    return parseArgv(args, o, help, err);
}

std::string exeDir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (n == 0) return ".";
    std::string p(buf, n);
    size_t s = p.find_last_of("\\/");
    return (s == std::string::npos) ? std::string(".") : p.substr(0, s);
}

std::vector<std::string> loadSources(const Options& o) {
    std::vector<std::string> urls;

    std::string path = o.sourcesFile;
    std::ifstream f(path.c_str());
    if (!f && path.find('\\') == std::string::npos && path.find('/') == std::string::npos) {
        path = exeDir() + "\\" + o.sourcesFile;
        f.open(path.c_str());
    }

    if (f) {
        std::string line;
        while (std::getline(f, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            urls.push_back(line);
        }
        if (!urls.empty()) {
            logf(LV_INFO, "从 %s 读取了 %u 个来源", path.c_str(), (unsigned)urls.size());
            return urls;
        }
    }

    logf(LV_WARN, "未找到有效的 %s，使用内置来源列表", o.sourcesFile.c_str());
    for (int i = 0; kDefaultSources[i]; ++i) urls.push_back(kDefaultSources[i]);
    return urls;
}

size_t refreshPool(const Options& o, ProxyPool& pool, std::vector<Proxy>* outList) {
    RefreshGuard guard;
    g_refresh.begin();

    long long t0 = nowMs();

    std::string checkHost, checkPath;
    unsigned short checkPort = 80;
    if (!splitHttpUrl(o.checkUrl, checkHost, checkPort, checkPath)) {
        logf(LV_ERROR, "--check 必须是 http:// 开头的地址");
        return 0;
    }

    std::vector<std::string> urls = loadSources(o);
    logf(LV_INFO, "开始刷新代理池（%u 个来源，%d 并发校验）", (unsigned)urls.size(), o.threads);

    g_refresh.setStage("抓取来源");
    std::set<std::string> uniq = fetchAll(urls, 20000);
    if (uniq.empty()) {
        logf(LV_ERROR, "所有来源都没抓到代理，保留旧池");
        return 0;
    }

    std::vector<std::string> keys(uniq.begin(), uniq.end());
    std::mt19937 rng((unsigned)time(NULL) ^ (unsigned)GetTickCount());
    std::shuffle(keys.begin(), keys.end(), rng);

    size_t cap = 20000;
    if (keys.size() > cap) keys.resize(cap);

    std::vector<Proxy> good;
    std::vector<std::string> aliveKeys;

    std::vector<std::string> ownIps;
    if (!o.noValidate) {
        g_refresh.setStage("探测本机出口 IP");
        ownIps = probeOwnIps();
        if (!ownIps.empty()) {
            std::string joined;
            for (size_t i = 0; i < ownIps.size(); ++i) {
                if (i) joined += ", ";
                joined += ownIps[i];
            }
            logf(LV_INFO, "本机公网 IP %s（返回该 IP 的节点判为假代理）", joined.c_str());
        } else {
            logf(LV_WARN, "取本机公网 IP 失败，跳过假代理检测");
        }
    }

    if (o.noValidate) {
        good = validateAll(keys, o, checkHost, checkPort, checkPath, ownIps, "TCP");
    } else {
        // 第一轮：短超时的 TCP 连通性筛选
        int fastThreads = o.threads * 4;
        if (fastThreads < 256) fastThreads = 256;
        if (fastThreads > 512) fastThreads = 512;

        Options fast = o;
        fast.threads = fastThreads;
        fast.noValidate = true;
        fast.timeoutMs = (o.timeoutMs < 1500) ? o.timeoutMs : 1500;

        std::vector<Proxy> alive =
            validateAll(keys, fast, checkHost, checkPort, checkPath, ownIps, "TCP 筛选");
        logf(LV_INFO, "TCP 可连通 %u 个，进入 HTTP 校验", (unsigned)alive.size());

        aliveKeys.reserve(alive.size());
        for (size_t i = 0; i < alive.size(); ++i) aliveKeys.push_back(alive[i].key());

        // 第二轮：通过代理请求回显地址，确认真的能上网
        if (!aliveKeys.empty())
            good = validateAll(aliveKeys, o, checkHost, checkPort, checkPath, ownIps, "HTTP 校验");
    }

    if (!o.noValidate && good.size() < (size_t)o.minPool && !aliveKeys.empty()) {
        std::string fHost, fPath;
        unsigned short fPort = 80;
        if (splitHttpUrl(kFallbackCheckUrl, fHost, fPort, fPath) && fHost != checkHost) {
            logf(LV_WARN, "可用节点只有 %u 个，改用备用回显地址 %s 再校验一次",
                 (unsigned)good.size(), kFallbackCheckUrl);
            std::vector<Proxy> alt =
                validateAll(aliveKeys, o, fHost, fPort, fPath, ownIps, "HTTP 备用");
            if (alt.size() > good.size()) good = alt;
        }
    }

    if (good.empty()) {
        logf(LV_ERROR, "校验后没有可用代理，保留旧池");
        return 0;
    }

    std::sort(good.begin(), good.end(),
              [](const Proxy& a, const Proxy& b) { return a.latencyMs < b.latencyMs; });
    if (good.size() > (size_t)o.maxProxies) good.resize((size_t)o.maxProxies);

    pool.install(good);
    if (outList) *outList = good;

    char secs[32];
    snprintf(secs, sizeof(secs), "%.1f", (nowMs() - t0) / 1000.0);
    logf(LV_INFO, "刷新完成：候选 %u 个 -> 可用 %u 个，耗时 %s 秒", (unsigned)uniq.size(),
         (unsigned)good.size(), secs);

    for (size_t i = 0; i < good.size() && i < 8; ++i)
        logf(LV_INFO, "  #%u  %-22s %d ms", (unsigned)(i + 1), good[i].key().c_str(),
             good[i].latencyMs);

    return good.size();
}

} // namespace pp
