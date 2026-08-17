#pragma once

#include <filesystem>
#include <functional>

#include "../db/mysql_pool.hpp"
#include "../db/redis_pool.hpp"
#include "../http/http_server.hpp"
#include "app_config.hpp"
#include "combo_query_limiter.hpp"

class ComboBackend {
public:
    virtual ~ComboBackend() = default;
    virtual asio::awaitable<std::string> get_cache() = 0;
    virtual asio::awaitable<MysqlPool::Result> query() = 0;
};

struct AppServices {
    MysqlPool* mysql = nullptr;
    RedisPool* redis = nullptr;
    std::shared_ptr<ComboQueryLimiter> combo_query_limiter;
    std::shared_ptr<ComboBackend> combo_backend;
    int combo_deadline_ms = 500;
    std::filesystem::path config_base;
    ConfigSyncConfig config_sync;
    std::function<asio::awaitable<RedisPool::Reply>(std::vector<std::string>)> redis_command;
};

std::shared_ptr<ComboBackend> make_pool_combo_backend(MysqlPool* mysql, RedisPool* redis);
asio::awaitable<void> handle_api_combo(HttpContext& ctx, AppServices services);
asio::awaitable<void> handle_api_admin_login(HttpContext& ctx, AppServices services);
asio::awaitable<void> handle_api_admin_config(HttpContext& ctx, AppServices services);
asio::awaitable<void> handle_api_admin_machines(HttpContext& ctx, AppServices services);
asio::awaitable<void> handle_admin_page(HttpContext& ctx);
asio::awaitable<void> handle_admin_settings_page(HttpContext& ctx);
void register_routes(HttpServer& server, AppServices services);
