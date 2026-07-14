#pragma once
#include <asio.hpp>
#include <hiredis/hiredis.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../common/logger.hpp"
#include "redis_command.hpp"
#include "redis_connection.hpp"
#include "redis_pool_stats.hpp"
#include "redis_reply.hpp"

// RedisPool v2: dual mode (direct / worker)
// - direct: thread-local dedicated connection, lock-free, fastest path.
// - worker: shared connection pool + dedicated thread pool, isolates sync hiredis from io_context.
//
// Lifecycle contract: stop HTTP/coroutine scheduling before destroying RedisPool.
class RedisPool {
public:
    enum class Mode { Direct, Worker };

    struct Config {
        std::string host = "127.0.0.1";
        int port = 6379;
        int db = 0;
        int connect_timeout_ms = 1000;
        int cmd_timeout_ms = 0;  // <= 0 uses a 30s safety timeout.
        Mode mode = Mode::Direct;

        // Worker mode only.
        size_t min_size = 4;
        size_t max_size = 32;
        int max_idle_sec = 120;
        size_t worker_threads = 16;
        size_t max_creating = 0;
        int acquire_timeout_ms = 3000;
    };

    using Reply = RedisReplyData;

    explicit RedisPool(asio::io_context&, Config cfg)
        : running_(true),
          cfg_(validate_config(std::move(cfg))),
          generation_(next_generation_.fetch_add(1, std::memory_order_relaxed)) {
        LOG_INFO("RedisPool initializing, mode=", cfg_.mode == Mode::Direct ? "direct" : "worker",
                 ", host=", cfg_.host, ":", cfg_.port,
                 ", cmd_timeout_ms=", effective_cmd_timeout_ms());

        if (cfg_.mode == Mode::Worker) {
            init_worker_mode();
        } else {
            LOG_INFO("RedisPool direct mode: TLS connection per io thread");
        }
    }

    ~RedisPool() { shutdown(); }

    void shutdown() {
        if (!running_.exchange(false)) return;

        if (cfg_.mode == Mode::Worker) {
            shutdown_worker_mode();
        }

        // Only the current thread's TLS connection can be cleared here. Other
        // thread-local connections are rejected by running_ and released when
        // their threads exit, or when a later pool generation resets the TLS slot.
        if (redis_tls_owner_matches(tls_, this, generation_)) {
            tls_.conn.reset();
            tls_.owner = nullptr;
            tls_.generation = 0;
        }

        LOG_INFO("Redis pool shutdown, created_total=", created_total_.load());
    }

    RedisPoolStatsSnapshot snapshot() const {
        if (cfg_.mode == Mode::Worker) {
            std::lock_guard lock(mtx_);
            return stats_.snapshot(
                created_total_.load(std::memory_order_relaxed),
                total_, idle_pool_.size(), creating_, max_creating_limit_);
        }
        return stats_.base_snapshot(created_total_.load(std::memory_order_relaxed));
    }

    std::string stats() const {
        return format_redis_pool_stats(snapshot());
    }

    // NOTE: the former printf-style cmd(fmt, ...) was removed. It expanded the
    // format string and then passed the *result* back to redisCommand() as a
    // second format string, so any '%' in dynamic data was reparsed —
    // crash / memory corruption / command injection. Use cmd_argv() instead,
    // which routes every argument through redisCommandArgv() (binary-safe, no
    // format reparse).
    asio::awaitable<Reply> cmd_argv(std::vector<std::string> args) {
        if (!running_) {
            co_return make_error("redis pool is shutdown");
        }
        if (cfg_.mode == Mode::Direct) {
            co_return cmd_argv_sync_impl(std::move(args));
        }

        // Worker mode: switch executor only — args stays in the coroutine frame.
        // Capturing non-POD into a post() lambda triggers GCC 11 coroutine UAF;
        // see docs/REDIS_POOL_LAMBDA_FIX_2026-07-12.md and docs/SUMMARY_2026-07-12.md.
        co_await asio::post(*worker_pool_, asio::use_awaitable);
        if (!running_) {
            co_return make_error("redis pool is shutdown");
        }
        co_return cmd_argv_sync_impl(std::move(args));
    }

