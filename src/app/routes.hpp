#pragma once

#include "../db/mysql_pool.hpp"
#include "../db/redis_pool.hpp"
#include "../http/http_server.hpp"

struct AppServices {
    MysqlPool* mysql = nullptr;
    RedisPool* redis = nullptr;
};

void register_routes(HttpServer& server, AppServices services);
