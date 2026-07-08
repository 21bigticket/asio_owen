#include "redis_pool_stats.hpp"

#include <sstream>

void RedisPoolStats::inc_reconnect() { ++reconnect_total_; }
void RedisPoolStats::inc_cmd_ok() { ++cmd_ok_total_; }
void RedisPoolStats::inc_cmd_fail() { ++cmd_fail_total_; }
void RedisPoolStats::inc_nil() { ++nil_total_; }
void RedisPoolStats::inc_timeout() { ++timeout_total_; }

RedisPoolStatsSnapshot RedisPoolStats::snapshot(size_t created_total) const {
    return RedisPoolStatsSnapshot{
        .created_total = created_total,
        .reconnect_total = reconnect_total_.load(std::memory_order_relaxed),
        .cmd_ok_total = cmd_ok_total_.load(std::memory_order_relaxed),
        .cmd_fail_total = cmd_fail_total_.load(std::memory_order_relaxed),
        .nil_total = nil_total_.load(std::memory_order_relaxed),
        .timeout_total = timeout_total_.load(std::memory_order_relaxed)
    };
}

std::string format_redis_pool_stats(const RedisPoolStatsSnapshot& s) {
    std::ostringstream oss;
    oss << "created_total=" << s.created_total
        << ", reconnect_total=" << s.reconnect_total
        << ", cmd_ok_total=" << s.cmd_ok_total
        << ", cmd_fail_total=" << s.cmd_fail_total
        << ", nil_total=" << s.nil_total
        << ", timeout_total=" << s.timeout_total;
    return oss.str();
}
