#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

#include "../common/config.hpp"
#include "../common/logger.hpp"
#include "../db/mysql_pool.hpp"
#include "../db/redis_pool.hpp"
#include "../http/http_pool.hpp"

struct AdminAccountConfig {
    std::string username;
    std::string password_hash;
};

struct AdminConfig {
    std::vector<AdminAccountConfig> accounts;
    std::string jwt_private_key;
    std::string jwt_public_key;
    int token_ttl_min = 120;
    bool insecure_no_auth = false;
};

struct ConfigHistoryConfig {
    std::string read_mode = "required";
    bool auto_migrate_legacy = true;
    int retention_versions = 100;
    int retention_days = 90;
    size_t warn_snapshot_bytes = 256 * 1024;
    size_t max_snapshot_bytes = 512 * 1024;
    size_t warn_file_bytes = 64 * 1024;
    size_t max_file_bytes = 128 * 1024;
    size_t warn_files = 80;
    size_t max_files = 100;
    size_t max_reason_bytes = 512;
    size_t history_page_size = 20;
    size_t history_page_size_max = 50;
    size_t max_diff_response_bytes = 2 * 1024 * 1024;
    size_t gc_batch_size = 20;
    int gc_interval_sec = 300;
    int machine_ttl_sec = 3600;
};

struct ConfigSyncConfig {
    bool enabled = false;
    int sync_interval_sec = 5;
    std::string machine_name;
    std::string first_pull = "async";
    int first_pull_timeout_ms = 3000;
    AdminConfig admin;
    ConfigHistoryConfig history;
};

struct AppConfig {
    LogLevel log_level = INFO;
    std::string log_file = "server.log";
    int server_port = 8080;
    int downstream_write_timeout_ms = 30000;
    int client_header_read_timeout_ms = 10000;
    int client_body_read_timeout_ms = 30000;
    int combo_deadline_ms = 500;
    size_t combo_max_in_flight_queries = 8;
    MysqlPool::Config mysql;
    RedisPool::Config redis;
    HttpPool::Config http_pool;
    int snapshot_interval_sec = 30;
    int reload_interval_sec = 30;
    int http_pool_stats_interval_sec = 30;
    ConfigSyncConfig config_sync;
};

inline AdminConfig admin_config_from(const Config& cfg) {
    AdminConfig admin;
    admin.jwt_private_key = cfg.get("admin", "jwt_private_key", "");
    admin.jwt_public_key = cfg.get("admin", "jwt_public_key", "");
    admin.token_ttl_min = std::max(1, cfg.get_int("admin", "token_ttl_min", 120));
    admin.insecure_no_auth = cfg.get_bool("admin", "insecure_no_auth", false);
    for (const auto& [key, value] : cfg.get_section("admin")) {
        if (key == "jwt_private_key" || key == "jwt_public_key" ||
            key == "token_ttl_min" || key == "insecure_no_auth") {
            continue;
        }
        if (key.empty() || value.empty()) {
            continue;
        }
        if (value.rfind("pbkdf2_sha256$", 0) != 0) {
            LOG_WARN("ignoring unknown [admin] option or invalid account hash: ", key);
            continue;
        }
        auto it = std::find_if(admin.accounts.begin(), admin.accounts.end(),
            [&](const AdminAccountConfig& account) {
                return account.username == key;
            });
        if (it != admin.accounts.end()) {
            it->password_hash = value;
        } else {
            admin.accounts.push_back({key, value});
        }
    }
    return admin;
}

