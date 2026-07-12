#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>

#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>

#include "http/http_pool.hpp"

namespace {

using tcp = asio::ip::tcp;

asio::awaitable<void> acquire_closed_port_expect_failure_and_clean_counters(
    HttpPool& pool,
    int closed_port,
    std::atomic<bool>& saw_failure,
    std::atomic<bool>& counters_clean) {
    try {
        auto conn = co_await pool.acquire("127.0.0.1", closed_port);
        (void)conn;
    } catch (...) {
        saw_failure.store(true, std::memory_order_relaxed);
    }

    auto state = pool.state();
    counters_clean.store(
        state->total_count.load(std::memory_order_relaxed) == 0 &&
        state->in_flight_count.load(std::memory_order_relaxed) == 0,
        std::memory_order_relaxed);
}

asio::awaitable<void> accept_one_and_hold(
    tcp::acceptor& acceptor,
    std::shared_ptr<std::optional<tcp::socket>> held_socket) {
    auto socket = co_await acceptor.async_accept(asio::use_awaitable);
    held_socket->emplace(std::move(socket));
}

asio::awaitable<void> acquire_release_acquire(
    HttpPool& pool,
    int port,
    std::atomic<bool>& reused) {
    auto first = co_await pool.acquire("127.0.0.1", port);
    if (!first) co_return;
    HttpPool::release(pool.state(), std::move(first));
    for (size_t i = 0; i < HttpPool::kShards - 1; ++i) {
        HttpPool::State::pick_shard();
    }

    auto second = co_await pool.acquire("127.0.0.1", port);
    if (!second) co_return;
    reused.store(second->reused_from_idle, std::memory_order_relaxed);
    HttpPool::release_bad(pool.state(), std::move(second));
}

asio::awaitable<void> acquire_twice_expect_second_null(
    HttpPool& pool,
    int port,
    std::atomic<bool>& second_was_null) {
    auto first = co_await pool.acquire("127.0.0.1", port);
    if (!first) co_return;

    auto second = co_await pool.acquire("127.0.0.1", port);
    second_was_null.store(second == nullptr, std::memory_order_relaxed);

    if (second) {
        HttpPool::release_bad(pool.state(), std::move(second));
    }
    HttpPool::release_bad(pool.state(), std::move(first));
}

}  // namespace

TEST(HttpPool, FailedConnectCleansGlobalAndShardCounters) {
    asio::io_context ioc;
    int closed_port = 1;

    HttpPool::Config cfg;
    cfg.max_size = 1;
    cfg.connect_timeout_ms = 100;
    HttpPool pool(ioc, cfg);
    auto state = pool.state();

    std::atomic<bool> saw_failure{false};
    std::atomic<bool> counters_clean{false};
    co_spawn(ioc, acquire_closed_port_expect_failure_and_clean_counters(
        pool, closed_port, saw_failure, counters_clean), asio::detached);

    ioc.run();

    EXPECT_TRUE(saw_failure.load(std::memory_order_relaxed));
    EXPECT_TRUE(counters_clean.load(std::memory_order_relaxed));
    EXPECT_EQ(state->total_count.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state->in_flight_count.load(std::memory_order_relaxed), 0u);

    for (auto& shard : state->shards) {
        std::lock_guard lock(shard.mtx);
        shard.total = 0;
        shard.in_flight = 0;
        shard.active.clear();
        shard.idle.clear();
    }
}

