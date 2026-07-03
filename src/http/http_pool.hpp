#pragma once
#include <asio.hpp>
#include <string>
#include <deque>
#include <mutex>
#include <atomic>
#include <chrono>
#include <optional>
#include <unordered_set>
#include <memory>
#include "../common/logger.hpp"

// HTTP 连接池：懒创建 + 空闲回收 + 硬上限
class HttpPool {
public:
    struct Config {
        size_t max_size = 256;
        size_t max_concurrent = 0;   // 0 表示不限制
        size_t max_body_size = 10 * 1024 * 1024;  // 10MB
        int connect_timeout_ms = 1000;
        int read_timeout_ms = 30000;
        int request_timeout_ms = 60000;
        int idle_timeout_sec = 60;
        const Config& ref() const { return *this; }
    };

    struct HttpConn {
        asio::ip::tcp::socket socket;
        std::chrono::steady_clock::time_point last_used_at;
        bool connection_close = false;
        bool reused_from_idle = false;
        std::string read_buffer;

        HttpConn() = delete;
        explicit HttpConn(asio::io_context& ioc) : socket(ioc) {}
        HttpConn(HttpConn&&) = default;
        HttpConn& operator=(HttpConn&&) = default;
    };

    struct State {
        asio::io_context& ioc;
        Config cfg;
        std::atomic<bool> running{true};
        std::deque<HttpConn> idle;
        std::unordered_set<HttpConn*> active;
        size_t total = 0;
        size_t in_flight = 0;
        std::mutex mtx;

        State(asio::io_context& io, Config c) : ioc(io), cfg(std::move(c)) {}

        ~State() {
            std::lock_guard lock(mtx);
            while (!idle.empty()) {
                asio::error_code ec;
                idle.front().socket.cancel(ec);
                idle.front().socket.close(ec);
                idle.pop_front();
            }
            for (auto* conn : active) {
                asio::error_code ec;
                conn->socket.cancel(ec);
                conn->socket.close(ec);
            }
        }
    };

    HttpPool(asio::io_context& ioc, Config cfg)
        : state_(std::make_shared<State>(ioc, std::move(cfg))) {}

    ~HttpPool() { shutdown(); }

    void shutdown() {
        auto state = state_;
        if (!state->running.exchange(false)) return;
        std::lock_guard lock(state->mtx);
        // 关闭 idle 连接
        while (!state->idle.empty()) {
            asio::error_code ec;
            state->idle.front().socket.cancel(ec);
            state->idle.front().socket.close(ec);
            state->idle.pop_front();
            --state->total;
        }
        // 关闭 in-flight 连接
        for (auto* conn : state->active) {
            asio::error_code ec;
            conn->socket.cancel(ec);
            conn->socket.close(ec);
        }
    }

    void track_active(HttpConn* conn) {
        track_active(state_, conn);
    }

    void untrack_active(HttpConn* conn) {
        untrack_active(state_, conn);
    }

    std::shared_ptr<State> state() const { return state_; }

    static void track_active(const std::shared_ptr<State>& state, HttpConn* conn) {
        std::lock_guard lock(state->mtx);
        state->active.insert(conn);
    }

    static void untrack_active(const std::shared_ptr<State>& state, HttpConn* conn) {
        std::lock_guard lock(state->mtx);
        state->active.erase(conn);
    }

    asio::awaitable<std::unique_ptr<HttpConn>> acquire(const std::string& host, int port) {
        auto state = state_;
        if (!state->running) co_return nullptr;

        {
            std::lock_guard lock(state->mtx);
            evict_stale_idle(state);
            while (!state->idle.empty()) {
                auto conn = std::make_unique<HttpConn>(std::move(state->idle.front()));
                state->idle.pop_front();
                if (!is_reusable_idle(*conn)) {
                    asio::error_code ec;
                    conn->socket.close(ec);
                    --state->total;
                    continue;
                }
                conn->reused_from_idle = true;
                ++state->in_flight;
                state->active.insert(conn.get());
                co_return std::move(conn);
            }
            if (state->total >= state->cfg.max_size) {
                LOG_WARN("HttpPool: max_size reached");
                co_return nullptr;
            }
            if (state->cfg.max_concurrent > 0 && state->in_flight >= state->cfg.max_concurrent) {
                LOG_WARN("HttpPool: max_concurrent reached");
                co_return nullptr;
            }
            ++state->total;
            ++state->in_flight;
        }

        try {
            auto conn = std::make_unique<HttpConn>(state->ioc);
            asio::ip::tcp::resolver resolver(state->ioc);
            auto eps = co_await resolve_with_timeout(
                resolver, host, std::to_string(port), state->cfg.connect_timeout_ms);
            if (!eps) {
                throw std::runtime_error("resolve timeout");
            }
            if (!co_await connect_with_timeout(conn->socket, *eps, state->cfg.connect_timeout_ms)) {
                throw std::runtime_error("connect timeout");
            }
            conn->last_used_at = std::chrono::steady_clock::now();
            conn->reused_from_idle = false;
            {
                std::lock_guard lock(state->mtx);
                state->active.insert(conn.get());
            }
            co_return std::move(conn);
        } catch (...) {
            std::lock_guard lock(state->mtx);
            --state->total;
            --state->in_flight;
            throw;
        }
    }

