#pragma once
#include <asio.hpp>
#include <mysql/mysql.h>
#include <queue>
#include <mutex>
#include <cstring>
#include <atomic>
#include "../common/logger.hpp"

// MySQL 连接池 + thread_pool，用栈数组传递 SQL 避免 std::string 跨线程问题
class MysqlPool {
public:
    struct Config {
        std::string host;
        int port;
        std::string user;
        std::string pass;
        std::string db;
        size_t pool_size = 8;
        int keepalive_sec = 30;
    };

    struct Result {
        bool ok = false;
        std::string error;
        std::string json;
    };

    MysqlPool(asio::io_context& ioc, Config cfg)
        : ioc_(ioc), cfg_(std::move(cfg)), running_(true),
          worker_pool_(cfg_.pool_size)
    {
        for (size_t i = 0; i < cfg_.pool_size; ++i) {
            auto conn = create_connection();
            if (conn) pool_.push(conn);
        }
        LOG_INFO("MySQL pool initialized, connections=", pool_.size());
    }

    ~MysqlPool() { shutdown(); }

    void shutdown() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        worker_pool_.join();
        std::lock_guard lock(mtx_);
        while (!pool_.empty()) {
            mysql_close(pool_.front());
            pool_.pop();
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
    Result do_query(const char* sql) {
        MYSQL* conn = acquire();
        if (!conn) return {false, "no available connection", ""};

        if (mysql_query(conn, sql)) {
            std::string err = mysql_error(conn);
            mysql_close(conn);
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

    MYSQL* create_connection() {
        MYSQL* conn = mysql_init(nullptr);
        if (!conn) { LOG_ERROR("MySQL init failed"); return nullptr; }
        if (!mysql_real_connect(conn, cfg_.host.c_str(), cfg_.user.c_str(),
            cfg_.pass.c_str(), cfg_.db.c_str(), cfg_.port, nullptr, 0)) {
            LOG_ERROR("MySQL connect failed: ", mysql_error(conn));
            mysql_close(conn);
            return nullptr;
        }
        return conn;
    }

    MYSQL* acquire() {
        std::unique_lock lock(mtx_);
        cv_.wait(lock, [&]() { return !pool_.empty() || !running_; });
        if (!running_ || pool_.empty()) return nullptr;
        auto conn = pool_.front();
        pool_.pop();
        return conn;
    }

    void release(MYSQL* conn) {
        if (!conn) return;
        std::lock_guard lock(mtx_);
        pool_.push(conn);
        cv_.notify_one();
    }

    std::atomic<bool> running_;
    asio::io_context& ioc_;
    Config cfg_;
    std::queue<MYSQL*> pool_;
    std::mutex mtx_;
    std::condition_variable cv_;
    asio::thread_pool worker_pool_;
};
