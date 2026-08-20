#pragma once

#include <asio.hpp>
#include <asio/co_spawn.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../common/logger.hpp"
#include "../db/redis_pool.hpp"
#include "admin/config_history.hpp"
#include "app_config.hpp"

class ConfigHistoryService :
    public std::enable_shared_from_this<ConfigHistoryService> {
public:
    using Reply = RedisPool::Reply;
    using CommandCompletion = std::function<void(Reply)>;
    using Command =
        std::function<void(std::vector<std::string>, CommandCompletion)>;

    struct Stats {
        uint64_t checks = 0;
        uint64_t inconsistent_checks = 0;
        uint64_t gc_deleted = 0;
        uint64_t gc_failures = 0;
        int64_t max_observed_version = 0;
        bool inconsistent = false;
    };

    ConfigHistoryService(asio::io_context& ioc, RedisPool& redis,
                         ConfigHistoryConfig cfg, int redis_cmd_timeout_ms)
        : ConfigHistoryService(
              ioc,
              [&ioc, &redis](std::vector<std::string> args,
                             CommandCompletion completion) {
                  auto done = std::make_shared<CommandCompletion>(
                      std::move(completion));
                  try {
                      co_spawn(ioc, redis.cmd_argv(std::move(args)),
                          [done](const std::exception_ptr& ep, Reply reply) mutable {
                              if (ep) reply = exception_reply(ep);
                              (*done)(std::move(reply));
                          });
                  } catch (...) {
                      (*done)(exception_reply(std::current_exception()));
                  }
              },
              std::move(cfg), redis_cmd_timeout_ms) {}

    ConfigHistoryService(asio::io_context& ioc, Command command,
                         ConfigHistoryConfig cfg, int redis_cmd_timeout_ms)
        : ioc_(ioc),
          timer_(ioc),
          command_(std::move(command)),
          cfg_(std::move(cfg)),
          redis_cmd_timeout_ms_(redis_cmd_timeout_ms) {}

    void start() {
        running_.store(true, std::memory_order_release);
        schedule_after(std::chrono::milliseconds(0));
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        try {
            timer_.cancel();
        } catch (...) {
        }
        if (ioc_.stopped()) return;
        std::unique_lock lock(in_flight_mu_);
        const int timeout = (redis_cmd_timeout_ms_ <= 0 ? 30000 :
            redis_cmd_timeout_ms_) + 500;
        if (!in_flight_cv_.wait_for(lock, std::chrono::milliseconds(timeout),
                [this] { return !in_flight_; })) {
            LOG_ERROR("ConfigHistoryService stop timed out");
        }
    }

    bool inconsistent() const {
        return inconsistent_.load(std::memory_order_acquire);
    }

    Stats stats() const {
        return Stats{
            .checks = checks_.load(std::memory_order_relaxed),
            .inconsistent_checks =
                inconsistent_checks_.load(std::memory_order_relaxed),
            .gc_deleted = gc_deleted_.load(std::memory_order_relaxed),
            .gc_failures = gc_failures_.load(std::memory_order_relaxed),
            .max_observed_version =
                max_observed_version_.load(std::memory_order_relaxed),
            .inconsistent = inconsistent()
        };
    }

    void run_once_for_test(std::function<void()> completion) {
        request_health_check(std::move(completion));
    }

    void refresh(std::function<void()> completion) {
        request_health_check(std::move(completion));
    }

private:
    void schedule_after(std::chrono::milliseconds delay) {
        if (!running_.load(std::memory_order_acquire)) return;
        timer_.expires_after(delay);
        auto self = shared_from_this();
        timer_.async_wait([self](std::error_code ec) {
            if (ec || !self->running_.load(std::memory_order_acquire)) return;
            self->request_health_check([self]() {
                if (self->running_.load(std::memory_order_acquire)) {
                    self->schedule_after(
                        std::chrono::seconds(self->cfg_.gc_interval_sec));
                }
            });
        });
    }

    void request_health_check(std::function<void()> completion) {
        bool launch = false;
        {
            std::lock_guard lock(in_flight_mu_);
            pending_health_checks_.push_back(std::move(completion));
            if (!in_flight_) {
                in_flight_ = true;
                launch = true;
            }
        }
        if (launch) launch_health_check();
    }

    void launch_health_check() {
        std::vector<std::function<void()>> completions;
        {
            std::lock_guard lock(in_flight_mu_);
            completions.swap(pending_health_checks_);
        }
        auto self = shared_from_this();
        run_health_check(
            [self, completions = std::move(completions)]() mutable {
                self->finish_health_check(completions);
            });
    }

    void finish_health_check(
        const std::vector<std::function<void()>>& completions) {
        for (auto& completion : completions) {
            try {
                if (completion) completion();
            } catch (const std::exception& e) {
                LOG_ERROR("ConfigHistory completion failed: ", e.what());
            } catch (...) {
                LOG_ERROR("ConfigHistory completion failed with unknown exception");
            }
        }

        bool launch = false;
        {
            std::lock_guard lock(in_flight_mu_);
            if (pending_health_checks_.empty()) {
                in_flight_ = false;
            } else {
                launch = true;
            }
        }
        if (launch) {
            launch_health_check();
        } else {
            in_flight_cv_.notify_all();
        }
    }

    void run_health_check(std::function<void()> completion) {
        ++checks_;
        auto self = shared_from_this();
        run_command({"EVAL", health_script(), "5",
                     "asio_owen:config:version",
                     std::string(config_history::kIndexKey),
                     "asio_owen:config:machines",
                     std::string(config_history::kMetaKey),
                     "asio_owen:config:files",
                     std::string(config_history::kSnapshotPrefix),
                     std::to_string(static_cast<int64_t>(std::time(nullptr))),
                     std::to_string(cfg_.machine_ttl_sec)},
            [self, completion = std::move(completion)](Reply reply) mutable {
                if (!reply.ok || reply.type != "array" ||
                    reply.elements.size() != 4) {
                    ++self->gc_failures_;
                    LOG_WARN("ConfigHistory health check failed: ",
                        reply.ok ? "malformed reply" : reply.error);
                    completion();
                    return;
                }
                auto current = config_history::parse_int64(reply.elements[0]);
                auto index_high = config_history::parse_int64(reply.elements[1]);
                auto machine_high = config_history::parse_int64(reply.elements[2]);
                if (!current || !index_high || !machine_high) {
                    self->mark_inconsistent("non-numeric history high-water reply");
                    completion();
                    return;
                }
                int64_t observed = self->max_observed_version_.load(
                    std::memory_order_relaxed);
                while (*current > observed &&
                       !self->max_observed_version_.compare_exchange_weak(
                           observed, *current, std::memory_order_relaxed)) {
                }
                const int64_t trusted_high = std::max(
                    self->max_observed_version_.load(std::memory_order_relaxed),
                    std::max(*index_high, *machine_high));
                if (*current < trusted_high || *index_high > *current) {
                    self->mark_inconsistent(
                        "current=" + std::to_string(*current) +
                        ", index_high=" + std::to_string(*index_high) +
                        ", machine_high=" + std::to_string(*machine_high) +
                        ", observed_high=" + std::to_string(trusted_high));
                    completion();
                    return;
                }
                const std::string& integrity = reply.elements[3];
                if (integrity == "legacy") {
                    if (self->cfg_.auto_migrate_legacy) {
                        self->migrate_legacy(*current, std::move(completion));
                        return;
                    }
                    if (self->cfg_.read_mode != "compat") {
                        self->mark_inconsistent(
                            "legacy version/files require history migration");
                        completion();
                        return;
                    }
                } else if (integrity == "legacy-empty") {
                    if (self->cfg_.read_mode != "compat") {
                        self->mark_inconsistent(
                            "legacy empty version requires compat read mode");
                        completion();
                        return;
                    }
                } else if (integrity != "ok") {
                    self->mark_inconsistent(
                        "current history snapshot/meta/index is incomplete or key type is invalid");
                    completion();
                    return;
                }
                self->inconsistent_.store(false, std::memory_order_release);
                if (*current <= 0) {
                    completion();
                    return;
                }
                self->read_gc_candidates(*current, std::move(completion));
            });
    }

    void mark_inconsistent(const std::string& detail) {
        inconsistent_.store(true, std::memory_order_release);
        ++inconsistent_checks_;
        LOG_ERROR("ConfigHistory inconsistent; save/rollback/GC frozen: ", detail);
    }

    void migrate_legacy(int64_t current, std::function<void()> completion) {
        if (current <= 0) {
            mark_inconsistent("legacy history migration has invalid current version");
            completion();
            return;
        }
        auto self = shared_from_this();
        run_command({"HGETALL", "asio_owen:config:files"},
            [self, current, completion = std::move(completion)](
                const Reply& reply) mutable {
                auto files = parse_hgetall(reply);
                if (!files || files->empty()) {
                    self->mark_inconsistent(
                        "legacy history migration mirror is missing or malformed");
                    completion();
                    return;
                }
                config_history::SnapshotInfo info;
                if (auto error = config_history::validate_snapshot(
                        *files, self->cfg_, info)) {
                    self->mark_inconsistent(
                        "legacy history migration refused: " + *error);
                    completion();
                    return;
                }
                self->publish_legacy_snapshot(
                    current, *files, info, std::move(completion));
            });
    }

    void publish_legacy_snapshot(
        int64_t current,
        const std::map<std::string, std::string>& files,
        const config_history::SnapshotInfo& info,
        std::function<void()> completion) {
        const int64_t timestamp = static_cast<int64_t>(std::time(nullptr));
        const std::string reason = "automatic Phase 4 legacy history migration";
        const std::string meta = config_history::metadata_json(
            current, current, timestamp, "system", "migration", reason, info);
        const std::string audit =
            "{\"user\":\"system\",\"base_version\":" +
            std::to_string(current) + ",\"new_version\":" +
            std::to_string(current) +
            ",\"action\":\"migration\",\"reason\":\"" + reason + "\"}";
        const size_t file_count = files.size();
        std::vector<std::string> args{
            "EVAL", config_history::migration_script(), "7",
            "asio_owen:config:version",
            "asio_owen:config:files",
            std::string(config_history::kMetaKey),
            std::string(config_history::kIndexKey),
            config_history::snapshot_key(current),
            std::string(config_history::kSnapshotStagingKey),
            "asio_owen:config:audit",
            std::to_string(current), meta, audit,
            std::to_string(cfg_.max_files),
            std::to_string(cfg_.max_file_bytes),
            std::to_string(cfg_.max_snapshot_bytes)
        };
        for (const auto& [name, content] : files) {
            args.push_back(name);
            args.push_back(content);
        }
        auto self = shared_from_this();
        run_command(std::move(args),
            [self, current, file_count,
             completion = std::move(completion)](const Reply& reply) mutable {
                auto code = reply.ok ? redis_integer(reply) : std::nullopt;
                if (!code) {
                    ++self->gc_failures_;
                    self->mark_inconsistent(
                        "legacy history migration command failed: " +
                        (reply.ok ? std::string("non-integer reply") : reply.error));
                    completion();
                    return;
                }
                if (*code != current && *code != -6) {
                    self->mark_inconsistent(
                        "legacy history migration refused with code " +
                        std::to_string(*code));
                    completion();
                    return;
                }
                if (*code == current) {
                    LOG_WARN("ConfigHistory automatically migrated legacy version/files "
                        "to immutable history, version=", current,
                        ", files=", file_count);
                }
                // -6 can be a concurrent migrator winning the race. Re-read all
                // invariants before declaring the state healthy.
                self->run_health_check(std::move(completion));
            });
    }

    static std::optional<std::map<std::string, std::string>> parse_hgetall(
        const Reply& reply) {
        if (!reply.ok || reply.type != "array" || reply.elements.size() % 2 != 0) {
            return std::nullopt;
        }
        std::map<std::string, std::string> files;
        for (size_t i = 0; i < reply.elements.size(); i += 2) {
            if (!files.emplace(reply.elements[i], reply.elements[i + 1]).second) {
                return std::nullopt;
            }
        }
        return files;
    }

    void read_gc_candidates(int64_t current, std::function<void()> completion) {
        const int64_t cutoff = static_cast<int64_t>(std::time(nullptr)) -
            static_cast<int64_t>(cfg_.retention_days) * 24 * 60 * 60;
        auto self = shared_from_this();
        run_command({"EVAL", gc_candidates_script(), "2",
                     std::string(config_history::kIndexKey),
                     std::string(config_history::kMetaKey),
                     std::to_string(current),
                     std::to_string(cfg_.retention_versions),
                     std::to_string(cutoff),
                     std::to_string(cfg_.gc_batch_size)},
            [self, current, cutoff, completion = std::move(completion)](
                const Reply& reply) mutable {
                if (!reply.ok || reply.type != "array") {
                    ++self->gc_failures_;
                    LOG_WARN("ConfigHistory GC candidate query failed: ",
                        reply.ok ? "malformed reply" : reply.error);
                    completion();
                    return;
                }
                auto candidates = std::make_shared<std::vector<int64_t>>();
                for (const auto& value : reply.elements) {
                    auto version = config_history::parse_int64(value);
                    if (version && *version > 0 && *version < current) {
                        candidates->push_back(*version);
                    }
                }
                self->delete_next_candidate(
                    current, cutoff, std::move(candidates), 0,
                    std::move(completion));
            });
    }

    void delete_next_candidate(
        int64_t current,
        int64_t cutoff,
        std::shared_ptr<std::vector<int64_t>> candidates,
        size_t index,
        std::function<void()> completion) {
        if (index >= candidates->size() || inconsistent()) {
            completion();
            return;
        }
        const int64_t candidate = (*candidates)[index];
        auto self = shared_from_this();
        run_command({"EVAL", gc_delete_script(), "4",
                     "asio_owen:config:version",
                     std::string(config_history::kIndexKey),
                     std::string(config_history::kMetaKey),
                     config_history::snapshot_key(candidate),
                     std::to_string(candidate),
                     std::to_string(cfg_.retention_versions),
                     std::to_string(cutoff)},
            [self, current, cutoff, candidates = std::move(candidates), index,
             completion = std::move(completion)](const Reply& reply) mutable {
                auto code = reply.ok ? redis_integer(reply) : std::nullopt;
                if (!code) {
                    ++self->gc_failures_;
                    LOG_WARN("ConfigHistory GC delete failed: ",
                        reply.ok ? "non-integer reply" : reply.error);
                } else if (*code == 1) {
                    ++self->gc_deleted_;
                } else if (*code == -5) {
                    self->mark_inconsistent("GC observed version high-water rollback");
                    completion();
                    return;
                }
                self->delete_next_candidate(
                    current, cutoff, std::move(candidates), index + 1,
                    std::move(completion));
            });
    }

    void run_command(std::vector<std::string> args, CommandCompletion completion) {
        auto done = std::make_shared<CommandCompletion>(std::move(completion));
        try {
            command_(std::move(args),
                [done](Reply reply) mutable {
                    try {
                        (*done)(std::move(reply));
                    } catch (const std::exception& e) {
                        LOG_ERROR("ConfigHistory callback failed: ", e.what());
                    } catch (...) {
                        LOG_ERROR("ConfigHistory callback failed with unknown exception");
                    }
                });
        } catch (...) {
            (*done)(exception_reply(std::current_exception()));
        }
    }

    static std::optional<int64_t> redis_integer(const Reply& reply) {
        if (reply.type == "integer") return reply.integer;
        if (reply.type == "string") {
            return config_history::parse_int64(reply.str);
        }
        return std::nullopt;
    }

    static Reply exception_reply(const std::exception_ptr& ep) {
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

    static std::string health_script() {
        return R"(
local expected = {'string', 'zset', 'hash', 'hash', 'hash'}
for i = 1, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then
    return {'invalid', 'invalid', 'invalid', 'invalid'}
  end
end
local current = tonumber(redis.call('GET', KEYS[1]) or '0')
if current == nil then return {'invalid', '0', '0', 'invalid'} end
local index_high = 0
local top = redis.call('ZREVRANGE', KEYS[2], 0, 0)
if top[1] ~= nil then index_high = tonumber(top[1]) or -1 end
local machine_high = 0
local now = tonumber(ARGV[2]) or 0
local machine_ttl = tonumber(ARGV[3]) or 3600
local machines = redis.call('HGETALL', KEYS[3])
for i = 1, #machines, 2 do
  local value = machines[i + 1]
  local timestamp = tonumber(string.match(value, '^[^|]+|([^|]+)'))
  if timestamp == nil or now - timestamp > machine_ttl then
    redis.call('HDEL', KEYS[3], machines[i])
  else
    local version = tonumber(string.match(value, '^([^|]+)'))
    if version ~= nil and version > machine_high then machine_high = version end
  end
end
local integrity = 'ok'
if current > 0 then
  local member = tostring(current)
  local current_snapshot_exists = redis.call('EXISTS', ARGV[1] .. member)
  local no_history = redis.call('ZCARD', KEYS[2]) == 0 and
    redis.call('HLEN', KEYS[4]) == 0 and current_snapshot_exists == 0
  if no_history and current == 1 and redis.call('HLEN', KEYS[5]) == 0 then
    integrity = 'legacy-empty'
  elseif no_history and redis.call('HLEN', KEYS[5]) > 0 then
    integrity = 'legacy'
  elseif index_high ~= current or not redis.call('ZSCORE', KEYS[2], member) or
     not redis.call('HGET', KEYS[4], member) or current_snapshot_exists ~= 1 then
    integrity = 'incomplete'
  end
end
return {tostring(current), tostring(index_high), tostring(machine_high), integrity}
)";
    }

    static std::string gc_candidates_script() {
        return R"(
local current = tonumber(ARGV[1])
local retain = tonumber(ARGV[2])
local cutoff = tonumber(ARGV[3])
local batch = tonumber(ARGV[4])
if current == nil or retain == nil or cutoff == nil or batch == nil then return {} end
local count = redis.call('ZCARD', KEYS[1])
local eligible = count - retain
if eligible <= 0 then return {} end
local versions = redis.call('ZRANGE', KEYS[1], 0, math.min(eligible - 1, batch * 4 - 1))
local out = {}
for _, version in ipairs(versions) do
  local numeric = tonumber(version)
  local meta = redis.call('HGET', KEYS[2], version)
  local ts = meta and tonumber(string.match(meta, '"ts"%s*:%s*(%d+)')) or nil
  if numeric ~= nil and numeric < current and ts ~= nil and ts < cutoff then
    table.insert(out, version)
    if #out >= batch then break end
  end
end
return out
)";
    }

    static std::string gc_delete_script() {
        return R"(
local candidate = tonumber(ARGV[1])
local retain = tonumber(ARGV[2])
local cutoff = tonumber(ARGV[3])
local current = tonumber(redis.call('GET', KEYS[1]) or '')
if candidate == nil or retain == nil or cutoff == nil or current == nil then return -2 end
local top = redis.call('ZREVRANGE', KEYS[2], 0, 0)
if top[1] ~= nil then
  local high = tonumber(top[1])
  if high == nil or high > current then return -5 end
end
if candidate >= current then return 0 end
local member = tostring(candidate)
local rank = redis.call('ZRANK', KEYS[2], member)
local count = redis.call('ZCARD', KEYS[2])
if not rank or rank >= count - retain then return 0 end
local meta = redis.call('HGET', KEYS[3], member)
local ts = meta and tonumber(string.match(meta, '"ts"%s*:%s*(%d+)')) or nil
if ts == nil or ts >= cutoff then return 0 end
redis.call('UNLINK', KEYS[4])
redis.call('HDEL', KEYS[3], member)
redis.call('ZREM', KEYS[2], member)
return 1
)";
    }

    asio::io_context& ioc_;
    asio::steady_timer timer_;
    Command command_;
    ConfigHistoryConfig cfg_;
    int redis_cmd_timeout_ms_ = 500;
    std::atomic<bool> running_{false};
    std::atomic<bool> inconsistent_{true};
    std::atomic<int64_t> max_observed_version_{0};
    std::atomic<uint64_t> checks_{0};
    std::atomic<uint64_t> inconsistent_checks_{0};
    std::atomic<uint64_t> gc_deleted_{0};
    std::atomic<uint64_t> gc_failures_{0};
    std::mutex in_flight_mu_;
    std::condition_variable in_flight_cv_;
    std::vector<std::function<void()>> pending_health_checks_;
    bool in_flight_ = false;
};