    void release(HttpConn conn) {
        release(state_, std::make_unique<HttpConn>(std::move(conn)));
    }

    static void release(const std::shared_ptr<State>& state, std::unique_ptr<HttpConn> conn) {
        if (!conn->socket.is_open() || conn->connection_close) {
            asio::error_code ec;
            conn->socket.close(ec);
            std::lock_guard lock(state->mtx);
            --state->total;
            --state->in_flight;
            return;
        }
        if (conn->read_buffer.size() > 64 * 1024) {
            asio::error_code ec;
            conn->socket.close(ec);
            std::lock_guard lock(state->mtx);
            --state->total;
            --state->in_flight;
            return;
        }
        conn->last_used_at = std::chrono::steady_clock::now();
        std::lock_guard lock(state->mtx);
        state->idle.push_back(std::move(*conn));
        --state->in_flight;
    }

    // 标记为 bad 的连接：直接关闭，不归还池
    void release_bad(HttpConn conn) {
        release_bad(state_, std::make_unique<HttpConn>(std::move(conn)));
    }

    static void release_bad(const std::shared_ptr<State>& state, std::unique_ptr<HttpConn> conn) {
        asio::error_code ec;
        conn->socket.cancel(ec);
        conn->socket.close(ec);
        std::lock_guard lock(state->mtx);
        --state->total;
        --state->in_flight;
    }

    const Config& cfg() const { return state_->cfg; }

private:
    asio::awaitable<std::optional<asio::ip::tcp::resolver::results_type>> resolve_with_timeout(
        asio::ip::tcp::resolver& resolver, const std::string& host,
        const std::string& service, int timeout_ms) {
        auto ex = co_await asio::this_coro::executor;
        auto timed_out = std::make_shared<bool>(false);
        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds(timeout_ms));
        timer.async_wait([timed_out, &resolver](asio::error_code ec) {
            if (!ec) {
                *timed_out = true;
                resolver.cancel();
            }
        });

        asio::error_code ec;
        auto results = co_await resolver.async_resolve(
            host, service, asio::redirect_error(asio::use_awaitable, ec));
        timer.cancel();
        if (*timed_out || ec) {
            co_return std::nullopt;
        }
        co_return results;
    }

    asio::awaitable<bool> connect_with_timeout(
        asio::ip::tcp::socket& socket, const asio::ip::tcp::resolver::results_type& endpoints,
        int timeout_ms) {
        auto ex = co_await asio::this_coro::executor;
        auto timed_out = std::make_shared<bool>(false);
        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds(timeout_ms));
        timer.async_wait([timed_out, &socket](asio::error_code ec) {
            if (!ec) {
                *timed_out = true;
                asio::error_code ignored;
                socket.cancel(ignored);
                socket.close(ignored);
            }
        });

        asio::error_code ec;
        co_await asio::async_connect(
            socket, endpoints, asio::redirect_error(asio::use_awaitable, ec));
        timer.cancel();
        co_return !*timed_out && !ec;
    }

    static void evict_stale_idle(const std::shared_ptr<State>& state) {
        auto now = std::chrono::steady_clock::now();
        while (!state->idle.empty()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - state->idle.front().last_used_at).count();
            if (age < state->cfg.idle_timeout_sec) break;
            asio::error_code ec;
            state->idle.front().socket.close(ec);
            state->idle.pop_front();
            --state->total;
        }
    }

    static bool is_reusable_idle(HttpConn& conn) {
        if (!conn.socket.is_open() || conn.connection_close) return false;
        if (conn.read_buffer.size() > 64 * 1024) return false;
        if (!conn.read_buffer.empty()) return true;

        asio::error_code ec;
        bool was_non_blocking = conn.socket.non_blocking();

        conn.socket.non_blocking(true, ec);
        if (ec) return false;

        char byte = 0;
        size_t n = conn.socket.receive(
            asio::buffer(&byte, 1), asio::socket_base::message_peek, ec);

        asio::error_code restore_ec;
        conn.socket.non_blocking(was_non_blocking, restore_ec);

        if (!ec) {
            return n > 0;
        }
        if (ec == asio::error::would_block || ec == asio::error::try_again) {
            return true;
        }
        return false;
    }

    std::shared_ptr<State> state_;
};
