#include "admin_history_operation.hpp"

#include <ctime>

#include "../config_history_service.hpp"
#include "../../http/response.hpp"

namespace admin_route_detail {

void AdminHistoryOperation::start_repair() {
    if (ctx_.method != "POST") {
        method_not_allowed(ctx_, "POST");
        complete();
        return;
    }
    auto parsed = config_admin::parse_repair_request(ctx_.body);
    if (!parsed.ok) {
        bad_request(parsed.error);
        return;
    }
    if (parsed.request.reason.size() >
        services_.config_sync.history.max_reason_bytes) {
        bad_request("reason exceeds configured limit");
        return;
    }
    if (repair_plan_.compat_only &&
        services_.config_sync.history.read_mode != "compat") {
        ctx_.status_code = 409;
        ctx_.response_body = json_resp(
            409, "migration is allowed only in local compat mode");
        complete();
        return;
    }
    repair_request_ = std::move(parsed.request);
    if (repair_plan_.read_history_snapshot) {
        auto self = shared_from_this();
        read_detail(repair_request_.version,
            [self](std::optional<config_history::SnapshotRecord> record) {
                if (!record ||
                    record->current_version != self->repair_request_.version) {
                    self->inconsistent(
                        "current history snapshot is not complete");
                    return;
                }
                if (!valid_record_hash(*record)) {
                    self->inconsistent("current history snapshot hash mismatch");
                    return;
                }
                self->submit_repair(
                    record->files, record->meta_json,
                    self->repair_plan_.script(), self->repair_plan_.action);
            });
        return;
    }
    read_repair_mirror();
}



void AdminHistoryOperation::read_repair_mirror() {
    auto self = shared_from_this();
    run_command({"HGETALL", std::string(config_admin::kFilesKey)},
        [self](const RedisPool::Reply& reply) {
            auto files = config_admin::parse_hgetall(reply);
            if (!files || files->empty()) {
                self->inconsistent("current mirror is missing or malformed");
                return;
            }
            config_history::SnapshotInfo info;
            if (auto error = config_history::validate_snapshot(
                    *files, self->services_.config_sync.history, info)) {
                self->inconsistent(*error);
                return;
            }
            if (self->repair_plan_.synthesize_metadata) {
                const std::string user = self->ctx_.admin_principal ?
                    (self->ctx_.admin_principal->username.empty() ?
                        self->ctx_.admin_principal->subject :
                        self->ctx_.admin_principal->username) : "insecure";
                auto meta = config_history::metadata_json(
                    self->repair_request_.version,
                    self->repair_request_.version,
                    static_cast<int64_t>(std::time(nullptr)), user,
                    "migration", self->repair_request_.reason, info);
                self->submit_repair(*files, std::move(meta),
                    self->repair_plan_.script(), self->repair_plan_.action);
                return;
            }
            self->read_repair_meta(std::move(*files), info.content_sha256);
        });
}

void AdminHistoryOperation::read_repair_meta(
    std::map<std::string, std::string> files, std::string mirror_hash) {
    auto self = shared_from_this();
    run_command({"HGET", std::string(config_history::kMetaKey),
                 std::to_string(repair_request_.version)},
        [self, files = std::move(files), mirror_hash = std::move(mirror_hash)](
            const RedisPool::Reply& reply) mutable {
            if (!reply.ok || reply.type != "string") {
                self->inconsistent("current history metadata is missing");
                return;
            }
            auto expected = config_history::json_string_field(
                reply.str, "content_sha256");
            if (!expected || *expected != mirror_hash) {
                self->inconsistent(
                    "current mirror hash does not match history metadata");
                return;
            }
            self->submit_repair(files, reply.str,
                self->repair_plan_.script(), self->repair_plan_.action);
        });
}

void AdminHistoryOperation::submit_repair(
    const std::map<std::string, std::string>& files,
    std::string meta,
    std::string script,
    std::string action) {
    std::vector<config_admin::ManagedFile> audit_files;
    audit_files.reserve(files.size());
    for (const auto& [name, content] : files) {
        audit_files.push_back({name, content});
    }
    const auto& cfg = services_.config_sync.history;
    std::vector<std::string> keys{
        std::string(config_admin::kVersionKey),
        std::string(config_admin::kFilesKey),
        std::string(config_history::kMetaKey),
        std::string(config_history::kIndexKey)
    };
    if (repair_plan_.use_config_staging) {
        keys.push_back(config_history::snapshot_key(repair_request_.version));
        keys.push_back(std::string(config_admin::kStagingKey));
    } else {
        keys.push_back(config_history::snapshot_key(repair_request_.version));
        keys.push_back(std::string(config_history::kSnapshotStagingKey));
    }
    keys.push_back(std::string(config_admin::kAuditKey));
    std::vector<std::string> args{"EVAL", std::move(script), "7"};
    args.insert(args.end(), keys.begin(), keys.end());
    args.push_back(std::to_string(repair_request_.version));
    args.push_back(std::move(meta));
    args.push_back(config_admin::audit_json(
        ctx_.admin_principal ? &*ctx_.admin_principal : nullptr,
        repair_request_.version, repair_request_.version, action,
        repair_request_.reason, audit_files));
    args.push_back(std::to_string(cfg.max_files));
    args.push_back(std::to_string(cfg.max_file_bytes));
    args.push_back(std::to_string(cfg.max_snapshot_bytes));
    for (const auto& [name, content] : files) {
        args.push_back(name);
        args.push_back(content);
    }
    LOG_WARN("config history repair requested: action=", action,
        ", version=", repair_request_.version,
        ", reason=", sanitize_body_preview(repair_request_.reason));
    auto self = shared_from_this();
    run_command(std::move(args),
        [self, action = std::move(action)](const RedisPool::Reply& reply) {
            self->handle_repair_reply(reply, action);
        });
}

void AdminHistoryOperation::handle_repair_reply(const RedisPool::Reply& reply, const std::string& action) {
    if (!reply.ok) {
        redis_failed(reply.error);
        return;
    }
    auto code = config_admin::redis_integer(reply);
    if (!code) {
        inconsistent("history repair returned non-integer");
        return;
    }
    if (*code > 0) {
        finish_repair_success(action, *code);
        return;
    }
    ctx_.status_code = (*code == -2 || *code == -3 || *code == -4) ? 500 : 409;
    ctx_.response_body = json_resp(ctx_.status_code,
        "history repair refused with code " + std::to_string(*code));
    complete();
}

void AdminHistoryOperation::finish_repair_success(const std::string& action, int64_t result) {
    auto self = shared_from_this();
    auto respond = [self, action, result]() {
        const bool consistent = !self->services_.config_history_service ||
            !self->services_.config_history_service->inconsistent();
        const int64_t version = action == "delete-orphan" ?
            self->orphan_request_.current_version : result;
        self->ctx_.status_code = 200;
        self->ctx_.response_body = resp_ok(
            "{\"version\":" + std::to_string(version) +
            ",\"action\":\"" + json_escape(action) +
            "\",\"history_consistent\":" +
            (consistent ? "true" : "false") + "}");
        self->complete();
    };
    if (services_.config_history_service) {
        services_.config_history_service->refresh(std::move(respond));
    } else {
        respond();
    }
}

}  // namespace admin_route_detail
