# GoogleTest test targets
include(GoogleTest)

# Placeholder — 验证 GoogleTest harness 正常
add_executable(test_placeholder test_placeholder.cpp)
target_link_libraries(test_placeholder PRIVATE gtest_main asio_owen_deps)
gtest_discover_tests(test_placeholder)

# 上游路由与路径重写
add_executable(test_upstream_manager test_upstream_manager.cpp)
target_link_libraries(test_upstream_manager PRIVATE gtest_main asio_owen_deps)
gtest_discover_tests(test_upstream_manager)

# 配置目录加载
add_executable(test_config_load test_config_load.cpp)
target_link_libraries(test_config_load PRIVATE gtest_main asio_owen_deps)
gtest_discover_tests(test_config_load)

# 安全路径规范化
add_executable(test_path_normalize test_path_normalize.cpp)
target_link_libraries(test_path_normalize PRIVATE gtest_main asio_owen_deps)
gtest_discover_tests(test_path_normalize)

# IP 黑名单 / CIDR
add_executable(test_ip_blacklist test_ip_blacklist.cpp)
target_link_libraries(test_ip_blacklist PRIVATE gtest_main asio_owen_deps)
gtest_discover_tests(test_ip_blacklist)

# JWT 鉴权语义
add_executable(test_jwt_auth test_jwt_auth.cpp)
target_link_libraries(test_jwt_auth PRIVATE gtest_main asio_owen_deps)
gtest_discover_tests(test_jwt_auth)

# MySQL 连接池集成测试（需要真实 MySQL 实例）
add_executable(test_mysql_pool test_mysql_pool.cpp)
target_link_libraries(test_mysql_pool PRIVATE gtest_main asio_owen_deps)
gtest_discover_tests(test_mysql_pool)

# Redis 连接池集成测试（需要真实 Redis 实例）
add_executable(test_redis_pool test_redis_pool.cpp)
target_link_libraries(test_redis_pool PRIVATE gtest_main asio_owen_deps)
gtest_discover_tests(test_redis_pool)
