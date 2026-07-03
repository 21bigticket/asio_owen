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

    struct RouteResult {
        UpstreamConfig config;
        HttpPool* pool;
        std::string upstream_path;
    };

    explicit UpstreamManager(asio::io_context& ioc) : ioc_(ioc) {}

    void add_upstream(const std::string& name, std::string host, int port,
                      HttpPool::Config pool_cfg = HttpPool::Config{}) {
        upstreams_[name] = {std::move(host), port};
        pools_.try_emplace(name, ioc_, std::move(pool_cfg));
    }

    // 路由 /{service}/...，返回上游配置、连接池和去掉服务名前缀后的路径
    // 例如 /zebra-config/xxx → service=zebra-config
    std::optional<RouteResult> route(const std::string& path) {
        if (path.empty() || path[0] != '/') return std::nullopt;
        auto end = path.find('/', 1);
        auto svc = (end == std::string::npos) ? path.substr(1) : path.substr(1, end - 1);
        auto it = upstreams_.find(svc);
        if (it == upstreams_.end()) return std::nullopt;
        std::string upstream_path = (end == std::string::npos) ? "/" : path.substr(end);
        return RouteResult{it->second, &pools_.at(svc), std::move(upstream_path)};
    }

private:
    asio::io_context& ioc_;
    std::unordered_map<std::string, UpstreamConfig> upstreams_;
    std::unordered_map<std::string, HttpPool> pools_;
};
