#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <iostream>
#include <chrono>
#include <memory>
#include <sstream>
#include <atomic>
#include <thread>
#include <vector>

#include "common/logger.hpp"
#include "common/config.hpp"
#include "common/signal_exit.hpp"
#include "db/mysql_pool.hpp"
#include "db/redis_pool.hpp"
#include "http/http_server.hpp"
#include "http/response.hpp"
#include "security/security_rules.hpp"

using namespace std::chrono_literals;

std::unique_ptr<MysqlPool> g_mysql;
std::unique_ptr<RedisPool> g_redis;
std::unique_ptr<HttpServer> g_server;
std::unique_ptr<asio::steady_timer> g_drain_timer;
std::unique_ptr<SecurityRules> g_security_rules;
std::unique_ptr<asio::steady_timer> g_reload_timer;

void cleanup_runtime_objects() {
    g_reload_timer.reset();
    if (g_server) {
        g_server->stop();
    }
    if (g_mysql) {
        g_mysql->shutdown();
    }
    if (g_redis) {
        g_redis->shutdown();
    }

    g_server.reset();
    g_redis.reset();
    g_mysql.reset();
    g_drain_timer.reset();
    g_security_rules.reset();
}

// MySQL query — worker thread builds JSON directly
asio::awaitable<void> api_mysql(HttpContext& ctx) {
    auto res = co_await g_mysql->execute("SELECT * FROM sys_dict_type LIMIT 20");
    ctx.response_headers.emplace_back("Content-Type", "application/json");
    if (!res.ok) {
        ctx.status_code = 500;
        ctx.response_body = resp_err(DB_ERROR, res.error);
    } else {
        ctx.status_code = 200;
        ctx.response_body = resp_ok(res.json);
    }
}

// Redis query — single GET command throughput
asio::awaitable<void> api_redis(HttpContext& ctx) {
    try {
        auto g = co_await g_redis->get("demo_key");
        ctx.response_headers.emplace_back("Content-Type", "application/json");
        if (!g.ok) {
            ctx.status_code = 500;
            ctx.response_body = resp_err(DB_ERROR, g.error);
        } else {
            ctx.status_code = 200;
            ctx.response_body = resp_ok_str(g.str);
        }
    } catch (const std::exception& e) {
        ctx.status_code = 500;
        ctx.response_body = resp_err(DB_ERROR, e.what());
    }
}

// Combo query: Redis cache + MySQL fallback + timeout
asio::awaitable<void> api_combo(HttpContext& ctx) {
    auto redis_resp = co_await g_redis->cmd("GET cache:user:1");
    std::string redis_ret = redis_resp.ok ? redis_resp.str : "";

    std::string data;
    if (!redis_ret.empty()) {
        data = redis_ret;
    } else {
        auto mysql_ret = co_await g_mysql->execute("SELECT 'from_mysql' AS name");
        if (mysql_ret.ok && mysql_ret.json.size() > 2) {
            auto pos = mysql_ret.json.find(":\"");
            if (pos != std::string::npos) {
                auto end = mysql_ret.json.find("\"", pos + 2);
                if (end != std::string::npos) {
                    data = mysql_ret.json.substr(pos + 2, end - pos - 2);
                }
            }
            auto ex2 = co_await asio::this_coro::executor;
            co_spawn(ex2, [data]() -> asio::awaitable<void> {
                co_await g_redis->cmd("SET cache:user:1 %s", data.c_str());
                co_await g_redis->cmd("EXPIRE cache:user:1 300");
            }, asio::detached);
        }
    }

    ctx.response_headers.emplace_back("Content-Type", "application/json");
    ctx.status_code = 200;
    ctx.response_body = resp_ok_str(data);
}

// Health check
asio::awaitable<void> api_health(HttpContext& ctx) {
    ctx.response_headers.emplace_back("Content-Type", "application/json");
    ctx.status_code = 200;
    ctx.response_body = resp_ok_str("running");
    co_return;
}

asio::awaitable<void> api_build(HttpContext& ctx) {
    ctx.response_headers.emplace_back("Content-Type", "application/json");
    ctx.status_code = 200;
    ctx.response_body = "{\"code\":0,\"build\":\"gateway-debug-20260703-client-close-trace\"}";
    co_return;
}

