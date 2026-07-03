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

# MySQL 连接池集成测试（需要真实 MySQL 实例）
add_executable(test_mysql_pool test_mysql_pool.cpp)
target_link_libraries(test_mysql_pool PRIVATE gtest_main asio_owen_deps)
gtest_discover_tests(test_mysql_pool)

# Redis 连接池集成测试（需要真实 Redis 实例）
add_executable(test_redis_pool test_redis_pool.cpp)
target_link_libraries(test_redis_pool PRIVATE gtest_main asio_owen_deps)
gtest_discover_tests(test_redis_pool)
