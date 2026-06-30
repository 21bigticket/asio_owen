#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <iostream>
#include <chrono>
#include <memory>
#include <sstream>

#include "common/logger.hpp"
#include "common/config.hpp"
#include "common/timeout.hpp"
#include "common/signal_exit.hpp"
#include "db/mysql_pool.hpp"
#include "db/redis_pool.hpp"
#include "http/http_server.hpp"
#include "http/response.hpp"

using namespace std::chrono_literals;

std::unique_ptr<MysqlPool> g_mysql;
std::unique_ptr<RedisPool> g_redis;
std::unique_ptr<HttpServer> g_server;

// 查询 MySQL — worker 线程直接拼好 JSON 返回
asio::awaitable<std::string> api_mysql(const std::string&, const std::string&) {
    auto res = co_await g_mysql->execute("SELECT * FROM sys_dict_type LIMIT 20");
    if (!res.ok) {
        co_return resp_err(DB_ERROR, res.error);
    }
    co_return resp_ok(res.json);
}

// 查询 Redis — 单 GET 命令吞吐
asio::awaitable<std::string> api_redis(const std::string&, const std::string&) {
    try {
        auto g = co_await g_redis->cmd("GET demo_key");
        if (!g.ok) co_return resp_err(DB_ERROR, g.error);
        co_return resp_ok_str(g.str);
    } catch (const std::exception& e) {
        co_return resp_err(DB_ERROR, e.what());
    }
}

// 组合查询：Redis 缓存 + MySQL 兜底 + 超时
asio::awaitable<std::string> api_combo(const std::string&, const std::string&) {
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer timer(ex);

    auto redis_ret = co_await with_timeout<std::string>(timer,
        []() -> asio::awaitable<std::string> {
            auto resp = co_await g_redis->cmd("GET cache:user:1");
            co_return resp.ok ? resp.str : "";
        }(),
        500ms
    );

    std::string data;
    if (redis_ret && !redis_ret->empty()) {
        data = *redis_ret;
    } else {
        auto mysql_ret = co_await g_mysql->execute("SELECT 'from_mysql' AS name");
        if (mysql_ret.ok && mysql_ret.json.size() > 2) {
            // json 格式: [{"name":"from_mysql"}]
            auto pos = mysql_ret.json.find(":\"");
            if (pos != std::string::npos) {
                auto end = mysql_ret.json.find("\"", pos + 2);
                if (end != std::string::npos) {
                    data = mysql_ret.json.substr(pos + 2, end - pos - 2);
                }
            }
            co_spawn(ex, [data]() -> asio::awaitable<void> {
                co_await g_redis->cmd("SET cache:user:1 %s", data.c_str());
                co_await g_redis->cmd("EXPIRE cache:user:1 300");
            }, asio::detached);
        }
    }

    co_return resp_ok_str(data);
}

// 健康检查
asio::awaitable<std::string> api_health(const std::string&, const std::string&) {
    co_return resp_ok_str("running");
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

        asio::io_context ioc;

        g_mysql = std::make_unique<MysqlPool>(ioc, MysqlPool::Config{
            .host = cfg.get("mysql", "host", "127.0.0.1"),
            .port = cfg.get_int("mysql", "port", 3306),
            .user = cfg.get("mysql", "user", "root"),
            .pass = cfg.get("mysql", "pass", ""),
            .db = cfg.get("mysql", "db", "test"),
            .pool_size = (size_t)cfg.get_int("mysql", "pool_size", 8),
            .keepalive_sec = cfg.get_int("mysql", "keepalive_sec", 30)
        });

        g_redis = std::make_unique<RedisPool>(ioc, RedisPool::Config{
            .host = cfg.get("redis", "host", "127.0.0.1"),
            .port = cfg.get_int("redis", "port", 6379),
            .pool_size = (size_t)cfg.get_int("redis", "pool_size", 8),
            .keepalive_sec = cfg.get_int("redis", "keepalive_sec", 30)
        });

        int port = cfg.get_int("server", "port", 8080);
        g_server = std::make_unique<HttpServer>(ioc, port);
        g_server->route("/api/health", api_health);
        g_server->route("/api/mysql", api_mysql);
        g_server->route("/api/redis", api_redis);
        g_server->route("/api/combo", api_combo);

        co_spawn(ioc, g_server->start(), asio::detached);

        SignalExit sig(ioc);
        sig.on_exit([&]() {
            LOG_INFO("Graceful shutdown started...");
            g_server->stop();
            g_mysql->shutdown();
            g_redis->shutdown();
            ioc.stop();
            LOG_INFO("Graceful shutdown done");
        });

        // 多线程运行 io_context，提升并发处理能力
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
        LOG_INFO("Server exited");

    } catch (const std::exception& e) {
        LOG_ERROR("Fatal error: ", e.what());
        return 1;
    }
    return 0;
}
