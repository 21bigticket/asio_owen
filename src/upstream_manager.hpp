#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <memory>
#include <asio.hpp>
#include "http_pool.hpp"
#include "../common/config.hpp"
#include "../common/logger.hpp"

// Upstream manager: route /{service}/... -> connection pool
// Supports hot-reload via reload(const Config&) — adds new, updates existing, removes deleted upstreams.
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

    // Route /{service}/..., returns upstream config, pool, and path with service prefix stripped
    // example: /zebra-config/xxx -> service=zebra-config
    std::optional<RouteResult> route(const std::string& path) {
        if (path.empty() || path[0] != '/') return std::nullopt;
        auto end = path.find('/', 1);
        auto svc = (end == std::string::npos) ? path.substr(1) : path.substr(1, end - 1);
        auto it = upstreams_.find(svc);
        if (it == upstreams_.end()) return std::nullopt;
        std::string upstream_path = (end == std::string::npos) ? "/" : path.substr(end);
        return RouteResult{it->second, &pools_.at(svc), std::move(upstream_path)};
    }

    // Hot-reload upstream config from [upstream] section.
    // Adds new upstreams, updates existing (host:port), removes deleted (shuts down pool).
    void reload(const Config& cfg, const HttpPool::Config& pool_cfg) {
        auto new_upstreams = cfg.get_section("upstream");

        // Track which upstreams are still present after reload
        std::unordered_set<std::string> kept;

        for (auto& [name, val] : new_upstreams) {
            auto colon = val.find(':');
            if (colon == std::string::npos) continue;

            auto host = val.substr(0, colon);
            int port = std::stoi(val.substr(colon + 1));
            kept.insert(name);

            auto it = upstreams_.find(name);
            if (it != upstreams_.end()) {
                // Update existing: check if host:port changed
                if (it->second.host != host || it->second.port != port) {
                    // Shutdown old pool, replace config
                    pools_.at(name).shutdown();
                    pools_.erase(name);
                    pools_.try_emplace(name, ioc_, pool_cfg);
                    it->second = {std::move(host), port};
                    LOG_INFO("upstream updated: ", name, " -> ", host, ":", port);
                }
                // else unchanged, skip
            } else {
                // New upstream
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
            pools_.at(name).shutdown();
            pools_.erase(name);
            upstreams_.erase(name);
            LOG_INFO("upstream removed: ", name);
        }
    }

private:
    asio::io_context& ioc_;
    std::unordered_map<std::string, UpstreamConfig> upstreams_;
    std::unordered_map<std::string, HttpPool> pools_;
};
