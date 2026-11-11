#pragma once

#include <string>

namespace pp {

// 用 WinHTTP 拉取文本（跟随重定向，支持 HTTPS）
bool httpGet(const std::string& url, std::string& body, std::string& err, int timeoutMs);

} // namespace pp
