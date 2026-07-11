#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

struct RedisPoolStatsSnapshot {
    uint64_t created_total = 0;
    uint64_t reconnect_total = 0;
    uint64_t cmd_ok_total = 0;
    uint64_t cmd_fail_total = 0;
    uint64_t nil_total = 0;
    uint64_t timeout_total = 0;
    uint64_t connect_ok_total = 0;
    uint64_t connect_fail_total = 0;
    uint64_t acquire_wait_total = 0;
    uint64_t acquire_timeout_total = 0;
    uint64_t acquire_retry_exhausted_total = 0;
    uint64_t idle_recycled_total = 0;
    uint64_t ping_fail_total = 0;
    size_t total_conn = 0;
    size_t idle_conn = 0;
    size_t creating_conn = 0;
    size_t max_creating = 0;
};

class RedisPoolStats {
public:
    void inc_reconnect();
    void inc_cmd_ok();
    void inc_cmd_fail();
    void inc_nil();
    void inc_timeout();
    void inc_connect_ok();
    void inc_connect_fail();
    void inc_acquire_wait();
    void inc_acquire_timeout();
    void inc_acquire_retry_exhausted();
    void add_idle_recycled(uint64_t n);
    void inc_ping_fail();

    RedisPoolStatsSnapshot base_snapshot(size_t created_total) const;
    RedisPoolStatsSnapshot snapshot(
        size_t created_total,
        size_t total_conn = 0,
        size_t idle_conn = 0,
        size_t creating_conn = 0,
        size_t max_creating = 0) const;

private:
    std::atomic<uint64_t> reconnect_total_{0};
    std::atomic<uint64_t> cmd_ok_total_{0};
    std::atomic<uint64_t> cmd_fail_total_{0};
    std::atomic<uint64_t> nil_total_{0};
    std::atomic<uint64_t> timeout_total_{0};
    std::atomic<uint64_t> connect_ok_total_{0};
    std::atomic<uint64_t> connect_fail_total_{0};
    std::atomic<uint64_t> acquire_wait_total_{0};
    std::atomic<uint64_t> acquire_timeout_total_{0};
    std::atomic<uint64_t> acquire_retry_exhausted_total_{0};
    std::atomic<uint64_t> idle_recycled_total_{0};
    std::atomic<uint64_t> ping_fail_total_{0};
};

std::string format_redis_pool_stats(const RedisPoolStatsSnapshot& s);
