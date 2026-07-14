#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <optional>

#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>

#include "http/client_session.hpp"
#include "http/http_pool.hpp"

namespace {

using tcp = asio::ip::tcp;

// Accept one connection and hold it open so acquire() succeeds and the
// connection stays live (in_flight) until we release it.
asio::awaitable<void> accept_and_hold(
    tcp::acceptor& acceptor,
    std::shared_ptr<std::optional<tcp::socket>> held) {
    auto socket = co_await acceptor.async_accept(asio::use_awaitable);
    held->emplace(std::move(socket));
}

// VULN-1: acquire while holding, assert in_flight_count reflects the live
// connection even though max_concurrent == 0 (throttling disabled).
asio::awaitable<void> acquire_hold_check_release(
    HttpPool& pool,
    int port,
    std::atomic<size_t>& in_flight_while_held,
    std::atomic<size_t>& in_flight_after_release) {
    auto conn = co_await pool.acquire("127.0.0.1", port);
    if (!conn) co_return;

    in_flight_while_held.store(
        pool.state()->in_flight_count.load(std::memory_order_relaxed),
        std::memory_order_relaxed);

    HttpPool::release(pool.state(), std::move(conn));

    in_flight_after_release.store(
        pool.state()->in_flight_count.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
}

}  // namespace

// VULN-1 fix: in_flight_count must be maintained even when max_concurrent == 0,
// otherwise the monitoring stat is stuck at 0 in the default configuration.
TEST(VulnFixes, VULN1_InFlightCountTrackedWithoutThrottling) {
    asio::io_context ioc;

    tcp::acceptor acceptor(ioc, tcp::endpoint(tcp::v4(), 0));
    int port = acceptor.local_endpoint().port();
    auto held = std::make_shared<std::optional<tcp::socket>>();
    asio::co_spawn(ioc, accept_and_hold(acceptor, held), asio::detached);

    HttpPool::Config cfg;
    cfg.max_concurrent = 0;  // throttling disabled (default)
    cfg.max_size = 10;
    HttpPool pool(ioc, cfg);

    std::atomic<size_t> in_flight_while_held{999};
    std::atomic<size_t> in_flight_after_release{999};
    asio::co_spawn(
        ioc,
        acquire_hold_check_release(
            pool, port, in_flight_while_held, in_flight_after_release),
        asio::detached);

    ioc.run();

    EXPECT_EQ(in_flight_while_held.load(), 1u)
        << "in_flight_count must be 1 while a connection is held, "
           "even with max_concurrent=0";
    EXPECT_EQ(in_flight_after_release.load(), 0u)
        << "in_flight_count must return to 0 after release";
}

// VULN-2 fix: PUT and DELETE must NOT be treated as auto-retryable. They are
// theoretically idempotent per RFC but commonly have side effects, so replaying
// them on a stale-idle connection risks double-submit.
TEST(VulnFixes, VULN2_NonReplayableMethods) {
    EXPECT_FALSE(ClientSession::is_idempotent_method("POST"));
    EXPECT_FALSE(ClientSession::is_idempotent_method("PATCH"));
    EXPECT_FALSE(ClientSession::is_idempotent_method("PUT"));
    EXPECT_FALSE(ClientSession::is_idempotent_method("DELETE"));
}

// VULN-2 fix: safe methods remain retryable.
TEST(VulnFixes, VULN2_ReplayableMethods) {
    EXPECT_TRUE(ClientSession::is_idempotent_method("GET"));
    EXPECT_TRUE(ClientSession::is_idempotent_method("HEAD"));
    EXPECT_TRUE(ClientSession::is_idempotent_method("OPTIONS"));
    EXPECT_TRUE(ClientSession::is_idempotent_method("TRACE"));
}