    asio::awaitable<Reply> get(const char* key) {
        if (!running_) {
            co_return make_error("redis pool is shutdown");
        }
        std::string key_copy(key);
        if (cfg_.mode == Mode::Direct) {
            co_return get_sync(key_copy.c_str());
        }

        // Worker mode: switch executor only — key_copy stays in the coroutine frame.
        co_await asio::post(*worker_pool_, asio::use_awaitable);
        if (!running_) {
            co_return make_error("redis pool is shutdown");
        }
        co_return get_sync(key_copy.c_str());
    }

    // Synchronous compatibility path. It is intentionally kept for GCC 11.4
    // coroutine ICE workarounds in fire-and-forget call sites.
    Reply cmd_argv_sync(std::vector<std::string> args) {
        return cmd_argv_sync_impl(std::move(args));
    }

private:
    Reply cmd_argv_sync_impl(std::vector<std::string> args) {
        if (!running_) {
            return make_error("redis pool is shutdown");
        }
        if (args.empty()) {
            stats_.inc_cmd_fail();
            return make_error("empty Redis command");
        }

        redisContext* ctx = acquire_conn();
        if (!ctx) {
            stats_.inc_cmd_fail();
            return make_error("no available Redis connection");
        }

        ConnectionGuard guard(this, ctx);
        RedisCommandArgv command(args);
        redisReply* reply = static_cast<redisReply*>(
            redisCommandArgv(ctx, command.argc(), command.argv.data(), command.argv_len.data()));

        const int saved_errno = errno;
        if (!reply) {
            std::string err = fill_error_if_empty(ctx);
            LOG_WARN("Redis cmd_argv failed: ", err);
            record_command_failure(ctx, saved_errno, err);
            guard.drop();
            return make_error(std::move(err));
        }

        RedisReplyGuard reply_guard(reply);
        Reply r;
        parse_redis_reply(reply_guard.get(), r);
        record_command_result(r);
        return r;
    }

    struct RedisReplyGuard {
        redisReply* reply = nullptr;

        RedisReplyGuard() = default;
        explicit RedisReplyGuard(redisReply* r) : reply(r) {}
        ~RedisReplyGuard() { reset(); }

        RedisReplyGuard(const RedisReplyGuard&) = delete;
        RedisReplyGuard& operator=(const RedisReplyGuard&) = delete;

        RedisReplyGuard(RedisReplyGuard&& other) noexcept : reply(other.reply) {
            other.reply = nullptr;
        }

        RedisReplyGuard& operator=(RedisReplyGuard&& other) noexcept {
            if (this != &other) {
                reset();
                reply = other.reply;
                other.reply = nullptr;
            }
            return *this;
        }

        void reset() {
            if (reply) {
                freeReplyObject(reply);
                reply = nullptr;
            }
        }

        [[nodiscard]] redisReply* get() const { return reply; }
        explicit operator bool() const { return reply != nullptr; }
    };

    class ConnectionGuard {
    public:
        ConnectionGuard(RedisPool* pool, redisContext* ctx)
            : pool_(pool), ctx_(ctx) {}

        ~ConnectionGuard() {
            if (!dropped_ && ctx_) {
                pool_->release_conn(ctx_);
            }
        }

        void drop() {
            if (dropped_ || !ctx_) return;
            dropped_ = true;
            pool_->drop_bad_connection(ctx_);
            ctx_ = nullptr;
        }

        ConnectionGuard(const ConnectionGuard&) = delete;
        ConnectionGuard& operator=(const ConnectionGuard&) = delete;

    private:
        RedisPool* pool_;
        redisContext* ctx_;
        bool dropped_ = false;
    };

    friend class ConnectionGuard;

    struct IdleConn {
        redisContext* ctx;
        std::chrono::steady_clock::time_point last_used_at;
    };

