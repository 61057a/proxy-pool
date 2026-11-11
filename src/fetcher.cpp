#include "fetcher.h"

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <cstdio>

namespace pp {

static std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

bool httpGet(const std::string& url, std::string& body, std::string& err, int timeoutMs) {
    body.clear();
    err.clear();

    std::wstring wurl = widen(url);

    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;

    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
        err = "URL 解析失败";
        return false;
    }

    const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength > 0 && uc.lpszExtraInfo)
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    INTERNET_PORT port = uc.nPort;
    if (port == 0) port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;

    HINTERNET hSession = WinHttpOpen(L"ProxyPool/1.0 (Windows)",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        err = "WinHttpOpen 失败";
        return false;
    }

    WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    DWORD protos = 0x00000800 /* TLS1.2 */;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protos |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos));

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) {
        err = "WinHttpConnect 失败: " + narrow(host);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        err = "WinHttpOpenRequest 失败";
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // 不要压缩响应，省去解压
    WinHttpAddRequestHeaders(hRequest, L"Accept-Encoding: identity", (DWORD)-1,
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    bool ok = false;
    do {
        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            err = "请求发送失败 (错误码 " + std::to_string((int)GetLastError()) + ")";
            break;
        }
        if (!WinHttpReceiveResponse(hRequest, NULL)) {
            err = "等待响应失败 (错误码 " + std::to_string((int)GetLastError()) + ")";
            break;
        }

        DWORD status = 0;
        DWORD sz = sizeof(status);
        if (!WinHttpQueryHeaders(hRequest,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                 WINHTTP_NO_HEADER_INDEX)) {
            err = "读取状态码失败";
            break;
        }
        if (status != 200) {
            err = "HTTP " + std::to_string((int)status);
            break;
        }

        std::vector<char> buf(16384);
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(hRequest, &buf[0], (DWORD)buf.size(), &got)) {
                err = "读取响应体失败";
                break;
            }
            if (got == 0) break;
            body.append(&buf[0], (size_t)got);
            if (body.size() > 16u * 1024u * 1024u) break;
        }

        if (!err.empty()) break;
        ok = true;
    } while (false);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

} // namespace pp
