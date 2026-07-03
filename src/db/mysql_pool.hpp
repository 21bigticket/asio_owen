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

// MySQL 连接池 v3.3：共享互斥池 + 独立 maintain 线程
//
// 核心设计：
//   - idle_pool_ (deque<IdleConn>) — 队首最老，队尾最新
//   - creating_ — 同一时刻至多 1 个建连（防 thundering herd）
//   - total_ — 当前总连接数（含 idle + 正在使用的）
//   - max_idle_sec — 按时间回收，不按数量
//   - maintain 独立线程 — 不阻塞 io_context
//   - SQL 用 char[4096] 栈数组跨 asio::post 边界（防 double free）

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
        size_t worker_threads = 32;  // SQL worker 线程数，mysql_query 是同步阻塞 IO，建议 > CPU 核数
    };

    struct Result {
        bool ok = false;
        std::string error;
        std::string json;
    };

    MysqlPool(asio::io_context& ioc, Config cfg)
        : ioc_(ioc), cfg_(std::move(cfg)), running_(true),
          worker_pool_(cfg_.worker_threads)  // SQL worker 线程数，独立于连接池大小
    {
        LOG_INFO("MySQL pool initializing, host=", cfg_.host, ":", cfg_.port,
                 ", min=", cfg_.min_size, ", max=", cfg_.max_size);

        // 启动时预创建 min_size 个连接（锁外建连，1s 超时）
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

        // 启动独立 maintain 线程
        maintain_thread_ = std::thread(&MysqlPool::maintain_loop, this);
        LOG_INFO("MySQL pool started, total=", total_, ", idle=", idle_pool_.size());
    }

    ~MysqlPool() { shutdown(); }

    void shutdown() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();

        // 等待 SQL worker 完成
        worker_pool_.join();

        // 等待 maintain 线程退出
        if (maintain_thread_.joinable())
            maintain_thread_.join();

        // 关闭所有空闲连接
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
        // 将 SQL 拷贝到栈数组，避免 std::string 跨线程
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
            // 连接已关闭，通知池减少计数
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

    // --- acquire: 迭代重试（最多 2 次），防递归死循环 ---
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

            // 锁外建连
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

            // 新建连接需要会话重置（回滚可能残留的事务）
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

    // --- release: 标记时间戳后归还 idle 池 ---
    void release(MYSQL* conn) {
        if (!conn) return;
        std::lock_guard lock(mtx_);
        idle_pool_.push_back({conn, std::chrono::steady_clock::now()});
        cv_.notify_one();
    }

    // --- maintain 独立线程 ---
    void maintain_loop() {
        LOG_INFO("MySQL maintain thread started");
        while (running_) {
            // 用 cv_.wait_for 替代 sleep_for，shutdown 时通过 cv_.notify_all 立即唤醒
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

        // ---- 第一阶段：回收超时空闲连接（锁外 close） ----
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

        // ---- 第二阶段：补充到 min_size（锁外建连） ----
        size_t need = 0;
        {
            std::lock_guard lock(mtx_);
            if (idle_pool_.size() < cfg_.min_size) {
                size_t headroom = (total_ > cfg_.max_size) ? 0 : cfg_.max_size - total_;
                need = std::min(cfg_.min_size - idle_pool_.size(), headroom);
                total_ += need;  // 预订 slot，防止 worker 在此期间超额建连
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
        // 回滚未成功创建的 slot
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

        // ---- 第三阶段：空闲连接健康检查（可选，仅取最旧 max_check 个） ----
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
        // shutdown 后关闭尚未处理的残留连接
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

        // 水位快照
        {
            std::lock_guard lock(mtx_);
            LOG_INFO("maintain: pool status total=", total_, ", idle=", idle_pool_.size());
        }
    }

    // --- 建连（带超时）---
    MYSQL* create_connection_with_timeout() {
        MYSQL* conn = mysql_init(nullptr);
        if (!conn) {
            LOG_ERROR("MySQL init failed");
            return nullptr;
        }

        // 建连超时
        unsigned int timeout_sec = cfg_.connect_timeout_ms / 1000;
        if (timeout_sec < 1) timeout_sec = 1;
        mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_sec);

        // 注意：建连阶段不设 MYSQL_OPT_READ_TIMEOUT。
        // read_timeout 只由 mysql_ping_with_timeout 在 ping 路径上临时设，
        // 且 ping 结束后恢复，避免影响后续 mysql_query 的默认无超时行为。

        if (!mysql_real_connect(conn, cfg_.host.c_str(), cfg_.user.c_str(),
                cfg_.pass.c_str(), cfg_.db.c_str(), cfg_.port, nullptr, 0)) {
            LOG_ERROR("MySQL connect failed: ", mysql_error(conn));
            mysql_close(conn);
            return nullptr;
        }
        return conn;
    }

    // --- ping（设 read_timeout，ping 后恢复为 0 不限制）---
    int mysql_ping_with_timeout(MYSQL* conn) {
        // MYSQL_OPT_READ_TIMEOUT 单位是秒，向上取整保证 >= 1 秒
        // 例如 read_timeout_ms=500 → rt=1
        unsigned int rt = (cfg_.read_timeout_ms + 999) / 1000;
        if (rt < 1) rt = 1;
        mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &rt);

        int ret = mysql_ping(conn);

        // 恢复 read_timeout 为 0（不限制），避免影响后续 mysql_query 的执行
        unsigned int restore_rt = 0;
        mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &restore_rt);

        return ret;
    }

    std::atomic<bool> running_;
    asio::io_context& ioc_;
    Config cfg_;

    std::deque<IdleConn> idle_pool_;
    size_t total_ = 0;             // 由 mtx_ 保护
    size_t creating_ = 0;          // 由 mtx_ 保护
    std::mutex mtx_;
    std::condition_variable cv_;

    asio::thread_pool worker_pool_;
    std::thread maintain_thread_;
};
