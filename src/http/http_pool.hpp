#pragma once
#include <asio.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <string>
#include <deque>
#include <mutex>
#include <atomic>
#include <chrono>
#include <optional>
#include <unordered_set>
#include <memory>
#include <sstream>
#include <cassert>
#include "../common/logger.hpp"

// HTTP connection pool: lazy create + idle reclaim + hard limit
// Uses sharded locking (kShards independent mutexes) to reduce contention.
class HttpPool {
public:
    struct Config {
        size_t max_size = 256;                  // pool-level: hard cap on total connections
        size_t max_concurrent = 0;              // pool-level: 0 means unlimited
        size_t max_body_size = 10 * 1024 * 1024;  // consumed by proxy layer, not pool
        int connect_timeout_ms = 1000;          // pool-level: DNS resolve + TCP connect
        int read_timeout_ms = 30000;            // pool-level: upstream read timeout
        int request_timeout_ms = 60000;         // pool-level: upstream write timeout
        int idle_timeout_sec = 60;              // pool-level: evict idle connections
        bool send_keep_alive_header = false;    // consumed by proxy layer, not pool
        const Config& ref() const { return *this; }
    };

    struct HttpConn {
        asio::ip::tcp::socket socket;
        std::chrono::steady_clock::time_point last_used_at;
        bool connection_close = false;
        bool reused_from_idle = false;
        std::string read_buffer;
        size_t shard_idx = 0;  // which shard this connection belongs to

        HttpConn() = delete;
        explicit HttpConn(asio::io_context& ioc) : socket(ioc) {}
        HttpConn(HttpConn&&) = default;
        HttpConn& operator=(HttpConn&&) = default;
    };

    static constexpr size_t kShards = 16;

    struct Shard {
        std::mutex mtx;
        std::deque<HttpConn> idle;
        std::unordered_set<HttpConn*> active;
        size_t total = 0;
        size_t in_flight = 0;
    };

    struct State {
        asio::io_context& ioc;
        Config cfg;
        std::atomic<bool> running{true};
        Shard shards[kShards];
        std::atomic<size_t> total_count{0};
        std::atomic<size_t> in_flight_count{0};
        // Global counters (atomic, no lock needed)
        std::atomic<size_t> acquire_reused{0};
        std::atomic<size_t> acquire_created{0};
        std::atomic<size_t> released_idle{0};
        std::atomic<size_t> released_closed{0};
        std::atomic<size_t> released_bad{0};
        // Throttle for cross-shard eviction sweep (avoids per-acquire overhead).
        std::atomic<int64_t> last_global_evict_ms{0};

        State(asio::io_context& io, Config c) : ioc(io), cfg(std::move(c)) {}

        ~State() {
            for (auto& shard : shards) {
                std::lock_guard lock(shard.mtx);
                while (!shard.idle.empty()) {
                    asio::error_code ec;
                    shard.idle.front().socket.cancel(ec);
                    shard.idle.front().socket.close(ec);
                    shard.idle.pop_front();
                }
                for (auto* conn : shard.active) {
                    asio::error_code ec;
                    conn->socket.cancel(ec);
                    conn->socket.close(ec);
                }
            }
        }

        // Round-robin shard selection (per-thread)
        static size_t pick_shard() {
            static thread_local size_t idx = 0;
            idx = (idx + 1) % kShards;
            return idx;
        }
    };

    HttpPool(asio::io_context& ioc, Config cfg)
        : state_(std::make_shared<State>(ioc, std::move(cfg))) {}

    ~HttpPool() { shutdown(); }

    void shutdown() {
        auto state = state_;
        if (!state->running.exchange(false)) return;
        for (auto& shard : state->shards) {
            std::lock_guard lock(shard.mtx);
            while (!shard.idle.empty()) {
                asio::error_code ec;
                shard.idle.front().socket.cancel(ec);
                shard.idle.front().socket.close(ec);
                shard.idle.pop_front();
                --shard.total;
                decrement_counter(state->total_count);
            }
            for (auto* conn : shard.active) {
                asio::error_code ec;
                conn->socket.cancel(ec);
                conn->socket.close(ec);
            }
        }
    }

