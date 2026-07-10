#pragma once

#include <atomic>
#include <filesystem>
#include <memory>

#include <asio.hpp>

#include "app_config.hpp"
#include "pool_stats_service.hpp"
#include "reload_service.hpp"
#include "snapshot_service.hpp"
#include "../common/signal_exit.hpp"
#include "../db/mysql_pool.hpp"
#include "../db/redis_pool.hpp"
#include "../http/http_server.hpp"
#include "../security/security_rules.hpp"

class Application {
public:
    int run(int argc, char* argv[]);

private:
    static std::filesystem::path executable_dir(const char* argv0);

    void initialize(const Config& cfg, const AppConfig& app_cfg,
                    const std::filesystem::path& config_base);
    void register_upstreams(const Config& cfg, const HttpPool::Config& http_pool_cfg);
    void request_stop();
    void cleanup();

    asio::io_context ioc_;
    std::unique_ptr<MysqlPool> mysql_;
    std::unique_ptr<RedisPool> redis_;
    std::unique_ptr<HttpServer> server_;
    std::unique_ptr<SecurityRules> security_rules_;
    std::unique_ptr<ReloadService> reload_service_;
    std::unique_ptr<SnapshotService> snapshot_service_;
    std::unique_ptr<PoolStatsService> pool_stats_service_;
    std::unique_ptr<SignalExit> signal_exit_;
    std::unique_ptr<asio::steady_timer> drain_timer_;
    std::atomic<bool> stop_requested_{false};
};
