#pragma once

#include "../db/mysql_pool.hpp"
#include "../db/redis_pool.hpp"
#include "../http/http_server.hpp"
#include "combo_query_limiter.hpp"

class ComboBackend {
public:
    virtual ~ComboBackend() = default;
    virtual asio::awaitable<std::string> get_cache() = 0;
    virtual asio::awaitable<MysqlPool::Result> query() = 0;
    virtual void set_cache(std::string data) = 0;
};

struct AppServices {
    MysqlPool* mysql = nullptr;
    RedisPool* redis = nullptr;
    std::shared_ptr<ComboQueryLimiter> combo_query_limiter;
    std::shared_ptr<ComboBackend> combo_backend;
    int combo_deadline_ms = 500;
};

std::shared_ptr<ComboBackend> make_pool_combo_backend(MysqlPool* mysql, RedisPool* redis);
asio::awaitable<void> handle_api_combo(HttpContext& ctx, AppServices services);
void register_routes(HttpServer& server, AppServices services);