    void track_active(HttpConn* conn) {
        if (!conn) return;
        auto& shard = state_->shards[conn->shard_idx];
        std::lock_guard lock(shard.mtx);
        shard.active.insert(conn);
    }

    void untrack_active(HttpConn* conn) {
        if (!conn) return;
        auto& shard = state_->shards[conn->shard_idx];
        std::lock_guard lock(shard.mtx);
        shard.active.erase(conn);
    }

    static void untrack_active(const std::shared_ptr<State>& state, HttpConn* conn) {
        if (!conn) return;
        auto& shard = state->shards[conn->shard_idx];
        std::lock_guard lock(shard.mtx);
        shard.active.erase(conn);
    }

    std::shared_ptr<State> state() const { return state_; }

    asio::awaitable<std::unique_ptr<HttpConn>> acquire(const std::string& host, int port) {
        auto state = state_;
        if (!state->running) co_return nullptr;

        // Throttled global eviction sweep: at most once per second across all callers,
        // walk all shards and evict stale idle connections. Without this, idle conns
        // parked in shards that nobody acquires from (e.g. after a traffic shift)
        // linger until idle_timeout_sec * shard_traffic, leaking fds.
        {
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t last = state->last_global_evict_ms.load(std::memory_order_relaxed);
            if (now_ms - last >= 1000 &&
                state->last_global_evict_ms.compare_exchange_strong(
                    last, now_ms, std::memory_order_relaxed)) {
                for (auto& s : state->shards) {
                    std::lock_guard lock(s.mtx);
                    evict_stale_idle(state, s, state->cfg);
                }
            }
        }

        // Reserve in-flight slot once at entry, instead of inside the idle-pop loop.
        // This fixes a bug where max_concurrent check in the while-loop on the first
        // shard would return nullptr even if other shards have available idle connections.
        bool reserved_in_flight = false;
        if (state->cfg.max_concurrent > 0) {
            if (!try_increment_counter(state->in_flight_count, state->cfg.max_concurrent)) {
                LOG_WARN("HttpPool: max_concurrent reached");
                co_return nullptr;
            }
            reserved_in_flight = true;
        }

        size_t start_shard = State::pick_shard();

        // Step 1: Try to pop an idle connection from any shard.
        std::unique_ptr<HttpConn> idle_conn;
        size_t shard_idx = start_shard;
        for (size_t try_idx = 0; try_idx < kShards && !idle_conn; ++try_idx) {
            shard_idx = (start_shard + try_idx) % kShards;
            auto& shard = state->shards[shard_idx];
            std::lock_guard lock(shard.mtx);
            evict_stale_idle(state, shard, state->cfg);
            while (!shard.idle.empty()) {
                idle_conn = std::make_unique<HttpConn>(std::move(shard.idle.front()));
                shard.idle.pop_front();
                if (idle_conn->connection_close) {
                    asio::error_code ec;
                    idle_conn->socket.close(ec);
                    --shard.total;
                    decrement_counter(state->total_count);
                    idle_conn.reset();
                    continue;
                }
                if (!idle_conn->read_buffer.empty()) {
                    asio::error_code ec;
                    idle_conn->socket.close(ec);
                    --shard.total;
                    decrement_counter(state->total_count);
                    idle_conn.reset();
                    continue;
                }
                ++shard.in_flight;
                break;
            }
        }

        if (idle_conn) {
            auto& shard = state->shards[idle_conn->shard_idx];
            idle_conn->reused_from_idle = true;
            {
                std::lock_guard lock(shard.mtx);
                shard.active.insert(idle_conn.get());
            }
            state->acquire_reused.fetch_add(1, std::memory_order_relaxed);
            co_return std::move(idle_conn);
        }

        // No reusable idle connection -> create new one.
        // in_flight slot already reserved above.
        if (!try_increment_counter(state->total_count, state->cfg.max_size)) {
            if (reserved_in_flight) decrement_counter(state->in_flight_count);
            LOG_WARN("HttpPool: max_size reached");
            co_return nullptr;
        }
        bool reserved_total = true;

        std::unique_ptr<HttpConn> new_conn;
        {
            shard_idx = start_shard;
            auto& shard = state->shards[shard_idx];
            std::lock_guard lock(shard.mtx);
            ++shard.total;
            ++shard.in_flight;
            new_conn = std::make_unique<HttpConn>(state->ioc);
            new_conn->shard_idx = shard_idx;
            shard.active.insert(new_conn.get());
        }

        try {
            asio::ip::tcp::resolver resolver(state->ioc);
            auto eps = co_await resolve_with_timeout(
                resolver, host, std::to_string(port), state->cfg.connect_timeout_ms);
            if (!eps) {
                throw std::runtime_error("resolve timeout");
            }
            if (!co_await connect_with_timeout(new_conn->socket, *eps, state->cfg.connect_timeout_ms)) {
                throw std::runtime_error("connect timeout");
            }
            new_conn->last_used_at = std::chrono::steady_clock::now();
            new_conn->reused_from_idle = false;
            state->acquire_created.fetch_add(1, std::memory_order_relaxed);
            co_return std::move(new_conn);
        } catch (...) {
            auto& failed_shard = state->shards[new_conn->shard_idx];
            {
                std::lock_guard lock(failed_shard.mtx);
                failed_shard.active.erase(new_conn.get());
                --failed_shard.total;
                --failed_shard.in_flight;
            }
            if (reserved_total) decrement_counter(state->total_count);
            if (reserved_in_flight) decrement_counter(state->in_flight_count);
            throw;
        }
    }

