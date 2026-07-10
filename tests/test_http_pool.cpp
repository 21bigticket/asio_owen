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

asio::awaitable<void> acquire_closed_port(
    HttpPool& pool,
    int closed_port,
    std::atomic<bool>& saw_failure) {
    try {
        auto conn = co_await pool.acquire("127.0.0.1", closed_port);
        (void)conn;
    } catch (...) {
        saw_failure.store(true, std::memory_order_relaxed);
    }
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

}  // namespace

TEST(HttpPool, FailedConnectAfterShardSkipCleansActualReservedShard) {
    asio::io_context ioc;
    int closed_port = 1;

    HttpPool::Config cfg;
    cfg.max_size = HttpPool::kShards;
    cfg.connect_timeout_ms = 100;
    HttpPool pool(ioc, cfg);
    auto state = pool.state();

    size_t last = HttpPool::State::pick_shard();
    size_t start_shard = (last + 1) % HttpPool::kShards;
    size_t reserved_shard = (start_shard + 1) % HttpPool::kShards;
    for (size_t i = 0; i < HttpPool::kShards; ++i) {
        auto& shard = state->shards[i];
        std::lock_guard lock(shard.mtx);
        shard.total = (i == reserved_shard) ? 0 : 1;
        shard.in_flight = 0;
        shard.active.clear();
        shard.idle.clear();
    }

    std::atomic<bool> saw_failure{false};
    co_spawn(ioc, acquire_closed_port(pool, closed_port, saw_failure), asio::detached);

    ioc.run();

    EXPECT_TRUE(saw_failure.load(std::memory_order_relaxed));
    {
        auto& shard = state->shards[start_shard];
        std::lock_guard lock(shard.mtx);
        EXPECT_EQ(shard.total, 1u);
        EXPECT_EQ(shard.in_flight, 0u);
        EXPECT_TRUE(shard.active.empty());
    }
    {
        auto& shard = state->shards[reserved_shard];
        std::lock_guard lock(shard.mtx);
        EXPECT_EQ(shard.total, 0u);
        EXPECT_EQ(shard.in_flight, 0u);
        EXPECT_TRUE(shard.active.empty());
    }

    for (auto& shard : state->shards) {
        std::lock_guard lock(shard.mtx);
        shard.total = 0;
        shard.in_flight = 0;
        shard.active.clear();
        shard.idle.clear();
    }
}

TEST(HttpPool, ReusesIdleConnectionWhenProbeWouldBlock) {
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
    EXPECT_EQ(state->idle_probe_dropped.load(std::memory_order_relaxed), 0u);

    asio::error_code ec;
    if (held_socket->has_value()) {
        (*held_socket)->close(ec);
    }
}
