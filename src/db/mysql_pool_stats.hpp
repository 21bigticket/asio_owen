#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

struct MysqlPoolStatsSnapshot {
    uint64_t query_ok_total = 0;
    uint64_t query_fail_total = 0;
    uint64_t connect_ok_total = 0;
    uint64_t connect_fail_total = 0;
    uint64_t reset_conn_fail_total = 0;
    uint64_t acquire_wait_total = 0;
    uint64_t acquire_timeout_total = 0;
    uint64_t acquire_retry_exhausted_total = 0;
    uint64_t idle_recycled_total = 0;
    uint64_t ping_fail_total = 0;
    size_t total = 0;
    size_t idle = 0;
    size_t creating = 0;
    size_t max_creating = 0;
};

class MysqlPoolStats {
public:
    void inc_query_ok();
    void inc_query_fail();
    void inc_connect_ok();
    void inc_connect_fail();
    void inc_reset_conn_fail();
    void inc_acquire_wait();
    void inc_acquire_timeout();
    void inc_acquire_retry_exhausted();
    void add_idle_recycled(uint64_t n);
    void inc_ping_fail();

    MysqlPoolStatsSnapshot snapshot(
        size_t total,
        size_t idle,
        size_t creating,
        size_t max_creating) const;

private:
    std::atomic<uint64_t> query_ok_total_{0};
    std::atomic<uint64_t> query_fail_total_{0};
    std::atomic<uint64_t> connect_ok_total_{0};
    std::atomic<uint64_t> connect_fail_total_{0};
    std::atomic<uint64_t> reset_conn_fail_total_{0};
    std::atomic<uint64_t> acquire_wait_total_{0};
    std::atomic<uint64_t> acquire_timeout_total_{0};
    std::atomic<uint64_t> acquire_retry_exhausted_total_{0};
    std::atomic<uint64_t> idle_recycled_total_{0};
    std::atomic<uint64_t> ping_fail_total_{0};
};

std::string format_mysql_pool_stats(const MysqlPoolStatsSnapshot& s);
