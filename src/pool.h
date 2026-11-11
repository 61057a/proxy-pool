#pragma once

#include "common.h"

#include <vector>
#include <mutex>
#include <string>

namespace pp {

// 线程安全的代理池
//   - 默认轮询分发
//   - pin() 之后固定使用指定节点（GUI 里点选某个节点）
//   - 单个代理连续失败 3 次即被跳过
class ProxyPool {
public:
    ProxyPool() : rr_(0) {}

    void install(const std::vector<Proxy>& list);

    // 取下一个要用的代理
    bool next(Proxy& out);

    void reportSuccess(const std::string& key);
    void reportFailure(const std::string& key);

    // key 为空表示恢复自动轮换
    void pin(const std::string& key);
    std::string pinned() const;

    size_t size() const;              // 可用（未失效）数量
    size_t total() const;             // 池内总数
    std::vector<Proxy> snapshot() const;

private:
    mutable std::mutex mu_;
    std::vector<Proxy> list_;
    std::string pinned_;
    size_t rr_;
};

} // namespace pp
