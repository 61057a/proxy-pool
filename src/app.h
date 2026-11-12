#pragma once

#include "common.h"
#include "pool.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace pp {

struct Options {
    int port;
    int ctlPort;   // 本机控制接口，-1 = 自动用 port+1，0 = 关闭
    int threads;
    int timeoutMs;
    int refreshSec;
    int maxProxies;
    int minPool;
    std::string host;
    std::string sourcesFile;
    std::string checkUrl;
    bool once;
    bool verbose;
    bool noValidate;

    Options();
};

// 刷新进度，供 GUI 轮询显示（CLI 模式下无人读取，开销可忽略）
struct RefreshStatus {
    std::atomic<bool> isRunning;
    std::atomic<int> done;
    std::atomic<int> total;
    std::atomic<int> found;

    RefreshStatus();

    void begin();
    void setStage(const std::string& s);
    void setTotal(int n);
    void tick(bool ok);
    void end();
    std::string stage() const;

private:
    mutable std::mutex mu_;
    std::string stage_;
};

extern RefreshStatus g_refresh;
extern std::atomic<bool> g_stop;

void printUsage();
bool parseArgs(int argc, char** argv, Options& o, bool& help, std::string& err);
bool parseArgv(const std::vector<std::string>& args, Options& o, bool& help, std::string& err);

std::string exeDir();
std::vector<std::string> loadSources(const Options& o);

// 抓取 + 两段校验 + 装池；返回可用数量
size_t refreshPool(const Options& o, ProxyPool& pool, std::vector<Proxy>* outList);

} // namespace pp
