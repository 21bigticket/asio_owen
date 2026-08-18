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
    if (ConfigSyncService::blocking_first_pull(
            config_base, app_cfg.config_sync, app_cfg.redis, app_cfg)) {
        if (app_cfg.config_sync.enabled && app_cfg.config_sync.first_pull == "blocking") {
            Config reloaded_cfg;
            if (reloaded_cfg.load(config_base)) {
                cfg = std::move(reloaded_cfg);
                app_cfg = app_config_from(cfg);
            } else {
                std::cerr << "Reload config failed after config_sync blocking first pull; "
                          << "using pre-pull config" << std::endl;
            }
        }
    } else if (app_cfg.config_sync.enabled && app_cfg.config_sync.first_pull == "blocking") {
        std::cerr << "config_sync blocking first pull failed; using local config"
                  << std::endl;
    }

    Logger::instance().init(app_cfg.log_file, app_cfg.log_level);

    LOG_INFO("Server starting...");

    // Declared outside the try so the catch path can join any worker threads
    // that were already spawned before rethrowing. Otherwise a throw between
    // spawn and join would run their std::thread destructors on a still-running
    // io_context -> std::terminate / data race during unwind.
    std::vector<std::thread> threads;
    auto join_all = [&threads]() {
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
    };
    try {
        initialize(cfg, app_cfg, config_base);

        co_spawn(server_->executor(), server_->start(), [this](std::exception_ptr ep) {
            if (!ep) return;
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                LOG_ERROR("HTTP accept loop failed: ", e.what());
            } catch (...) {
                LOG_ERROR("HTTP accept loop failed with an unknown exception");
            }
            request_stop();
        });

        signal_exit_ = std::make_unique<SignalExit>(ioc_);
        signal_exit_->on_exit([this]() {
            request_stop();
        });

        unsigned int thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) thread_count = 4;
        for (unsigned int i = 1; i < thread_count; ++i) {
            threads.emplace_back([this]() { run_io_context(); });
        }
        run_io_context();

        join_all();

        cleanup();
        LOG_INFO("Server exited");
        // A fatal handler exception already forced the shutdown: report it via
        // the exit code so Restart=on-failure / containers / CLI do not mistake
        // it for a normal exit (systemd Restart=always would restart either way).
        return fatal_handler_exception_.load(std::memory_order_acquire) ? 1 : 0;
    } catch (...) {
        ioc_.stop();
        join_all();
        cleanup();
        throw;
    }
}

void Application::run_io_context() noexcept {
    for (;;) {
        try {
            ioc_.run();
            return;
        } catch (const std::exception& e) {
            try {
                LOG_ERROR("Unhandled io_context handler exception; stopping server: ", e.what());
            } catch (...) {
                try {
                    std::cerr << "Unhandled io_context handler exception: "
                              << e.what() << std::endl;
                } catch (...) {
                }
            }
            stop_after_handler_exception();
        } catch (...) {
            try {
                LOG_ERROR("Unhandled non-standard io_context handler exception; stopping server");
            } catch (...) {
                try {
                    std::cerr << "Unhandled non-standard io_context handler exception"
                              << std::endl;
                } catch (...) {
                }
            }
            stop_after_handler_exception();
        }
    }
}

void Application::stop_after_handler_exception() noexcept {
    fatal_handler_exception_.store(true, std::memory_order_release);
    const bool already_stopping = stop_requested_.load(std::memory_order_acquire);
    try {
        request_stop();
    } catch (...) {
        ioc_.stop();
        return;
    }
    if (already_stopping) {
        // An exception escaped while shutdown was already in progress. Do not
        // risk waiting forever for a drain timer that may not have been armed.
        ioc_.stop();
    }
}

void Application::initialize(const Config& cfg, const AppConfig& app_cfg,
                             const std::filesystem::path& config_base) {
    mysql_ = std::make_unique<MysqlPool>(ioc_, app_cfg.mysql);
    redis_ = std::make_unique<RedisPool>(ioc_, app_cfg.redis);
    combo_query_limiter_ = std::make_shared<ComboQueryLimiter>(
        app_cfg.combo_max_in_flight_queries);
    server_ = std::make_unique<HttpServer>(
        ioc_, app_cfg.server_port, app_cfg.downstream_write_timeout_ms,
        app_cfg.client_header_read_timeout_ms, app_cfg.client_body_read_timeout_ms);

    security_rules_ = std::make_unique<SecurityRules>();
    security_rules_->load_from_config(cfg);
    server_->set_security_rules(security_rules_.get());

    snapshot_service_ = std::make_unique<SnapshotService>(ioc_, *security_rules_);
    snapshot_service_->start(app_cfg.snapshot_interval_sec);

    config_history_service_ = std::make_shared<ConfigHistoryService>(
        ioc_, *redis_, app_cfg.config_sync.history, app_cfg.redis.cmd_timeout_ms);

    register_routes(*server_, AppServices{
        .mysql = mysql_.get(),
        .redis = redis_.get(),
        .combo_query_limiter = combo_query_limiter_,
        .combo_backend = make_pool_combo_backend(mysql_.get(), redis_.get()),
        .combo_deadline_ms = app_cfg.combo_deadline_ms,
        .config_base = config_base,
        .config_sync = app_cfg.config_sync,
        .config_history_service = config_history_service_,
        .redis_command = [redis = redis_.get()](std::vector<std::string> args) {
            return redis->cmd_argv(std::move(args));
        }
    });

    register_upstreams(cfg, app_cfg.http_pool);

    pool_stats_service_ = std::make_unique<PoolStatsService>(ioc_, server_->upstreams());
    pool_stats_service_->start(app_cfg.http_pool_stats_interval_sec);

    config_sync_service_ = std::make_shared<ConfigSyncService>(
        ioc_, *redis_, config_base, app_cfg.config_sync, app_cfg);
    config_sync_service_->start();
    config_history_service_->start();

    reload_service_ = std::make_unique<ReloadService>(
        ioc_, config_base, *security_rules_, server_->upstreams());
    reload_service_->start(app_cfg.reload_interval_sec);
}

void Application::register_upstreams(const Config& cfg, const HttpPool::Config& http_pool_cfg) {
    server_->upstreams().reload(cfg, http_pool_cfg);
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
    if (config_history_service_) config_history_service_->stop();
    if (config_sync_service_) config_sync_service_->stop();
    if (reload_service_) reload_service_->stop();
    if (pool_stats_service_) pool_stats_service_->stop();
    if (snapshot_service_) snapshot_service_->stop();
    if (server_) server_->stop();

    signal_exit_.reset();
    config_history_service_.reset();
    config_sync_service_.reset();
    reload_service_.reset();
    pool_stats_service_.reset();
    snapshot_service_.reset();
    server_.reset();

    if (mysql_) mysql_->shutdown();
    if (redis_) redis_->shutdown();
    redis_.reset();
    mysql_.reset();
    combo_query_limiter_.reset();
    drain_timer_.reset();
    security_rules_.reset();
}
