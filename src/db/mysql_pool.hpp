#pragma once
#include <asio.hpp>
#include <mysql/mysql.h>
#include <deque>
#include <mutex>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include "../common/logger.hpp"

// MySQL connection pool v3.3: shared mutex pool + dedicated maintain thread
//
// Core design:
//   - idle_pool_ (deque<IdleConn>) — front is oldest, back is newest
//   - creating_ — at most 1 concurrent connect (prevents thundering herd)
//   - total_ — current total connections (idle + in-use)
//   - max_idle_sec — time-based reclamation, not count-based
//   - maintain runs on its own thread — does not block io_context
//   - SQL uses char[4096] stack buffer across asio::post boundary (prevents double free)

class MysqlPool {
public:
    struct Config {
        std::string host;
        int port;
        std::string user;
        std::string pass;
        std::string db;
        size_t min_size = 8;
        size_t max_size = 64;
        int max_idle_sec = 60;
        int connect_timeout_ms = 1000;
        int read_timeout_ms = 500;
        int keepalive_sec = 30;
        size_t worker_threads = 32;  // SQL worker threads, mysql_query is synchronous blocking IO, recommended > CPU cores
    };

    struct Result {
        bool ok = false;
        std::string error;
        std::string json;
    };

    MysqlPool(asio::io_context& ioc, Config cfg)
        : ioc_(ioc), cfg_(std::move(cfg)), running_(true),
          worker_pool_(cfg_.worker_threads)  // SQL worker threads, independent of pool size
    {
        LOG_INFO("MySQL pool initializing, host=", cfg_.host, ":", cfg_.port,
                 ", min=", cfg_.min_size, ", max=", cfg_.max_size);

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

        // wait for SQL workers to finish
        worker_pool_.join();

        // wait for maintain thread to exit
        if (maintain_thread_.joinable())
            maintain_thread_.join();

        // close all idle connections
        {
            std::lock_guard lock(mtx_);
            while (!idle_pool_.empty()) {
                mysql_close(idle_pool_.front().conn);
                idle_pool_.pop_front();
            }
            total_ = 0;
        }
        LOG_INFO("MySQL pool shutdown");
    }

    asio::awaitable<Result> execute(const std::string& sql) {
        // copy SQL to stack buffer to avoid std::string across threads
        char sql_buf[4096];
        size_t len = sql.size();
        if (len > sizeof(sql_buf) - 1) len = sizeof(sql_buf) - 1;
        std::memcpy(sql_buf, sql.data(), len);
        sql_buf[len] = '\0';

        Result res = co_await asio::post(
            [this, sql_buf]() -> Result {
                return do_query(sql_buf);
            },
            worker_pool_, asio::use_awaitable);
        co_return res;
    }

private:
    struct IdleConn {
        MYSQL* conn;
        std::chrono::steady_clock::time_point last_used_at;
    };

    Result do_query(const char* sql) {
        MYSQL* conn = acquire();
        if (!conn) return {false, "no available connection", ""};

        if (mysql_query(conn, sql)) {
            std::string err = mysql_error(conn);
            LOG_WARN("MySQL query failed: ", err);
            mysql_close(conn);
            // connection closed, notify pool to decrement count
            {
                std::lock_guard lock(mtx_);
                --total_;
                cv_.notify_all();
            }
            return {false, std::move(err), ""};
        }

        MYSQL_RES* mr = mysql_store_result(conn);
        release(conn);

        if (!mr) return {true, "", "[]"};

        unsigned int nf = mysql_num_fields(mr);
        MYSQL_FIELD* fields = mysql_fetch_fields(mr);

        std::string json;
        json.reserve(4096);
        json += '[';
        MYSQL_ROW row;
        bool first = true;
        while ((row = mysql_fetch_row(mr))) {
            if (!first) json += ',';
            first = false;
            json += '{';
            for (unsigned int i = 0; i < nf; ++i) {
                if (i > 0) json += ',';
                json += '"';
                json += fields[i].name;
                json += "\":\"";
                const char* val = row[i] ? row[i] : "NULL";
                for (const char* p = val; *p; ++p) {
                    if (*p == '"' || *p == '\\') json += '\\';
                    json += *p;
                }
                json += '"';
            }
            json += '}';
        }
        json += ']';
        mysql_free_result(mr);
        return {true, "", std::move(json)};
    }

