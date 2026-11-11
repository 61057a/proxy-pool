// 代理池固定节点逻辑的单元测试（不需要网络）
// 构建：g++ -std=c++11 -Isrc test\pool_test.cpp src\pool.cpp src\common.cpp -o build\pool_test.exe
#include "pool.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace pp;

static int g_pass = 0;
static int g_fail = 0;

static void ok(bool cond, const char* what) {
    if (cond) {
        ++g_pass;
        printf("  PASS  %s\n", what);
    } else {
        ++g_fail;
        printf("  FAIL  %s\n", what);
    }
}

static Proxy mk(const char* host, unsigned short port) {
    Proxy p;
    p.host = host;
    p.port = port;
    p.latencyMs = 100;
    return p;
}

int main() {
    printf("pool_test: 固定节点 / 失败回退 / 刷新清理\n");

    std::vector<Proxy> list;
    list.push_back(mk("1.1.1.1", 1111));
    list.push_back(mk("2.2.2.2", 2222));
    list.push_back(mk("3.3.3.3", 3333));

    // ---- 轮询 ----
    {
        ProxyPool pool;
        pool.install(list);
        ok(pool.total() == 3 && pool.size() == 3, "install 后 total/size 都是 3");

        std::vector<std::string> seen;
        for (int i = 0; i < 6; ++i) {
            Proxy p;
            if (pool.next(p)) seen.push_back(p.key());
        }
        bool rotated = !seen.empty() && seen[0] != seen[1] && seen[1] != seen[2];
        ok(rotated, "未固定时连续取节点会轮换");
    }

    // ---- 固定 ----
    {
        ProxyPool pool;
        pool.install(list);
        pool.pin("2.2.2.2:2222");
        ok(pool.pinned() == "2.2.2.2:2222", "pin() 之后 pinned() 返回该节点");

        bool allSame = true;
        for (int i = 0; i < 8; ++i) {
            Proxy p;
            if (!pool.next(p) || p.key() != "2.2.2.2:2222") allSame = false;
        }
        ok(allSame, "固定后每次都拿到同一节点");

        // 取消固定
        pool.pin("");
        ok(pool.pinned().empty(), "pin(\"\") 恢复自动轮换");
    }

    // ---- 固定节点失败 3 次后临时回退 ----
    {
        ProxyPool pool;
        pool.install(list);
        pool.pin("2.2.2.2:2222");

        pool.reportFailure("2.2.2.2:2222");
        pool.reportFailure("2.2.2.2:2222");
        Proxy p;
        pool.next(p);
        ok(p.key() == "2.2.2.2:2222", "失败 2 次时仍然用固定节点");

        pool.reportFailure("2.2.2.2:2222");
        bool neverPinned = true;
        for (int i = 0; i < 8; ++i) {
            Proxy q;
            if (!pool.next(q) || q.key() == "2.2.2.2:2222") neverPinned = false;
        }
        ok(neverPinned, "失败 3 次后临时回退到其他节点");
        ok(pool.pinned() == "2.2.2.2:2222", "回退期间仍记得用户选的是哪个节点");

        // 固定节点恢复后自动切回
        pool.reportSuccess("2.2.2.2:2222");
        Proxy q;
        pool.next(q);
        ok(q.key() == "2.2.2.2:2222", "节点恢复后自动切回固定节点");
    }

    // ---- 刷新后固定节点消失 ----
    {
        ProxyPool pool;
        pool.install(list);
        pool.pin("3.3.3.3:3333");

        std::vector<Proxy> fresh;
        fresh.push_back(mk("4.4.4.4", 4444));
        fresh.push_back(mk("5.5.5.5", 5555));
        pool.install(fresh);
        ok(pool.pinned().empty(), "新列表里没有固定节点时自动清除固定");
        Proxy p;
        ok(pool.next(p) && (p.key() == "4.4.4.4:4444" || p.key() == "5.5.5.5:5555"),
           "清除固定后使用新列表");
    }

    // ---- 全部失败时 next() 返回 false ----
    {
        ProxyPool pool;
        std::vector<Proxy> one;
        one.push_back(mk("9.9.9.9", 9999));
        pool.install(one);
        pool.pin("9.9.9.9:9999");
        pool.reportFailure("9.9.9.9:9999");
        pool.reportFailure("9.9.9.9:9999");
        pool.reportFailure("9.9.9.9:9999");
        Proxy p;
        ok(!pool.next(p), "唯一节点被剔除后 next() 返回 false");
        ok(pool.size() == 0 && pool.total() == 1, "size() 只算可用节点，total() 算全部");
    }

    printf("\n结果：%d 通过，%d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
