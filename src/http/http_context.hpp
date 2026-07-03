#pragma once
#include <string>
#include <vector>
#include <asio.hpp>

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
        auto to_lower = [](std::string_view s) {
            std::string out; out.reserve(s.size());
            for (char c : s) out += std::tolower(c);
            return out;
        };
        auto lk = to_lower(key);
        for (auto& [k, v] : headers) {
            if (to_lower(k) == lk) return v;
        }
        return {};
    }
};

using Handler = std::function<asio::awaitable<void>(HttpContext& ctx)>;
