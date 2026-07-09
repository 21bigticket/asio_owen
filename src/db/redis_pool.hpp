#pragma once
#include <asio.hpp>
#include <hiredis/hiredis.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "../common/logger.hpp"
#include "redis_command.hpp"
#include "redis_connection.hpp"
#include "redis_pool_stats.hpp"
#include "redis_reply.hpp"

// One dedicated Redis connection per io_context thread, lock-free, synchronous hiredis
class RedisPool {
public:
    struct Config {
        std::string host = "127.0.0.1";
        int port = 6379;
        int connect_timeout_ms = 1000;
        int cmd_timeout_ms = 0;    // 0 表示不设命令超时（性能模式），>0 表示毫秒数
    };

    using Reply = RedisReplyData;

    RedisPool(asio::io_context& ioc, Config cfg)
        : running_(true),
          ioc_(ioc),
          cfg_(std::move(cfg)),
          generation_(next_generation_.fetch_add(1, std::memory_order_relaxed))
    {
        LOG_INFO("RedisPool created, host=", cfg_.host, ":", cfg_.port);
    }

    ~RedisPool() { shutdown(); }

    void shutdown() {
        if (!running_.exchange(false)) return;
        // 清理当前线程的专属连接（其他线程的连接在线程退出时由 unique_ptr 自动释放）
        if (redis_tls_owner_matches(tls_, this, generation_)) {
            tls_.conn.reset();
            tls_.owner = nullptr;
            tls_.generation = 0;
        }
        LOG_INFO("Redis pool shutdown, created_total=", created_total_.load());
    }

    RedisPoolStatsSnapshot snapshot() const {
        return stats_.snapshot(created_total_.load(std::memory_order_relaxed));
    }

    std::string stats() const {
        return format_redis_pool_stats(snapshot());
    }

    // varargs 格式化后用 redisCommand 执行
    // 注意：varargs 函数不能包含 co_return，全部委托给 do_cmd
    // 单次 cmd 失败即返回错误给上层，不重试。连接断开自动重建
    [[deprecated("Use cmd_argv() for commands carrying dynamic arguments")]]
    asio::awaitable<Reply> cmd(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);

        va_list ap_copy;
        va_copy(ap_copy, ap);
        int len = vsnprintf(nullptr, 0, fmt, ap_copy);
        va_end(ap_copy);

        if (len < 0) {
            Reply r;
            r.error = "failed to format Redis command";
            va_end(ap);
            stats_.inc_cmd_fail();
            return err_awaitable(std::move(r));
        }

        std::vector<char> buffer(static_cast<size_t>(len) + 1);
        vsnprintf(buffer.data(), buffer.size(), fmt, ap);
        va_end(ap);

