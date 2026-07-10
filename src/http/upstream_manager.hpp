#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <memory>
#include <shared_mutex>
#include <sstream>
#include <asio.hpp>
#include "http_pool.hpp"
#include "../common/config.hpp"
#include "../common/logger.hpp"

// Upstream manager: route /{service}/... -> connection pool
// Uses shared_ptr for pools so in-flight requests keep the old pool alive during hot-reload.
class UpstreamManager {
public:
    struct UpstreamConfig {
        std::string host;
        int port;
    };

    struct RouteResult {
        UpstreamConfig config;
        std::shared_ptr<HttpPool> pool;
        std::string upstream_path;
    };

    explicit UpstreamManager(asio::io_context& ioc) : ioc_(ioc) {}

    void add_upstream(const std::string& name, std::string host, int port,
                      HttpPool::Config pool_cfg = HttpPool::Config{}) {
        upstreams_[name] = {std::move(host), port};
        pools_[name] = std::make_shared<HttpPool>(ioc_, std::move(pool_cfg));
    }

    // Route /{service}/..., returns upstream config, shared pool, and path with service prefix stripped
    // example: /zebra-config/xxx -> service=zebra-config
    std::optional<RouteResult> route(const std::string& path) {
        std::shared_lock lock(mtx_);
        if (path.empty() || path[0] != '/') return std::nullopt;
        auto end = path.find('/', 1);
        auto svc = (end == std::string::npos) ? path.substr(1) : path.substr(1, end - 1);
        auto it = upstreams_.find(svc);
        if (it == upstreams_.end()) return std::nullopt;
        std::string upstream_path = (end == std::string::npos) ? "/" : path.substr(end);
        auto pool = pools_.at(svc);
        return RouteResult{it->second, pool, std::move(upstream_path)};
    }

    // Hot-reload upstream config from [upstream] section.
    // Old pool is kept alive by in-flight requests holding shared_ptr references.
    void reload(const Config& cfg, const HttpPool::Config& pool_cfg) {
        std::unique_lock lock(mtx_);
        auto new_upstreams = cfg.get_section("upstream");

        std::unordered_set<std::string> kept;

        for (auto& [name, val] : new_upstreams) {
            auto colon = val.find(':');
            if (colon == std::string::npos) continue;

            auto host = val.substr(0, colon);
            auto port_str = val.substr(colon + 1);
            if (port_str.empty()) continue;
            int port = 0;
            try { port = std::stoi(port_str); } catch (...) { continue; }
            kept.insert(name);

            auto it = upstreams_.find(name);
            if (it != upstreams_.end()) {
                if (it->second.host != host || it->second.port != port) {
                    // Replace pool: old one lives until last shared_ptr goes out of scope
                    pools_[name] = std::make_shared<HttpPool>(ioc_, pool_cfg);
                    it->second = {std::move(host), port};
                    LOG_INFO("upstream updated: ", name, " -> ", host, ":", port);
                }
            } else {
                add_upstream(name, host, port, pool_cfg);
                LOG_INFO("upstream added: ", name, " -> ", host, ":", port);
            }
        }

        // Remove upstreams that are no longer in config
        std::vector<std::string> to_remove;
        for (auto& [name, _] : upstreams_) {
            if (!kept.count(name)) {
                to_remove.push_back(name);
            }
        }
        for (auto& name : to_remove) {
            pools_.erase(name);
            upstreams_.erase(name);
            LOG_INFO("upstream removed: ", name);
        }
    }

    std::string pool_stats() const {
        std::shared_lock lock(mtx_);
        if (pools_.empty()) return "none";

        std::ostringstream oss;
        bool first = true;
        for (const auto& [name, pool] : pools_) {
            if (!first) oss << "; ";
            first = false;
            oss << name << "={" << pool->stats() << "}";
        }
        return oss.str();
    }

private:
    mutable std::shared_mutex mtx_;
    asio::io_context& ioc_;
    std::unordered_map<std::string, UpstreamConfig> upstreams_;
    std::unordered_map<std::string, std::shared_ptr<HttpPool>> pools_;
};
