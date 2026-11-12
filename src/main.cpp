#include "app.h"
#include "net.h"
#include "server.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace pp;

namespace {

ProxyServer* g_server = NULL;

BOOL WINAPI ctrlHandler(DWORD) {
    g_stop = true;
    if (g_server) g_server->stop();
    return TRUE;
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    Options o;
    bool help = false;
    std::string err;
    if (!parseArgs(argc, argv, o, help, err)) {
        outf("参数错误：%s\n\n", err.c_str());
        printUsage();
        return 2;
    }
    if (help) {
        printUsage();
        return 0;
    }

    setVerbose(o.verbose);
    netInit();

    ProxyPool pool;
    std::vector<Proxy> initial;
    size_t n = refreshPool(o, pool, &initial);

    if (o.once) {
        if (n == 0) {
            logf(LV_ERROR, "没有可用代理");
            netShutdown();
            return 1;
        }
        outf("\n可用代理（按延迟排序，共 %u 个）：\n", (unsigned)initial.size());
        for (size_t i = 0; i < initial.size(); ++i)
            outf("  %s\t%d ms\n", initial[i].key().c_str(), initial[i].latencyMs);
        netShutdown();
        return 0;
    }

    ProxyServer server(pool, o.host, o.port, o.timeoutMs, o.ctlPort);
    std::string serr;
    if (!server.start(serr)) {
        logf(LV_ERROR, "本地代理启动失败：%s", serr.c_str());
        netShutdown();
        return 1;
    }
    g_server = &server;

    const std::string shownHost =
        (o.host == "0.0.0.0" || o.host == "*") ? std::string("127.0.0.1") : o.host;

    outf(
        "\n"
        "============================================================\n"
        " 免费代理池已启动\n"
        " 本地代理：  http://%s:%d\n"
        " 可用节点：  %u 个（每 %d 秒自动刷新）\n"
        " 测试：      curl -x http://%s:%d https://api.ipify.org\n"
        " 浏览器：    HTTP 代理 %s 端口 %d\n"
        " 提示：      免费代理不安全，勿用于登录账号或支付\n"
        "============================================================\n\n",
        shownHost.c_str(), o.port, (unsigned)pool.size(), o.refreshSec, shownHost.c_str(),
        o.port, shownHost.c_str(), o.port);

    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    std::thread refresher([&]() {
        int since = 0;
        while (!g_stop) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (g_stop) break;
            ++since;
            bool due = (since >= o.refreshSec);
            if (!due && since > 30 && pool.size() < (size_t)o.minPool) {
                logf(LV_WARN, "可用节点只剩 %u 个，提前刷新", (unsigned)pool.size());
                due = true;
            }
            if (due) {
                since = 0;
                refreshPool(o, pool, NULL);
            }
        }
    });

    server.runUntilStopped();

    g_stop = true;
    if (refresher.joinable()) refresher.join();

    logf(LV_INFO, "已退出");
    return 0;
}
