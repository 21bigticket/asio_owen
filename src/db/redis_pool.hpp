#pragma once
#include <asio.hpp>
#include <hiredis/hiredis.h>
#include <string>
#include <thread>
#include "../common/logger.hpp"

// 每 io_context 线程一个专属 Redis 连接，无锁、同步 hiredis
class RedisPool {
public:
    struct Config {
        std::string host = "127.0.0.1";
        int port = 6379;
        size_t pool_size = 64;
        int keepalive_sec = 30;
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
        keepalive_timer_ = std::make_unique<asio::steady_timer>(ioc_);
        start_keepalive();
    }

    ~RedisPool() { shutdown(); }

    void shutdown() {
        if (!running_.exchange(false)) return;
        if (keepalive_timer_) keepalive_timer_->cancel();
        LOG_INFO("Redis pool shutdown");
    }

    // varargs 格式化后用 redisCommand 执行
    // 注意：varargs 函数不能包含 co_return，全部委托给 do_cmd
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

private:
    asio::awaitable<Reply> err_awaitable(Reply r) {
        co_return r;
    }

    asio::awaitable<Reply> do_cmd(std::string cmdline) {
        auto* ctx = get_conn();
        if (!ctx) {
            co_return Reply{false, "no Redis connection", "", 0};
        }

        redisReply* reply = (redisReply*)redisCommand(ctx, cmdline.c_str());
        if (!reply) {
            std::string err = ctx->errstr;
            LOG_WARN("Redis cmd failed: ", err);
            co_return Reply{false, std::move(err), "", 0};
        }

        Reply r;
        parse_reply(reply, r);
        freeReplyObject(reply);
        co_return r;
    }

    using ConnPtr = redisContext*;

    ConnPtr get_conn() {
        auto& ctx = tls_conn_;
        if (ctx) return ctx;
        ctx = redisConnect(cfg_.host.c_str(), cfg_.port);
        if (!ctx || ctx->err) {
            std::string err = ctx ? ctx->errstr : "allocation failed";
            LOG_ERROR("Redis connect failed: ", err);
            if (ctx) redisFree(ctx);
            ctx = nullptr;
        }
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

    void start_keepalive() {
        if (!running_) return;
        keepalive_timer_->expires_after(std::chrono::seconds(cfg_.keepalive_sec));
        keepalive_timer_->async_wait([this](std::error_code) {
            if (!running_) return;
            start_keepalive();
        });
    }

    std::atomic<bool> running_;
    asio::io_context& ioc_;
    Config cfg_;
    std::unique_ptr<asio::steady_timer> keepalive_timer_;
    static thread_local redisContext* tls_conn_;
};

thread_local redisContext* RedisPool::tls_conn_ = nullptr;
