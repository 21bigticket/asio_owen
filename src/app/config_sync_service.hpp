#pragma once

#include <algorithm>
#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <atomic>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>

#include "../common/config.hpp"
#include "../common/logger.hpp"
#include "../db/redis_pool.hpp"
#include "app_config.hpp"

class ConfigSyncService : public std::enable_shared_from_this<ConfigSyncService> {
public:
    using Reply = RedisPool::Reply;
    using Completion = std::function<void(bool)>;
    using CommandCompletion = std::function<void(Reply)>;
    using Command = std::function<void(std::vector<std::string>, CommandCompletion)>;

    struct ValidationResult {
        bool ok = true;
        std::string reason;
    };

    struct State {
        bool exists = false;
        int64_t synced_version = 0;
        std::string status = "ok";
        std::map<std::string, std::string> managed_files;
        std::map<std::string, std::string> last_ok;
        std::map<std::string, std::string> failures;
    };

    ConfigSyncService(asio::io_context& ioc, RedisPool& redis,
                      std::filesystem::path config_base,
                      ConfigSyncConfig cfg, AppConfig running_app_cfg)
        : ConfigSyncService(
              ioc,
              [&ioc, &redis](std::vector<std::string> args,
                             CommandCompletion completion) {
                  auto done = std::make_shared<CommandCompletion>(std::move(completion));
                  try {
                      co_spawn(ioc, redis.cmd_argv(std::move(args)),
                          [done](std::exception_ptr ep, Reply reply) mutable {
                              if (ep) {
                                  reply = exception_reply(ep);
                              }
                              (*done)(std::move(reply));
                          });
                  } catch (...) {
                      (*done)(exception_reply(std::current_exception()));
                  }
              },
              std::move(config_base), std::move(cfg), std::move(running_app_cfg)) {}

    ConfigSyncService(asio::io_context& ioc, Command command,
                      std::filesystem::path config_base,
                      ConfigSyncConfig cfg, AppConfig running_app_cfg)
        : ioc_(ioc),
          timer_(ioc),
          command_(std::move(command)),
          config_base_(std::move(config_base)),
          cfg_(normalize_config(std::move(cfg))),
          running_app_cfg_(std::move(running_app_cfg)) {}

    void start() {
        if (!cfg_.enabled || cfg_.sync_interval_sec <= 0) return;
        running_.store(true, std::memory_order_release);
        schedule_after(std::chrono::milliseconds(0));
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        try {
            timer_.cancel();
        } catch (...) {
        }

        std::unique_lock lock(in_flight_mu_);
        const auto wait_ms = std::chrono::milliseconds(effective_drain_timeout_ms());
        if (!in_flight_cv_.wait_for(lock, wait_ms, [this] { return in_flight_ == 0; })) {
            try {
                LOG_ERROR("ConfigSyncService stop timed out with in_flight=", in_flight_);
            } catch (...) {
            }
        }
    }

    void sync_once_for_test(Completion completion) {
        sync_once(std::move(completion));
    }