TEST(HttpPool, MaxSizeIsGlobalHardLimit) {
    asio::io_context ioc;
    tcp::acceptor acceptor(ioc, {tcp::v4(), 0});
    auto held_socket = std::make_shared<std::optional<tcp::socket>>();

    HttpPool::Config cfg;
    cfg.max_size = 1;
    cfg.connect_timeout_ms = 1000;
    HttpPool pool(ioc, cfg);

    std::atomic<bool> second_was_null{false};
    co_spawn(ioc, accept_one_and_hold(acceptor, held_socket), asio::detached);
    co_spawn(ioc, acquire_twice_expect_second_null(
        pool, acceptor.local_endpoint().port(), second_was_null), asio::detached);

    ioc.run();

    EXPECT_TRUE(second_was_null.load(std::memory_order_relaxed));
    EXPECT_EQ(pool.state()->total_count.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(pool.state()->in_flight_count.load(std::memory_order_relaxed), 0u);

    asio::error_code ec;
    if (held_socket->has_value()) {
        (*held_socket)->close(ec);
    }
}

TEST(HttpPool, MaxConcurrentIsGlobalHardLimit) {
    asio::io_context ioc;
    tcp::acceptor acceptor(ioc, {tcp::v4(), 0});
    auto held_socket = std::make_shared<std::optional<tcp::socket>>();

    HttpPool::Config cfg;
    cfg.max_size = 2;
    cfg.max_concurrent = 1;
    cfg.connect_timeout_ms = 1000;
    HttpPool pool(ioc, cfg);

    std::atomic<bool> second_was_null{false};
    co_spawn(ioc, accept_one_and_hold(acceptor, held_socket), asio::detached);
    co_spawn(ioc, acquire_twice_expect_second_null(
        pool, acceptor.local_endpoint().port(), second_was_null), asio::detached);

    ioc.run();

    EXPECT_TRUE(second_was_null.load(std::memory_order_relaxed));
    EXPECT_EQ(pool.state()->total_count.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(pool.state()->in_flight_count.load(std::memory_order_relaxed), 0u);

    asio::error_code ec;
    if (held_socket->has_value()) {
        (*held_socket)->close(ec);
    }
}

TEST(HttpPool, ReusesIdleConnectionWhenHealthy) {
    asio::io_context ioc;
    tcp::acceptor acceptor(ioc, {tcp::v4(), 0});
    auto held_socket = std::make_shared<std::optional<tcp::socket>>();

    HttpPool::Config cfg;
    cfg.max_size = HttpPool::kShards;
    cfg.connect_timeout_ms = 1000;
    HttpPool pool(ioc, cfg);
    auto state = pool.state();

    std::atomic<bool> reused{false};
    co_spawn(ioc, accept_one_and_hold(acceptor, held_socket), asio::detached);
    co_spawn(ioc, acquire_release_acquire(
        pool, acceptor.local_endpoint().port(), reused), asio::detached);

    ioc.run();

    EXPECT_TRUE(reused.load(std::memory_order_relaxed));
    EXPECT_EQ(state->acquire_reused.load(std::memory_order_relaxed), 1u);

    asio::error_code ec;
    if (held_socket->has_value()) {
        (*held_socket)->close(ec);
    }
}

asio::awaitable<void> accept_two_and_hold(
    tcp::acceptor& acceptor,
    std::shared_ptr<std::optional<tcp::socket>> held1,
    std::shared_ptr<std::optional<tcp::socket>> held2) {
    auto s1 = co_await acceptor.async_accept(asio::use_awaitable);
    held1->emplace(std::move(s1));
    auto s2 = co_await acceptor.async_accept(asio::use_awaitable);
    held2->emplace(std::move(s2));
}

asio::awaitable<void> acquire_dirty_release_acquire(
    HttpPool& pool,
    int port,
    std::atomic<bool>& first_reused,
    std::atomic<bool>& second_reused) {
    auto first = co_await pool.acquire("127.0.0.1", port);
    if (!first) co_return;
    first_reused.store(first->reused_from_idle, std::memory_order_relaxed);
    first->read_buffer.push_back('x');   // simulate protocol desync residual
    HttpPool::release(pool.state(), std::move(first));

    for (size_t i = 0; i < HttpPool::kShards - 1; ++i) {
        HttpPool::State::pick_shard();
    }

    auto second = co_await pool.acquire("127.0.0.1", port);
    if (!second) co_return;
    second_reused.store(second->reused_from_idle, std::memory_order_relaxed);
    HttpPool::release_bad(pool.state(), std::move(second));
}

TEST(HttpPool, DropsIdleWithResidualReadBuffer) {
    asio::io_context ioc;
    tcp::acceptor acceptor(ioc, {tcp::v4(), 0});
    auto held1 = std::make_shared<std::optional<tcp::socket>>();
    auto held2 = std::make_shared<std::optional<tcp::socket>>();

    HttpPool::Config cfg;
    cfg.max_size = HttpPool::kShards;
    cfg.connect_timeout_ms = 1000;
    HttpPool pool(ioc, cfg);
    auto state = pool.state();

    std::atomic<bool> first_reused{true};
    std::atomic<bool> second_reused{true};
    co_spawn(ioc, accept_two_and_hold(acceptor, held1, held2), asio::detached);
    co_spawn(ioc, acquire_dirty_release_acquire(
        pool, acceptor.local_endpoint().port(), first_reused, second_reused),
        asio::detached);

    ioc.run();

    EXPECT_FALSE(first_reused.load(std::memory_order_relaxed));
    EXPECT_FALSE(second_reused.load(std::memory_order_relaxed));
    EXPECT_EQ(state->acquire_reused.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state->acquire_created.load(std::memory_order_relaxed), 2u);
    EXPECT_EQ(state->released_closed.load(std::memory_order_relaxed), 1u);

    asio::error_code ec;
    if (held1->has_value()) (*held1)->close(ec);
    if (held2->has_value()) (*held2)->close(ec);
}

asio::awaitable<void> acquire_backdate_release_acquire(
    HttpPool& pool,
    std::shared_ptr<HttpPool::State> state,
    int port) {
    auto conn = co_await pool.acquire("127.0.0.1", port);
    if (!conn) co_return;
    auto shard_idx = conn->shard_idx;
    HttpPool::release(state, std::move(conn));

    {
        auto& shard = state->shards[shard_idx];
        std::lock_guard lock(shard.mtx);
        for (auto& idle_conn : shard.idle) {
            idle_conn.last_used_at = std::chrono::steady_clock::time_point{};
        }
    }
    state->last_global_evict_ms.store(0, std::memory_order_relaxed);

    auto second = co_await pool.acquire("127.0.0.1", port);
    if (!second) co_return;
    // global sweep should have evicted the stale idle conn before reuse,
    // forcing a fresh connect (reused_from_idle == false).
    EXPECT_FALSE(second->reused_from_idle);
    HttpPool::release_bad(state, std::move(second));
}

TEST(HttpPool, ThrottledGlobalEvictSweepsStaleIdleFromShards) {
    // Regression for P2.3: a throttled global eviction sweep at acquire()
    // entry should reclaim idle conns from all shards, not just the calling shard.
    asio::io_context ioc;
    tcp::acceptor acceptor(ioc, {tcp::v4(), 0});
    auto held_socket = std::make_shared<std::optional<tcp::socket>>();

    HttpPool::Config cfg;
    cfg.max_size = HttpPool::kShards;
    cfg.connect_timeout_ms = 1000;
    cfg.idle_timeout_sec = 0;
    HttpPool pool(ioc, cfg);
    auto state = pool.state();

    co_spawn(ioc, accept_one_and_hold(acceptor, held_socket), asio::detached);
    co_spawn(ioc, acquire_backdate_release_acquire(
        pool, state, acceptor.local_endpoint().port()), asio::detached);

    ioc.run();

    EXPECT_EQ(state->acquire_created.load(std::memory_order_relaxed), 2u);
    EXPECT_EQ(state->acquire_reused.load(std::memory_order_relaxed), 0u);

    asio::error_code ec;
    if (held_socket->has_value()) (*held_socket)->close(ec);
}
