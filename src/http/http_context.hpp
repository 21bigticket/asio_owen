#pragma once
#include <string>
#include <vector>
#include <asio.hpp>
#include "http_protocol.hpp"

inline std::string get_header_value(
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& key) {
    for (auto& [k, v] : headers) {
        if (header_iequals(k, key)) return v;
    }
    return {};
}

struct HttpContext {
    std::string method;
    std::string path;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    int status_code = 0;
    std::string response_status_text;
    std::string response_body;
    std::vector<std::pair<std::string, std::string>> response_headers;

    std::string get_header(const std::string& key) const {
        for (auto& [k, v] : headers) {
            if (header_iequals(k, key)) return v;
        }
        return {};
    }
};

using Handler = std::function<asio::awaitable<void>(HttpContext& ctx)>;
