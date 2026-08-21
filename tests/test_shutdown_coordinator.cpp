#include <gtest/gtest.h>

#include <asio/co_spawn.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <memory>

#include "common/shutdown_coordinator.hpp"

TEST(ShutdownCoordinator, TracksSessionsAndTransitionsOnce) {
    auto coordinator = std::make_shared<ShutdownCoordinator>();
    auto lease = coordinator->enter_session();

    EXPECT_EQ(coordinator->active_sessions(), 1u);
    EXPECT_TRUE(coordinator->begin_draining());
    EXPECT_FALSE(coordinator->begin_draining());
    EXPECT_TRUE(coordinator->draining());
    EXPECT_EQ(coordinator->phase(), ShutdownCoordinator::Phase::Draining);

    lease = {};
    EXPECT_EQ(coordinator->active_sessions(), 0u);
    coordinator->mark_stopped();
    EXPECT_EQ(coordinator->phase(), ShutdownCoordinator::Phase::Stopped);
}

TEST(ShutdownCoordinator, DrainWaitIsBoundedAndCompletesAfterLeaseRelease) {
    auto coordinator = std::make_shared<ShutdownCoordinator>();
    auto lease = coordinator->enter_session();
    asio::io_context ioc;
    bool drained = true;
    asio::co_spawn(ioc, coordinator->wait_for_drain(std::chrono::milliseconds(2)),
        [&](std::exception_ptr error, bool result) {
            ASSERT_FALSE(error);
            drained = result;
        });
    ioc.run();
    EXPECT_FALSE(drained);

    lease = {};
    asio::io_context completed_ioc;
    drained = false;
    asio::co_spawn(completed_ioc,
        coordinator->wait_for_drain(std::chrono::milliseconds(20)),
        [&](std::exception_ptr error, bool result) {
            ASSERT_FALSE(error);
            drained = result;
        });
    completed_ioc.run();
    EXPECT_TRUE(drained);
}

TEST(ShutdownCoordinator, EnforcesSessionCapacityAndCountsRejections) {
    auto coordinator = std::make_shared<ShutdownCoordinator>();
    auto first = coordinator->try_enter_session(1);
    auto rejected = coordinator->try_enter_session(1);

    EXPECT_TRUE(first);
    EXPECT_FALSE(rejected);
    EXPECT_EQ(coordinator->active_sessions(), 1u);
    EXPECT_EQ(coordinator->capacity_rejections(), 1u);

    first = {};
    auto next = coordinator->try_enter_session(1);
    EXPECT_TRUE(next);
    EXPECT_EQ(coordinator->active_sessions(), 1u);
}
