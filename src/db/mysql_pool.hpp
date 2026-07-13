#pragma once
#include <asio.hpp>
#include <mysql/mysql.h>
#include <mysql/errmsg.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <vector>
#include "../common/logger.hpp"
#include "mysql_connection.hpp"
#include "mysql_pool_stats.hpp"
#include "mysql_result_json.hpp"

// MySQL connection pool v3.3: shared mutex pool + dedicated maintain thread
//
// Core design:
//   - idle_pool_ (deque<IdleConn>) — front is oldest, back is newest
//   - creating_ — bounded concurrent connects (prevents thundering herd)
//   - total_ — current total connections (idle + in-use)
//   - max_idle_sec — time-based reclamation, not count-based
//   - maintain runs on its own thread — does not block io_context
//   - SQL is moved into the worker lambda; synchronous mysql calls never run on io_context.

class MysqlPool {
public:
    struct Config {
        std::string host;
        int port = 3306;
        std::string user;
        std::string pass;
        std::string db;
        size_t min_size = 8;
        size_t max_size = 64;
        int max_idle_sec = 60;
        int connect_timeout_ms = 1000;
        int read_timeout_ms = 500;  // deprecated compat field; MySQL read timeout is connection-level.
        int query_timeout_ms = 0;   // <= 0 uses a 30s connection-level read timeout for query/ping.
        int acquire_timeout_ms = 3000;
        int keepalive_sec = 30;
        size_t worker_threads = 32;  // SQL worker threads, mysql_query is synchronous blocking IO, recommended > CPU cores
        size_t max_creating = 0;     // 0 表示按 max_size/worker_threads 保守推导
    };

    struct Result {
        bool ok = false;
        std::string error;
        std::string json;
    };

    explicit MysqlPool(asio::io_context&, Config cfg)
        : running_(true),
          cfg_(validate_config(std::move(cfg))),
          max_creating_limit_(compute_max_creating_limit(cfg_)),
          worker_pool_(cfg_.worker_threads)  // SQL worker threads, independent of pool size
    {
        ensure_mysql_thread_initialized();  // main thread MySQL client init
        LOG_INFO("MySQL pool initializing, host=", cfg_.host, ":", cfg_.port,
                 ", min=", cfg_.min_size, ", max=", cfg_.max_size,
                 ", max_creating=", max_creating_limit_);

        // pre-create min_size connections at startup (outside lock, 1s timeout)
        size_t created = 0;
        for (size_t i = 0; i < cfg_.min_size; ++i) {
            auto conn = create_connection_with_timeout();
            if (conn) {
                std::lock_guard lock(mtx_);
                idle_pool_.push_back({conn, std::chrono::steady_clock::now()});
                ++total_;
                ++created;
            }
        }
        LOG_INFO("MySQL pool pre-created ", created, " connections");

        // start dedicated maintain thread
        maintain_thread_ = std::thread(&MysqlPool::maintain_loop, this);
        LOG_INFO("MySQL pool started, total=", total_, ", idle=", idle_pool_.size());
    }

    ~MysqlPool() { shutdown(); }

    void shutdown() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();

        // stop maintain first so shutdown is not gated by keepalive_sec.
        if (maintain_thread_.joinable())
            maintain_thread_.join();

        // wait for SQL workers to finish
        worker_pool_.join();

        // close all idle connections
        {
            ensure_mysql_thread_initialized();
            std::lock_guard lock(mtx_);
            while (!idle_pool_.empty()) {
                mysql_close(idle_pool_.front().conn);
                idle_pool_.pop_front();
            }
            total_ = 0;
        }
        LOG_INFO("MySQL pool shutdown");
    }

    asio::awaitable<Result> execute(std::string sql) {
        if (!running_) {
            stats_.inc_query_fail();
            co_return Result{false, "mysql pool stopped", ""};
        }

        co_await asio::post(worker_pool_, asio::use_awaitable);
        if (!running_) {
            stats_.inc_query_fail();
            co_return Result{false, "mysql pool stopped", ""};
        }
        co_return do_query(sql.c_str());
    }

    MysqlPoolStatsSnapshot snapshot() const {
        std::lock_guard lock(mtx_);
        return stats_.snapshot(total_, idle_pool_.size(), creating_, max_creating_limit_);
    }

    std::string stats() const {
        return format_mysql_pool_stats(snapshot());
    }

