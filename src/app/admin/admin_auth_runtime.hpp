#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <memory>

class AdminLoginThrottle {
public:
    bool locked(const std::string& client_ip) {
        std::lock_guard<std::mutex> lock(mu_);
        sweep_expired_locked();
        auto it = entries_.find(client_ip);
        return it != entries_.end() && it->second.locked_until > Clock::now();
    }

    void record_failure(const std::string& client_ip) {
        std::lock_guard<std::mutex> lock(mu_);
        sweep_expired_locked();
        auto& entry = entries_[client_ip];
        ++entry.failures;
        if (entry.failures >= kMaxFailures) {
            entry.locked_until = Clock::now() + kLockDuration;
        }
    }

    void record_success(const std::string& client_ip) {
        std::lock_guard<std::mutex> lock(mu_);
        entries_.erase(client_ip);
    }

    size_t locked_entries() const {
        std::lock_guard<std::mutex> lock(mu_);
        const auto now = Clock::now();
        size_t count = 0;
        for (const auto& [unused_ip, entry] : entries_) {
            if (entry.locked_until > now) ++count;
        }
        return count;
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr int kMaxFailures = 5;
    static constexpr auto kLockDuration = std::chrono::minutes(15);
    static constexpr size_t kMaxEntries = 4096;

    struct Entry {
        int failures = 0;
        Clock::time_point locked_until{};
    };

    void sweep_expired_locked() {
        if (entries_.size() <= kMaxEntries) return;
        const auto now = Clock::now();
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.locked_until <= now) it = entries_.erase(it);
            else ++it;
        }
        while (entries_.size() > kMaxEntries) entries_.erase(entries_.begin());
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> entries_;
};

class AdminAuthWorkLimiter {
public:
    static constexpr size_t kMaxInFlight = 16;

    bool try_acquire() {
        size_t current = in_flight_.load(std::memory_order_relaxed);
        while (current < kMaxInFlight) {
            if (in_flight_.compare_exchange_weak(
                    current, current + 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void release() { in_flight_.fetch_sub(1, std::memory_order_acq_rel); }
    size_t in_flight() const { return in_flight_.load(std::memory_order_relaxed); }

private:
    std::atomic<size_t> in_flight_{0};
};
