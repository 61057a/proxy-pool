// 解析器单元测试：g++ -std=c++11 -Isrc test/parse_test.cpp src/common.cpp -o build/parse_test.exe -lws2_32
#include "common.h"

#include <cstdio>
#include <string>

using namespace pp;

static int failed = 0;

static void expectHost(const char* input, const char* wantHost, unsigned short wantPort) {
    std::string h;
    unsigned short p = 0;
    bool ok = parseProxyLine(input, h, p);
    if (!ok || h != wantHost || p != wantPort) {
        std::printf("FAIL  %-42s -> ok=%d host=%s port=%u （期望 %s:%u）\n", input, (int)ok,
                    h.c_str(), (unsigned)p, wantHost, (unsigned)wantPort);
        ++failed;
        return;
    }
    std::printf("ok    %-42s -> %s:%u\n", input, h.c_str(), (unsigned)p);
}

static void expectReject(const char* input) {
    std::string h;
    unsigned short p = 0;
    bool ok = parseProxyLine(input, h, p);
    if (ok) {
        std::printf("FAIL  %-42s 应拒绝，却解析成 %s:%u\n", input, h.c_str(), (unsigned)p);
        ++failed;
    } else {
        std::printf("ok    %-42s -> 已拒绝\n", input);
    }
}

int main() {
    expectHost("1.2.3.4:8080", "1.2.3.4", 8080);
    expectHost("  1.2.3.4:8080  ", "1.2.3.4", 8080);
    expectHost("http://1.2.3.4:8080", "1.2.3.4", 8080);
    expectHost("https://1.2.3.4:8443/", "1.2.3.4", 8443);
    expectHost("socks5://user:pass@1.2.3.4:1080", "1.2.3.4", 1080);
    expectHost("94.43.164.242:8080:Georgia", "94.43.164.242", 8080);
    expectHost("proxy.example.com:3128", "proxy.example.com", 3128);
    expectHost("1.2.3.4:8080\n", "1.2.3.4", 8080);
    expectHost("1.2.3.4:8080\t# comment", "1.2.3.4", 8080);

    expectReject("");
    expectReject("# comment");
    expectReject("{\"ip\":\"1.2.3.4\"}");
    expectReject("<html>");
    expectReject("1.2.3.4");
    expectReject("1.2.3.4:0");
    expectReject("1.2.3.4:70000");
    expectReject("1.2.3.4:80a");

    std::string host;
    unsigned short port = 0;
    std::string path;
    bool ok = splitHttpUrl("http://example.com:8080/a?b=1", host, port, path);
    if (!ok || host != "example.com" || port != 8080 || path != "/a?b=1") {
        std::printf("FAIL  splitHttpUrl -> ok=%d host=%s port=%u path=%s\n", (int)ok, host.c_str(),
                    (unsigned)port, path.c_str());
        ++failed;
    } else {
        std::printf("ok    splitHttpUrl -> %s:%u%s\n", host.c_str(), (unsigned)port, path.c_str());
    }

    std::printf(failed ? "\n失败 %d 项\n" : "\n全部通过\n", failed);
    return failed ? 1 : 0;
}
