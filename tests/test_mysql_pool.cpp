#include <gtest/gtest.h>
#include <asio.hpp>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <optional>

#include "db/mysql_pool.hpp"
#include "db/redis_pool.hpp"

// Service-free lifecycle/configuration checks live here. The real MySQL and
// Redis pool paths, HTTP routes, wrong-type handling and restart recovery run
// through tests/service_integration.sh when integration tests are enabled.

namespace {

asio::awaitable<void> store_sql_result(
    MysqlPool& pool,
    std::string sql,
    std::optional<MysqlPool::Result>& out) {
    out = co_await pool.execute(sql);
}

asio::awaitable<void> store_empty_redis_argv_reply(
    RedisPool& pool,
    std::optional<RedisPool::Reply>& out) {
    out = co_await pool.cmd_argv({});
}

}  // namespace

TEST(MysqlPoolTest, ExecuteAfterShutdownFailsBeforeAcquire) {
    asio::io_context ioc;
    MysqlPool::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.min_size = 0;
    cfg.max_size = 1;
    cfg.worker_threads = 1;
    cfg.keepalive_sec = 60;
    MysqlPool pool(ioc, cfg);
    pool.shutdown();

    std::optional<MysqlPool::Result> result;
    co_spawn(ioc, store_sql_result(pool, "SELECT 1", result), asio::detached);

    ioc.run();

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->ok);
    EXPECT_EQ(result->error, "mysql pool stopped");

    auto stats = pool.snapshot();
    EXPECT_EQ(stats.query_fail_total, 1u);
    EXPECT_EQ(stats.max_creating, 1u);
}

TEST(MysqlPoolTest, ComputesDefaultMaxCreatingFromPoolAndWorkerSize) {
    asio::io_context ioc;
    MysqlPool::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.min_size = 0;
    cfg.max_size = 64;
    cfg.worker_threads = 4;
    cfg.keepalive_sec = 60;
    MysqlPool pool(ioc, cfg);

    auto stats = pool.snapshot();
    pool.shutdown();

    EXPECT_EQ(stats.max_creating, 2u);
}

TEST(RedisPoolTest, EmptyArgvCommandReturnsErrorBeforeConnect) {
    asio::io_context ioc;
    RedisPool pool(ioc, RedisPool::Config{});

    std::optional<RedisPool::Reply> reply;
    co_spawn(ioc, store_empty_redis_argv_reply(pool, reply), asio::detached);

    ioc.run();
    pool.shutdown();

    ASSERT_TRUE(reply.has_value());
    EXPECT_FALSE(reply->ok);
    EXPECT_EQ(reply->error, "empty Redis command");

    auto stats = pool.snapshot();
    EXPECT_EQ(stats.cmd_fail_total, 1u);
    EXPECT_EQ(stats.cmd_ok_total, 0u);
}
