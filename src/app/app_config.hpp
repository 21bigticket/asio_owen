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
    int downstream_write_timeout_ms = 30000;
    int client_header_read_timeout_ms = 10000;
    int client_body_read_timeout_ms = 30000;
    MysqlPool::Config mysql;
    RedisPool::Config redis;
    HttpPool::Config http_pool;
    int snapshot_interval_sec = 30;
    int reload_interval_sec = 30;
    int http_pool_stats_interval_sec = 30;
};

// Parse the [http_pool] section into an HttpPool::Config. Factored out so the
// hot-reload path (ReloadService) can re-read it on every reload instead of
// being stuck with the value captured at startup.
inline HttpPool::Config http_pool_config_from(const Config& cfg) {
    return HttpPool::Config{
        .max_size = static_cast<size_t>(std::max(1, cfg.get_int("http_pool", "max_size", 256))),
        .max_concurrent = static_cast<size_t>(std::max(0, cfg.get_int("http_pool", "max_concurrent", 0))),
        .max_body_size = static_cast<size_t>(std::max(1, cfg.get_int("http_pool", "max_body_size", 10 * 1024 * 1024))),
        .connect_timeout_ms = cfg.get_int("http_pool", "connect_timeout_ms", 1000),
        .read_timeout_ms = cfg.get_int("http_pool", "read_timeout_ms", 30000),
        .request_timeout_ms = cfg.get_int("http_pool", "request_timeout_ms", 60000),
        .idle_timeout_sec = cfg.get_int("http_pool", "idle_timeout_sec", 60),
        .send_keep_alive_header = cfg.get_bool("http_pool", "send_keep_alive_header", false)
    };
}

inline AppConfig app_config_from(const Config& cfg) {
    AppConfig app;

    auto level = cfg.get("server", "log_level", "INFO");
    if (level == "DEBUG") app.log_level = DEBUG;
    else if (level == "WARN") app.log_level = WARN;
    else if (level == "ERROR") app.log_level = ERROR;

    app.log_file = cfg.get("server", "log_file", "server.log");
    app.server_port = cfg.get_int("server", "port", 8080);
    app.downstream_write_timeout_ms = cfg.get_int("server", "downstream_write_timeout_ms", 30000);
    app.client_header_read_timeout_ms = cfg.get_int("server", "client_header_read_timeout_ms", 10000);
    app.client_body_read_timeout_ms = cfg.get_int("server", "client_body_read_timeout_ms", 30000);

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
        .query_timeout_ms = cfg.get_int("mysql", "query_timeout_ms", 0),
        .acquire_timeout_ms = cfg.get_int("mysql", "acquire_timeout_ms", 3000),
        .keepalive_sec = cfg.get_int("mysql", "keepalive_sec", 30),
        .worker_threads = static_cast<size_t>(std::max(1, cfg.get_int("mysql", "worker_threads", 32))),
        .max_creating = static_cast<size_t>(std::max(0, cfg.get_int("mysql", "max_creating", 0)))
    };

    auto redis_mode = cfg.get("redis", "mode", "direct");
    RedisPool::Mode redis_pool_mode = RedisPool::Mode::Direct;
    if (redis_mode == "worker" || redis_mode == "WORKER") {
        redis_pool_mode = RedisPool::Mode::Worker;
    } else if (redis_mode != "direct" && redis_mode != "DIRECT") {
        LOG_WARN("invalid redis.mode '", redis_mode, "', using direct");
    }

    app.redis = RedisPool::Config{
        .host = cfg.get("redis", "host", "127.0.0.1"),
        .port = cfg.get_int("redis", "port", 6379),
        .db = cfg.get_int("redis", "db", 0),
        .connect_timeout_ms = cfg.get_int("redis", "connect_timeout_ms", 1000),
        .cmd_timeout_ms = cfg.get_int("redis", "cmd_timeout_ms", 1000),
        .mode = redis_pool_mode,
        .min_size = static_cast<size_t>(std::max(0, cfg.get_int("redis", "min_size", 4))),
        .max_size = static_cast<size_t>(std::max(1, cfg.get_int("redis", "max_size", 32))),
        .max_idle_sec = cfg.get_int("redis", "max_idle_sec", 120),
        .worker_threads = static_cast<size_t>(std::max(1, cfg.get_int("redis", "worker_threads", 16))),
        .max_creating = static_cast<size_t>(std::max(0, cfg.get_int("redis", "max_creating", 0))),
        .acquire_timeout_ms = cfg.get_int("redis", "acquire_timeout_ms", 3000)
    };

    app.http_pool = http_pool_config_from(cfg);

    app.snapshot_interval_sec = cfg.get_int("rate_limit", "snapshot_interval_sec", 30);
    app.reload_interval_sec = cfg.get_int("security", "config_reload_interval_sec", 30);
    app.http_pool_stats_interval_sec = cfg.get_int("http_pool", "stats_interval_sec", 30);
    return app;
}