    static Config validate_config(Config cfg) {
        if (cfg.host.empty())
            throw std::invalid_argument("Redis host cannot be empty");
        if (cfg.port <= 0 || cfg.port > 65535)
            throw std::invalid_argument("Redis port out of range");
        if (cfg.db < 0)
            throw std::invalid_argument("Redis db cannot be negative");
        if (cfg.connect_timeout_ms <= 0)
            cfg.connect_timeout_ms = 1000;
        if (cfg.cmd_timeout_ms <= 0)
            cfg.cmd_timeout_ms = 30000;

        if (cfg.mode == Mode::Worker) {
            if (cfg.max_size == 0) cfg.max_size = 32;
            if (cfg.min_size > cfg.max_size) cfg.min_size = cfg.max_size;
            if (cfg.worker_threads == 0) cfg.worker_threads = 4;
            if (cfg.max_idle_sec <= 0) cfg.max_idle_sec = 120;
            if (cfg.acquire_timeout_ms <= 0) cfg.acquire_timeout_ms = 3000;
        }

        return cfg;
    }

    int effective_cmd_timeout_ms() const {
        return cfg_.cmd_timeout_ms <= 0 ? 30000 : cfg_.cmd_timeout_ms;
    }

    void init_worker_mode() {
        max_creating_limit_ = compute_max_creating_limit(cfg_);
        worker_pool_ = std::make_unique<asio::thread_pool>(cfg_.worker_threads);

        size_t created = 0;
        for (size_t i = 0; i < cfg_.min_size; ++i) {
            auto* ctx = create_connection();
            if (ctx) {
                std::lock_guard lock(mtx_);
                idle_pool_.push_back({ctx, std::chrono::steady_clock::now()});
                ++total_;
                ++created;
            }
        }
        LOG_INFO("RedisPool worker pre-created ", created, " connections");

        maintain_thread_ = std::thread(&RedisPool::maintain_loop, this);
        LOG_INFO("RedisPool worker started, total=", total_, ", idle=", idle_pool_.size());
    }

    void shutdown_worker_mode() {
        cv_.notify_all();

        if (maintain_thread_.joinable())
            maintain_thread_.join();

        if (worker_pool_)
            worker_pool_->join();

        std::lock_guard lock(mtx_);
        while (!idle_pool_.empty()) {
            redisFree(idle_pool_.front().ctx);
            idle_pool_.pop_front();
        }
        total_ = 0;
    }

    redisContext* acquire_conn() {
        if (!running_) return nullptr;
        if (cfg_.mode == Mode::Worker) {
            return acquire_worker();
        }
        return acquire_direct();
    }

    void release_conn(redisContext* ctx) {
        if (!ctx) return;
        if (cfg_.mode == Mode::Worker) {
            release_worker(ctx);
        }
    }

    redisContext* acquire_direct() {
        bool did_reconnect = false;
        redisContext* ctx = ensure_redis_tls_connection(
            tls_, this, generation_, connection_config(), created_total_, &did_reconnect);
        if (did_reconnect) {
            stats_.inc_reconnect();
        }
        return ctx;
    }

    redisContext* acquire_worker() {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(cfg_.acquire_timeout_ms);

        for (int retry = 0; retry < 2; ++retry) {
            bool should_create = false;
            {
                std::unique_lock lock(mtx_);
                while (true) {
                    if (!running_) return nullptr;
                    if (!idle_pool_.empty()) {
                        auto* ctx = idle_pool_.front().ctx;
                        idle_pool_.pop_front();
                        lock.unlock();
                        if (reset_worker_connection(ctx)) {
                            return ctx;
                        }
                        drop_bad_connection(ctx);
                        break;
                    }
                    if (total_ < cfg_.max_size && creating_ < max_creating_limit_) {
                        ++total_;
                        ++creating_;
                        should_create = true;
                        break;
                    }
                    stats_.inc_acquire_wait();
                    const auto status = cv_.wait_until(lock, deadline);
                    if (status == std::cv_status::timeout) {
                        stats_.inc_acquire_timeout();
                        return nullptr;
                    }
                }
            }

            if (!should_create) {
                continue;
            }

            redisContext* ctx = create_connection();

            {
                std::lock_guard lock(mtx_);
                --creating_;
                if (!ctx) {
                    --total_;
                    cv_.notify_all();
                    continue;
                }
            }
            return ctx;
        }

        stats_.inc_acquire_retry_exhausted();
        return nullptr;
    }

    void release_worker(redisContext* ctx) {
        if (!ctx) return;
        std::lock_guard lock(mtx_);
        idle_pool_.push_back({ctx, std::chrono::steady_clock::now()});
        cv_.notify_one();
    }

