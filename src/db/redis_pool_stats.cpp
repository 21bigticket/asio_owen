#include "redis_pool_stats.hpp"

#include <sstream>

void RedisPoolStats::inc_reconnect() { ++reconnect_total_; }
void RedisPoolStats::inc_cmd_ok() { ++cmd_ok_total_; }
void RedisPoolStats::inc_cmd_fail() { ++cmd_fail_total_; }
void RedisPoolStats::inc_nil() { ++nil_total_; }
void RedisPoolStats::inc_timeout() { ++timeout_total_; }
void RedisPoolStats::inc_connect_ok() { ++connect_ok_total_; }
void RedisPoolStats::inc_connect_fail() { ++connect_fail_total_; }
void RedisPoolStats::inc_acquire_wait() { ++acquire_wait_total_; }
void RedisPoolStats::inc_acquire_timeout() { ++acquire_timeout_total_; }
void RedisPoolStats::inc_acquire_retry_exhausted() { ++acquire_retry_exhausted_total_; }
void RedisPoolStats::add_idle_recycled(uint64_t n) { idle_recycled_total_ += n; }
void RedisPoolStats::inc_ping_fail() { ++ping_fail_total_; }

RedisPoolStatsSnapshot RedisPoolStats::base_snapshot(size_t created_total) const {
    return RedisPoolStatsSnapshot{
        .created_total = created_total,
        .reconnect_total = reconnect_total_.load(std::memory_order_relaxed),
        .cmd_ok_total = cmd_ok_total_.load(std::memory_order_relaxed),
        .cmd_fail_total = cmd_fail_total_.load(std::memory_order_relaxed),
        .nil_total = nil_total_.load(std::memory_order_relaxed),
        .timeout_total = timeout_total_.load(std::memory_order_relaxed),
        .connect_ok_total = connect_ok_total_.load(std::memory_order_relaxed),
        .connect_fail_total = connect_fail_total_.load(std::memory_order_relaxed),
        .acquire_wait_total = acquire_wait_total_.load(std::memory_order_relaxed),
        .acquire_timeout_total = acquire_timeout_total_.load(std::memory_order_relaxed),
        .acquire_retry_exhausted_total = acquire_retry_exhausted_total_.load(std::memory_order_relaxed),
        .idle_recycled_total = idle_recycled_total_.load(std::memory_order_relaxed),
        .ping_fail_total = ping_fail_total_.load(std::memory_order_relaxed)
    };
}

RedisPoolStatsSnapshot RedisPoolStats::snapshot(
    size_t created_total,
    size_t total_conn,
    size_t idle_conn,
    size_t creating_conn,
    size_t max_creating) const {
    auto s = base_snapshot(created_total);
    s.total_conn = total_conn;
    s.idle_conn = idle_conn;
    s.creating_conn = creating_conn;
    s.max_creating = max_creating;
    return s;
}

std::string format_redis_pool_stats(const RedisPoolStatsSnapshot& s) {
    std::ostringstream oss;
    oss << "created_total=" << s.created_total
        << ", reconnect_total=" << s.reconnect_total
        << ", cmd_ok_total=" << s.cmd_ok_total
        << ", cmd_fail_total=" << s.cmd_fail_total
        << ", nil_total=" << s.nil_total
        << ", timeout_total=" << s.timeout_total
        << ", connect_ok_total=" << s.connect_ok_total
        << ", connect_fail_total=" << s.connect_fail_total
        << ", acquire_wait_total=" << s.acquire_wait_total
        << ", acquire_timeout_total=" << s.acquire_timeout_total
        << ", acquire_retry_exhausted_total=" << s.acquire_retry_exhausted_total
        << ", idle_recycled_total=" << s.idle_recycled_total
        << ", ping_fail_total=" << s.ping_fail_total
        << ", total_conn=" << s.total_conn
        << ", idle_conn=" << s.idle_conn
        << ", creating_conn=" << s.creating_conn
        << ", max_creating=" << s.max_creating;
    return oss.str();
}