    // --- acquire: iterative retry (max 2), prevents recursive deadlock ---
    MYSQL* acquire() {
        int retries = 2;
        for (int retry = 0; retry < retries; ++retry) {
            {
                std::unique_lock lock(mtx_);
                while (true) {
                    if (!running_) return nullptr;
                    if (!idle_pool_.empty()) {
                        auto conn = idle_pool_.front().conn;
                        idle_pool_.pop_front();
                        return conn;
                    }
                    if (creating_ > 0) {
                        cv_.wait(lock);
                        continue;
                    }
                    if (total_ < cfg_.max_size) {
                        ++total_;
                        ++creating_;
                        break;
                    }
                    cv_.wait(lock);
                }
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

            // new connection needs session reset (rollback any pending transactions)
            if (mysql_reset_connection(conn) != 0) {
                LOG_WARN("acquire: mysql_reset_connection failed: ", mysql_error(conn));
                mysql_close(conn);
                {
                    std::lock_guard lock(mtx_);
                    --total_;
                    cv_.notify_all();
                }
                continue;
            }

            return conn;
        }

        LOG_WARN("acquire: exhausted retries, returning nullptr");
        return nullptr;
    }

    // --- release: timestamp then return to idle pool ---
    void release(MYSQL* conn) {
        if (!conn) return;
        std::lock_guard lock(mtx_);
        idle_pool_.push_back({conn, std::chrono::steady_clock::now()});
        cv_.notify_one();
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
        if (!to_close.empty())
            LOG_INFO("maintain: recycled ", to_close.size(), " idle connections");

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
                mysql_close(to_check[checked].conn);
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

    // --- Create connection (with timeout) ---
    MYSQL* create_connection_with_timeout() {
        MYSQL* conn = mysql_init(nullptr);
        if (!conn) {
            LOG_ERROR("MySQL init failed");
            return nullptr;
        }

        // connect timeout
        unsigned int timeout_sec = cfg_.connect_timeout_ms / 1000;
        if (timeout_sec < 1) timeout_sec = 1;
        mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_sec);

        // note: MYSQL_OPT_READ_TIMEOUT is NOT set during connect.
        // read_timeout is only set temporarily by mysql_ping_with_timeout
        // during ping, then restored to avoid affecting subsequent mysql_query.

        if (!mysql_real_connect(conn, cfg_.host.c_str(), cfg_.user.c_str(),
                cfg_.pass.c_str(), cfg_.db.c_str(), cfg_.port, nullptr, 0)) {
            LOG_ERROR("MySQL connect failed: ", mysql_error(conn));
            mysql_close(conn);
            return nullptr;
        }
        return conn;
    }

    // --- Ping (set read_timeout, restore to 0 after ping) ---
    int mysql_ping_with_timeout(MYSQL* conn) {
        // MYSQL_OPT_READ_TIMEOUT unit is seconds, round up to ensure >= 1s
        unsigned int rt = (cfg_.read_timeout_ms + 999) / 1000;
        if (rt < 1) rt = 1;
        mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &rt);

        int ret = mysql_ping(conn);

        // restore read_timeout to 0 (unlimited), avoid affecting subsequent mysql_query
        unsigned int restore_rt = 0;
        mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &restore_rt);

        return ret;
    }

    std::atomic<bool> running_;
    asio::io_context& ioc_;
    Config cfg_;

    std::deque<IdleConn> idle_pool_;
    size_t total_ = 0;             // protected by mtx_
    size_t creating_ = 0;          // protected by mtx_
    std::mutex mtx_;
    std::condition_variable cv_;

    asio::thread_pool worker_pool_;
    std::thread maintain_thread_;
};
