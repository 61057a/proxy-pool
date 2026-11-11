#include "pool.h"

namespace pp {

static const int kMaxFails = 3;

void ProxyPool::install(const std::vector<Proxy>& list) {
    std::lock_guard<std::mutex> lk(mu_);
    list_ = list;
    rr_ = 0;
    // 固定节点如果不在新列表里，自动回到轮询模式
    if (!pinned_.empty()) {
        bool found = false;
        for (size_t i = 0; i < list_.size(); ++i)
            if (list_[i].key() == pinned_) { found = true; break; }
        if (!found) pinned_.clear();
    }
}

bool ProxyPool::next(Proxy& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (list_.empty()) return false;

    if (!pinned_.empty()) {
        for (size_t i = 0; i < list_.size(); ++i) {
            if (list_[i].key() == pinned_ && list_[i].fails < kMaxFails) {
                out = list_[i];
                return true;
            }
        }
        // 固定的节点挂了就临时回退到轮询，等它恢复后自动切回
    }

    size_t n = list_.size();
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (rr_++) % n;
        if (list_[idx].fails < kMaxFails) {
            out = list_[idx];
            return true;
        }
    }
    return false;
}

void ProxyPool::reportSuccess(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = 0; i < list_.size(); ++i) {
        if (list_[i].key() == key) {
            list_[i].fails = 0;
            return;
        }
    }
}

void ProxyPool::reportFailure(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = 0; i < list_.size(); ++i) {
        if (list_[i].key() == key) {
            list_[i].fails++;
            if (list_[i].fails == kMaxFails)
                logf(LV_WARN, "代理 %s 连续失败 %d 次，暂时剔除", key.c_str(), kMaxFails);
            return;
        }
    }
}

void ProxyPool::pin(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    pinned_ = key;
    rr_ = 0;
}

std::string ProxyPool::pinned() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pinned_;
}

size_t ProxyPool::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (size_t i = 0; i < list_.size(); ++i)
        if (list_[i].fails < kMaxFails) ++n;
    return n;
}

size_t ProxyPool::total() const {
    std::lock_guard<std::mutex> lk(mu_);
    return list_.size();
}

std::vector<Proxy> ProxyPool::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return list_;
}

} // namespace pp