private:
    struct IdleConn {
        MYSQL* conn;
        std::chrono::steady_clock::time_point last_used_at;
    };

    using MysqlResultPtr = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>;

    class ConnectionGuard {
    public:
        ConnectionGuard(MysqlPool* pool, MYSQL* conn)
            : pool_(pool), conn_(conn) {}

        ~ConnectionGuard() {
            if (!conn_) return;
            if (bad_) {
                pool_->drop_bad_connection(conn_);
            } else {
                pool_->release(conn_);
            }
        }

        MYSQL* get() const { return conn_; }

        void mark_bad() { bad_ = true; }

        ConnectionGuard(const ConnectionGuard&) = delete;
        ConnectionGuard& operator=(const ConnectionGuard&) = delete;

    private:
        MysqlPool* pool_;
        MYSQL* conn_;
        bool bad_ = false;
    };

    Result do_query(const char* sql) {
        ensure_mysql_thread_initialized();

        MYSQL* conn = acquire();
        if (!conn) {
            stats_.inc_query_fail();
            return {false, "no available connection", ""};
        }
        ConnectionGuard guard(this, conn);

        if (mysql_query(conn, sql)) {
            unsigned int err_no = mysql_errno(conn);
            std::string err = mysql_error(conn);
            LOG_WARN("MySQL query failed: ", err);
            if (is_connection_error(err_no)) {
                guard.mark_bad();
            }
            stats_.inc_query_fail();
            return {false, std::move(err), ""};
        }

        MysqlResultPtr mr(mysql_store_result(conn), mysql_free_result);

        if (!mr) {
            if (mysql_field_count(conn) != 0) {
                unsigned int err_no = mysql_errno(conn);
                std::string err = mysql_error(conn);
                LOG_WARN("MySQL store result failed: ", err);
                if (is_connection_error(err_no)) {
                    guard.mark_bad();
                }
                stats_.inc_query_fail();
                return {false, std::move(err), ""};
            }
            stats_.inc_query_ok();
            return {true, "", "[]"};
        }

        std::string json = mysql_result_to_json(mr.get());
        stats_.inc_query_ok();
        return {true, "", std::move(json)};
    }

    // --- acquire: iterative retry (max 2), prevents recursive deadlock ---
    MYSQL* acquire() {
        ensure_mysql_thread_initialized();

        int retries = 2;
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(cfg_.acquire_timeout_ms);

        for (int retry = 0; retry < retries; ++retry) {
            MYSQL* idle_conn = nullptr;
            {
                std::unique_lock lock(mtx_);
                while (true) {
                    if (!running_) return nullptr;
                    if (!idle_pool_.empty()) {
                        idle_conn = idle_pool_.back().conn;
                        idle_pool_.pop_back();
                        break;
                    }
                    if (total_ < cfg_.max_size && creating_ < max_creating_limit_) {
                        ++total_;
                        ++creating_;
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

            if (idle_conn) {
                if (mysql_reset_connection(idle_conn) == 0) {
                    return idle_conn;
                }
                LOG_WARN("acquire: mysql_reset_connection failed: ", mysql_error(idle_conn));
                stats_.inc_reset_conn_fail();
                drop_bad_connection(idle_conn);
                continue;
            }

            // create connection outside lock
            MYSQL* conn = create_connection_with_timeout();

            {
                std::lock_guard lock(mtx_);
                --creating_;
                if (!conn) {
                    --total_;
                    cv_.notify_all();
                    LOG_WARN("acquire: create connection failed, retry=", retry);
                    continue;
                }
            }

            return conn;
        }

        LOG_WARN("acquire: exhausted retries, returning nullptr");
        stats_.inc_acquire_retry_exhausted();
        return nullptr;
    }

    // --- release: timestamp then return to idle pool ---
    void release(MYSQL* conn) {
        if (!conn) return;
        std::lock_guard lock(mtx_);
        idle_pool_.push_back({conn, std::chrono::steady_clock::now()});
        cv_.notify_one();
    }

    void drop_bad_connection(MYSQL* conn) {
        if (!conn) return;
        ensure_mysql_thread_initialized();
        mysql_close(conn);
        std::lock_guard lock(mtx_);
        --total_;
        cv_.notify_all();
    }

    // --- dedicated maintain thread ---
    void maintain_loop() {
        LOG_INFO("MySQL maintain thread started");
        while (running_) {
            // use cv_.wait_for instead of sleep_for, shutdown triggers immediate wake via cv_.notify_all
            {
                std::unique_lock lock(mtx_);
                cv_.wait_for(lock, std::chrono::seconds(cfg_.keepalive_sec), [&]() { return !running_; });
            }
            if (!running_) break;
            do_maintain();
        }
        LOG_INFO("MySQL maintain thread exited");
    }

    void do_maintain() {
        ensure_mysql_thread_initialized();

        auto now = std::chrono::steady_clock::now();

        // ---- Phase 1: Reclaim stale idle connections (close outside lock) ----
        std::vector<MYSQL*> to_close;
        {
            std::lock_guard lock(mtx_);
            while (!idle_pool_.empty()) {
                auto age = std::chrono::duration_cast<std::chrono::seconds>(
                    now - idle_pool_.front().last_used_at).count();
                if (age < cfg_.max_idle_sec)
                    break;
                to_close.push_back(idle_pool_.front().conn);
                idle_pool_.pop_front();
                --total_;
            }
            cv_.notify_all();
        }
        for (auto* c : to_close) {
            mysql_close(c);
        }
        if (!to_close.empty()) {
            stats_.add_idle_recycled(to_close.size());
            LOG_INFO("maintain: recycled ", to_close.size(), " idle connections");
        }

        if (!running_) return;

        // ---- Phase 2: Refill to min_size (create outside lock) ----
        size_t need = 0;
        {
            std::lock_guard lock(mtx_);
            if (idle_pool_.size() < cfg_.min_size) {
                size_t headroom = (total_ > cfg_.max_size) ? 0 : cfg_.max_size - total_;
                need = std::min(cfg_.min_size - idle_pool_.size(), headroom);
                total_ += need;  // reserve slots, preventing worker from over-creating concurrently
            }
        }
        std::vector<MYSQL*> new_conns;
        size_t created = 0;
        for (size_t i = 0; i < need; ++i) {
            if (!running_) break;
            auto* conn = create_connection_with_timeout();
            if (conn) {
                new_conns.push_back(conn);
                ++created;
            }
        }
        // rollback unused slots
        {
            std::lock_guard lock(mtx_);
            size_t unused = need - created;
            if (unused > 0) {
                total_ -= unused;
                cv_.notify_all();
            }
            for (auto* c : new_conns) {
                idle_pool_.push_back({c, std::chrono::steady_clock::now()});
            }
            cv_.notify_all();
            if (created > 0)
                LOG_INFO("maintain: added ", created, " connections");
        }

        if (!running_) return;

        // ---- Phase 3: Idle connection health check (optional, check oldest max_check connections) ----
        const size_t max_check = 4;
        std::vector<IdleConn> to_check;
        {
            std::lock_guard lock(mtx_);
            size_t cnt = std::min(idle_pool_.size(), max_check);
            for (size_t i = 0; i < cnt; ++i) {
                to_check.push_back(idle_pool_.front());
                idle_pool_.pop_front();
            }
        }
        size_t dead_count = 0;
        size_t checked = 0;
        for (; checked < to_check.size(); ++checked) {
            if (!running_) break;
            if (mysql_ping_with_timeout(to_check[checked].conn) != 0) {
                LOG_WARN("maintain: mysql_ping failed: ", mysql_error(to_check[checked].conn));
                mysql_close(to_check[checked].conn);
                stats_.inc_ping_fail();
                ++dead_count;
            } else {
                std::lock_guard lock(mtx_);
                idle_pool_.push_back({to_check[checked].conn, std::chrono::steady_clock::now()});
            }
        }
        // after shutdown, close any remaining unchecked connections
        if (!running_) {
            for (size_t i = checked; i < to_check.size(); ++i) {
                mysql_close(to_check[i].conn);
                ++dead_count;
            }
        }
        if (dead_count > 0) {
            std::lock_guard lock(mtx_);
            total_ -= dead_count;
            cv_.notify_all();
            LOG_INFO("maintain: ping check removed ", dead_count, " dead connections");
        }

        // pool status snapshot
        {
            std::lock_guard lock(mtx_);
            LOG_INFO("maintain: pool status total=", total_, ", idle=", idle_pool_.size());
        }
    }

    MYSQL* create_connection_with_timeout() {
        MYSQL* conn = ::create_mysql_connection_with_timeout(connection_config());
        if (conn) {
            stats_.inc_connect_ok();
        } else {
            stats_.inc_connect_fail();
        }
        return conn;
    }

    int mysql_ping_with_timeout(MYSQL* conn) {
        return ::mysql_ping_with_timeout(conn, cfg_.read_timeout_ms);
    }

    MysqlConnectionConfig connection_config() const {
        return MysqlConnectionConfig{
            .host = cfg_.host,
            .port = cfg_.port,
            .user = cfg_.user,
            .pass = cfg_.pass,
            .db = cfg_.db,
            .connect_timeout_ms = cfg_.connect_timeout_ms,
            .read_timeout_ms = cfg_.read_timeout_ms,
            .query_timeout_ms = cfg_.query_timeout_ms
        };
    }

    static bool is_connection_error(unsigned int err) {
        switch (err) {
            case CR_SERVER_GONE_ERROR:
            case CR_SERVER_LOST:
            case CR_CONNECTION_ERROR:
            case CR_CONN_HOST_ERROR:
                return true;
            default:
                return false;
        }
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

    static Config validate_config(Config cfg) {
        if (cfg.host.empty()) {
            throw std::invalid_argument("mysql.host cannot be empty");
        }
        if (cfg.port <= 0 || cfg.port > 65535) {
            throw std::invalid_argument("mysql.port out of range");
        }
        if (cfg.max_size == 0) cfg.max_size = 1;
        if (cfg.min_size > cfg.max_size) cfg.min_size = cfg.max_size;
        if (cfg.max_idle_sec <= 0) cfg.max_idle_sec = 60;
        if (cfg.connect_timeout_ms <= 0) cfg.connect_timeout_ms = 1000;
        if (cfg.query_timeout_ms <= 0) cfg.query_timeout_ms = 30000;
        if (cfg.acquire_timeout_ms <= 0) cfg.acquire_timeout_ms = 3000;
        if (cfg.keepalive_sec <= 0) cfg.keepalive_sec = 30;
        if (cfg.worker_threads == 0) cfg.worker_threads = 1;
        return cfg;
    }

    std::atomic<bool> running_;
    Config cfg_;
    size_t max_creating_limit_;

    std::deque<IdleConn> idle_pool_;
    size_t total_ = 0;             // protected by mtx_
    size_t creating_ = 0;          // protected by mtx_
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    MysqlPoolStats stats_;

    asio::thread_pool worker_pool_;
    std::thread maintain_thread_;
};