    void release(HttpConn conn) {
        release(state_, std::make_unique<HttpConn>(std::move(conn)));
    }

    static void release(const std::shared_ptr<State>& state, std::unique_ptr<HttpConn> conn) {
        auto shard_idx = conn->shard_idx;
        auto& shard = state->shards[shard_idx];

        if (!conn->socket.is_open() || conn->connection_close) {
            asio::error_code ec;
            conn->socket.close(ec);
            {
                std::lock_guard lock(shard.mtx);
                --shard.total;
                --shard.in_flight;
                shard.active.erase(conn.get());
            }
            decrement_counter(state->total_count);
            decrement_counter(state->in_flight_count);
            state->released_closed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!conn->read_buffer.empty()) {
            asio::error_code ec;
            conn->socket.close(ec);
            {
                std::lock_guard lock(shard.mtx);
                --shard.total;
                --shard.in_flight;
                shard.active.erase(conn.get());
            }
            decrement_counter(state->total_count);
            decrement_counter(state->in_flight_count);
            state->released_closed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        conn->last_used_at = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(shard.mtx);
            shard.active.erase(conn.get());
            shard.idle.push_back(std::move(*conn));
            --shard.in_flight;
        }
        decrement_counter(state->in_flight_count);
        state->released_idle.fetch_add(1, std::memory_order_relaxed);
    }

    void release_bad(HttpConn conn) {
        release_bad(state_, std::make_unique<HttpConn>(std::move(conn)));
    }

    static void release_bad(const std::shared_ptr<State>& state, std::unique_ptr<HttpConn> conn) {
        auto shard_idx = conn->shard_idx;
        auto& shard = state->shards[shard_idx];
        asio::error_code ec;
        conn->socket.cancel(ec);
        conn->socket.close(ec);
        {
            std::lock_guard lock(shard.mtx);
            --shard.total;
            --shard.in_flight;
            shard.active.erase(conn.get());
        }
        decrement_counter(state->total_count);
        decrement_counter(state->in_flight_count);
        state->released_bad.fetch_add(1, std::memory_order_relaxed);
    }

    const Config& cfg() const { return state_->cfg; }

