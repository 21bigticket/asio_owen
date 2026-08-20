#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

#include "../db/mysql_pool.hpp"
#include "../db/redis_pool.hpp"
#include "../http/http_server.hpp"
#include "app_config.hpp"
#include "combo_query_limiter.hpp"
#include "admin/admin_auth_runtime.hpp"
#include "route_runtime.hpp"

class ConfigHistoryService;
class AdminCredentialStore;

struct AdminRuntimeMetrics {
    std::atomic<uint64_t> auth_attempts{0};
    std::atomic<uint64_t> auth_rejected{0};
    std::atomic<uint64_t> login_failures{0};
    std::atomic<uint64_t> login_successes{0};
    std::atomic<uint64_t> auth_in_flight{0};
    std::atomic<uint64_t> auth_work_rejections{0};
    std::atomic<uint64_t> throttle_locked_ips{0};
    std::atomic<uint64_t> drain_started_ns{0};
    std::atomic<uint64_t> drain_completed_ns{0};
    std::atomic<uint64_t> drain_duration_ns{0};
    std::atomic<uint64_t> drain_timeout_count{0};
    std::atomic<uint64_t> forced_stop_count{0};
};

class ComboBackend {
public:
    virtual ~ComboBackend() = default;
    virtual asio::awaitable<std::string> get_cache() = 0;
    virtual asio::awaitable<MysqlPool::Result> query() = 0;
};

struct AppServices {
    std::shared_ptr<RouteRuntime> runtime;
    MysqlPool* mysql = nullptr;
    RedisPool* redis = nullptr;
    std::shared_ptr<ComboQueryLimiter> combo_query_limiter;
    std::shared_ptr<ComboBackend> combo_backend;
    int combo_deadline_ms = 500;
    std::filesystem::path config_base;
    ConfigSyncConfig config_sync;
    std::shared_ptr<ConfigHistoryService> config_history_service;
    std::function<asio::awaitable<RedisPool::Reply>(std::vector<std::string>)> redis_command;
    asio::thread_pool* admin_auth_workers = nullptr;
    std::shared_ptr<std::atomic<bool>> draining_state;
    std::shared_ptr<AdminCredentialStore> admin_credentials;
    std::shared_ptr<AdminRuntimeMetrics> admin_metrics;
    std::shared_ptr<AdminLoginThrottle> admin_login_throttle;
    std::shared_ptr<AdminAuthWorkLimiter> admin_auth_limiter;
    UpstreamManager* upstreams = nullptr;
};

std::shared_ptr<ComboBackend> make_pool_combo_backend(MysqlPool* mysql, RedisPool* redis);
asio::awaitable<void> handle_api_combo(HttpContext& ctx, AppServices services);
asio::awaitable<void> api_ready(HttpContext& ctx, AppServices services);
asio::awaitable<void> api_metrics(HttpContext& ctx, AppServices services);
asio::awaitable<void> handle_api_admin_login(HttpContext& ctx, AppServices services);
asio::awaitable<void> handle_api_admin_config(HttpContext& ctx, AppServices services);
asio::awaitable<void> handle_api_admin_machines(HttpContext& ctx, AppServices services);
asio::awaitable<void> handle_api_admin_history(HttpContext& ctx, AppServices services);
asio::awaitable<void> handle_api_admin_history_path(HttpContext& ctx, AppServices services);
asio::awaitable<void> handle_api_admin_rollback(HttpContext& ctx, AppServices services);
asio::awaitable<void> handle_api_admin_snapshot_repair(HttpContext& ctx, AppServices services);
asio::awaitable<void> handle_api_admin_mirror_rebuild(HttpContext& ctx, AppServices services);
asio::awaitable<void> handle_api_admin_history_migration(HttpContext& ctx, AppServices services);
asio::awaitable<void> handle_api_admin_orphan_resolution(HttpContext& ctx, AppServices services);
asio::awaitable<void> handle_admin_page(HttpContext& ctx);
asio::awaitable<void> handle_admin_settings_page(HttpContext& ctx);
void register_routes(HttpServer& server, AppServices services);
