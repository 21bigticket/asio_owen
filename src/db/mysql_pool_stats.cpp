#include "mysql_pool_stats.hpp"

#include <sstream>

void MysqlPoolStats::inc_query_ok() { ++query_ok_total_; }
void MysqlPoolStats::inc_query_fail() { ++query_fail_total_; }
void MysqlPoolStats::inc_connect_ok() { ++connect_ok_total_; }
void MysqlPoolStats::inc_connect_fail() { ++connect_fail_total_; }
void MysqlPoolStats::inc_reset_conn_fail() { ++reset_conn_fail_total_; }
void MysqlPoolStats::inc_acquire_wait() { ++acquire_wait_total_; }
void MysqlPoolStats::inc_acquire_timeout() { ++acquire_timeout_total_; }
void MysqlPoolStats::inc_acquire_retry_exhausted() { ++acquire_retry_exhausted_total_; }
void MysqlPoolStats::add_idle_recycled(uint64_t n) { idle_recycled_total_ += n; }
void MysqlPoolStats::inc_ping_fail() { ++ping_fail_total_; }

MysqlPoolStatsSnapshot MysqlPoolStats::snapshot(
    size_t total,
    size_t idle,
    size_t creating,
    size_t max_creating) const {
    return MysqlPoolStatsSnapshot{
        .query_ok_total = query_ok_total_.load(std::memory_order_relaxed),
        .query_fail_total = query_fail_total_.load(std::memory_order_relaxed),
        .connect_ok_total = connect_ok_total_.load(std::memory_order_relaxed),
        .connect_fail_total = connect_fail_total_.load(std::memory_order_relaxed),
        .reset_conn_fail_total = reset_conn_fail_total_.load(std::memory_order_relaxed),
        .acquire_wait_total = acquire_wait_total_.load(std::memory_order_relaxed),
        .acquire_timeout_total = acquire_timeout_total_.load(std::memory_order_relaxed),
        .acquire_retry_exhausted_total = acquire_retry_exhausted_total_.load(std::memory_order_relaxed),
        .idle_recycled_total = idle_recycled_total_.load(std::memory_order_relaxed),
        .ping_fail_total = ping_fail_total_.load(std::memory_order_relaxed),
        .total = total,
        .idle = idle,
        .creating = creating,
        .max_creating = max_creating
    };
}

std::string format_mysql_pool_stats(const MysqlPoolStatsSnapshot& s) {
    std::ostringstream oss;
    oss << "query_ok_total=" << s.query_ok_total
        << ", query_fail_total=" << s.query_fail_total
        << ", connect_ok_total=" << s.connect_ok_total
        << ", connect_fail_total=" << s.connect_fail_total
        << ", reset_conn_fail_total=" << s.reset_conn_fail_total
        << ", acquire_wait_total=" << s.acquire_wait_total
        << ", acquire_timeout_total=" << s.acquire_timeout_total
        << ", acquire_retry_exhausted_total=" << s.acquire_retry_exhausted_total
        << ", idle_recycled_total=" << s.idle_recycled_total
        << ", ping_fail_total=" << s.ping_fail_total
        << ", total=" << s.total
        << ", idle=" << s.idle
        << ", creating=" << s.creating
        << ", max_creating=" << s.max_creating;
    return oss.str();
}
