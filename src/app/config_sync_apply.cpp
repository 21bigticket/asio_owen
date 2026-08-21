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

void ConfigSyncServiceImpl::seed_if_eligible(Completion completion) {
    std::string reason;
    {
        State state = ConfigSyncServiceImpl::load_state(config_base_);
        if (!ConfigSyncServiceImpl::has_seed_eligibility(state, reason)) {
            LOG_ERROR("ConfigSync seed refused: ", reason);
            completion(false);
            return;
        }
    }

    auto seeded_state = std::make_shared<State>();
    std::map<std::string, std::string> local_files;
    {
        std::vector<std::string> errors;
        if (!ConfigSyncServiceImpl::collect_local_managed_files(config_base_, local_files, errors)) {
            for (const auto& err : errors) {
                LOG_ERROR("ConfigSync seed refused: ", err);
            }
            completion(false);
            return;
        }

        seeded_state->exists = true;
        seeded_state->synced_version = 1;
        seeded_state->status = "ok";
        for (const auto& [name, content] : local_files) {
            seeded_state->managed_files[name] = ConfigSyncServiceImpl::content_hash(content);
            seeded_state->last_ok[name] = seeded_state->managed_files[name];
        }
    }
    config_history::SnapshotInfo snapshot_info;
    if (auto error = config_history::validate_snapshot(
            local_files, cfg_.history, snapshot_info, false)) {
        LOG_ERROR("ConfigSync seed refused: ", *error);
        completion(false);
        return;
    }
    if (local_files.empty() && cfg_.history.read_mode == "required") {
        LOG_ERROR("ConfigSync seed refused: required history mode cannot publish "
            "an empty snapshot");
        completion(false);
        return;
    }
    const int64_t timestamp = static_cast<int64_t>(std::time(nullptr));
    std::vector<std::string> args{
        "EVAL", ConfigSyncServiceImpl::seed_script(), "7",
        std::string(kVersionKey), std::string(kFilesKey), std::string(kStagingKey),
        std::string(config_history::kMetaKey),
        std::string(config_history::kIndexKey),
        config_history::snapshot_key(1),
        std::string(config_history::kSnapshotStagingKey),
        config_history::metadata_json(
            1, 0, timestamp, ConfigSyncServiceImpl::machine_name(), "seed",
            "initial configuration seed", snapshot_info),
        std::to_string(cfg_.history.max_files),
        std::to_string(cfg_.history.max_file_bytes),
        std::to_string(cfg_.history.max_snapshot_bytes)
    };
    for (const auto& [name, content] : local_files) {
        args.push_back(name);
        args.push_back(content);
    }
    const size_t local_file_count = local_files.size();
    auto self = shared_from_this();
    ConfigSyncServiceImpl::run_command(std::move(args),
        [self, seeded_state = std::move(seeded_state), local_file_count,
         completion = std::move(completion)](const Reply& reply) mutable {
            if (!reply.ok) {
                LOG_ERROR("ConfigSync seed EVAL failed: ", reply.error);
                completion(false);
                return;
            }
            auto code = ConfigSyncServiceImpl::script_integer(reply);
            if (!code) {
                LOG_ERROR("ConfigSync seed EVAL returned non-integer type=", reply.type);
                completion(false);
                return;
            }
            if (*code == 1) {
                if (!ConfigSyncServiceImpl::write_state(self->config_base_, *seeded_state)) {
                    LOG_ERROR("ConfigSync failed to write state after seed");
                    completion(false);
                    return;
                }
                self->heartbeat(ConfigSyncServiceImpl::heartbeat_payload(1, "ok"),
                    [self, local_file_count,
                     completion = std::move(completion)]() mutable {
                        self->ConfigSyncServiceImpl::warn_startup_drift_once(1);
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

bool ConfigSyncServiceImpl::has_seed_eligibility(const State& state, std::string& reason) const {
    if (!state.exists) return true;
    if (state.status != "ok") {
        reason = "state status is not ok";
        return false;
    }
    std::map<std::string, std::string> local_files;
    std::vector<std::string> errors;
    if (!ConfigSyncServiceImpl::collect_local_managed_files(config_base_, local_files, errors)) {
        reason = errors.empty() ? "failed to collect local files" : errors.front();
        return false;
    }
    std::map<std::string, std::string> local_hashes;
    for (const auto& [name, content] : local_files) {
        local_hashes[name] = ConfigSyncServiceImpl::content_hash(content);
    }
    if (local_hashes != state.last_ok) {
        reason = "local files do not match last_ok state";
        return false;
    }
    return true;
}

bool ConfigSyncServiceImpl::apply_remote_files(int64_t version,
                        const std::map<std::string, std::string>& remote_files,
                        const State& previous_state,
                        std::map<std::string, std::string>& failures,
                        const std::string& forced_partial_detail) {
    State next = previous_state;
    next.status = "partial";
    next.failures.clear();
    failures.clear();

    auto persist_partial = [&]() {
        failures = next.failures;
        if (!ConfigSyncServiceImpl::write_state(config_base_, next)) {
            LOG_ERROR("ConfigSync failed to write partial state");
        }
        for (const auto& [name, reason] : next.failures) {
            LOG_ERROR("ConfigSync partial sync: file=", name, ", reason=", reason);
        }
        return false;
    };

    std::set<std::string> local_managed_names =
        ConfigSyncServiceImpl::collect_local_managed_file_names(config_base_);
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
        auto validation = ConfigSyncServiceImpl::validate_managed_file(name, content);
        if (!validation.ok) {
            next.failures[name] = validation.reason;
            continue;
        }
        valid_files[name] = content;
    }

    for (const auto& [name, content] : valid_files) {
        if (!ConfigSyncServiceImpl::write_managed_file_if_changed(name, content)) {
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
            std::filesystem::remove(ConfigSyncServiceImpl::config_dir() / name, ec);
            if (ec) {
                next.failures[name] = "failed to remove stale file: " + ec.message();
            }
        }
    }

    if (!next.failures.empty()) {
        return persist_partial();
    }

    State clean;
    clean.exists = true;
    clean.synced_version = version;
    clean.status = forced_partial_detail.empty() ? "ok" : "partial";
    for (const auto& [name, content] : remote_files) {
        clean.managed_files[name] = ConfigSyncServiceImpl::content_hash(content);
        clean.last_ok[name] = clean.managed_files[name];
    }
    if (!forced_partial_detail.empty()) {
        clean.failures["history_snapshot"] = forced_partial_detail;
    }
    if (!ConfigSyncServiceImpl::write_state(config_base_, clean)) {
        LOG_ERROR("ConfigSync failed to write ok state");
        return false;
    }
    if (!forced_partial_detail.empty()) {
        failures = clean.failures;
        LOG_ERROR("ConfigSync applied verified mirror fallback for version ",
            version, ", snapshot remains unhealthy");
        return false;
    }
    LOG_INFO("ConfigSync applied version ", version, ", files=", remote_files.size());
    return true;
}

std::string ConfigSyncServiceImpl::heartbeat_payload(
    int64_t version,
    std::string_view status,
    const std::map<std::string, std::string>* failures) {
    std::string value = std::to_string(version) + "|" +
        std::to_string(static_cast<int64_t>(std::time(nullptr))) + "|" +
        std::to_string(static_cast<int64_t>(getpid())) + "|" + std::string(status);
    if (failures && !failures->empty()) {
        value += "|";
        bool first = true;
        for (const auto& [name, reason] : *failures) {
            if (!first) value += ",";
            first = false;
            value += name;
            value += ":";
            value += reason;
        }
    }
    return value;
}

void ConfigSyncServiceImpl::heartbeat(std::string value, std::function<void()> completion) {
    ConfigSyncServiceImpl::run_command({
            "HSET", std::string(kMachinesKey), ConfigSyncServiceImpl::machine_name(), std::move(value)
        },
        [completion = std::move(completion)](const Reply& reply) mutable {
            if (!reply.ok) {
                LOG_WARN("ConfigSync heartbeat failed: ", reply.error);
            }
            completion();
        });
}