    // Evict idle connections that have exceeded idle_timeout_sec.
    // Called periodically by pool_stats_service to prevent fd accumulation
    // during traffic peaks. Also called inline during acquire.
    // Returns number of idle connections remaining after eviction.
    size_t evict_stale() {
        auto state = state_;
        size_t idle_total = 0;
        for (auto& shard : state->shards) {
            std::lock_guard lock(shard.mtx);
            evict_stale_idle(state, shard, state->cfg);
            idle_total += shard.idle.size();
        }
        return idle_total;
    }

    std::string stats() const {
        auto state = state_;
        size_t total = 0, idle = 0, active = 0, in_flight = 0;
        for (auto& shard : state->shards) {
            std::lock_guard lock(shard.mtx);
            total += shard.total;
            idle += shard.idle.size();
            active += shard.active.size();
            in_flight += shard.in_flight;
        }
        std::ostringstream oss;
        oss << "total=" << total
            << ", idle=" << idle
            << ", active=" << active
            << ", in_flight=" << in_flight
            << ", reused=" << state->acquire_reused.load(std::memory_order_relaxed)
            << ", created=" << state->acquire_created.load(std::memory_order_relaxed)
            << ", released_idle=" << state->released_idle.load(std::memory_order_relaxed)
            << ", released_closed=" << state->released_closed.load(std::memory_order_relaxed)
            << ", released_bad=" << state->released_bad.load(std::memory_order_relaxed);
        return oss.str();
    }

private:
    static bool try_increment_counter(std::atomic<size_t>& counter, size_t limit) {
        size_t cur = counter.load(std::memory_order_relaxed);
        while (cur < limit) {
            if (counter.compare_exchange_weak(
                    cur, cur + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    static void decrement_counter(std::atomic<size_t>& counter) {
        size_t cur = counter.load(std::memory_order_relaxed);
        while (cur > 0) {
            if (counter.compare_exchange_weak(
                    cur, cur - 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return;
            }
        }
    }

    asio::awaitable<std::optional<asio::ip::tcp::resolver::results_type>> resolve_with_timeout(
        asio::ip::tcp::resolver& resolver, const std::string& host,
        const std::string& service, int timeout_ms) {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds(timeout_ms));

        auto [order, resolve_ec, results, timer_ec] = co_await asio::experimental::make_parallel_group(
            [&](auto token) {
                return resolver.async_resolve(host, service, token);
            },
            [&](auto token) {
                return timer.async_wait(token);
            }
        ).async_wait(asio::experimental::wait_for_one(), asio::use_awaitable);

        if ((order[0] == 1 && !timer_ec) || resolve_ec) {
            if (order[0] == 1 && !timer_ec) {
                resolver.cancel();  // explicitly abort outstanding resolve
            }
            co_return std::nullopt;
        }
        co_return results;
    }

    asio::awaitable<bool> connect_with_timeout(
        asio::ip::tcp::socket& socket, const asio::ip::tcp::resolver::results_type& endpoints,
        int timeout_ms) {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds(timeout_ms));

        auto [order, connect_ec, endpoint, timer_ec] = co_await asio::experimental::make_parallel_group(
            [&](auto token) {
                return asio::async_connect(socket, endpoints, token);
            },
            [&](auto token) {
                return timer.async_wait(token);
            }
        ).async_wait(asio::experimental::wait_for_one(), asio::use_awaitable);
        (void)endpoint;

        if (order[0] == 1 && !timer_ec) {
            asio::error_code ignored;
            socket.cancel(ignored);  // abort outstanding async_connect
            socket.close(ignored);
            co_return false;
        }
        co_return !connect_ec;
    }

    static void evict_stale_idle(
        const std::shared_ptr<State>& state, Shard& shard, const Config& cfg) {
        auto now = std::chrono::steady_clock::now();
        while (!shard.idle.empty()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - shard.idle.front().last_used_at).count();
            if (age < cfg.idle_timeout_sec) break;
            asio::error_code ec;
            shard.idle.front().socket.close(ec);
            shard.idle.pop_front();
            --shard.total;
            decrement_counter(state->total_count);
        }
    }

    std::shared_ptr<State> state_;
};
