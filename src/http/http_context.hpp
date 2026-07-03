#pragma once
#include <cctype>
#include <string>
#include <vector>
#include <asio.hpp>

inline bool http_header_iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

// HTTP 请求/响应上下文，支持代理转发和本地 handler
struct HttpContext {
    std::string method;
    std::string path;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    int status_code = 0;     // 0 表示未设置，框架检查必须 > 0
    std::string response_status_text;
    std::string response_body;
    std::vector<std::pair<std::string, std::string>> response_headers;

    std::string get_header(const std::string& key) const {
        for (auto& [k, v] : headers) {
            if (http_header_iequals(k, key)) return v;
        }
        return {};
    }
};

using Handler = std::function<asio::awaitable<void>(HttpContext& ctx)>;
