#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "common/process_fd_budget.hpp"

TEST(ProcessFdBudget, SumsAllConnectionClassesAndReserve) {
    const ProcessFdBudget budget{
        .client_connections = 100,
        .upstream_connections = 200,
        .mysql_connections = 10,
        .redis_connections = 5,
        .reserve = 64
    };

    EXPECT_EQ(budget.total(), 379u);
    EXPECT_NO_THROW(validate_process_fd_budget(budget, 379));
    EXPECT_THROW(validate_process_fd_budget(budget, 378), std::invalid_argument);
}

TEST(ProcessFdBudget, RejectsArithmeticOverflow) {
    const ProcessFdBudget budget{
        .client_connections = std::numeric_limits<uint64_t>::max(),
        .upstream_connections = 1
    };

    EXPECT_THROW(budget.total(), std::overflow_error);
}
