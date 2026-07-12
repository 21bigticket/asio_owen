#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <asio.hpp>

#include "db/redis_pool.hpp"

// These tests document the shutdown contract of RedisPool in Direct mode:
//   1. shutdown() flips running_ to false exactly once (idempotent on re-entry).
//   2. After shutdown, command entry points (cmd_argv_sync) short-circuit with
//      an "redis pool is shutdown" error so other threads holding a stale TLS
//      slot cannot re-enter hiredis on the dying pool.
//   3. The pool's destructor is safe to call after shutdown().
//
// What this test does NOT verify (and intentionally cannot, without a thread
// registry): that other threads' TLS connections are synchronously closed by
// shutdown(). Those connections are released when their owning threads exit or
// when a later pool generation resets the TLS slot. This is a documented
// design trade-off for the io_context-pool model — see redis_pool.hpp
// shutdown() comment.

TEST(RedisPoolShutdown, CommandsFailAfterShutdown) {
    asio::io_context ioc;
    RedisPool::Config cfg;
    cfg.mode = RedisPool::Mode::Direct;
    cfg.host = "127.0.0.1";
    cfg.port = 1;  // closed port; we never actually issue a command pre-shutdown
    cfg.connect_timeout_ms = 100;

    RedisPool pool(ioc, cfg);
    pool.shutdown();

    auto reply = pool.cmd_argv_sync({"PING"});
    EXPECT_FALSE(reply.ok);
    EXPECT_NE(reply.error.find("shutdown"), std::string::npos) << reply.error;
}

TEST(RedisPoolShutdown, ShutdownIsIdempotent) {
    asio::io_context ioc;
    RedisPool::Config cfg;
    cfg.mode = RedisPool::Mode::Direct;
    cfg.connect_timeout_ms = 100;

    RedisPool pool(ioc, cfg);
    pool.shutdown();
    // Second call must be safe (exchange returns false, no work done).
    pool.shutdown();
    // And a third for good measure.
    pool.shutdown();
}

TEST(RedisPoolShutdown, DestructorSafeAfterExplicitShutdown) {
    asio::io_context ioc;
    RedisPool::Config cfg;
    cfg.mode = RedisPool::Mode::Direct;
    cfg.connect_timeout_ms = 100;

    auto* pool = new RedisPool(ioc, cfg);
    pool->shutdown();
    // Destructor must not double-close anything.
    delete pool;
}

TEST(RedisPoolShutdown, DestructorCallsShutdownIfNotAlreadyCalled) {
    asio::io_context ioc;
    RedisPool::Config cfg;
    cfg.mode = RedisPool::Mode::Direct;
    cfg.connect_timeout_ms = 100;

    // No explicit shutdown — destructor must handle it.
    auto* pool = new RedisPool(ioc, cfg);
    delete pool;  // should not crash
}
