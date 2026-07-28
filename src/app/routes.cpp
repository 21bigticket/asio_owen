#include "routes.hpp"

#include <asio/co_spawn.hpp>
#include <asio/this_coro.hpp>

#include <chrono>
#include <optional>

#include "combo_deadline.hpp"
#include "../db/mysql_result_json.hpp"
#include "../http/response.hpp"

namespace {

class PoolComboBackend final : public ComboBackend {
public:
    PoolComboBackend(MysqlPool* mysql, RedisPool* redis) : mysql_(mysql), redis_(redis) {}

    asio::awaitable<std::string> get_cache() override {
        auto reply = co_await redis_->get("cache:user:1");
        co_return reply.ok ? reply.str : "";
    }

    asio::awaitable<MysqlPool::Result> query() override {
        co_return co_await mysql_->execute("SELECT 'from_mysql' AS name");
    }

    asio::awaitable<void> set_cache(std::string data) override {
        auto set = co_await redis_->cmd_argv(
            {"SET", "cache:user:1", std::move(data), "EX", "300"});
        if (!set.ok) LOG_WARN("combo cache SET failed: ", set.error);
        co_return;
    }

private:
    MysqlPool* mysql_;
    RedisPool* redis_;
};

asio::awaitable<std::optional<MysqlPool::Result>> execute_combo_with_deadline(
    asio::awaitable<MysqlPool::Result> query, ComboQueryPermit permit,
    std::chrono::milliseconds deadline) {
    co_return co_await await_combo_query_with_deadline(
        std::move(query), std::move(permit), deadline,
        [](std::exception_ptr ep) {
            MysqlPool::Result result;
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    result = {false, e.what(), {}};
                } catch (...) {
                    result = {false, "unknown MySQL exception", {}};
                }
            }
            return result;
        });
}

asio::awaitable<void> api_mysql(HttpContext& ctx, AppServices services) {
    auto res = co_await services.mysql->execute("SELECT * FROM sys_dict_type LIMIT 20");
    ctx.response_headers.emplace_back("Content-Type", "application/json");
    if (!res.ok) {
        ctx.status_code = 500;
        ctx.response_body = resp_err(DB_ERROR, res.error);
    } else {
        ctx.status_code = 200;
        ctx.response_body = resp_ok(res.json);
    }
}

asio::awaitable<void> api_redis(HttpContext& ctx, AppServices services) {
    try {
        auto g = co_await services.redis->get("demo_key");
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

}  // namespace

std::shared_ptr<ComboBackend> make_pool_combo_backend(MysqlPool* mysql, RedisPool* redis) {
    return std::make_shared<PoolComboBackend>(mysql, redis);
}

asio::awaitable<void> handle_api_combo(HttpContext& ctx, AppServices services) {
    auto redis_ret = co_await services.combo_backend->get_cache();

    std::string data;
    if (!redis_ret.empty()) {
        data = redis_ret;
    } else {
        auto permit = services.combo_query_limiter->try_acquire();
        if (!permit) {
            ctx.response_headers.emplace_back("Content-Type", "application/json");
            ctx.status_code = 503;
            ctx.response_body = resp_err(DB_ERROR, "too many in-flight MySQL queries");
            co_return;
        }
        auto mysql_ret = co_await execute_combo_with_deadline(
            services.combo_backend->query(), std::move(*permit),
            std::chrono::milliseconds(services.combo_deadline_ms));
        if (!mysql_ret) {
            ctx.response_headers.emplace_back("Content-Type", "application/json");
            ctx.status_code = 504;
            ctx.response_body = resp_err(DB_ERROR, "MySQL query deadline exceeded");
            co_return;
        }
        if (!mysql_ret->ok) {
            ctx.response_headers.emplace_back("Content-Type", "application/json");
            ctx.status_code = 500;
            ctx.response_body = resp_err(DB_ERROR, mysql_ret->error);
            co_return;
        }
        auto parsed = extract_first_string_value(mysql_ret->json);
        if (!parsed) {
            ctx.response_headers.emplace_back("Content-Type", "application/json");
            ctx.status_code = 500;
            ctx.response_body = resp_err(DB_ERROR, "invalid MySQL response");
            co_return;
        }
        data = std::move(*parsed);
        auto ex = co_await asio::this_coro::executor;
        auto backend = services.combo_backend;
        asio::co_spawn(ex, backend->set_cache(data), [backend](std::exception_ptr ep) {
            if (!ep) return;
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                LOG_WARN("combo cache SET failed: ", e.what());
            } catch (...) {
                LOG_WARN("combo cache SET failed with an unknown exception");
            }
        });
    }

    ctx.response_headers.emplace_back("Content-Type", "application/json");
    ctx.status_code = 200;
    ctx.response_body = resp_ok_str(data);
}

asio::awaitable<void> api_health(HttpContext& ctx) {
    ctx.response_headers.emplace_back("Content-Type", "application/json");
    ctx.status_code = 200;
    ctx.response_body = resp_ok_str("running");
    co_return;
}

asio::awaitable<void> api_build(HttpContext& ctx) {
    ctx.response_headers.emplace_back("Content-Type", "application/json");
    ctx.status_code = 200;
    ctx.response_body = "{\"code\":0,\"build\":\"asio_owen\"}";
    co_return;
}

void register_routes(HttpServer& server, AppServices services) {
    server.route("/api/health", api_health);
    server.route("/api/build", api_build);
    server.route("/api/redis", [services](HttpContext& ctx) {
        return api_redis(ctx, services);
    });
    server.route("/api/mysql", [services](HttpContext& ctx) {
        return api_mysql(ctx, services);
    });
    server.route("/api/combo", [services](HttpContext& ctx) {
        return handle_api_combo(ctx, services);
    });
}
