#pragma once

#include <memory>

#include <asio.hpp>

#include "admin/admin_auth_runtime.hpp"
#include "admin/admin_credential_store.hpp"
#include "../common/shutdown_coordinator.hpp"
#include "../db/mysql_pool.hpp"
#include "../db/redis_pool.hpp"
#include "../http/route_lifecycle.hpp"

struct ConfigSyncRuntimeMetrics;

struct AdminRuntimeMetrics;

class RouteRuntime : public RouteLifecycle {
public:
    bool begin_draining() {
        const bool transitioned = RouteLifecycle::begin_draining();
        if (shutdown) shutdown->begin_draining();
        return transitioned;
    }

    void mark_stopped() {
        RouteLifecycle::mark_stopped();
        if (shutdown) shutdown->mark_stopped();
    }

    std::shared_ptr<MysqlPool> mysql;
    std::shared_ptr<RedisPool> redis;
    std::shared_ptr<asio::thread_pool> admin_auth_workers;
    std::shared_ptr<asio::thread_pool> config_file_workers;
    std::shared_ptr<AdminCredentialStore> admin_credentials;
    std::shared_ptr<AdminRuntimeMetrics> admin_metrics;
    std::shared_ptr<AdminLoginThrottle> admin_login_throttle;
    std::shared_ptr<AdminAuthWorkLimiter> admin_auth_limiter;
    std::shared_ptr<ConfigSyncRuntimeMetrics> config_metrics;
    std::shared_ptr<ShutdownCoordinator> shutdown =
        std::make_shared<ShutdownCoordinator>();

};
