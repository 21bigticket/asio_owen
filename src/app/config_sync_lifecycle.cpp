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

ConfigSyncServiceImpl::ConfigSyncServiceImpl(asio::io_context& ioc, RedisPool& redis,
                  std::filesystem::path config_base,
                  ConfigSyncConfig cfg, AppConfig running_app_cfg,
                  asio::thread_pool* file_workers,
                  std::shared_ptr<ConfigSyncRuntimeMetrics> metrics)
    : ConfigSyncServiceImpl(
          ioc,
          [&ioc, &redis](std::vector<std::string> args,
                         CommandCompletion completion) {
              auto done = std::make_shared<CommandCompletion>(std::move(completion));
              try {
                  co_spawn(ioc, redis.cmd_argv(std::move(args)),
                      [done](const std::exception_ptr& ep, Reply reply) mutable {
                          if (ep) {
                              reply = ConfigSyncServiceImpl::exception_reply(ep);
                          }
                          (*done)(std::move(reply));
                      });
              } catch (...) {
                  (*done)(ConfigSyncServiceImpl::exception_reply(std::current_exception()));
              }
          },
          std::move(config_base), std::move(cfg), std::move(running_app_cfg),
          file_workers, std::move(metrics)) {}

ConfigSyncServiceImpl::ConfigSyncServiceImpl(asio::io_context& ioc, Command command,
                  std::filesystem::path config_base,
                  ConfigSyncConfig cfg, AppConfig running_app_cfg,
                  asio::thread_pool* file_workers,
                  std::shared_ptr<ConfigSyncRuntimeMetrics> metrics)
    : ioc_(ioc),
      timer_(ioc),
      command_(std::move(command)),
      config_base_(std::move(config_base)),
      cfg_(ConfigSyncServiceImpl::normalize_config(std::move(cfg))),
      running_app_cfg_(std::move(running_app_cfg)),
      file_workers_(file_workers), metrics_(std::move(metrics)) {}

void ConfigSyncServiceImpl::start() {
    if (!cfg_.enabled || cfg_.sync_interval_sec <= 0) return;
    running_.store(true, std::memory_order_release);
    ConfigSyncServiceImpl::schedule_after(std::chrono::milliseconds(0));
}

void ConfigSyncServiceImpl::stop() {
    running_.store(false, std::memory_order_release);
    try {
        timer_.cancel();
    } catch (...) {
    }

    if (ioc_.stopped()) return;

    std::unique_lock lock(in_flight_mu_);
    const auto wait_ms = std::chrono::milliseconds(ConfigSyncServiceImpl::effective_drain_timeout_ms());
    if (!in_flight_cv_.wait_for(lock, wait_ms, [this] { return in_flight_ == 0; })) {
        try {
            LOG_ERROR("ConfigSyncService stop timed out with in_flight=", in_flight_);
        } catch (...) {
        }
    }
}

void ConfigSyncServiceImpl::sync_once_for_test(Completion completion) {
    ConfigSyncServiceImpl::sync_once(std::move(completion));
}

bool ConfigSyncServiceImpl::blocking_first_pull(const std::filesystem::path& config_base,
                                const ConfigSyncConfig& sync_cfg,
                                RedisPool::Config redis_cfg,
                                const AppConfig& running_app_cfg) {
    if (!sync_cfg.enabled || sync_cfg.first_pull != "blocking") return true;

    const int total_ms = std::max(100, sync_cfg.first_pull_timeout_ms);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(total_ms);
    redis_cfg.mode = RedisPool::Mode::Direct;
    redis_cfg.min_size = 0;
    redis_cfg.max_size = 1;
    redis_cfg.worker_threads = 1;
    redis_cfg.connect_timeout_ms = std::max(100, std::min(1000, total_ms / 3));
    redis_cfg.cmd_timeout_ms = std::max(100, std::min(500, total_ms));

    asio::io_context pull_ioc;
    bool result = false;
    try {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        RedisPool temp_pool(pull_ioc, redis_cfg);
        Command command = [&pull_ioc, &temp_pool, deadline](
                              std::vector<std::string> args,
                              CommandCompletion completion) {
            if (std::chrono::steady_clock::now() >= deadline) {
                Reply reply;
                reply.ok = false;
                reply.type = "error";
                reply.error = "config sync blocking first_pull deadline exceeded";
                completion(std::move(reply));
                return;
            }
            auto done = std::make_shared<CommandCompletion>(std::move(completion));
            try {
                co_spawn(pull_ioc, temp_pool.cmd_argv(std::move(args)),
                    [done](const std::exception_ptr& ep, Reply reply) mutable {
                        if (ep) {
                            reply = ConfigSyncServiceImpl::exception_reply(ep);
                        }
                        (*done)(std::move(reply));
                    });
            } catch (...) {
                (*done)(ConfigSyncServiceImpl::exception_reply(std::current_exception()));
            }
        };
        auto service = std::make_shared<ConfigSyncServiceImpl>(
            pull_ioc, std::move(command), config_base, sync_cfg, running_app_cfg);
        service->ConfigSyncServiceImpl::sync_once_for_test([&](bool ok) { result = ok; });
        pull_ioc.run();
        temp_pool.shutdown();
    } catch (const std::exception& e) {
        try {
            LOG_WARN("ConfigSync blocking first pull failed before run: ", e.what());
        } catch (...) {
        }
        return false;
    }
    return result;
}

ConfigSyncConfig ConfigSyncServiceImpl::normalize_config(ConfigSyncConfig cfg) {
    if (cfg.sync_interval_sec <= 0) cfg.sync_interval_sec = 5;
    if (cfg.first_pull.empty()) cfg.first_pull = "async";
    if (cfg.first_pull != "async" && cfg.first_pull != "blocking") {
        LOG_WARN("invalid config_sync.first_pull '", cfg.first_pull, "', using async");
        cfg.first_pull = "async";
    }
    if (cfg.first_pull_timeout_ms <= 0) cfg.first_pull_timeout_ms = 3000;
    return cfg;
}


ConfigSyncServiceImpl::TickCompletion::TickCompletion(
    std::shared_ptr<ConfigSyncServiceImpl> service)
    : service_(std::move(service)) {}

ConfigSyncServiceImpl::TickCompletion::~TickCompletion() {
    finish();
}

void ConfigSyncServiceImpl::TickCompletion::finish() noexcept {
    if (finished_.exchange(true, std::memory_order_acq_rel)) return;
    auto service = std::move(service_);
    if (!service) return;
    try {
        service->finish_tick();
    } catch (const std::exception& e) {
        try {
            LOG_ERROR("ConfigSync tick completion failed: ", e.what());
        } catch (...) {
        }
    } catch (...) {
        try {
            LOG_ERROR("ConfigSync tick completion failed with unknown exception");
        } catch (...) {
        }
    }
}
