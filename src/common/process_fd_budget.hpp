#pragma once

#include <cerrno>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <sys/resource.h>

struct ProcessFdBudget {
    uint64_t client_connections = 0;
    uint64_t upstream_connections = 0;
    uint64_t mysql_connections = 0;
    uint64_t redis_connections = 0;
    uint64_t reserve = 0;

    uint64_t total() const {
        uint64_t result = 0;
        for (uint64_t value : {client_connections, upstream_connections,
                               mysql_connections, redis_connections, reserve}) {
            if (value > std::numeric_limits<uint64_t>::max() - result) {
                throw std::overflow_error("process FD budget overflow");
            }
            result += value;
        }
        return result;
    }

    std::string describe() const {
        std::ostringstream out;
        out << "clients=" << client_connections
            << ", upstreams=" << upstream_connections
            << ", mysql=" << mysql_connections
            << ", redis=" << redis_connections
            << ", reserve=" << reserve;
        return out.str();
    }
};

inline std::optional<uint64_t> process_soft_fd_limit() {
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "getrlimit(RLIMIT_NOFILE)");
    }
    if (limit.rlim_cur == RLIM_INFINITY) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(limit.rlim_cur);
}

inline void validate_process_fd_budget(
    const ProcessFdBudget& budget, std::optional<uint64_t> soft_limit) {
    const uint64_t required = budget.total();
    if (!soft_limit || required <= *soft_limit) return;

    throw std::invalid_argument(
        "process FD budget exceeds RLIMIT_NOFILE: required=" +
        std::to_string(required) + ", soft_limit=" +
        std::to_string(*soft_limit) + " (" + budget.describe() + ")");
}

inline void validate_process_fd_budget(const ProcessFdBudget& budget) {
    validate_process_fd_budget(budget, process_soft_fd_limit());
}
