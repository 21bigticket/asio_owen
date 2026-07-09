#include "routes.hpp"

#include <asio/post.hpp>
#include <asio/this_coro.hpp>

#include "../http/response.hpp"

namespace {

void set_cache(RedisPool* redis, const std::string& data);

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

asio::awaitable<void> api_combo(HttpContext& ctx, AppServices services) {
    auto redis_resp = co_await services.redis->get("cache:user:1");
    std::string redis_ret = redis_resp.ok ? redis_resp.str : "";

    std::string data;
    if (!redis_ret.empty()) {
        data = redis_ret;
    } else {
        auto mysql_ret = co_await services.mysql->execute("SELECT 'from_mysql' AS name");
        if (mysql_ret.ok && mysql_ret.json.size() > 2) {
            auto pos = mysql_ret.json.find(":\"");
            if (pos != std::string::npos) {
                auto end = mysql_ret.json.find("\"", pos + 2);
                if (end != std::string::npos) {
                    data = mysql_ret.json.substr(pos + 2, end - pos - 2);
                }
            }
            auto ex = co_await asio::this_coro::executor;
            auto redis = services.redis;
            asio::post(ex, [redis, data] {
                set_cache(redis, data);
            });
        }
    }

    ctx.response_headers.emplace_back("Content-Type", "application/json");
    ctx.status_code = 200;
    ctx.response_body = resp_ok_str(data);
}

void set_cache(RedisPool* redis, const std::string& data) {
    redis->cmd_argv_sync({"SET", "cache:user:1", data});
    redis->cmd_argv_sync({"EXPIRE", "cache:user:1", "300"});
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
    ctx.response_body = "{\"code\":0,\"build\":\"gateway-debug-20260703-client-close-trace\"}";
    co_return;
}

}  // namespace

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
        return api_combo(ctx, services);
    });
}
