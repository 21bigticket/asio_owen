#pragma once

#include <atomic>
#include <cstdint>

class RouteLifecycle {
public:
    enum class Phase : uint8_t { Running, Draining, Stopped };

    class HandlerLease {
    public:
        HandlerLease() = default;
        explicit HandlerLease(RouteLifecycle* owner) : owner_(owner) {}
        HandlerLease(const HandlerLease&) = delete;
        HandlerLease& operator=(const HandlerLease&) = delete;
        HandlerLease(HandlerLease&& other) noexcept : owner_(other.owner_) {
            other.owner_ = nullptr;
        }
        HandlerLease& operator=(HandlerLease&& other) noexcept {
            if (this != &other) {
                release();
                owner_ = other.owner_;
                other.owner_ = nullptr;
            }
            return *this;
        }
        ~HandlerLease() { release(); }

        explicit operator bool() const noexcept { return owner_ != nullptr; }

        void release() noexcept {
            if (owner_) {
                owner_->active_handlers_.fetch_sub(1, std::memory_order_acq_rel);
                owner_ = nullptr;
            }
        }

    private:
        RouteLifecycle* owner_ = nullptr;
    };

    HandlerLease enter_handler() {
        if (phase() != Phase::Running) return {};
        active_handlers_.fetch_add(1, std::memory_order_acq_rel);
        if (phase() != Phase::Running) {
            active_handlers_.fetch_sub(1, std::memory_order_acq_rel);
            return {};
        }
        return HandlerLease(this);
    }

    bool begin_draining() {
        auto expected = Phase::Running;
        return phase_.compare_exchange_strong(
            expected, Phase::Draining, std::memory_order_acq_rel);
    }

    void mark_stopped() {
        phase_.store(Phase::Stopped, std::memory_order_release);
    }

    Phase phase() const { return phase_.load(std::memory_order_acquire); }
    bool draining() const { return phase() != Phase::Running; }
    uint64_t active_handlers() const {
        return active_handlers_.load(std::memory_order_acquire);
    }

private:
    std::atomic<Phase> phase_{Phase::Running};
    std::atomic<uint64_t> active_handlers_{0};
};