    static bool blocking_first_pull(const std::filesystem::path& config_base,
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
                        [done](std::exception_ptr ep, Reply reply) mutable {
                            if (ep) {
                                reply = exception_reply(ep);
                            }
                            (*done)(std::move(reply));
                        });
                } catch (...) {
                    (*done)(exception_reply(std::current_exception()));
                }
            };
            auto service = std::make_shared<ConfigSyncService>(
                pull_ioc, std::move(command), config_base, sync_cfg, running_app_cfg);
            service->sync_once_for_test([&](bool ok) { result = ok; });
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

    static ConfigSyncConfig normalize_config(ConfigSyncConfig cfg) {
        if (cfg.sync_interval_sec <= 0) cfg.sync_interval_sec = 5;
        if (cfg.first_pull.empty()) cfg.first_pull = "async";
        if (cfg.first_pull != "async" && cfg.first_pull != "blocking") {
            LOG_WARN("invalid config_sync.first_pull '", cfg.first_pull, "', using async");
            cfg.first_pull = "async";
        }
        if (cfg.first_pull_timeout_ms <= 0) cfg.first_pull_timeout_ms = 3000;
        return cfg;
    }

    static bool is_never_sync_file(const std::string& name) {
        return name == "11-redis.ini" ||
               name == "12-config-sync.ini" ||
               name == "99-local.ini";
    }

    static bool is_valid_managed_filename(const std::string& name) {
        if (name.size() < 7) return false;
        if (!std::isdigit(static_cast<unsigned char>(name[0])) ||
            !std::isdigit(static_cast<unsigned char>(name[1])) ||
            name[2] != '-') {
            return false;
        }
        if (name.size() < 5 || name.substr(name.size() - 4) != ".ini") {
            return false;
        }
        for (size_t i = 3; i + 4 < name.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(name[i]);
            if (!(std::islower(c) || std::isdigit(c) || c == '_' || c == '-')) {
                return false;
            }
        }
        return true;
    }

    static ValidationResult validate_managed_file(
        const std::string& name, const std::string& content) {
        if (is_never_sync_file(name)) {
            return {false, "never-sync file is not Redis-managed"};
        }
        if (!is_valid_managed_filename(name)) {
            return {false, "invalid managed filename"};
        }
        if (contains_section(content, "redis")) {
            return {false, "managed file contains [redis] section"};
        }
        if (contains_section(content, "admin")) {
            return {false, "managed file contains [admin] section"};
        }
        if (contains_section(content, "config_sync")) {
            return {false, "managed file contains [config_sync] section"};
        }
        if (has_reserved_admin_rule(content)) {
            return {false, "managed file touches reserved admin path"};
        }
        return {};
    }

    static bool has_reserved_admin_rule(const std::string& content) {
        std::string section;
        for (auto line : split_lines(content)) {
            strip_cr(line);
            trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            if (line.front() == '[' && line.back() == ']') {
                section = line.substr(1, line.size() - 2);
                trim(section);
                to_lower_in_place(section);
                continue;
            }
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            trim(key);
            trim(value);
            if (section == "auth_whitelist") {
                if (is_reserved_admin_path(key) || is_reserved_admin_path(value)) {
                    return true;
                }
            } else if (section == "path_blacklist") {
                if (is_reserved_admin_path(key)) {
                    return true;
                }
            }
        }
        return false;
    }

    static bool is_reserved_admin_path(std::string path) {
        trim(path);
        if (path.empty() || path.front() != '/') return false;
        to_lower_in_place(path);
        return path == "/admin" || path.rfind("/admin/", 0) == 0 ||
               path == "/api/admin" || path.rfind("/api/admin/", 0) == 0;
    }

    static bool contains_section(const std::string& content, const std::string& target) {
        std::string lowered_target = target;
        to_lower_in_place(lowered_target);
        for (auto line : split_lines(content)) {
            strip_cr(line);
            trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            if (line.front() == '[' && line.back() == ']') {
                auto section = line.substr(1, line.size() - 2);
                trim(section);
                to_lower_in_place(section);
                if (section == lowered_target) return true;
            }
        }
        return false;
    }

    static std::string content_hash(const std::string& content) {
        uint64_t hash = 1469598103934665603ull;
        for (unsigned char c : content) {
            hash ^= c;
            hash *= 1099511628211ull;
        }
        std::ostringstream oss;
        oss << std::hex << std::setw(16) << std::setfill('0') << hash;
        return oss.str();
    }

    static State load_state(const std::filesystem::path& config_base) {
        State state;
        auto path = state_path(config_base);
        std::ifstream in(path);
        if (!in.is_open()) return state;
        state.exists = true;
        std::string line;
        while (std::getline(in, line)) {
            strip_cr(line);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto key = line.substr(0, eq);
            auto value = line.substr(eq + 1);
            if (key == "synced_version") {
                auto parsed = parse_int64(value);
                if (parsed) state.synced_version = *parsed;
            } else if (key == "status") {
                state.status = value;
            } else if (key.rfind("file.", 0) == 0) {
                state.managed_files[key.substr(5)] = value;
            } else if (key.rfind("last_ok.", 0) == 0) {
                state.last_ok[key.substr(8)] = value;
            } else if (key.rfind("failure.", 0) == 0) {
                state.failures[key.substr(8)] = value;
            }
        }
        return state;
    }

    static bool write_state(const std::filesystem::path& config_base, const State& state) {
        auto path = state_path(config_base);
        auto tmp = path;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) return false;
            out << "synced_version=" << state.synced_version << "\n";
            out << "status=" << state.status << "\n";
            for (const auto& [name, hash] : state.managed_files) {
                out << "file." << name << "=" << hash << "\n";
            }
            for (const auto& [name, hash] : state.last_ok) {
                out << "last_ok." << name << "=" << hash << "\n";
            }
            for (const auto& [name, reason] : state.failures) {
                out << "failure." << name << "=" << reason << "\n";
            }
        }
        return std::rename(tmp.string().c_str(), path.string().c_str()) == 0;
    }