inline ConfigHistoryConfig config_history_from(const Config& cfg) {
    ConfigHistoryConfig history;
    history.read_mode = cfg.get("config_history", "read_mode", "required");
    std::transform(history.read_mode.begin(), history.read_mode.end(),
        history.read_mode.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (history.read_mode != "compat" && history.read_mode != "required") {
        LOG_WARN("invalid config_history.read_mode '", history.read_mode,
            "', using required");
        history.read_mode = "required";
    }
    history.auto_migrate_legacy = cfg.get_bool(
        "config_history", "auto_migrate_legacy", true);

    history.retention_versions = std::max(
        1, cfg.get_int("config_history", "retention_versions", 100));
    history.retention_days = std::max(
        1, cfg.get_int("config_history", "retention_days", 90));
    history.warn_snapshot_bytes = static_cast<size_t>(std::clamp(
        cfg.get_int("config_history", "warn_snapshot_bytes", 256 * 1024),
        1, 512 * 1024));
    history.max_snapshot_bytes = static_cast<size_t>(std::clamp(
        cfg.get_int("config_history", "max_snapshot_bytes", 512 * 1024),
        1, 512 * 1024));
    history.warn_file_bytes = static_cast<size_t>(std::clamp(
        cfg.get_int("config_history", "warn_file_bytes", 64 * 1024),
        1, 128 * 1024));
    history.max_file_bytes = static_cast<size_t>(std::clamp(
        cfg.get_int("config_history", "max_file_bytes", 128 * 1024),
        1, 128 * 1024));
    history.warn_files = static_cast<size_t>(std::clamp(
        cfg.get_int("config_history", "warn_files", 80), 1, 100));
    history.max_files = static_cast<size_t>(std::clamp(
        cfg.get_int("config_history", "max_files", 100), 1, 100));
    history.max_reason_bytes = static_cast<size_t>(std::clamp(
        cfg.get_int("config_history", "max_reason_bytes", 512), 1, 512));
    history.history_page_size = static_cast<size_t>(std::clamp(
        cfg.get_int("config_history", "history_page_size", 20), 1, 50));
    history.history_page_size_max = static_cast<size_t>(std::clamp(
        cfg.get_int("config_history", "history_page_size_max", 50), 1, 50));
    history.max_diff_response_bytes = static_cast<size_t>(std::clamp(
        cfg.get_int("config_history", "max_diff_response_bytes", 2 * 1024 * 1024),
        1, 2 * 1024 * 1024));
    history.gc_batch_size = static_cast<size_t>(std::clamp(
        cfg.get_int("config_history", "gc_batch_size", 20), 1, 20));
    history.gc_interval_sec = std::max(
        10, cfg.get_int("config_history", "gc_interval_sec", 300));
    history.machine_ttl_sec = std::max(
        60, cfg.get_int("config_history", "machine_ttl_sec", 3600));

    history.max_snapshot_bytes = std::max(
        history.max_snapshot_bytes, history.max_file_bytes);
    history.warn_snapshot_bytes = std::min(
        history.warn_snapshot_bytes, history.max_snapshot_bytes);
    history.warn_file_bytes = std::min(
        history.warn_file_bytes, history.max_file_bytes);
    history.max_files = std::max(history.max_files, history.warn_files);
    history.history_page_size = std::min(
        history.history_page_size, history.history_page_size_max);
    return history;
}

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
    app.combo_deadline_ms = std::max(1, cfg.get_int("server", "combo_deadline_ms", 500));
    app.combo_max_in_flight_queries = static_cast<size_t>(std::max(
        1, cfg.get_int("server", "combo_max_in_flight_queries", 8)));

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
    app.config_sync.enabled = cfg.get_bool("config_sync", "enabled", false);
    app.config_sync.sync_interval_sec = cfg.get_int("config_sync", "sync_interval_sec", 5);
    app.config_sync.machine_name = cfg.get("config_sync", "machine_name", "");
    app.config_sync.first_pull = cfg.get("config_sync", "first_pull", "async");
    app.config_sync.first_pull_timeout_ms = cfg.get_int(
        "config_sync", "first_pull_timeout_ms", 3000);
    app.config_sync.admin = admin_config_from(cfg);
    app.config_sync.history = config_history_from(cfg);
    return app;
}