int main(int argc, char* argv[]) {
    try {
        Config cfg;
        if (!cfg.load("config.ini")) {
            std::cerr << "Load config failed" << std::endl;
            return 1;
        }

        LogLevel lvl = INFO;
        std::string lv = cfg.get("server", "log_level", "INFO");
        if (lv == "DEBUG") lvl = DEBUG;
        else if (lv == "WARN") lvl = WARN;
        else if (lv == "ERROR") lvl = ERROR;
        Logger::instance().init(cfg.get("server", "log_file", "server.log"), lvl);

        LOG_INFO("Server starting...");
        LOG_INFO("Build marker: gateway-debug-20260703-client-close-trace");

        asio::io_context ioc;
        try {
            g_mysql = std::make_unique<MysqlPool>(ioc, MysqlPool::Config{
                .host = cfg.get("mysql", "host", "127.0.0.1"),
                .port = cfg.get_int("mysql", "port", 3306),
                .user = cfg.get("mysql", "user", "root"),
                .pass = cfg.get("mysql", "pass", ""),
                .db = cfg.get("mysql", "db", "test"),
                .min_size = (size_t)cfg.get_int("mysql", "min_size", 8),
                .max_size = (size_t)cfg.get_int("mysql", "max_size", 64),
                .max_idle_sec = cfg.get_int("mysql", "max_idle_sec", 60),
                .connect_timeout_ms = cfg.get_int("mysql", "connect_timeout_ms", 1000),
                .read_timeout_ms = cfg.get_int("mysql", "read_timeout_ms", 500),
                .keepalive_sec = cfg.get_int("mysql", "keepalive_sec", 30),
                .worker_threads = (size_t)std::max(1, cfg.get_int("mysql", "worker_threads", 32))
            });

            g_redis = std::make_unique<RedisPool>(ioc, RedisPool::Config{
                .host = cfg.get("redis", "host", "127.0.0.1"),
                .port = cfg.get_int("redis", "port", 6379),
                .connect_timeout_ms = cfg.get_int("redis", "connect_timeout_ms", 1000),
                .cmd_timeout_ms = cfg.get_int("redis", "cmd_timeout_ms", 1000)
            });

            int port = cfg.get_int("server", "port", 8080);
            g_server = std::make_unique<HttpServer>(ioc, port);

            // initialize security rules
            g_security_rules = std::make_unique<SecurityRules>();
            g_security_rules->load_from_config(cfg);
            g_server->set_security_rules(g_security_rules.get());

            // security rules hot-reload timer (shared_ptr wraps self-reference to avoid dangling)
            int reload_sec = cfg.get_int("security", "config_reload_interval_sec", 30);
            if (reload_sec > 0) {
                g_reload_timer = std::make_unique<asio::steady_timer>(ioc);
                auto schedule_reload = std::make_shared<std::function<void()>>();
                *schedule_reload = [this_reload = schedule_reload, timer = g_reload_timer.get(), reload_sec]() {
                    timer->expires_after(std::chrono::seconds(reload_sec));
                    timer->async_wait([this_reload](std::error_code ec) {
                        if (ec) return;
                        Config new_cfg;
                        if (new_cfg.load("config.ini") && g_security_rules) {
                            g_security_rules->reload(new_cfg);
                        }
                        if (*this_reload) (*this_reload)();
                    });
                };
                (*schedule_reload)();
            }

            // rate limit snapshot persist timer (shared_ptr wraps self-reference)
            int snapshot_sec = cfg.get_int("rate_limit", "snapshot_interval_sec", 30);
            if (snapshot_sec > 0 && g_security_rules->has_rate_limiter()) {
                auto snap_timer = std::make_shared<asio::steady_timer>(ioc);
                auto schedule_snapshot = std::make_shared<std::function<void()>>();
                *schedule_snapshot = [this_snap = schedule_snapshot, snap_timer, snapshot_sec]() {
                    snap_timer->expires_after(std::chrono::seconds(snapshot_sec));
                    snap_timer->async_wait([this_snap](std::error_code ec) {
                        if (ec) return;
                        if (g_security_rules && g_security_rules->has_rate_limiter()) {
                            g_security_rules->rate_limiter().persist_snapshot();
                        }
                        if (*this_snap) (*this_snap)();
                    });
                };
                (*schedule_snapshot)();
            }

            // register local routes
            g_server->route("/api/health", api_health);
            g_server->route("/api/build", api_build);
            g_server->route("/api/redis", api_redis);
            g_server->route("/api/mysql", api_mysql);
            g_server->route("/api/combo", api_combo);

            // register proxy routes: read from [upstream] config section
            // format: service_name = host:port
            // example: config = 127.0.0.1:30001
            HttpPool::Config http_pool_cfg{
                .max_size = (size_t)std::max(1, cfg.get_int("http_pool", "max_size", 256)),
                .max_concurrent = (size_t)std::max(0, cfg.get_int("http_pool", "max_concurrent", 0)),
                .max_body_size = (size_t)std::max(1, cfg.get_int("http_pool", "max_body_size", 10 * 1024 * 1024)),
                .connect_timeout_ms = cfg.get_int("http_pool", "connect_timeout_ms", 1000),
                .read_timeout_ms = cfg.get_int("http_pool", "read_timeout_ms", 30000),
                .request_timeout_ms = cfg.get_int("http_pool", "request_timeout_ms", 60000),
                .idle_timeout_sec = cfg.get_int("http_pool", "idle_timeout_sec", 60)
            };
            for (auto& [key, val] : cfg.get_section("upstream")) {
                auto colon = val.find(':');
                if (colon != std::string::npos) {
                    auto host = val.substr(0, colon);
                    int port = std::stoi(val.substr(colon + 1));
                    g_server->upstreams().add_upstream(key, host, port, http_pool_cfg);
                    LOG_INFO("upstream ", key, " -> ", host, ":", port);
                }
            }

            co_spawn(ioc, g_server->start(), asio::detached);

            std::atomic<bool> stop_requested{false};
            SignalExit sig(ioc);
            sig.on_exit([&]() {
                if (stop_requested.exchange(true)) return;
                LOG_INFO("Graceful shutdown requested...");
                if (g_server) g_server->stop();
                // 5 second drain timeout for in-flight requests
                g_drain_timer = std::make_unique<asio::steady_timer>(ioc);
                g_drain_timer->expires_after(std::chrono::seconds(5));
                g_drain_timer->async_wait([&](std::error_code ec) {
                    if (ec) return;
                    LOG_INFO("Drain timeout, stopping io_context...");
                    ioc.stop();
                });
            });

            // multi-thread io_context for higher concurrency
            std::vector<std::thread> threads;
            unsigned int thread_count = std::thread::hardware_concurrency();
            if (thread_count == 0) thread_count = 4;
            for (unsigned int i = 1; i < thread_count; ++i) {
                threads.emplace_back([&ioc]() { ioc.run(); });
            }
            ioc.run();

            for (auto& t : threads) {
                if (t.joinable()) t.join();
            }

            cleanup_runtime_objects();
        } catch (...) {
            ioc.stop();
            cleanup_runtime_objects();
            throw;
        }
        LOG_INFO("Server exited");

    } catch (const std::exception& e) {
        LOG_ERROR("Fatal error: ", e.what());
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
