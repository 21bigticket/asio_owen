#include "application.hpp"

#include <algorithm>
#include <iostream>
#include <thread>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>

#include "routes.hpp"
#include "../common/config.hpp"
#include "../common/logger.hpp"

std::filesystem::path Application::executable_dir(const char* argv0) {
    std::error_code ec;
    auto path = std::filesystem::absolute(argv0, ec);
    if (ec) return std::filesystem::current_path();
    return path.parent_path();
}

int Application::run(int argc, char* argv[]) {
    auto config_base = executable_dir((argc > 0 && argv[0]) ? argv[0] : ".");

    Config cfg;
    if (!cfg.load(config_base)) {
        std::cerr << "Load config failed from " << config_base << std::endl;
        return 1;
    }

    auto app_cfg = app_config_from(cfg);
    Logger::instance().init(app_cfg.log_file, app_cfg.log_level);

    LOG_INFO("Server starting...");
    LOG_INFO("Build marker: gateway-debug-20260703-client-close-trace");

    try {
        initialize(cfg, app_cfg, config_base);

        co_spawn(ioc_, server_->start(), asio::detached);

        signal_exit_ = std::make_unique<SignalExit>(ioc_);
        signal_exit_->on_exit([this]() {
            request_stop();
        });

        std::vector<std::thread> threads;
        unsigned int thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) thread_count = 4;
        for (unsigned int i = 1; i < thread_count; ++i) {
            threads.emplace_back([this]() { ioc_.run(); });
        }
        ioc_.run();

        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }

        cleanup();
        LOG_INFO("Server exited");
        return 0;
    } catch (...) {
        ioc_.stop();
        cleanup();
        throw;
    }
}

void Application::initialize(const Config& cfg, const AppConfig& app_cfg,
                             const std::filesystem::path& config_base) {
    mysql_ = std::make_unique<MysqlPool>(ioc_, app_cfg.mysql);
    redis_ = std::make_unique<RedisPool>(ioc_, app_cfg.redis);
    server_ = std::make_unique<HttpServer>(
        ioc_, app_cfg.server_port, app_cfg.downstream_write_timeout_ms);

    security_rules_ = std::make_unique<SecurityRules>();
    security_rules_->load_from_config(cfg);
    server_->set_security_rules(security_rules_.get());

    snapshot_service_ = std::make_unique<SnapshotService>(ioc_, *security_rules_);
    snapshot_service_->start(app_cfg.snapshot_interval_sec);

    register_routes(*server_, AppServices{
        .mysql = mysql_.get(),
        .redis = redis_.get()
    });

    register_upstreams(cfg, app_cfg.http_pool);

    pool_stats_service_ = std::make_unique<PoolStatsService>(ioc_, server_->upstreams());
    pool_stats_service_->start(app_cfg.http_pool_stats_interval_sec);

    reload_service_ = std::make_unique<ReloadService>(
        ioc_, config_base, *security_rules_, server_->upstreams(), app_cfg.http_pool);
    reload_service_->start(app_cfg.reload_interval_sec);
}

void Application::register_upstreams(const Config& cfg, const HttpPool::Config& http_pool_cfg) {
    for (auto& [key, val] : cfg.get_section("upstream")) {
        auto colon = val.find(':');
        if (colon == std::string::npos) continue;

        auto host = val.substr(0, colon);
        auto port_str = val.substr(colon + 1);
        if (port_str.empty()) continue;

        try {
            int port = std::stoi(port_str);
            server_->upstreams().add_upstream(key, host, port, http_pool_cfg);
            LOG_INFO("upstream ", key, " -> ", host, ":", port);
        } catch (...) {
            LOG_WARN("upstream ", key, ": invalid port '", port_str, "'");
        }
    }
}

void Application::request_stop() {
    if (stop_requested_.exchange(true)) return;
    LOG_INFO("Graceful shutdown requested...");
    if (server_) server_->stop();

    drain_timer_ = std::make_unique<asio::steady_timer>(ioc_);
    drain_timer_->expires_after(std::chrono::seconds(5));
    drain_timer_->async_wait([this](std::error_code ec) {
        if (ec) return;
        LOG_INFO("Drain timeout, stopping io_context...");
        ioc_.stop();
    });
}

void Application::cleanup() {
    if (reload_service_) reload_service_->stop();
    if (pool_stats_service_) pool_stats_service_->stop();
    if (snapshot_service_) snapshot_service_->stop();
    if (server_) server_->stop();

    signal_exit_.reset();
    reload_service_.reset();
    pool_stats_service_.reset();
    snapshot_service_.reset();
    server_.reset();

    if (mysql_) mysql_->shutdown();
    if (redis_) redis_->shutdown();
    redis_.reset();
    mysql_.reset();
    drain_timer_.reset();
    security_rules_.reset();
}
