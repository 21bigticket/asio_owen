#pragma once

#include "routes.hpp"

asio::awaitable<void> api_mysql(HttpContext& ctx, AppServices services);
asio::awaitable<void> api_redis(HttpContext& ctx, AppServices services);
asio::awaitable<void> api_health(HttpContext& ctx);
asio::awaitable<void> api_ready(HttpContext& ctx, AppServices services);
asio::awaitable<void> api_metrics(HttpContext& ctx, AppServices services);
asio::awaitable<void> api_build(HttpContext& ctx);
