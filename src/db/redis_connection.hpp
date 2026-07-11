#pragma once

#include <hiredis/hiredis.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class RedisPool;

using RedisContextPtr = std::unique_ptr<redisContext, decltype(&redisFree)>;

struct RedisConnectionConfig {
    std::string host = "127.0.0.1";
    int port = 6379;
    int db = 0;
    int connect_timeout_ms = 1000;
    int cmd_timeout_ms = 0;
};

struct TlsRedisConn {
    const RedisPool* owner = nullptr;
    uint64_t generation = 0;
    RedisContextPtr conn{nullptr, redisFree};
};

bool redis_tls_owner_matches(
    const TlsRedisConn& tls,
    const RedisPool* owner,
    uint64_t generation);

void reset_redis_tls_owner(
    TlsRedisConn& tls,
    const RedisPool* owner,
    uint64_t generation);

redisContext* create_redis_connection(
    const RedisConnectionConfig& cfg,
    std::atomic<size_t>& created_total);

redisContext* ensure_redis_tls_connection(
    TlsRedisConn& tls,
    const RedisPool* owner,
    uint64_t generation,
    const RedisConnectionConfig& cfg,
    std::atomic<size_t>& created_total);
