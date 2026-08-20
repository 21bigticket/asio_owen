#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include <asio.hpp>

#include "admin/admin_auth_runtime.hpp"
#include "admin/admin_credential_store.hpp"
#include "shutdown_coordinator.hpp"
#include "../db/mysql_pool.hpp"
#include "../db/redis_pool.hpp"

struct ConfigSyncRuntimeMetrics;

struct AdminRuntimeMetrics;

class RouteRuntime : public std::enable_shared_from_this<RouteRuntime> {
public:
    enum class Phase : uint8_t { Running, Draining, Stopped };

    class HandlerLease {
    public:
        HandlerLease() = default;
        explicit HandlerLease(RouteRuntime* owner) : owner_(owner) {}
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
        RouteRuntime* owner_ = nullptr;
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
        const bool transitioned = phase_.compare_exchange_strong(
            expected, Phase::Draining, std::memory_order_acq_rel);
        if (shutdown) shutdown->begin_draining();
        return transitioned;
    }

    void mark_stopped() {
        phase_.store(Phase::Stopped, std::memory_order_release);
        if (shutdown) shutdown->mark_stopped();
    }

    Phase phase() const { return phase_.load(std::memory_order_acquire); }
    bool draining() const { return phase() != Phase::Running; }
    uint64_t active_handlers() const {
        return active_handlers_.load(std::memory_order_acquire);
    }

    std::shared_ptr<MysqlPool> mysql;
    std::shared_ptr<RedisPool> redis;
    std::shared_ptr<asio::thread_pool> admin_auth_workers;
    std::shared_ptr<asio::thread_pool> config_file_workers;
    std::shared_ptr<AdminCredentialStore> admin_credentials;
    std::shared_ptr<AdminRuntimeMetrics> admin_metrics;
    std::shared_ptr<AdminLoginThrottle> admin_login_throttle;
    std::shared_ptr<AdminAuthWorkLimiter> admin_auth_limiter;
    std::shared_ptr<ConfigSyncRuntimeMetrics> config_metrics;
    std::shared_ptr<ShutdownCoordinator> shutdown =
        std::make_shared<ShutdownCoordinator>();

private:
    std::atomic<Phase> phase_{Phase::Running};
    std::atomic<uint64_t> active_handlers_{0};
};
