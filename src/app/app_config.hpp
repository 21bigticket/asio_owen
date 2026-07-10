#pragma once

#include <algorithm>
#include <cstddef>
#include <string>

#include "../common/config.hpp"
#include "../common/logger.hpp"
#include "../db/mysql_pool.hpp"
#include "../db/redis_pool.hpp"
#include "../http/http_pool.hpp"

struct AppConfig {
    LogLevel log_level = INFO;
    std::string log_file = "server.log";
    int server_port = 8080;
    MysqlPool::Config mysql;
    RedisPool::Config redis;
    HttpPool::Config http_pool;
    int snapshot_interval_sec = 30;
    int reload_interval_sec = 30;
    int http_pool_stats_interval_sec = 30;
};

inline AppConfig app_config_from(const Config& cfg) {
    AppConfig app;

    auto level = cfg.get("server", "log_level", "INFO");
    if (level == "DEBUG") app.log_level = DEBUG;
    else if (level == "WARN") app.log_level = WARN;
    else if (level == "ERROR") app.log_level = ERROR;

    app.log_file = cfg.get("server", "log_file", "server.log");
    app.server_port = cfg.get_int("server", "port", 8080);

    app.mysql = MysqlPool::Config{
        .host = cfg.get("mysql", "host", "127.0.0.1"),
        .port = cfg.get_int("mysql", "port", 3306),
        .user = cfg.get("mysql", "user", "root"),
        .pass = cfg.get("mysql", "pass", ""),
        .db = cfg.get("mysql", "db", "test"),
        .min_size = static_cast<size_t>(std::max(0, cfg.get_int("mysql", "min_size", 8))),
        .max_size = static_cast<size_t>(std::max(1, cfg.get_int("mysql", "max_size", 64))),
        .max_idle_sec = cfg.get_int("mysql", "max_idle_sec", 60),
        .connect_timeout_ms = cfg.get_int("mysql", "connect_timeout_ms", 1000),
        .read_timeout_ms = cfg.get_int("mysql", "read_timeout_ms", 500),
        .keepalive_sec = cfg.get_int("mysql", "keepalive_sec", 30),
        .worker_threads = static_cast<size_t>(std::max(1, cfg.get_int("mysql", "worker_threads", 32))),
        .max_creating = static_cast<size_t>(std::max(0, cfg.get_int("mysql", "max_creating", 0)))
    };

    app.redis = RedisPool::Config{
        .host = cfg.get("redis", "host", "127.0.0.1"),
        .port = cfg.get_int("redis", "port", 6379),
        .connect_timeout_ms = cfg.get_int("redis", "connect_timeout_ms", 1000),
        .cmd_timeout_ms = cfg.get_int("redis", "cmd_timeout_ms", 1000)
    };

    app.http_pool = HttpPool::Config{
        .max_size = static_cast<size_t>(std::max(1, cfg.get_int("http_pool", "max_size", 256))),
        .max_concurrent = static_cast<size_t>(std::max(0, cfg.get_int("http_pool", "max_concurrent", 0))),
        .max_body_size = static_cast<size_t>(std::max(1, cfg.get_int("http_pool", "max_body_size", 10 * 1024 * 1024))),
        .connect_timeout_ms = cfg.get_int("http_pool", "connect_timeout_ms", 1000),
        .read_timeout_ms = cfg.get_int("http_pool", "read_timeout_ms", 30000),
        .request_timeout_ms = cfg.get_int("http_pool", "request_timeout_ms", 60000),
        .idle_timeout_sec = cfg.get_int("http_pool", "idle_timeout_sec", 60),
        .send_keep_alive_header = cfg.get_bool("http_pool", "send_keep_alive_header", false)
    };

    app.snapshot_interval_sec = cfg.get_int("rate_limit", "snapshot_interval_sec", 30);
    app.reload_interval_sec = cfg.get_int("security", "config_reload_interval_sec", 30);
    app.http_pool_stats_interval_sec = cfg.get_int("http_pool", "stats_interval_sec", 30);
    return app;
}
