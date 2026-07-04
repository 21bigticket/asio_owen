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

    // Case-insensitive header value lookup from header vector
    inline std::string get_header_value(const std::vector<std::pair<std::string, std::string>>& headers,
                                         const std::string& key) {
        for (auto& [k, v] : headers) {
            if (http_header_iequals(k, key)) return v;
        }
        return {};
    }

// HTTP request/response context, supports proxy forwarding and local handlers
struct HttpContext {
    std::string method;
    std::string path;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    int status_code = 0;     // 0 means unset, must be > 0 before response
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