        std::string cmdline(buffer.data(), static_cast<size_t>(len));
        return do_cmd(std::move(cmdline));
    }

    asio::awaitable<Reply> cmd_argv(std::vector<std::string> args) {
        co_return cmd_argv_sync(std::move(args));
    }

    Reply cmd_argv_sync(std::vector<std::string> args) {
        if (args.empty()) {
            stats_.inc_cmd_fail();
            return Reply{false, "empty Redis command", "", 0};
        }

        redisContext* ctx = get_conn();
        if (!ctx) {
            stats_.inc_cmd_fail();
            return Reply{false, "no Redis connection", "", 0};
        }

        RedisCommandArgv command(args);

        redisReply* reply = (redisReply*)redisCommandArgv(
            ctx, command.argc(), command.argv.data(), command.argv_len.data());
        if (!reply) {
            if (ctx->err == 0) {
                ctx->err = REDIS_ERR_IO;
                snprintf(ctx->errstr, sizeof(ctx->errstr), "redisCommandArgv returned nullptr (OOM or disconnect)");
            }
            std::string err = ctx->errstr;
            LOG_WARN("Redis cmd failed: ", err, ", connection will be rebuilt");
            record_command_failure(err);
            return Reply{false, std::move(err), "", 0};
        }

        Reply r;
        parse_redis_reply(reply, r);
        freeReplyObject(reply);
        record_command_result(r);
        return r;
    }

    // 快速路径：直接执行固定命令，跳过 vsnprintf 格式化和 string 分配
    asio::awaitable<Reply> get(const char* key) {
        redisContext* ctx = get_conn();
        if (!ctx) {
            stats_.inc_cmd_fail();
            co_return Reply{false, "no Redis connection", "", 0};
        }

        redisReply* reply = (redisReply*)redisCommand(ctx, "GET %s", key);
        if (!reply) {
            if (ctx->err == 0) {
                ctx->err = REDIS_ERR_IO;
                snprintf(ctx->errstr, sizeof(ctx->errstr), "redisCommand returned nullptr (OOM or disconnect)");
            }
            std::string err = ctx->errstr;
            LOG_WARN("Redis cmd failed: ", err, ", connection will be rebuilt");
            record_command_failure(err);
            co_return Reply{false, std::move(err), "", 0};
        }

        Reply r;
        parse_redis_reply(reply, r);
        freeReplyObject(reply);
        record_command_result(r);
        co_return r;
    }

private:
    asio::awaitable<Reply> err_awaitable(Reply r) {
        co_return r;
    }

    asio::awaitable<Reply> do_cmd(std::string cmdline) {
        redisContext* ctx = get_conn();
        if (!ctx) {
            stats_.inc_cmd_fail();
            co_return Reply{false, "no Redis connection", "", 0};
        }

        redisReply* reply = (redisReply*)redisCommand(ctx, cmdline.c_str());
        if (!reply) {
            // reply==nullptr 不一定保证 ctx->err 非零（如 OOM 场景），主动标记断开
            if (ctx->err == 0) {
                ctx->err = REDIS_ERR_IO;
                snprintf(ctx->errstr, sizeof(ctx->errstr), "redisCommand returned nullptr (OOM or disconnect)");
            }
            std::string err = ctx->errstr;
            LOG_WARN("Redis cmd failed: ", err, ", connection will be rebuilt");
            record_command_failure(err);
            co_return Reply{false, std::move(err), "", 0};
        }

        Reply r;
        parse_redis_reply(reply, r);
        freeReplyObject(reply);
        record_command_result(r);
        co_return r;
    }

    redisContext* get_conn() {
        bool reconnecting = redis_tls_owner_matches(tls_, this, generation_) &&
                            tls_.conn &&
                            tls_.conn->err != 0;
        redisContext* ctx = ensure_redis_tls_connection(
            tls_, this, generation_, connection_config(), created_total_);
        if (reconnecting) {
            stats_.inc_reconnect();
        }
        return ctx;
    }

    void record_command_result(const Reply& r) {
        if (r.ok) {
            stats_.inc_cmd_ok();
            if (r.type == "nil") {
                stats_.inc_nil();
            }
        } else {
            record_command_failure(r.error);
        }
    }

    void record_command_failure(const std::string& err) {
        stats_.inc_cmd_fail();
        if (err.find("timeout") != std::string::npos ||
            err.find("Timeout") != std::string::npos ||
            err.find("timed out") != std::string::npos) {
            stats_.inc_timeout();
        }
    }

    RedisConnectionConfig connection_config() const {
        return RedisConnectionConfig{
            .host = cfg_.host,
            .port = cfg_.port,
            .connect_timeout_ms = cfg_.connect_timeout_ms,
            .cmd_timeout_ms = cfg_.cmd_timeout_ms
        };
    }

    std::atomic<bool> running_;
    asio::io_context& ioc_;
    Config cfg_;
    uint64_t generation_;
    std::atomic<size_t> created_total_{0};
    RedisPoolStats stats_;

    inline static std::atomic<uint64_t> next_generation_{1};
    inline static thread_local TlsRedisConn tls_;
};
