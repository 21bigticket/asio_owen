#include "redis_connection.hpp"

#include <sys/time.h>

#include <string_view>

#include "../common/logger.hpp"

bool redis_tls_owner_matches(
    const TlsRedisConn& tls,
    const RedisPool* owner,
    uint64_t generation) {
    return tls.owner == owner && tls.generation == generation;
}

void reset_redis_tls_owner(
    TlsRedisConn& tls,
    const RedisPool* owner,
    uint64_t generation) {
    tls.conn.reset();
    tls.owner = owner;
    tls.generation = generation;
}

redisContext* create_redis_connection(
    const RedisConnectionConfig& cfg,
    std::atomic<size_t>& created_total) {
    int connect_ms = cfg.connect_timeout_ms;
    if (connect_ms < 100) connect_ms = 100;
    timeval tv = {connect_ms / 1000, (connect_ms % 1000) * 1000};
    redisContext* ctx = redisConnectWithTimeout(cfg.host.c_str(), cfg.port, tv);
    if (!ctx || ctx->err) {
        std::string err = ctx ? ctx->errstr : "allocation failed";
        LOG_ERROR("Redis connect failed: ", err);
        if (ctx) redisFree(ctx);
        return nullptr;
    }

    if (cfg.cmd_timeout_ms > 0) {
        int cmd_ms = cfg.cmd_timeout_ms;
        if (cmd_ms < 100) cmd_ms = 100;
        timeval cmd_tv = {cmd_ms / 1000, (cmd_ms % 1000) * 1000};
        redisSetTimeout(ctx, cmd_tv);
    }

    if (cfg.db != 0) {
        redisReply* reply = static_cast<redisReply*>(redisCommand(ctx, "SELECT %d", cfg.db));
        bool ok = reply &&
                  reply->type == REDIS_REPLY_STATUS &&
                  std::string_view(reply->str, reply->len) == "OK";
        if (reply) freeReplyObject(reply);
        if (!ok) {
            LOG_ERROR("Redis select db failed: ", ctx->errstr);
            redisFree(ctx);
            return nullptr;
        }
    }

    ++created_total;
    LOG_INFO("Redis connected (created_total=", created_total.load(), ")");
    return ctx;
}

redisContext* ensure_redis_tls_connection(
    TlsRedisConn& tls,
    const RedisPool* owner,
    uint64_t generation,
    const RedisConnectionConfig& cfg,
    std::atomic<size_t>& created_total) {
    if (!redis_tls_owner_matches(tls, owner, generation)) {
        reset_redis_tls_owner(tls, owner, generation);
    }

    if (!tls.conn) {
        tls.conn.reset(create_redis_connection(cfg, created_total));
    } else if (tls.conn->err != 0) {
        LOG_INFO("Redis connection broken, rebuilding");
        tls.conn.reset(create_redis_connection(cfg, created_total));
    }
    return tls.conn.get();
}
