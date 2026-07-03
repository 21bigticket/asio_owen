#pragma once
#include <string>
#include <unordered_map>
#include <optional>
#include <asio.hpp>
#include "http_pool.hpp"

// 上游管理：路由 /proxy/{service}/... → 连接池
class UpstreamManager {
public:
    struct UpstreamConfig {
        std::string host;
        int port;
    };

    explicit UpstreamManager(asio::io_context& ioc) : ioc_(ioc) {}

    void add_upstream(const std::string& name, std::string host, int port,
                      HttpPool::Config pool_cfg = HttpPool::Config{}) {
        upstreams_[name] = {std::move(host), port};
        pools_.try_emplace(name, ioc_, std::move(pool_cfg));
    }

    // 路由 /proxy/{service}/...，返回 {config, pool} 或 nullopt
    std::optional<std::pair<UpstreamConfig, HttpPool*>> route(const std::string& path) {
        if (path.size() < 7 || path.substr(0, 7) != "/proxy/") return std::nullopt;
        auto slash = path.find('/', 7);
        auto svc = (slash == std::string::npos) ? path.substr(7) : path.substr(7, slash - 7);
        auto it = upstreams_.find(svc);
        if (it == upstreams_.end()) return std::nullopt;
        return std::make_pair(it->second, &pools_.at(svc));
    }

private:
    asio::io_context& ioc_;
    std::unordered_map<std::string, UpstreamConfig> upstreams_;
    std::unordered_map<std::string, HttpPool> pools_;
};
