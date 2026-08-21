#pragma once
#include <string>
#include <unordered_map>
#include <optional>
#include <memory>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <atomic>
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
        // Gateway response-transform switch ([gateway] json_keys_snake_to_camel,
        // default on). Read by the proxy layer to decide whether upstream JSON
        // bodies get their keys converted snake_case -> camelCase.
        bool json_keys_snake_to_camel = true;
    };

    class PreparedReload {
    public:
        PreparedReload(PreparedReload&&) = default;
        PreparedReload& operator=(PreparedReload&&) = default;
        PreparedReload(const PreparedReload&) = delete;
        PreparedReload& operator=(const PreparedReload&) = delete;

    private:
        friend class UpstreamManager;
        PreparedReload() = default;
        std::unordered_map<std::string, UpstreamConfig> upstreams_;
        std::unordered_map<std::string, std::shared_ptr<HttpPool>> pools_;
        bool json_keys_snake_to_camel_ = true;
        size_t max_total_connections_ = 0;
    };

    explicit UpstreamManager(asio::io_context& ioc)
        : ioc_(ioc), connection_budget_(std::make_shared<HttpConnectionBudget>()) {}

    // Monotonic counter incremented on every publish. Client sessions compare
    // it with the security-rules generation to detect that a hot-reload landed
    // between their security check and route lookup, keeping rules and routes
    // from the same config generation per request (see client_session.hpp).
    uint64_t generation() const {
        return generation_.load(std::memory_order_acquire);
    }

    // Route /{service}/..., returns upstream config, shared pool, and path with service prefix stripped
    // example: /zebra-config/xxx -> service=zebra-config
    std::optional<RouteResult> route(const std::string& path) {
        std::shared_lock lock(mtx_);
        if (path.empty() || path[0] != '/') return std::nullopt;
        const auto query = path.find('?');
        const auto path_end = query == std::string::npos ? path.size() : query;
        auto slash = path.find('/', 1);
        if (slash == std::string::npos || slash > path_end) slash = path_end;
        auto svc = path.substr(1, slash - 1);
        auto it = upstreams_.find(svc);
        if (it == upstreams_.end()) return std::nullopt;
        std::string upstream_path;
        if (slash < path_end) {
            upstream_path = path.substr(slash);
        } else if (query != std::string::npos) {
            upstream_path = "/" + path.substr(query);
        } else {
            upstream_path = "/";
        }
        auto pool = pools_.at(svc);
        return RouteResult{it->second, pool, std::move(upstream_path),
            json_keys_snake_to_camel_};
    }

    // Hot-reload upstream config from [upstream] section.
    // Old pool is kept alive by in-flight requests holding shared_ptr references.
    // Any malformed entry (missing ':', empty host/port, non-numeric,
    // out-of-range or trailing-character port) rejects the WHOLE reload by
    // throwing, so one typo can never silently drop routes of other services.
    PreparedReload prepare_reload(const Config& cfg, const HttpPool::Config& pool_cfg) {
        // Gateway-wide response-transform switch. Parsed before anything else so
        // an invalid value rejects the whole reload (get_bool throws), same
        // fail-closed semantics as a malformed upstream entry below.
        PreparedReload prepared;
        prepared.max_total_connections_ = pool_cfg.max_total_connections;
        prepared.json_keys_snake_to_camel_ =
            cfg.get_bool("gateway", "json_keys_snake_to_camel", true);

        auto new_upstreams = cfg.get_section("upstream");
        std::unordered_map<std::string, UpstreamConfig> current_upstreams;
        std::unordered_map<std::string, std::shared_ptr<HttpPool>> current_pools;
        {
            std::shared_lock lock(mtx_);
            current_upstreams = upstreams_;
            current_pools = pools_;
        }

        for (auto& [name, val] : new_upstreams) {
            if (name.empty()) {
                throw std::invalid_argument("upstream entry with an empty service name");
            }
            auto colon = val.find(':');
            if (colon == std::string::npos) {
                std::string message = "upstream \"";
                message += name;
                message += "\": missing ':' in value \"";
                message += val;
                message += '"';
                throw std::invalid_argument(message);
            }

            auto host = val.substr(0, colon);
            auto port_str = val.substr(colon + 1);
            if (host.empty()) {
                throw std::invalid_argument("upstream \"" + name + "\": empty host");
            }
            if (host.find(':') != std::string::npos) {
                throw std::invalid_argument(
                    "upstream \"" + name + "\": IPv6 literals are not supported, "
                    "use an IPv4 address or a hostname");
            }
            if (port_str.empty()) {
                throw std::invalid_argument("upstream \"" + name + "\": empty port");
            }
            int port = 0;
            size_t parsed = 0;
            try {
                port = std::stoi(port_str, &parsed);
            } catch (...) {
                std::string message = "upstream \"";
                message += name;
                message += "\": invalid port \"";
                message += port_str;
                message += '"';
                throw std::invalid_argument(message);
            }
            if (parsed != port_str.size()) {
                std::string message = "upstream \"";
                message += name;
                message += "\": invalid port \"";
                message += port_str;
                message += "\" (trailing characters)";
                throw std::invalid_argument(message);
            }
            if (port < 1 || port > 65535) {
                std::string message = "upstream \"";
                message += name;
                message += "\": port out of range [1,65535]: ";
                message += port_str;
                throw std::invalid_argument(message);
            }
            auto current = current_upstreams.find(name);
            if (current != current_upstreams.end() &&
                current->second.host == host && current->second.port == port &&
                current_pools.at(name)->cfg().same_pool_settings(pool_cfg)) {
                prepared.pools_[name] = current_pools.at(name);
            } else {
                prepared.pools_[name] = std::make_shared<HttpPool>(
                    ioc_, pool_cfg, connection_budget_);
            }
            // Assign (not emplace) so that later config files (e.g.
            // 99-local.ini) override earlier ones for the same service name,
            // matching Config::get()'s "later files override earlier files"
            // semantics.
            prepared.upstreams_[name] = UpstreamConfig{std::move(host), port};
        }
        return prepared;
    }

    void publish_reload(PreparedReload prepared) {
        const auto count = prepared.upstreams_.size();
        {
            std::unique_lock lock(mtx_);
            upstreams_.swap(prepared.upstreams_);
            pools_.swap(prepared.pools_);
            // Swapped inside the same lock as the maps + generation bump, so a
            // request never sees the new flag mixed with the old route table.
            json_keys_snake_to_camel_ = prepared.json_keys_snake_to_camel_;
            connection_budget_->set_limit(prepared.max_total_connections_);
            // Increment inside the lock: a reader that observes the new
            // generation must also observe the new maps, and vice versa.
            generation_.fetch_add(1, std::memory_order_release);
        }
        try {
            LOG_INFO("upstreams hot-reloaded, count=", count);
        } catch (...) {
        }
    }

    void reload(const Config& cfg, const HttpPool::Config& pool_cfg) {
        publish_reload(prepare_reload(cfg, pool_cfg));
    }

    std::string pool_stats() const {
        std::shared_lock lock(mtx_);
        if (pools_.empty()) return "process_total=" +
            std::to_string(connection_budget_->current()) + "/" +
            std::to_string(connection_budget_->limit()) + "; none";

        std::ostringstream oss;
        oss << "process_total=" << connection_budget_->current()
            << "/" << connection_budget_->limit();
        for (const auto& [name, pool] : pools_) {
            oss << "; ";
            oss << name << "={" << pool->stats() << "}";
        }
        return oss.str();
    }

    void evict_stale() {
        std::shared_lock lock(mtx_);
        for (const auto& [_, pool] : pools_) {
            pool->evict_stale();
        }
    }

private:
    mutable std::shared_mutex mtx_;
    asio::io_context& ioc_;
    std::unordered_map<std::string, UpstreamConfig> upstreams_;
    std::unordered_map<std::string, std::shared_ptr<HttpPool>> pools_;
    std::shared_ptr<HttpConnectionBudget> connection_budget_;
    // Written only under the unique_lock in publish_reload, read under the
    // shared_lock in route(), so it always publishes atomically with the maps.
    bool json_keys_snake_to_camel_ = true;
    std::atomic<uint64_t> generation_{0};

};
