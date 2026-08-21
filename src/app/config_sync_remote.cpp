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

void ConfigSyncServiceImpl::schedule_after(std::chrono::milliseconds delay) {
    if (!running_.load(std::memory_order_acquire)) return;
    timer_.expires_after(delay);
    auto self = shared_from_this();
    timer_.async_wait([self](std::error_code ec) {
        if (ec || !self->running_.load(std::memory_order_acquire)) return;
        self->ConfigSyncServiceImpl::launch_tick();
    });
}

void ConfigSyncServiceImpl::launch_tick() {
    {
        std::lock_guard lock(in_flight_mu_);
        ++in_flight_;
    }
    auto self = shared_from_this();
    auto tick_completion = std::make_shared<TickCompletion>(self);
    try {
        ConfigSyncServiceImpl::sync_once([tick_completion](bool) { tick_completion->finish(); });
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

void ConfigSyncServiceImpl::finish_tick() {
    {
        std::lock_guard lock(in_flight_mu_);
        if (in_flight_ > 0) --in_flight_;
    }
    in_flight_cv_.notify_all();
    if (running_.load(std::memory_order_acquire)) {
        auto self = shared_from_this();
        asio::post(ioc_, [self]() {
            if (self->running_.load(std::memory_order_acquire)) {
                self->ConfigSyncServiceImpl::schedule_after(
                    std::chrono::seconds(self->cfg_.sync_interval_sec));
            }
        });
    }
}

void ConfigSyncServiceImpl::sync_once(Completion completion) {
    auto self = shared_from_this();
    ConfigSyncServiceImpl::run_command({"GET", std::string(kVersionKey)},
        [self, completion = std::move(completion)](const Reply& reply) mutable {
            auto version = ConfigSyncServiceImpl::read_version_reply(reply);
            if (version.status == VersionRead::Status::Error) {
                self->ConfigSyncServiceImpl::warn_limited(
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
                self->ConfigSyncServiceImpl::seed_if_eligible(std::move(completion));
                return;
            }

            const int64_t remote_version = version.value;
            const State local_state = ConfigSyncServiceImpl::load_state(self->config_base_);
            const int64_t local_version = local_state.synced_version;
            if (remote_version < local_version) {
                LOG_ERROR("ConfigSync ignored version rollback: remote=", remote_version,
                    ", local=", local_version);
                completion(false);
                return;
            }
            const bool history_recovery_pending =
                local_state.status == "partial" &&
                local_state.failures.find("history_snapshot") !=
                    local_state.failures.end();
            if (remote_version == local_version && !history_recovery_pending) {
                std::map<std::string, std::string> local_files;
                std::vector<std::string> errors;
                const bool collected = self->ConfigSyncServiceImpl::collect_local_managed_files(
                    self->config_base_, local_files, errors);
                std::map<std::string, std::string> local_hashes;
                if (collected) {
                    for (const auto& [name, content] : local_files) {
                        local_hashes[name] = ConfigSyncServiceImpl::content_hash(content);
                    }
                }
                const bool local_matches = collected &&
                    local_state.status == "ok" &&
                    local_state.managed_files == local_state.last_ok &&
                    local_hashes == local_state.last_ok;
                if (local_matches) {
                    self->heartbeat(ConfigSyncServiceImpl::heartbeat_payload(remote_version, "ok"),
                        [completion = std::move(completion)]() mutable {
                            completion(true);
                        });
                    return;
                }
                LOG_WARN("ConfigSync local drift detected; reapplying version ",
                    remote_version);
            }

            self->ConfigSyncServiceImpl::read_remote_files(remote_version, std::move(completion));
        });
}

void ConfigSyncServiceImpl::read_remote_files(int64_t remote_version, Completion completion) {
    auto self = shared_from_this();
    ConfigSyncServiceImpl::run_command({"HGETALL", config_history::snapshot_key(remote_version)},
        [self, remote_version, completion = std::move(completion)](
            const Reply& files_reply) mutable {
            if (!files_reply.ok) {
                self->ConfigSyncServiceImpl::warn_limited(
                    "ConfigSync history HGETALL failed: " + files_reply.error);
                completion(false);
                return;
            }
            auto remote_files = ConfigSyncServiceImpl::parse_hgetall(files_reply);
            if (!remote_files) {
                LOG_ERROR("ConfigSync history HGETALL returned malformed data");
                completion(false);
                return;
            }
            auto files = std::make_shared<std::map<std::string, std::string>>(
                std::move(*remote_files));
            if (files->empty()) {
                self->ConfigSyncServiceImpl::handle_missing_snapshot(
                    remote_version, std::move(completion));
                return;
            }
            self->ConfigSyncServiceImpl::verify_snapshot_hash(
                remote_version, std::move(files), "", std::move(completion));
        });
}

void ConfigSyncServiceImpl::handle_missing_snapshot(int64_t remote_version, Completion completion) {
    if (cfg_.history.read_mode == "compat") {
        ConfigSyncServiceImpl::warn_limited("ConfigSync history snapshot missing in compat mode, version=" +
            std::to_string(remote_version));
        ConfigSyncServiceImpl::read_current_mirror(remote_version, false, std::move(completion));
        return;
    }
    ConfigSyncServiceImpl::read_current_mirror(remote_version, true, std::move(completion));
}

void ConfigSyncServiceImpl::read_current_mirror(
    int64_t remote_version, bool require_meta_hash, Completion completion) {
    auto self = shared_from_this();
    ConfigSyncServiceImpl::run_command({"HGETALL", std::string(kFilesKey)},
        [self, remote_version, require_meta_hash,
         completion = std::move(completion)](const Reply& reply) mutable {
            if (!reply.ok) {
                self->ConfigSyncServiceImpl::record_history_failure(remote_version,
                    "history_snapshot_missing_mirror_read_failed",
                    std::move(completion));
                return;
            }
            auto mirror = ConfigSyncServiceImpl::parse_hgetall(reply);
            if (!mirror || mirror->empty()) {
                self->ConfigSyncServiceImpl::record_history_failure(remote_version,
                    "history_snapshot_and_mirror_missing",
                    std::move(completion));
                return;
            }
            auto files = std::make_shared<std::map<std::string, std::string>>(
                std::move(*mirror));
            if (!require_meta_hash) {
                self->ConfigSyncServiceImpl::verify_remote_version(
                    remote_version, std::move(files), "",
                    std::move(completion));
                return;
            }
            self->ConfigSyncServiceImpl::verify_snapshot_hash(remote_version, std::move(files),
                "history_snapshot_missing_fallback", std::move(completion));
        });
}

void ConfigSyncServiceImpl::verify_snapshot_hash(
    int64_t remote_version,
    std::shared_ptr<std::map<std::string, std::string>> files,
    std::string fallback_detail,
    Completion completion) {
    auto self = shared_from_this();
    ConfigSyncServiceImpl::run_command({"HGET", std::string(config_history::kMetaKey),
                 std::to_string(remote_version)},
        [self, remote_version, files = std::move(files),
         fallback_detail = std::move(fallback_detail),
         completion = std::move(completion)](const Reply& reply) mutable {
            if (!reply.ok || reply.type != "string") {
                self->ConfigSyncServiceImpl::record_history_failure(remote_version,
                    "history_snapshot_meta_missing", std::move(completion));
                return;
            }
            auto expected = config_history::json_string_field(
                reply.str, "content_sha256");
            auto actual = config_history::content_sha256(*files);
            if (!expected || !actual || *expected != *actual) {
                self->ConfigSyncServiceImpl::record_history_failure(remote_version,
                    "history_snapshot_hash_mismatch", std::move(completion));
                return;
            }
            self->ConfigSyncServiceImpl::verify_remote_version(remote_version, std::move(files),
                std::move(fallback_detail), std::move(completion));
        });
}

void ConfigSyncServiceImpl::record_history_failure(
    int64_t remote_version, std::string detail, Completion completion) {
    State state = ConfigSyncServiceImpl::load_state(config_base_);
    state.exists = true;
    state.status = "partial";
    state.failures["history_snapshot"] = detail;
    if (!ConfigSyncServiceImpl::write_state(config_base_, state)) {
        LOG_ERROR("ConfigSync failed to persist history partial state");
    }
    std::map<std::string, std::string> failures{
        {"history_snapshot", std::move(detail)}
    };
    heartbeat(ConfigSyncServiceImpl::heartbeat_payload(remote_version, "partial", &failures),
        [completion = std::move(completion)]() mutable { completion(false); });
}

void ConfigSyncServiceImpl::verify_remote_version(
    int64_t remote_version,
    std::shared_ptr<std::map<std::string, std::string>> remote_files,
    std::string forced_partial_detail,
    Completion completion) {
    auto self = shared_from_this();
    ConfigSyncServiceImpl::run_command({"GET", std::string(kVersionKey)},
        [self, remote_version, remote_files = std::move(remote_files),
         forced_partial_detail = std::move(forced_partial_detail),
         completion = std::move(completion)](const Reply& reply) mutable {
            auto version_after = ConfigSyncServiceImpl::read_version_reply(reply);
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

            State state = ConfigSyncServiceImpl::load_state(self->config_base_);
            std::map<std::string, std::string> failures;
            const bool applied = self->ConfigSyncServiceImpl::apply_remote_files(
                remote_version, *remote_files, state, failures,
                forced_partial_detail);
            std::string value = applied ?
                ConfigSyncServiceImpl::heartbeat_payload(remote_version, "ok") :
                ConfigSyncServiceImpl::heartbeat_payload(remote_version, "partial", &failures);
            if (applied) {
                self->ConfigSyncServiceImpl::warn_startup_drift_once(remote_version);
            }
            self->ConfigSyncServiceImpl::heartbeat(std::move(value),
                [completion = std::move(completion), applied]() mutable {
                    completion(applied);
                });
        });
}

ConfigSyncServiceImpl::VersionRead ConfigSyncServiceImpl::read_version_reply(
    const Reply& reply) {
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
        auto parsed = ConfigSyncServiceImpl::parse_int64(reply.str);
        if (!parsed || *parsed < 0) {
            return VersionRead{VersionRead::Status::Invalid, 0, reply.str};
        }
        return VersionRead{VersionRead::Status::Value, *parsed, ""};
    }
    return VersionRead{
        VersionRead::Status::Invalid, 0, "unexpected type " + reply.type};
}
