#pragma once
#include <asio.hpp>
#include <hiredis/hiredis.h>
#include <string>
#include <thread>
#include <cstdarg>
#include "../common/logger.hpp"

// One dedicated Redis connection per io_context thread, lock-free, synchronous hiredis
class RedisPool {
public:
    struct Config {
        std::string host = "127.0.0.1";
        int port = 6379;
        int connect_timeout_ms = 1000;
        int cmd_timeout_ms = 0;    // 0 表示不设命令超时（性能模式），>0 表示毫秒数
    };

    struct Reply {
        bool ok = false;
        std::string error;
        std::string str;
        int64_t integer = 0;
        std::vector<std::string> elements;
        std::string type;
    };

    RedisPool(asio::io_context& ioc, Config cfg)
        : ioc_(ioc), cfg_(std::move(cfg)), running_(true)
    {
        LOG_INFO("RedisPool created, host=", cfg_.host, ":", cfg_.port);
    }

    ~RedisPool() { shutdown(); }

    void shutdown() {
        if (!running_.exchange(false)) return;
        // 清理当前线程的专属连接（其他线程的连接在线程退出时由 unique_ptr 自动释放）
        tls_conn_.reset();
        LOG_INFO("Redis pool shutdown, total_conns=", total_conns_.load());
    }

    // varargs 格式化后用 redisCommand 执行
    // 注意：varargs 函数不能包含 co_return，全部委托给 do_cmd
    // 单次 cmd 失败即返回错误给上层，不重试。连接断开自动重建
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
            return err_awaitable(std::move(r));
        }

        std::string cmdline((size_t)len, '\0');
        vsnprintf(&cmdline[0], len + 1, fmt, ap);
        va_end(ap);

        return do_cmd(std::move(cmdline));
    }

    // 快速路径：直接执行固定命令，跳过 vsnprintf 格式化和 string 分配
    asio::awaitable<Reply> get(const char* key) {
        redisContext* ctx = get_conn();
        if (!ctx) {
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
            co_return Reply{false, std::move(err), "", 0};
        }

        Reply r;
        parse_reply(reply, r);
        freeReplyObject(reply);
        co_return r;
    }

private:
    asio::awaitable<Reply> err_awaitable(Reply r) {
        co_return r;
    }

    asio::awaitable<Reply> do_cmd(std::string cmdline) {
        redisContext* ctx = get_conn();
        if (!ctx) {
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
            co_return Reply{false, std::move(err), "", 0};
        }

        Reply r;
        parse_reply(reply, r);
        freeReplyObject(reply);
        co_return r;
    }

    redisContext* get_conn() {
        // 断线检测或懒创建
        if (!tls_conn_) {
            tls_conn_.reset(create_connection());
        } else if (tls_conn_->err != 0) {
            LOG_INFO("Redis connection broken, rebuilding");
            tls_conn_.reset(create_connection());
        }
        return tls_conn_.get();
    }

    redisContext* create_connection() {
        // 建连超时，有下限保护
        int connect_ms = cfg_.connect_timeout_ms;
        if (connect_ms < 100) connect_ms = 100;
        struct timeval tv = {connect_ms / 1000, (connect_ms % 1000) * 1000};
        redisContext* ctx = redisConnectWithTimeout(cfg_.host.c_str(), cfg_.port, tv);
        if (!ctx || ctx->err) {
            std::string err = ctx ? ctx->errstr : "allocation failed";
            LOG_ERROR("Redis connect failed: ", err);
            if (ctx) redisFree(ctx);
            return nullptr;
        }

        // 命令读写超时（可选，默认 0 不设）
        if (cfg_.cmd_timeout_ms > 0) {
            int cmd_ms = cfg_.cmd_timeout_ms;
            if (cmd_ms < 100) cmd_ms = 100;
            struct timeval cmd_tv = {cmd_ms / 1000, (cmd_ms % 1000) * 1000};
            redisSetTimeout(ctx, cmd_tv);
        }

        ++total_conns_;
        LOG_INFO("Redis connected (total_conns=", total_conns_.load(), ")");
        return ctx;
    }

    static void parse_reply(redisReply* reply, Reply& r) {
        switch (reply->type) {
            case REDIS_REPLY_STRING:
                r.type = "string";
                r.str.assign(reply->str, reply->len);
                r.ok = true;
                break;
            case REDIS_REPLY_INTEGER:
                r.type = "integer";
                r.integer = reply->integer;
                r.ok = true;
                break;
            case REDIS_REPLY_ARRAY:
                r.type = "array";
                r.ok = true;
                for (size_t i = 0; i < reply->elements; ++i) {
                    if (reply->element[i]->type == REDIS_REPLY_STRING || reply->element[i]->type == REDIS_REPLY_STATUS) {
                        r.elements.emplace_back(reply->element[i]->str, reply->element[i]->len);
                    } else if (reply->element[i]->type == REDIS_REPLY_INTEGER) {
                        r.elements.push_back(std::to_string(reply->element[i]->integer));
                    } else if (reply->element[i]->type == REDIS_REPLY_NIL) {
                        r.elements.push_back("(nil)");
                    } else {
                        r.elements.push_back("(unknown)");
                    }
                }
                break;
            case REDIS_REPLY_STATUS:
                r.type = "string";
                r.str.assign(reply->str, reply->len);
                r.ok = true;
                break;
            case REDIS_REPLY_ERROR:
                r.type = "error";
                r.error.assign(reply->str, reply->len);
                break;
            case REDIS_REPLY_NIL:
                r.type = "nil";
                r.ok = true;
                break;
            default:
                r.type = "unknown";
                r.ok = true;
                break;
        }
    }

    using RedisPtr = std::unique_ptr<redisContext, decltype(&redisFree)>;

    std::atomic<bool> running_;
    asio::io_context& ioc_;
    Config cfg_;
    std::atomic<size_t> total_conns_{0};

    inline static thread_local RedisPtr tls_conn_{nullptr, redisFree};
};
