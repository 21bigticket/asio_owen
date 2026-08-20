#include "../src/app/route_runtime.hpp"

#include <gtest/gtest.h>

TEST(RouteRuntime, HandlerLeaseTracksPhaseAndDraining) {
    auto runtime = std::make_shared<RouteRuntime>();
    auto lease = runtime->enter_handler();
    ASSERT_TRUE(static_cast<bool>(lease));
    ASSERT_EQ(runtime->active_handlers(), 1u);
    EXPECT_TRUE(runtime->begin_draining());
    EXPECT_FALSE(runtime->begin_draining());
    auto rejected = runtime->enter_handler();
    EXPECT_FALSE(static_cast<bool>(rejected));
    EXPECT_EQ(runtime->active_handlers(), 1u);
    lease.release();
    EXPECT_EQ(runtime->active_handlers(), 0u);
    runtime->mark_stopped();
    EXPECT_EQ(runtime->phase(), RouteRuntime::Phase::Stopped);
}
