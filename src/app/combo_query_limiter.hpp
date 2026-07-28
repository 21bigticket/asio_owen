#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace combo_query_detail {

struct Counter {
    explicit Counter(size_t max_in_flight) : max_in_flight(max_in_flight == 0 ? 1 : max_in_flight) {}

    const size_t max_in_flight;
    std::atomic<size_t> in_flight{0};
};

}  // namespace combo_query_detail

class ComboQueryPermit {
public:
    ComboQueryPermit() = default;
    ComboQueryPermit(const ComboQueryPermit&) = delete;
    ComboQueryPermit& operator=(const ComboQueryPermit&) = delete;

    ComboQueryPermit(ComboQueryPermit&& other) noexcept
        : counter_(std::exchange(other.counter_, {})) {}

    ComboQueryPermit& operator=(ComboQueryPermit&& other) noexcept {
        if (this != &other) {
            release();
            counter_ = std::exchange(other.counter_, {});
        }
        return *this;
    }

    ~ComboQueryPermit() {
        release();
    }

    void release() noexcept {
        auto counter = std::exchange(counter_, {});
        if (counter) {
            counter->in_flight.fetch_sub(1, std::memory_order_relaxed);
        }
    }

private:
    friend class ComboQueryLimiter;

    explicit ComboQueryPermit(std::shared_ptr<combo_query_detail::Counter> counter)
        : counter_(std::move(counter)) {}

    std::shared_ptr<combo_query_detail::Counter> counter_;
};

class ComboQueryLimiter {
public:
    // This cap is deliberately independent of MysqlPool::max_size. It bounds cache-miss
    // fallback work while a slow MySQL dependency is unhealthy.
    explicit ComboQueryLimiter(size_t max_in_flight)
        : counter_(std::make_shared<combo_query_detail::Counter>(max_in_flight)) {}

    std::optional<ComboQueryPermit> try_acquire() const {
        size_t current = counter_->in_flight.load(std::memory_order_relaxed);
        while (current < counter_->max_in_flight) {
            if (counter_->in_flight.compare_exchange_weak(
                    current, current + 1, std::memory_order_relaxed)) {
                return ComboQueryPermit(counter_);
            }
        }
        return std::nullopt;
    }

    size_t max_in_flight() const noexcept {
        return counter_->max_in_flight;
    }

private:
    std::shared_ptr<combo_query_detail::Counter> counter_;
};

class ComboQueryState {
public:
    explicit ComboQueryState(ComboQueryPermit permit) : permit_(std::move(permit)) {}

    bool claim_query() {
        std::lock_guard lock(mtx_);
        if (winner_ == Winner::Pending) {
            winner_ = Winner::Query;
            permit_.release();
            return true;
        }
        if (winner_ == Winner::Timeout) {
            permit_.release();
        }
        return false;
    }

    bool claim_timeout() {
        std::lock_guard lock(mtx_);
        if (winner_ != Winner::Pending) return false;
        winner_ = Winner::Timeout;
        return true;
    }

private:
    enum class Winner { Pending, Query, Timeout };

    std::mutex mtx_;
    Winner winner_ = Winner::Pending;
    ComboQueryPermit permit_;
};