private:
    struct VersionRead {
        enum class Status { Value, Missing, Invalid, Error };
        Status status = Status::Error;
        int64_t value = 0;
        std::string error;
    };

    class TickCompletion {
    public:
        explicit TickCompletion(std::shared_ptr<ConfigSyncService> service)
            : service_(std::move(service)) {}

        ~TickCompletion() {
            finish();
        }

        void finish() noexcept {
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

    private:
        std::shared_ptr<ConfigSyncService> service_;
        std::atomic<bool> finished_{false};
    };

    static constexpr std::string_view kVersionKey = "asio_owen:config:version";
    static constexpr std::string_view kFilesKey = "asio_owen:config:files";
    static constexpr std::string_view kStagingKey = "asio_owen:config:files:staging";
    static constexpr std::string_view kMachinesKey = "asio_owen:config:machines";

    void schedule_after(std::chrono::milliseconds delay) {
        if (!running_.load(std::memory_order_acquire)) return;
        timer_.expires_after(delay);
        auto self = shared_from_this();
        timer_.async_wait([self](std::error_code ec) {
            if (ec || !self->running_.load(std::memory_order_acquire)) return;
            self->launch_tick();
        });
    }

    void launch_tick() {
        {
            std::lock_guard lock(in_flight_mu_);
            ++in_flight_;
        }
        auto self = shared_from_this();
        auto tick_completion = std::make_shared<TickCompletion>(self);
        try {
            sync_once([tick_completion](bool) { tick_completion->finish(); });
        } catch (const std::exception& e) {
            try {
                LOG_ERROR("ConfigSync tick failed: ", e.what());
            } catch (...) {
            }
        } catch (...) {
            try {
                LOG_ERROR("ConfigSync tick failed with unknown exception");
            } catch (...) {
            }
        }
    }

    void finish_tick() {
        {
            std::lock_guard lock(in_flight_mu_);
            if (in_flight_ > 0) --in_flight_;
        }
        in_flight_cv_.notify_all();
        if (running_.load(std::memory_order_acquire)) {
            schedule_after(std::chrono::seconds(cfg_.sync_interval_sec));
        }
    }

    void sync_once(Completion completion) {
        auto self = shared_from_this();
        run_command({"GET", std::string(kVersionKey)},
            [self, completion = std::move(completion)](Reply reply) mutable {
                auto version = read_version_reply(reply);
                if (version.status == VersionRead::Status::Error) {
                    self->warn_limited(
                        "ConfigSync Redis version read failed: " + version.error);
                    completion(false);
                    return;
                }
                if (version.status == VersionRead::Status::Invalid) {
                    LOG_ERROR("config version key corrupted, manual fix required: ",
                        version.error);
                    completion(false);
                    return;
                }
                if (version.status == VersionRead::Status::Missing) {
                    self->seed_if_eligible(std::move(completion));
                    return;
                }

                const int64_t remote_version = version.value;
                const int64_t local_version = load_state(self->config_base_).synced_version;
                if (remote_version < local_version) {
                    LOG_ERROR("ConfigSync ignored version rollback: remote=", remote_version,
                        ", local=", local_version);
                    completion(false);
                    return;
                }
                if (remote_version == local_version) {
                    self->heartbeat(heartbeat_payload(remote_version, "ok"),
                        [completion = std::move(completion)]() mutable {
                            completion(true);
                        });
                    return;
                }

                self->read_remote_files(remote_version, std::move(completion));
            });
    }

    void read_remote_files(int64_t remote_version, Completion completion) {
        auto self = shared_from_this();
        run_command({"HGETALL", std::string(kFilesKey)},
            [self, remote_version, completion = std::move(completion)](
                Reply files_reply) mutable {
                if (!files_reply.ok) {
                    self->warn_limited("ConfigSync HGETALL failed: " + files_reply.error);
                    completion(false);
                    return;
                }
                auto remote_files = parse_hgetall(files_reply);
                if (!remote_files) {
                    LOG_ERROR("ConfigSync HGETALL returned malformed field/value list");
                    completion(false);
                    return;
                }
                self->verify_remote_version(
                    remote_version,
                    std::make_shared<std::map<std::string, std::string>>(
                        std::move(*remote_files)),
                    std::move(completion));
            });
    }

    void verify_remote_version(
        int64_t remote_version,
        std::shared_ptr<std::map<std::string, std::string>> remote_files,
        Completion completion) {
        auto self = shared_from_this();
        run_command({"GET", std::string(kVersionKey)},
            [self, remote_version, remote_files = std::move(remote_files),
             completion = std::move(completion)](Reply reply) mutable {
                auto version_after = read_version_reply(reply);
                if (version_after.status != VersionRead::Status::Value) {
                    LOG_ERROR("ConfigSync second version read failed while syncing version ",
                        remote_version);
                    completion(false);
                    return;
                }
                if (version_after.value != remote_version) {
                    LOG_INFO("ConfigSync version changed during read, retry next tick: before=",
                        remote_version, ", after=", version_after.value);
                    completion(false);
                    return;
                }

                State state = load_state(self->config_base_);
                std::map<std::string, std::string> failures;
                const bool applied = self->apply_remote_files(
                    remote_version, *remote_files, state, failures);
                std::string value = applied ?
                    heartbeat_payload(remote_version, "ok") :
                    heartbeat_payload(remote_version, "partial", &failures);
                if (applied) {
                    self->warn_startup_drift_once(remote_version);
                }
                self->heartbeat(std::move(value),
                    [completion = std::move(completion), applied]() mutable {
                        completion(applied);
                    });
            });
    }

    static VersionRead read_version_reply(const Reply& reply) {
        if (!reply.ok) {
            return VersionRead{VersionRead::Status::Error, 0, reply.error};
        }
        if (reply.type == "nil") {
            return VersionRead{VersionRead::Status::Missing, 0, ""};
        }
        if (reply.type == "integer") {
            if (reply.integer < 0) {
                return VersionRead{VersionRead::Status::Invalid, 0, "negative version"};
            }
            return VersionRead{VersionRead::Status::Value, reply.integer, ""};
        }
        if (reply.type == "string") {
            auto parsed = parse_int64(reply.str);
            if (!parsed || *parsed < 0) {
                return VersionRead{VersionRead::Status::Invalid, 0, reply.str};
            }
            return VersionRead{VersionRead::Status::Value, *parsed, ""};
        }
        return VersionRead{
            VersionRead::Status::Invalid, 0, "unexpected type " + reply.type};
    }

    void seed_if_eligible(Completion completion) {
        std::string reason;
        {
            State state = load_state(config_base_);
            if (!has_seed_eligibility(state, reason)) {
                LOG_ERROR("ConfigSync seed refused: ", reason);
                completion(false);
                return;
            }
        }

        std::vector<std::string> args{
            "EVAL", seed_script(), "3",
            std::string(kVersionKey), std::string(kFilesKey), std::string(kStagingKey)
        };
        auto seeded_state = std::make_shared<State>();
        size_t local_file_count = 0;
        {
            std::map<std::string, std::string> local_files;
            std::vector<std::string> errors;
            if (!collect_local_managed_files(config_base_, local_files, errors)) {
                for (const auto& err : errors) {
                    LOG_ERROR("ConfigSync seed refused: ", err);
                }
                completion(false);
                return;
            }

            seeded_state->exists = true;
            seeded_state->synced_version = 1;
            seeded_state->status = "ok";
            local_file_count = local_files.size();
            for (const auto& [name, content] : local_files) {
                args.push_back(name);
                args.push_back(content);
                seeded_state->managed_files[name] = content_hash(content);
                seeded_state->last_ok[name] = seeded_state->managed_files[name];
            }
        }
        auto self = shared_from_this();
        run_command(std::move(args),
            [self, seeded_state = std::move(seeded_state), local_file_count,
             completion = std::move(completion)](Reply reply) mutable {
                if (!reply.ok) {
                    LOG_ERROR("ConfigSync seed EVAL failed: ", reply.error);
                    completion(false);
                    return;
                }
                auto code = script_integer(reply);
                if (!code) {
                    LOG_ERROR("ConfigSync seed EVAL returned non-integer type=", reply.type);
                    completion(false);
                    return;
                }
                if (*code == 1) {
                    if (!write_state(self->config_base_, *seeded_state)) {
                        LOG_ERROR("ConfigSync failed to write state after seed");
                        completion(false);
                        return;
                    }
                    self->heartbeat(heartbeat_payload(1, "ok"),
                        [self, local_file_count,
                         completion = std::move(completion)]() mutable {
                            self->warn_startup_drift_once(1);
                            LOG_INFO("ConfigSync seeded Redis config, files=",
                                local_file_count);
                            completion(true);
                        });
                    return;
                }
                if (*code == 0) {
                    LOG_INFO("ConfigSync seed skipped because another machine already seeded");
                    completion(false);
                    return;
                }
                LOG_ERROR("ConfigSync seed rejected by script, code=", *code);
                completion(false);
            });
    }

    bool has_seed_eligibility(const State& state, std::string& reason) const {
        if (!state.exists) return true;
        if (state.status != "ok") {
            reason = "state status is not ok";
            return false;
        }
        std::map<std::string, std::string> local_files;
        std::vector<std::string> errors;
        if (!collect_local_managed_files(config_base_, local_files, errors)) {
            reason = errors.empty() ? "failed to collect local files" : errors.front();
            return false;
        }
        std::map<std::string, std::string> local_hashes;
        for (const auto& [name, content] : local_files) {
            local_hashes[name] = content_hash(content);
        }
        if (local_hashes != state.last_ok) {
            reason = "local files do not match last_ok state";
            return false;
        }
        return true;
    }

    bool apply_remote_files(int64_t version,
                            const std::map<std::string, std::string>& remote_files,
                            const State& previous_state,
                            std::map<std::string, std::string>& failures) {
        State next = previous_state;
        next.status = "partial";
        next.failures.clear();
        failures.clear();

        auto persist_partial = [&]() {
            failures = next.failures;
            if (!write_state(config_base_, next)) {
                LOG_ERROR("ConfigSync failed to write partial state");
            }
            for (const auto& [name, reason] : next.failures) {
                LOG_ERROR("ConfigSync partial sync: file=", name, ", reason=", reason);
            }
            return false;
        };

        std::set<std::string> local_managed_names =
            collect_local_managed_file_names(config_base_);
        for (const auto& [name, _] : previous_state.managed_files) {
            local_managed_names.insert(name);
        }
        if (remote_files.empty() && !local_managed_names.empty()) {
            next.failures["remote_files"] =
                "empty remote file set refused while local managed files exist";
            return persist_partial();
        }

        std::map<std::string, std::string> valid_files;
        for (const auto& [name, content] : remote_files) {
            auto validation = validate_managed_file(name, content);
            if (!validation.ok) {
                next.failures[name] = validation.reason;
                continue;
            }
            valid_files[name] = content;
        }

        for (const auto& [name, content] : valid_files) {
            if (!write_managed_file_if_changed(name, content)) {
                next.failures[name] = "failed to write file";
            }
        }

        if (!next.failures.empty()) {
            return persist_partial();
        }

        std::set<std::string> delete_candidates = std::move(local_managed_names);
        for (const auto& name : delete_candidates) {
            if (remote_files.find(name) == remote_files.end()) {
                std::error_code ec;
                std::filesystem::remove(config_dir() / name, ec);
                if (ec) {
                    LOG_WARN("ConfigSync failed to remove stale file ", name,
                        ": ", ec.message());
                }
            }
        }

        State clean;
        clean.exists = true;
        clean.synced_version = version;
        clean.status = "ok";
        for (const auto& [name, content] : remote_files) {
            clean.managed_files[name] = content_hash(content);
            clean.last_ok[name] = clean.managed_files[name];
        }
        if (!write_state(config_base_, clean)) {
            LOG_ERROR("ConfigSync failed to write ok state");
            return false;
        }
        LOG_INFO("ConfigSync applied version ", version, ", files=", remote_files.size());
        return true;
    }

    static std::string heartbeat_payload(
        int64_t version,
        std::string_view status,
        const std::map<std::string, std::string>* failures = nullptr) {
        std::string value = std::to_string(version) + "|" +
            std::to_string(static_cast<int64_t>(std::time(nullptr))) + "|" +
            std::to_string(static_cast<int64_t>(getpid())) + "|" + std::string(status);
        if (failures && !failures->empty()) {
            value += "|";
            bool first = true;
            for (const auto& [name, reason] : *failures) {
                if (!first) value += ",";
                first = false;
                value += name + ":" + reason;
            }
        }
        return value;
    }

    void heartbeat(std::string value, std::function<void()> completion) {
        run_command({
                "HSET", std::string(kMachinesKey), machine_name(), std::move(value)
            },
            [completion = std::move(completion)](Reply reply) mutable {
                if (!reply.ok) {
                    LOG_WARN("ConfigSync heartbeat failed: ", reply.error);
                }
                completion();
            });
    }

    void run_command(std::vector<std::string> args, CommandCompletion completion) {
        auto done = std::make_shared<CommandCompletion>(std::move(completion));
        auto callback_started = std::make_shared<std::atomic<bool>>(false);
        try {
            command_(std::move(args),
                [done, callback_started](Reply reply) mutable {
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
                            LOG_ERROR("ConfigSync command callback failed with unknown exception");
                        } catch (...) {
                        }
                    }
                });
        } catch (...) {
            if (!callback_started->load(std::memory_order_acquire)) {
                (*done)(exception_reply(std::current_exception()));
            }
        }
    }

    static Reply exception_reply(std::exception_ptr ep) {
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

    void warn_startup_drift_once(int64_t version) {
        {
            std::lock_guard lock(drift_mu_);
            if (drift_warned_versions_.count(version)) return;
            drift_warned_versions_.insert(version);
        }

        Config cfg;
        if (!cfg.load(config_base_)) return;
        try {
            auto current = app_config_from(cfg);
            if (startup_config_differs(current)) {
                LOG_WARN("startup config drift detected, restart required, version=", version);
            }
        } catch (const std::exception& e) {
            LOG_WARN("ConfigSync startup drift check failed: ", e.what());
        }
    }

    bool startup_config_differs(const AppConfig& current) const {
        return current.server_port != running_app_cfg_.server_port ||
               mysql_config_differs(current.mysql, running_app_cfg_.mysql) ||
               redis_config_differs(current.redis, running_app_cfg_.redis);
    }

    static bool mysql_config_differs(
        const MysqlPool::Config& a, const MysqlPool::Config& b) {
        return a.host != b.host || a.port != b.port || a.user != b.user ||
               a.pass != b.pass || a.db != b.db || a.min_size != b.min_size ||
               a.max_size != b.max_size || a.max_idle_sec != b.max_idle_sec ||
               a.connect_timeout_ms != b.connect_timeout_ms ||
               a.read_timeout_ms != b.read_timeout_ms ||
               a.query_timeout_ms != b.query_timeout_ms ||
               a.acquire_timeout_ms != b.acquire_timeout_ms ||
               a.keepalive_sec != b.keepalive_sec ||
               a.worker_threads != b.worker_threads ||
               a.max_creating != b.max_creating;
    }

    static bool redis_config_differs(
        const RedisPool::Config& a, const RedisPool::Config& b) {
        return a.host != b.host || a.port != b.port || a.db != b.db ||
               a.connect_timeout_ms != b.connect_timeout_ms ||
               a.cmd_timeout_ms != b.cmd_timeout_ms || a.mode != b.mode ||
               a.min_size != b.min_size || a.max_size != b.max_size ||
               a.max_idle_sec != b.max_idle_sec ||
               a.worker_threads != b.worker_threads ||
               a.max_creating != b.max_creating ||
               a.acquire_timeout_ms != b.acquire_timeout_ms;
    }

    std::filesystem::path config_dir() const {
        return config_base_ / "config.d";
    }

    bool write_managed_file_if_changed(
        const std::string& name, const std::string& content) const {
        std::error_code ec;
        std::filesystem::create_directories(config_dir(), ec);
        if (ec) return false;

        auto path = config_dir() / name;
        std::string existing;
        if (read_file(path, existing) && existing == content) {
            return true;
        }
        auto tmp = path;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) return false;
            out << content;
        }
        return std::rename(tmp.string().c_str(), path.string().c_str()) == 0;
    }

    static std::optional<std::map<std::string, std::string>> parse_hgetall(
        const Reply& reply) {
        if (reply.type != "array") return std::map<std::string, std::string>{};
        if (reply.elements.size() % 2 != 0) return std::nullopt;
        std::map<std::string, std::string> result;
        for (size_t i = 0; i < reply.elements.size(); i += 2) {
            result[reply.elements[i]] = reply.elements[i + 1];
        }
        return result;
    }

    static std::optional<int64_t> script_integer(const Reply& reply) {
        if (reply.type == "integer") return reply.integer;
        if (reply.type == "string") return parse_int64(reply.str);
        return std::nullopt;
    }

    static bool collect_local_managed_files(
        const std::filesystem::path& config_base,
        std::map<std::string, std::string>& files,
        std::vector<std::string>& errors) {
        auto dir = config_base / "config.d";
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            errors.push_back("config.d not found");
            return false;
        }
        std::vector<std::filesystem::path> paths;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) {
                errors.push_back("failed to scan config.d");
                return false;
            }
            if (entry.is_regular_file(ec) && entry.path().extension() == ".ini") {
                auto name = entry.path().filename().string();
                if (!is_never_sync_file(name)) {
                    paths.push_back(entry.path());
                }
            }
            ec.clear();
        }
        std::sort(paths.begin(), paths.end());
        for (const auto& path : paths) {
            auto name = path.filename().string();
            std::string content;
            if (!read_file(path, content)) {
                errors.push_back("failed to read " + name);
                continue;
            }
            auto validation = validate_managed_file(name, content);
            if (!validation.ok) {
                errors.push_back(name + ": " + validation.reason);
                continue;
            }
            files[name] = std::move(content);
        }
        return errors.empty();
    }

    static std::set<std::string> collect_local_managed_file_names(
        const std::filesystem::path& config_base) {
        std::set<std::string> names;
        auto dir = config_base / "config.d";
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return names;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) return names;
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".ini") {
                ec.clear();
                continue;
            }
            auto name = entry.path().filename().string();
            if (!is_never_sync_file(name) && is_valid_managed_filename(name)) {
                names.insert(std::move(name));
            }
            ec.clear();
        }
        return names;
    }

    static bool read_file(const std::filesystem::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }

    static std::filesystem::path state_path(const std::filesystem::path& config_base) {
        return config_base / ".config-sync-state";
    }

    static std::vector<std::string> split_lines(const std::string& text) {
        std::vector<std::string> lines;
        std::string line;
        std::istringstream in(text);
        while (std::getline(in, line)) lines.push_back(line);
        return lines;
    }

    static void trim(std::string& s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
            s.erase(s.begin());
        }
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
            s.pop_back();
        }
    }

    static void strip_cr(std::string& s) {
        if (!s.empty() && s.back() == '\r') s.pop_back();
    }

    static void to_lower_in_place(std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    static std::optional<int64_t> parse_int64(const std::string& value) {
        if (value.empty()) return std::nullopt;
        int64_t parsed = 0;
        auto [end, ec] = std::from_chars(
            value.data(), value.data() + value.size(), parsed);
        if (ec != std::errc{} || end != value.data() + value.size()) {
            return std::nullopt;
        }
        return parsed;
    }

    std::string machine_name() const {
        if (!cfg_.machine_name.empty()) return cfg_.machine_name;
        char host[256]{};
        if (gethostname(host, sizeof(host) - 1) == 0 && host[0] != '\0') {
            return host;
        }
        return "unknown";
    }

    int effective_drain_timeout_ms() const {
        int cmd_ms = running_app_cfg_.redis.cmd_timeout_ms <= 0 ?
            30000 : running_app_cfg_.redis.cmd_timeout_ms;
        return cmd_ms + 500;
    }

    void warn_limited(const std::string& message) {
        const auto now = std::chrono::steady_clock::now();
        bool should_log = false;
        {
            std::lock_guard lock(warn_mu_);
            if (now - last_warn_ > std::chrono::seconds(30)) {
                last_warn_ = now;
                should_log = true;
            }
        }
        if (should_log) {
            LOG_WARN(message);
        }
    }

    static std::string seed_script() {
        return R"(
if redis.call('TYPE', KEYS[1]).ok ~= 'none' then return 0 end
local files_type = redis.call('TYPE', KEYS[2]).ok
if files_type ~= 'hash' and files_type ~= 'none' then return -3 end
if (#ARGV) % 2 ~= 0 then return -2 end
if #ARGV == 0 then
  redis.call('DEL', KEYS[2])
  redis.call('SET', KEYS[1], 1)
  return 1
end
redis.call('DEL', KEYS[3])
redis.call('HSET', KEYS[3], unpack(ARGV))
redis.call('RENAME', KEYS[3], KEYS[2])
redis.call('SET', KEYS[1], 1)
return 1
)";
    }

    asio::io_context& ioc_;
    asio::steady_timer timer_;
    Command command_;
    std::filesystem::path config_base_;
    ConfigSyncConfig cfg_;
    AppConfig running_app_cfg_;
    std::atomic<bool> running_{false};

    std::mutex in_flight_mu_;
    std::condition_variable in_flight_cv_;
    int in_flight_ = 0;

    std::mutex warn_mu_;
    std::chrono::steady_clock::time_point last_warn_{};

    std::mutex drift_mu_;
    std::set<int64_t> drift_warned_versions_;
};
