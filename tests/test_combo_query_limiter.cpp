#include <gtest/gtest.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>

#include <array>
#include <chrono>

#include "app/combo_deadline.hpp"
#include "app/combo_query_limiter.hpp"

namespace {

std::array<std::optional<ComboQueryPermit>, 7>
fill_remaining_permits(const ComboQueryLimiter& limiter) {
    std::array<std::optional<ComboQueryPermit>, 7> permits;
    for (auto& permit : permits) {
        permit = limiter.try_acquire();
        EXPECT_TRUE(permit.has_value());
    }
    return permits;
}

asio::awaitable<int> delayed_value(int value, std::chrono::milliseconds delay) {
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer timer(ex);
    timer.expires_after(delay);
    co_await timer.async_wait(asio::use_awaitable);
    co_return value;
}

asio::awaitable<void> run_delayed_deadline(
    std::optional<int>& result, ComboQueryPermit permit,
    std::chrono::milliseconds query_delay, std::chrono::milliseconds deadline) {
    result = co_await await_combo_query_with_deadline(
        delayed_value(42, query_delay), std::move(permit), deadline,
        [](std::exception_ptr) { return -1; });
}

}  // namespace

TEST(ComboQueryLimiter, QueryWinnerReleasesPermitOnlyOnce) {
    ComboQueryLimiter limiter(8);
    auto permit = limiter.try_acquire();
    ASSERT_TRUE(permit.has_value());
    ComboQueryState state(std::move(*permit));
    auto saturated = fill_remaining_permits(limiter);
    EXPECT_FALSE(limiter.try_acquire().has_value());

    EXPECT_TRUE(state.claim_query());
    EXPECT_FALSE(state.claim_timeout());
    EXPECT_TRUE(limiter.try_acquire().has_value());
}

TEST(ComboQueryLimiter, TimeoutWinnerHoldsPermitUntilQueryCompletes) {
    ComboQueryLimiter limiter(8);
    auto permit = limiter.try_acquire();
    ASSERT_TRUE(permit.has_value());
    ComboQueryState state(std::move(*permit));
    auto saturated = fill_remaining_permits(limiter);
    EXPECT_FALSE(limiter.try_acquire().has_value());

    EXPECT_TRUE(state.claim_timeout());
    EXPECT_FALSE(limiter.try_acquire().has_value());
    EXPECT_FALSE(state.claim_query());
    EXPECT_TRUE(limiter.try_acquire().has_value());
}

TEST(ComboQueryDeadline, ReturnsTimeoutButReleasesPermitAfterDelayedQueryCompletes) {
    asio::io_context ioc;
    ComboQueryLimiter limiter(1);
    auto permit = limiter.try_acquire();
    ASSERT_TRUE(permit.has_value());
    std::optional<int> result;

    asio::co_spawn(ioc, run_delayed_deadline(
        result, std::move(*permit), std::chrono::milliseconds(30), std::chrono::milliseconds(1)),
        asio::detached);
    ioc.run();

    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(limiter.try_acquire().has_value());
}

TEST(ComboQueryDeadline, ReturnsQueryResultBeforeDeadline) {
    asio::io_context ioc;
    ComboQueryLimiter limiter(1);
    auto permit = limiter.try_acquire();
    ASSERT_TRUE(permit.has_value());
    std::optional<int> result;

    asio::co_spawn(ioc, run_delayed_deadline(
        result, std::move(*permit), std::chrono::milliseconds(1), std::chrono::milliseconds(30)),
        asio::detached);
    ioc.run();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
    EXPECT_TRUE(limiter.try_acquire().has_value());
}