    bool reset_worker_connection(redisContext* ctx) {
        RedisReplyGuard reply_guard(static_cast<redisReply*>(redisCommand(ctx, "SELECT %d", cfg_.db)));
        return reply_guard &&
               reply_guard.get()->type == REDIS_REPLY_STATUS &&
               std::string_view(reply_guard.get()->str, reply_guard.get()->len) == "OK";
    }

    void drop_bad_connection(redisContext* ctx) {
        if (!ctx) return;

        if (cfg_.mode == Mode::Worker) {
            redisFree(ctx);
            std::lock_guard lock(mtx_);
            --total_;
            cv_.notify_all();
            return;
        }

        if (redis_tls_owner_matches(tls_, this, generation_) && tls_.conn.get() == ctx) {
            tls_.conn.reset();
        } else {
            redisFree(ctx);
        }
    }

    Reply get_sync(const char* key) {
        redisContext* ctx = acquire_conn();
        if (!ctx) {
            stats_.inc_cmd_fail();
            return make_error("no available Redis connection");
        }

        ConnectionGuard guard(this, ctx);
        // Route through redisCommandArgv (binary-safe, no format reparse) rather
        // than redisCommand(ctx, "GET %s", key), whose %s runs strlen() and would
        // truncate a key containing an embedded NUL.
        RedisCommandArgv command(std::vector<std::string>{"GET", std::string(key)});
        redisReply* reply = static_cast<redisReply*>(
            redisCommandArgv(ctx, command.argc(), command.argv.data(), command.argv_len.data()));
        const int saved_errno = errno;

        if (!reply) {
            std::string err = fill_error_if_empty(ctx);
            LOG_WARN("Redis GET failed: ", err);
            record_command_failure(ctx, saved_errno, err);
            guard.drop();
            return make_error(std::move(err));
        }

        RedisReplyGuard reply_guard(reply);
        Reply r;
        parse_redis_reply(reply_guard.get(), r);
        record_command_result(r);
        return r;
    }

    void maintain_loop() {
        LOG_INFO("Redis maintain thread started");
        while (running_) {
            std::unique_lock lock(mtx_);
            cv_.wait_for(lock, std::chrono::seconds(30), [&] { return !running_; });
            if (!running_) break;
            lock.unlock();
            do_maintain();
        }
        LOG_INFO("Redis maintain thread exited");
    }

    void do_maintain() {
        const auto now = std::chrono::steady_clock::now();

        std::vector<redisContext*> to_close;
        {
            std::lock_guard lock(mtx_);
            while (!idle_pool_.empty()) {
                const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                    now - idle_pool_.front().last_used_at).count();
                if (age < cfg_.max_idle_sec) break;
                to_close.push_back(idle_pool_.front().ctx);
                idle_pool_.pop_front();
                --total_;
            }
            cv_.notify_all();
        }
        for (auto* c : to_close) redisFree(c);
        if (!to_close.empty()) {
            stats_.add_idle_recycled(to_close.size());
            LOG_INFO("maintain: recycled ", to_close.size(), " idle connections");
        }

        if (!running_) return;

        size_t need = 0;
        {
            std::lock_guard lock(mtx_);
            if (idle_pool_.size() < cfg_.min_size) {
                const size_t headroom = (total_ > cfg_.max_size) ? 0 : cfg_.max_size - total_;
                need = std::min(cfg_.min_size - idle_pool_.size(), headroom);
                total_ += need;
            }
        }
        std::vector<redisContext*> new_conns;
        size_t created = 0;
        for (size_t i = 0; i < need; ++i) {
            if (!running_) break;
            auto* ctx = create_connection();
            if (ctx) {
                new_conns.push_back(ctx);
                ++created;
            }
        }
        {
            std::lock_guard lock(mtx_);
            total_ -= (need - created);
            for (auto* c : new_conns) {
                idle_pool_.push_back({c, std::chrono::steady_clock::now()});
            }
            cv_.notify_all();
            if (created > 0)
                LOG_INFO("maintain: added ", created, " connections");
        }

        if (!running_) return;

