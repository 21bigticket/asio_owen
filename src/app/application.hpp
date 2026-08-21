#pragma once

#include <atomic>
#include <filesystem>
#include <memory>

#include <asio.hpp>

#include "app_config.hpp"
#include "admin/admin_credential_store.hpp"
#include "admin/admin_auth_runtime.hpp"
#include "combo_query_limiter.hpp"
#include "config_history_service.hpp"
#include "config_sync_service.hpp"
#include "pool_stats_service.hpp"
#include "reload_service.hpp"
#include "snapshot_service.hpp"
#include "../common/shutdown_coordinator.hpp"
#include "route_runtime.hpp"
#include "../common/signal_exit.hpp"
#include "../db/mysql_pool.hpp"
#include "../db/redis_pool.hpp"
#include "../http/http_server.hpp"
#include "../security/security_rules.hpp"

struct AdminRuntimeMetrics;
struct ConfigSyncRuntimeMetrics;

class Application {
public:
    int run(int argc, char* argv[]);

private:
    static std::filesystem::path executable_dir(const char* argv0);

    void initialize(const Config& cfg, const AppConfig& app_cfg,
                    const std::filesystem::path& config_base);
    void register_upstreams(const Config& cfg, const HttpPool::Config& http_pool_cfg);
    void run_io_context() noexcept;
    void stop_after_handler_exception() noexcept;
    void request_stop();
    void cleanup();

    asio::io_context ioc_;
    std::shared_ptr<ComboQueryLimiter> combo_query_limiter_;
    std::unique_ptr<HttpServer> server_;
    std::unique_ptr<SecurityRules> security_rules_;
    std::unique_ptr<ReloadService> reload_service_;
    std::unique_ptr<SnapshotService> snapshot_service_;
    std::unique_ptr<PoolStatsService> pool_stats_service_;
    std::shared_ptr<ConfigSyncService> config_sync_service_;
    std::shared_ptr<ConfigHistoryService> config_history_service_;
    std::unique_ptr<SignalExit> signal_exit_;
    std::atomic<bool> stop_requested_{false};
    std::shared_ptr<std::atomic<bool>> draining_state_ =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<AdminCredentialStore> admin_credentials_;
    std::shared_ptr<AdminRuntimeMetrics> admin_metrics_;
    std::shared_ptr<ConfigSyncRuntimeMetrics> config_metrics_;
    std::shared_ptr<AdminLoginThrottle> admin_login_throttle_;
    std::shared_ptr<AdminAuthWorkLimiter> admin_auth_limiter_;
    std::shared_ptr<ShutdownCoordinator> shutdown_coordinator_ =
        std::make_shared<ShutdownCoordinator>();
    std::shared_ptr<RouteRuntime> route_runtime_;
    unsigned int effective_io_threads_ = 1;
    // Set when an unhandled io_context handler exception forced the shutdown,
    // so run() reports a non-zero exit code instead of a clean "0" that
    // supervisors (systemd Restart=on-failure, containers, CLI) would treat
    // as a normal exit.
    std::atomic<bool> fatal_handler_exception_{false};
};
