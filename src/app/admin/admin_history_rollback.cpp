#include "admin_history_operation.hpp"

#include <algorithm>
#include <ctime>
#include <limits>
#include <sstream>

#include "../config_sync_service.hpp"
#include "../../common/logger.hpp"
#include "../../http/response.hpp"

namespace admin_route_detail {

void AdminHistoryOperation::start_rollback() {
    if (ctx_.method != "POST") {
        method_not_allowed(ctx_, "POST");
        complete();
        return;
    }
    auto parsed = config_admin::parse_rollback_request(ctx_.body);
    if (!parsed.ok) {
        bad_request(parsed.error);
        return;
    }
    if (parsed.request.reason.size() >
        services_.config_sync.history.max_reason_bytes) {
        bad_request("reason exceeds configured limit");
        return;
    }
    if (parsed.request.base_version == std::numeric_limits<int64_t>::max()) {
        bad_request("base_version is too large");
        return;
    }
    rollback_request_ = std::move(parsed.request);
    auto self = shared_from_this();
    read_detail(rollback_request_.target_version,
        [self](std::optional<config_history::SnapshotRecord> record) {
            self->prepare_rollback(std::move(record));
        });
}

void AdminHistoryOperation::prepare_rollback(
    std::optional<config_history::SnapshotRecord> record) {
    if (!record) {
        not_found();
        return;
    }
    if (record->current_version != rollback_request_.base_version) {
        ctx_.status_code = 409;
        ctx_.response_body = json_resp(409, "version conflict",
            "{\"current_version\":" +
            std::to_string(record->current_version) + "}");
        complete();
        return;
    }
    if (!valid_record_hash(*record)) {
        inconsistent("rollback source hash mismatch");
        return;
    }
    std::vector<config_admin::ManagedFile> files;
    std::vector<std::string> conflicts;
    for (const auto& [name, content] : record->files) {
        auto validation = ConfigSyncService::validate_managed_file(name, content);
        if (!validation.ok) {
            conflicts.push_back(name + ": " + validation.reason);
        }
        files.push_back({name, content});
    }
    if (!conflicts.empty()) {
        std::ostringstream data;
        data << "{\"conflicts\":[";
        for (size_t i = 0; i < conflicts.size(); ++i) {
            if (i > 0) data << ",";
            data << "\"" << json_escape(conflicts[i]) << "\"";
        }
        data << "]}";
        ctx_.status_code = 409;
        ctx_.response_body = json_resp(
            409, "rollback conflicts with current managed-file rules", data.str());
        complete();
        return;
    }
    if (auto error = config_admin::dry_run_config_set(
            services_.config_base, files)) {
        ctx_.status_code = 409;
        ctx_.response_body = json_resp(
            409, "rollback rejected by current validation: " + *error);
        complete();
        return;
    }
    config_history::SnapshotInfo info;
    if (auto error = config_history::validate_snapshot(
            record->files, services_.config_sync.history, info)) {
        ctx_.status_code = 409;
        ctx_.response_body = json_resp(409, *error);
        complete();
        return;
    }
    submit_rollback(std::move(files), *record, info);
}

void AdminHistoryOperation::submit_rollback(
    std::vector<config_admin::ManagedFile> files,
    const config_history::SnapshotRecord& source,
    const config_history::SnapshotInfo& info) {
    const int64_t new_version = rollback_request_.base_version + 1;
    const std::string user = ctx_.admin_principal ?
        (ctx_.admin_principal->username.empty() ?
            ctx_.admin_principal->subject : ctx_.admin_principal->username) :
        "insecure";
    const std::string meta = config_history::metadata_json(
        new_version, rollback_request_.base_version,
        static_cast<int64_t>(std::time(nullptr)), user, "rollback",
        rollback_request_.reason, info, rollback_request_.target_version);
    std::vector<std::string> args{
        "EVAL", config_history::rollback_script(), "10",
        std::string(config_admin::kVersionKey),
        std::string(config_admin::kFilesKey),
        std::string(config_admin::kAuditKey),
        std::string(config_admin::kStagingKey),
        std::string(config_history::kMetaKey),
        std::string(config_history::kIndexKey),
        config_history::snapshot_key(new_version),
        std::string(config_history::kSnapshotStagingKey),
        config_history::snapshot_key(rollback_request_.target_version),
        config_history::snapshot_key(rollback_request_.base_version),
        std::to_string(rollback_request_.base_version),
        config_admin::audit_json(
            ctx_.admin_principal ? &*ctx_.admin_principal : nullptr,
            rollback_request_.base_version, new_version, "rollback",
            rollback_request_.reason, files),
        meta,
        std::to_string(services_.config_sync.history.max_files),
        std::to_string(services_.config_sync.history.max_file_bytes),
        std::to_string(services_.config_sync.history.max_snapshot_bytes),
        source.meta_json,
        std::to_string(rollback_request_.target_version)
    };
    for (const auto& file : files) {
        args.push_back(file.name);
        args.push_back(file.content);
    }
    const bool sensitive = std::any_of(files.begin(), files.end(),
        [](const config_admin::ManagedFile& file) {
            return config_history::looks_sensitive(file.content);
        });
    if (sensitive) {
        LOG_WARN("sensitive configuration rollback requested: from=",
            rollback_request_.base_version, ", target=",
            rollback_request_.target_version, ", user=", user);
    }
    auto self = shared_from_this();
    run_command(std::move(args), [self, sensitive](const RedisPool::Reply& reply) {
        self->handle_rollback_reply(reply, sensitive);
    });
}

void AdminHistoryOperation::handle_rollback_reply(const RedisPool::Reply& reply, bool sensitive) {
    if (!reply.ok) {
        redis_failed(reply.error);
        return;
    }
    auto code = config_admin::redis_integer(reply);
    if (!code) {
        inconsistent("rollback script returned non-integer");
        return;
    }
    if (*code > 0) {
        ctx_.status_code = 200;
        ctx_.response_body = resp_ok(
            "{\"version\":" + std::to_string(*code) +
            ",\"rollback_from\":" +
            std::to_string(rollback_request_.target_version) +
            ",\"restart_required\":" +
            (restart_required_ ? "true" : "false") +
            ",\"sensitive\":" + (sensitive ? "true" : "false") + "}");
        complete();
        return;
    }
    ctx_.status_code = 500;
    if (*code == -1 || *code == -9) {
        ctx_.status_code = 409;
        ctx_.response_body = json_resp(409,
            *code == -1 ? "version conflict" :
                "rollback source changed or was deleted; refresh and retry");
    } else if (*code == -5 || *code == -6) {
        ctx_.status_code = 409;
        ctx_.response_body = json_resp(409,
            "history is inconsistent; rollback is frozen");
    } else if (*code == -8) {
        ctx_.status_code = 409;
        ctx_.response_body = json_resp(409,
            "rollback snapshot exceeds current capacity limits");
    } else if (*code == -4) {
        ctx_.response_body = resp_err(SERVER_ERROR,
            "version key content is not numeric, manual repair required");
    } else {
        ctx_.response_body = resp_err(SERVER_ERROR,
            "rollback script failed with code " + std::to_string(*code));
    }
    complete();
}

}  // namespace admin_route_detail
