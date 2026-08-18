# GoogleTest test targets
include(GoogleTest)

# Placeholder — 验证 GoogleTest harness 正常
add_executable(test_placeholder test_placeholder.cpp)
target_link_libraries(test_placeholder PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_placeholder)

# 上游路由与路径重写
add_executable(test_upstream_manager test_upstream_manager.cpp)
target_link_libraries(test_upstream_manager PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_upstream_manager)

# Configuration hot-reload prepare/publish and retry behavior
add_executable(test_reload_service test_reload_service.cpp)
target_link_libraries(test_reload_service PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_reload_service)

# Redis-backed config center mirror service
add_executable(test_config_sync_service test_config_sync_service.cpp)
target_link_libraries(test_config_sync_service PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_config_sync_service)

# Immutable config history model, integrity guard and bounded GC
add_executable(test_config_history test_config_history.cpp)
target_link_libraries(test_config_history PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_config_history)

# JSON key 转换
add_executable(test_json_transform test_json_transform.cpp)
target_link_libraries(test_json_transform PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_json_transform)

# MySQL result JSON helpers
add_executable(test_mysql_result_json test_mysql_result_json.cpp)
target_link_libraries(test_mysql_result_json PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_mysql_result_json)

# Proxy response framing
add_executable(test_proxy_framing test_proxy_framing.cpp ${PROJECT_SOURCE_DIR}/picohttpparser.c)
target_link_libraries(test_proxy_framing PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_proxy_framing)

# Proxy request forwarding
add_executable(test_proxy_forwarder test_proxy_forwarder.cpp)
target_link_libraries(test_proxy_forwarder PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_proxy_forwarder)

# HTTP protocol helpers
add_executable(test_http_protocol test_http_protocol.cpp)
target_link_libraries(test_http_protocol PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_http_protocol)

# Reusable downstream deadline state machine
add_executable(test_http_io test_http_io.cpp)
target_link_libraries(test_http_io PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_http_io)

# JSON response envelope (escape rules)
add_executable(test_response test_response.cpp)
target_link_libraries(test_response PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_response)

# HTTP connection pool
add_executable(test_http_pool test_http_pool.cpp)
target_link_libraries(test_http_pool PRIVATE gtest_main asio_owen_core)
target_compile_definitions(test_http_pool PRIVATE ASIO_OWEN_TESTING)
gtest_discover_tests(test_http_pool)

# 安全漏洞修复回归测试 (VULN-1 in_flight 统计, VULN-2 非幂等重试)
add_executable(test_vuln_fixes test_vuln_fixes.cpp ${PROJECT_SOURCE_DIR}/picohttpparser.c)
target_link_libraries(test_vuln_fixes PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_vuln_fixes)

# 配置目录加载
add_executable(test_config_load test_config_load.cpp)
target_link_libraries(test_config_load PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_config_load)

# 安全路径规范化
add_executable(test_path_normalize test_path_normalize.cpp)
target_link_libraries(test_path_normalize PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_path_normalize)

# IP 黑名单 / CIDR
add_executable(test_ip_blacklist test_ip_blacklist.cpp)
target_link_libraries(test_ip_blacklist PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_ip_blacklist)

# JWT 鉴权语义
add_executable(test_jwt_auth test_jwt_auth.cpp)
target_link_libraries(test_jwt_auth PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_jwt_auth)

# MySQL 连接池集成测试（需要真实 MySQL 实例）
add_executable(test_mysql_pool test_mysql_pool.cpp)
target_link_libraries(test_mysql_pool PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_mysql_pool)

# Redis 连接池集成测试（需要真实 Redis 实例）
add_executable(test_redis_pool test_redis_pool.cpp)
target_link_libraries(test_redis_pool PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_redis_pool)

# Rate limiter (token bucket + 32-shard LRU + snapshot)
add_executable(test_rate_limiter test_rate_limiter.cpp)
target_link_libraries(test_rate_limiter PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_rate_limiter)

# /api/combo soft-deadline permit accounting
add_executable(test_combo_query_limiter test_combo_query_limiter.cpp)
target_link_libraries(test_combo_query_limiter PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_combo_query_limiter)

# /api/combo response status and cache-fallback flow without external services
add_executable(test_combo_routes test_combo_routes.cpp ${PROJECT_SOURCE_DIR}/src/app/routes.cpp)
target_link_libraries(test_combo_routes PRIVATE gtest_main asio_owen_core)
add_dependencies(test_combo_routes asio_owen_generated_assets)
gtest_discover_tests(test_combo_routes)

# Config center Admin API and embedded /admin page
add_executable(test_admin_config_routes test_admin_config_routes.cpp ${PROJECT_SOURCE_DIR}/src/app/routes.cpp)
target_link_libraries(test_admin_config_routes PRIVATE gtest_main asio_owen_core)
add_dependencies(test_admin_config_routes asio_owen_generated_assets)
gtest_discover_tests(test_admin_config_routes)

# End-to-end client session via real HttpServer + tcp client
add_executable(test_client_session test_client_session.cpp ${PROJECT_SOURCE_DIR}/picohttpparser.c)
target_link_libraries(test_client_session PRIVATE gtest_main asio_owen_core)
target_compile_definitions(test_client_session PRIVATE ASIO_OWEN_TESTING)
gtest_discover_tests(test_client_session)

# Security chain integration (OPTIONS → real IP → blacklist → rate limit → JWT)
add_executable(test_security_chain test_security_chain.cpp)
target_link_libraries(test_security_chain PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_security_chain)

# Redis Direct mode shutdown semantics
add_executable(test_redis_pool_shutdown test_redis_pool_shutdown.cpp)
target_link_libraries(test_redis_pool_shutdown PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_redis_pool_shutdown)

# CORS policy (resolve_origin, preflight, header injection, config loading)
add_executable(test_cors test_cors.cpp)
target_link_libraries(test_cors PRIVATE gtest_main asio_owen_core)
gtest_discover_tests(test_cors)
