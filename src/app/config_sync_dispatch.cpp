#include "config_sync_service_impl.hpp"

#include <algorithm>
#include <asio/co_spawn.hpp>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#include <unistd.h>

#include "../common/config.hpp"
#include "../common/logger.hpp"
#include "admin/config_history.hpp"

void ConfigSyncServiceImpl::run_command(std::vector<std::string> args, CommandCompletion completion) {
    auto self = shared_from_this();
    auto done = std::make_shared<CommandCompletion>(std::move(completion));
    auto callback_started = std::make_shared<std::atomic<bool>>(false);
    try {
        command_(std::move(args),
            [self, done, callback_started](Reply reply) mutable {
                self->ConfigSyncServiceImpl::dispatch_command_completion(
                    done, callback_started, std::move(reply));
            });
    } catch (...) {
        if (!callback_started->load(std::memory_order_acquire)) {
            self->ConfigSyncServiceImpl::dispatch_command_completion(
                done, callback_started,
                ConfigSyncServiceImpl::exception_reply(std::current_exception()));
        }
    }
}

void ConfigSyncServiceImpl::dispatch_command_completion(
    std::shared_ptr<CommandCompletion> done,
    std::shared_ptr<std::atomic<bool>> callback_started,
    Reply reply) {
    auto invoke =
        [done = std::move(done),
         callback_started = std::move(callback_started),
         reply = std::move(reply)]() mutable {
            callback_started->store(true, std::memory_order_release);
            try {
                (*done)(std::move(reply));
            } catch (const std::exception& e) {
                try {
                    LOG_ERROR("ConfigSync command callback failed: ", e.what());
                } catch (...) {
                }
            } catch (...) {
                try {
                    LOG_ERROR(
                        "ConfigSync command callback failed with unknown exception");
                } catch (...) {
                }
            }
        };
    auto metrics = metrics_;
    const auto enqueued = std::chrono::steady_clock::now();
    auto instrumented = [metrics = std::move(metrics),
                         invoke = std::move(invoke), enqueued]() mutable {
        if (metrics) {
            const auto delay = static_cast<uint64_t>(std::max<int64_t>(0,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - enqueued).count()));
            metrics->file_worker_queue_delay_us_total.fetch_add(
                delay, std::memory_order_relaxed);
            auto old = metrics->file_worker_queue_delay_us_max.load(
                std::memory_order_relaxed);
            while (old < delay &&
                   !metrics->file_worker_queue_delay_us_max.compare_exchange_weak(
                       old, delay, std::memory_order_relaxed)) {
            }
        }
        if (metrics) metrics->file_worker_busy.fetch_add(1, std::memory_order_relaxed);
        invoke();
        if (metrics) {
            metrics->file_worker_busy.fetch_sub(1, std::memory_order_relaxed);
        }
    };
    if (!file_workers_) {
        if (metrics) metrics->file_worker_fallbacks.fetch_add(1, std::memory_order_relaxed);
        instrumented();
        return;
    }
    try {
        if (metrics) metrics->file_worker_jobs.fetch_add(1, std::memory_order_relaxed);
        asio::post(*file_workers_, std::move(instrumented));
    } catch (...) {
        if (metrics) metrics->file_worker_fallbacks.fetch_add(1, std::memory_order_relaxed);
        instrumented();
    }
}

ConfigSyncServiceImpl::Reply ConfigSyncServiceImpl::exception_reply(
    const std::exception_ptr& ep) {
    Reply reply;
    reply.ok = false;
    reply.type = "error";
    reply.error = "unknown Redis command exception";
    if (!ep) return reply;
    try {
        std::rethrow_exception(ep);
    } catch (const std::exception& e) {
        reply.error = e.what();
    } catch (...) {
    }
    return reply;
}

void ConfigSyncServiceImpl::warn_startup_drift_once(int64_t version) {
    {
        std::lock_guard lock(drift_mu_);
        if (drift_warned_versions_.count(version)) return;
        drift_warned_versions_.insert(version);
    }

    Config cfg;
    if (!cfg.load(config_base_)) return;
    try {
        auto current = app_config_from(cfg);
        if (ConfigSyncServiceImpl::startup_config_differs(current)) {
            LOG_WARN("startup config drift detected, restart required, version=", version);
        }
    } catch (const std::exception& e) {
        LOG_WARN("ConfigSync startup drift check failed: ", e.what());
    }
}