        constexpr size_t max_check = 4;
        std::vector<IdleConn> to_check;
        {
            std::lock_guard lock(mtx_);
            const size_t cnt = std::min(idle_pool_.size(), max_check);
            for (size_t i = 0; i < cnt; ++i) {
                to_check.push_back(idle_pool_.front());
                idle_pool_.pop_front();
            }
        }
        size_t dead_count = 0;
        size_t checked = 0;
        for (; checked < to_check.size(); ++checked) {
            if (!running_) break;
            auto* ctx = to_check[checked].ctx;
            bool alive = false;

            RedisReplyGuard reply_guard(static_cast<redisReply*>(redisCommand(ctx, "PING")));
            if (reply_guard) {
                alive = (reply_guard.get()->type == REDIS_REPLY_STATUS &&
                         std::string_view(reply_guard.get()->str, reply_guard.get()->len) == "PONG");
            }

            if (!alive) {
                redisFree(ctx);
                stats_.inc_ping_fail();
                ++dead_count;
            } else {
                std::lock_guard lock(mtx_);
                idle_pool_.push_back({ctx, std::chrono::steady_clock::now()});
            }
        }
        if (!running_) {
            for (size_t i = checked; i < to_check.size(); ++i) {
                redisFree(to_check[i].ctx);
                ++dead_count;
            }
        }
        if (dead_count > 0) {
            std::lock_guard lock(mtx_);
            total_ -= dead_count;
            cv_.notify_all();
            LOG_INFO("maintain: ping check removed ", dead_count, " dead connections");
        }

        {
            std::lock_guard lock(mtx_);
            LOG_INFO("maintain: pool status total=", total_, ", idle=", idle_pool_.size());
        }
    }

    redisContext* create_connection() {
        auto* ctx = ::create_redis_connection(connection_config(), created_total_);
        if (ctx) {
            stats_.inc_connect_ok();
        } else {
            stats_.inc_connect_fail();
        }
        return ctx;
    }

    std::string fill_error_if_empty(redisContext* ctx) const {
        if (ctx->err == 0) {
            ctx->err = REDIS_ERR_IO;
            snprintf(ctx->errstr, sizeof(ctx->errstr),
                     "redisCommand returned nullptr (OOM or disconnect)");
        }
        return ctx->errstr;
    }

    Reply make_error(std::string msg) const {
        return Reply{false, std::move(msg), "", 0};
    }

    void record_command_result(const Reply& r) {
        if (r.ok) {
            stats_.inc_cmd_ok();
            if (r.type == "nil") stats_.inc_nil();
        } else {
            stats_.inc_cmd_fail();
        }
    }

    void record_command_failure(const redisContext* ctx, int saved_errno, const std::string& err) {
        stats_.inc_cmd_fail();
        if (ctx->err == REDIS_ERR_IO &&
            (saved_errno == ETIMEDOUT ||
             saved_errno == EAGAIN ||
             saved_errno == EWOULDBLOCK)) {
            stats_.inc_timeout();
            return;
        }
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
            .db = cfg_.db,
            .connect_timeout_ms = cfg_.connect_timeout_ms,
            .cmd_timeout_ms = effective_cmd_timeout_ms()
        };
    }

    static size_t compute_max_creating_limit(const Config& cfg) {
        if (cfg.max_creating > 0) {
            return std::max<size_t>(1, std::min(cfg.max_creating, cfg.max_size));
        }
        return std::min({
            static_cast<size_t>(4),
            std::max<size_t>(1, cfg.max_size / 8),
            std::max<size_t>(1, cfg.worker_threads / 2)
        });
    }

    std::atomic<bool> running_;
    Config cfg_;
    uint64_t generation_;
    std::atomic<size_t> created_total_{0};
    RedisPoolStats stats_;

    std::unique_ptr<asio::thread_pool> worker_pool_;
    std::thread maintain_thread_;
    std::deque<IdleConn> idle_pool_;
    size_t total_ = 0;
    size_t creating_ = 0;
    size_t max_creating_limit_ = 0;
    mutable std::mutex mtx_;
    std::condition_variable cv_;

    inline static std::atomic<uint64_t> next_generation_{1};
    inline static thread_local TlsRedisConn tls_;
};
