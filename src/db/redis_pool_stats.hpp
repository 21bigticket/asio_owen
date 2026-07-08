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
};

class RedisPoolStats {
public:
    void inc_reconnect();
    void inc_cmd_ok();
    void inc_cmd_fail();
    void inc_nil();
    void inc_timeout();

    RedisPoolStatsSnapshot snapshot(size_t created_total) const;

private:
    std::atomic<uint64_t> reconnect_total_{0};
    std::atomic<uint64_t> cmd_ok_total_{0};
    std::atomic<uint64_t> cmd_fail_total_{0};
    std::atomic<uint64_t> nil_total_{0};
    std::atomic<uint64_t> timeout_total_{0};
};

std::string format_redis_pool_stats(const RedisPoolStatsSnapshot& s);
