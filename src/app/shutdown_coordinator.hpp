#pragma once

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <memory>

class ShutdownCoordinator : public std::enable_shared_from_this<ShutdownCoordinator> {
public:
    enum class Phase { Running, Draining, Stopped };

    class SessionLease {
    public:
        SessionLease() = default;
        explicit SessionLease(std::shared_ptr<ShutdownCoordinator> owner)
            : owner_(std::move(owner)) {}
        SessionLease(const SessionLease&) = delete;
        SessionLease& operator=(const SessionLease&) = delete;
        SessionLease(SessionLease&&) noexcept = default;
        SessionLease& operator=(SessionLease&& other) noexcept {
            if (this != &other) {
                release();
                owner_ = std::move(other.owner_);
            }
            return *this;
        }
        ~SessionLease() {
            release();
        }

    private:
        void release() noexcept {
            if (owner_) {
                owner_->leave_session();
                owner_.reset();
            }
        }

        std::shared_ptr<ShutdownCoordinator> owner_;
    };

    SessionLease enter_session() {
        active_sessions_.fetch_add(1, std::memory_order_acq_rel);
        return SessionLease(shared_from_this());
    }

    bool begin_draining() {
        Phase expected = Phase::Running;
        return phase_.compare_exchange_strong(
            expected, Phase::Draining, std::memory_order_acq_rel);
    }

    void mark_stopped() {
        phase_.store(Phase::Stopped, std::memory_order_release);
    }

    Phase phase() const { return phase_.load(std::memory_order_acquire); }
    bool draining() const { return phase() != Phase::Running; }
    size_t active_sessions() const {
        return active_sessions_.load(std::memory_order_acquire);
    }

    asio::awaitable<bool> wait_for_drain(
        std::chrono::milliseconds timeout,
        std::chrono::milliseconds poll_interval = std::chrono::milliseconds(10)) {
        auto executor = co_await asio::this_coro::executor;
        asio::steady_timer timer(executor);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (active_sessions() != 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) co_return false;
            timer.expires_at(std::min(deadline, now + poll_interval));
            std::error_code ec;
            co_await timer.async_wait(
                asio::redirect_error(asio::use_awaitable, ec));
            if (ec) co_return false;
        }
        co_return true;
    }

private:
    void leave_session() {
        auto current = active_sessions_.load(std::memory_order_acquire);
        while (current != 0 && !active_sessions_.compare_exchange_weak(
            current, current - 1, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        }
    }

    std::atomic<Phase> phase_{Phase::Running};
    std::atomic<size_t> active_sessions_{0};
};
