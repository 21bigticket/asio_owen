#include "admin_route_support.hpp"

#include <asio/co_spawn.hpp>

#include <atomic>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <type_traits>

#include "config_admin.hpp"
#include "../config_sync_service.hpp"
#include "../config_history_service.hpp"
#include "../../http/response.hpp"

namespace admin_route_detail {

class AdminHistoryOperation : public std::enable_shared_from_this<AdminHistoryOperation> {
public:
    enum class Kind {
        List,
        Path,
        Rollback,
        SnapshotRepair,
        MirrorRebuild,
        Migration,
        OrphanResolution
    };

    AdminHistoryOperation(HttpContext& ctx, AppServices services,
                          asio::any_io_executor executor,
                          std::function<void()> completion, Kind kind)
        : ctx_(ctx),
          services_(std::move(services)),
          executor_(std::move(executor)),
          executor_guard_(executor_),
          completion_(std::move(completion)),
          kind_(kind) {}

    void start() noexcept {
        auto self = shared_from_this();
        dispatch_admin_work(services_,
            [self]() { self->start_blocking(); },
            [self](std::exception_ptr ep) { self->fail(ep); });
    }

private:
    void start_blocking() noexcept {
        try {
            if (!authorize_admin(ctx_, services_)) {
                complete();
                return;
            }
            if (!redis_command_available(services_)) {
                ctx_.status_code = 503;
                ctx_.response_body = resp_err(SERVER_ERROR, "Redis service unavailable");
                complete();
                return;
            }
            if (kind_ == Kind::Rollback) {
                if (services_.config_history_service &&
                    services_.config_history_service->inconsistent()) {
                    ctx_.status_code = 409;
                    ctx_.response_body = json_resp(409,
                        "history is inconsistent; rollback is frozen");
                    complete();
                    return;
                }
                start_rollback();
            } else if (kind_ == Kind::OrphanResolution) {
                start_orphan_resolution();
            } else if (kind_ == Kind::SnapshotRepair ||
                       kind_ == Kind::MirrorRebuild ||
                       kind_ == Kind::Migration) {
                start_repair();
            } else if (kind_ == Kind::Path) {
                start_path();
            } else {
                start_list();
            }
        } catch (...) {
            fail(std::current_exception());
        }
    }

    using ReplyCallback = std::function<void(RedisPool::Reply)>;
    using DetailCallback =
        std::function<void(std::optional<config_history::SnapshotRecord>)>;

    static std::string path_only(std::string_view path) {
        auto pos = path.find('?');
        return std::string(path.substr(0, pos));
    }

    static std::optional<std::string> query_value(
        std::string_view path, std::string_view key) {
        auto query = path.find('?');
        if (query == std::string_view::npos) return std::nullopt;
        size_t pos = query + 1;
        while (pos <= path.size()) {
            auto end = path.find('&', pos);
            if (end == std::string_view::npos) end = path.size();
            auto eq = path.find('=', pos);
            if (eq != std::string_view::npos && eq < end &&
                path.substr(pos, eq - pos) == key) {
                return std::string(path.substr(eq + 1, end - eq - 1));
            }
            if (end == path.size()) break;
            pos = end + 1;
        }
        return std::nullopt;
    }

    static bool valid_record_hash(const config_history::SnapshotRecord& record) {
        auto expected = config_history::json_string_field(
            record.meta_json, "content_sha256");
        auto actual = config_history::content_sha256(record.files);
        return expected && actual && *expected == *actual;
    }

    // Query handlers are kept in a separate implementation fragment so this
    // operation file remains focused on lifecycle and shared response logic.
#include "admin_history_query_methods.inc"

    void start_rollback() {
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

    void prepare_rollback(
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
        submit_rollback(std::move(files), std::move(*record), info);
    }

    void submit_rollback(
        std::vector<config_admin::ManagedFile> files,
        config_history::SnapshotRecord source,
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
        run_command(std::move(args), [self, sensitive](RedisPool::Reply reply) {
            self->handle_rollback_reply(std::move(reply), sensitive);
        });
    }

    void handle_rollback_reply(RedisPool::Reply reply, bool sensitive) {
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

    void start_repair() {
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
        if (kind_ == Kind::Migration &&
            services_.config_sync.history.read_mode != "compat") {
            ctx_.status_code = 409;
            ctx_.response_body = json_resp(
                409, "migration is allowed only in local compat mode");
            complete();
            return;
        }
        repair_request_ = std::move(parsed.request);
        if (kind_ == Kind::MirrorRebuild) {
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
                        std::move(record->files), record->meta_json,
                        config_history::mirror_rebuild_script(), "mirror-rebuild");
                });
            return;
        }
        read_repair_mirror();
    }

    // Orphan inspection and confirmation live in their own mutation fragment.
#include "admin_history_orphan_methods.inc"

    std::string orphan_audit(
        std::string_view action,
        const std::map<std::string, std::string>& files) const {
        std::vector<config_admin::ManagedFile> audit_files;
        for (const auto& [name, content] : files) {
            audit_files.push_back({name, content});
        }
        return config_admin::audit_json(
            ctx_.admin_principal ? &*ctx_.admin_principal : nullptr,
            orphan_request_.current_version, orphan_request_.target_version,
            action, orphan_request_.reason, audit_files);
    }

    void submit_restore_version(
        std::map<std::string, std::string> files, const std::string& meta) {
        const auto& cfg = services_.config_sync.history;
        std::vector<std::string> args{
            "EVAL", config_history::restore_version_script(), "8",
            std::string(config_admin::kVersionKey),
            std::string(config_admin::kFilesKey),
            std::string(config_history::kIndexKey),
            std::string(config_history::kMetaKey),
            config_history::snapshot_key(orphan_request_.target_version),
            std::string(config_admin::kStagingKey),
            std::string(config_admin::kAuditKey),
            std::string(config_admin::kMachinesKey),
            std::to_string(orphan_request_.current_version),
            std::to_string(orphan_request_.target_version), meta,
            orphan_audit("restore-version", files),
            std::to_string(cfg.max_files),
            std::to_string(cfg.max_file_bytes),
            std::to_string(cfg.max_snapshot_bytes)
        };
        for (const auto& [name, content] : files) {
            args.push_back(name);
            args.push_back(content);
        }
        LOG_ERROR("restoring config version pointer after explicit confirmation: current=",
            orphan_request_.current_version, ", target=",
            orphan_request_.target_version, ", reason=",
            sanitize_body_preview(orphan_request_.reason));
        auto self = shared_from_this();
        run_command(std::move(args), [self](RedisPool::Reply result) {
            self->handle_orphan_resolution_reply(
                std::move(result), "restore-version");
        });
    }

    void submit_delete_orphan(
        std::map<std::string, std::string> files, const std::string& meta,
        bool indexed, bool snapshot_exists) {
        std::vector<std::string> args{
            "EVAL", config_history::delete_orphan_script(), "6",
            std::string(config_admin::kVersionKey),
            std::string(config_history::kIndexKey),
            std::string(config_history::kMetaKey),
            config_history::snapshot_key(orphan_request_.target_version),
            std::string(config_admin::kAuditKey),
            std::string(config_admin::kMachinesKey),
            std::to_string(orphan_request_.current_version),
            std::to_string(orphan_request_.target_version), meta,
            indexed ? "1" : "0", snapshot_exists ? "1" : "0",
            orphan_audit("delete-orphan", files)
        };
        for (const auto& [name, content] : files) {
            args.push_back(name);
            args.push_back(content);
        }
        LOG_ERROR("deleting confirmed unpublished config orphan: current=",
            orphan_request_.current_version, ", target=",
            orphan_request_.target_version, ", reason=",
            sanitize_body_preview(orphan_request_.reason));
        auto self = shared_from_this();
        run_command(std::move(args), [self](RedisPool::Reply result) {
            self->handle_orphan_resolution_reply(
                std::move(result), "delete-orphan");
        });
    }

    void handle_orphan_resolution_reply(
        RedisPool::Reply reply, const std::string& action) {
        auto code = reply.ok ? config_admin::redis_integer(reply) : std::nullopt;
        if (!code || *code <= 0) {
            ctx_.status_code = (!reply.ok || !code || *code == -2 ||
                *code == -3 || *code == -4) ? 500 : 409;
            const std::string message = !reply.ok ? reply.error :
                (!code ? "orphan resolution returned non-integer" :
                    "orphan resolution refused with code " +
                        std::to_string(*code));
            ctx_.response_body = json_resp(ctx_.status_code, message);
            complete();
            return;
        }
        finish_repair_success(action, *code);
    }

    void read_repair_mirror() {
        auto self = shared_from_this();
        run_command({"HGETALL", std::string(config_admin::kFilesKey)},
            [self](RedisPool::Reply reply) {
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
                if (self->kind_ == Kind::Migration) {
                    const std::string user = self->ctx_.admin_principal ?
                        (self->ctx_.admin_principal->username.empty() ?
                            self->ctx_.admin_principal->subject :
                            self->ctx_.admin_principal->username) : "insecure";
                    auto meta = config_history::metadata_json(
                        self->repair_request_.version,
                        self->repair_request_.version,
                        static_cast<int64_t>(std::time(nullptr)), user,
                        "migration", self->repair_request_.reason, info);
                    self->submit_repair(std::move(*files), std::move(meta),
                        config_history::migration_script(), "migration");
                    return;
                }
                self->read_repair_meta(std::move(*files), info.content_sha256);
            });
    }

    void read_repair_meta(
        std::map<std::string, std::string> files, std::string mirror_hash) {
        auto self = shared_from_this();
        run_command({"HGET", std::string(config_history::kMetaKey),
                     std::to_string(repair_request_.version)},
            [self, files = std::move(files), mirror_hash = std::move(mirror_hash)](
                RedisPool::Reply reply) mutable {
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
                self->submit_repair(std::move(files), reply.str,
                    config_history::snapshot_repair_script(), "snapshot-repair");
            });
    }

    void submit_repair(
        std::map<std::string, std::string> files,
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
        if (kind_ == Kind::MirrorRebuild) {
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
            [self, action = std::move(action)](RedisPool::Reply reply) {
                self->handle_repair_reply(std::move(reply), action);
            });
    }

    void handle_repair_reply(RedisPool::Reply reply, const std::string& action) {
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

    void finish_repair_success(const std::string& action, int64_t result) {
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

    // Shared Redis dispatch and response completion are isolated from the
    // query and mutation method fragments above.
#include "admin_history_common_methods.inc"

    HttpContext& ctx_;
    AppServices services_;
    asio::any_io_executor executor_;
    AdminExecutorGuard executor_guard_;
    std::function<void()> completion_;
    Kind kind_;
    config_admin::RollbackRequest rollback_request_;
    config_admin::RepairRequest repair_request_;
    config_admin::OrphanResolutionRequest orphan_request_;
    bool restart_required_ = false;
    std::atomic<bool> completed_{false};
};

asio::awaitable<void> start_admin_history_request(
    HttpContext& ctx, AppServices services, AdminHistoryOperation::Kind kind) {
    asio::use_awaitable_t<> token;
    return asio::async_initiate<asio::use_awaitable_t<>, void()>(
        [&ctx, services = std::move(services), kind](auto handler) mutable {
            asio::any_io_executor executor = asio::get_associated_executor(handler);
            using HandlerType = std::decay_t<decltype(handler)>;
            auto handler_ptr = std::make_shared<HandlerType>(std::move(handler));
            auto completion = [handler_ptr]() mutable { (*handler_ptr)(); };
            auto operation = std::make_shared<AdminHistoryOperation>(
                ctx, std::move(services), std::move(executor),
                std::move(completion), kind);
            operation->start();
        },
        token);
}

}  // namespace admin_route_detail

asio::awaitable<void> handle_api_admin_history(HttpContext& ctx, AppServices services) {
    return admin_route_detail::start_admin_history_request(
        ctx, std::move(services), admin_route_detail::AdminHistoryOperation::Kind::List);
}

asio::awaitable<void> handle_api_admin_history_path(
    HttpContext& ctx, AppServices services) {
    return admin_route_detail::start_admin_history_request(
        ctx, std::move(services), admin_route_detail::AdminHistoryOperation::Kind::Path);
}

asio::awaitable<void> handle_api_admin_rollback(HttpContext& ctx, AppServices services) {
    return admin_route_detail::start_admin_history_request(
        ctx, std::move(services), admin_route_detail::AdminHistoryOperation::Kind::Rollback);
}

asio::awaitable<void> handle_api_admin_snapshot_repair(
    HttpContext& ctx, AppServices services) {
    return admin_route_detail::start_admin_history_request(
        ctx, std::move(services),
        admin_route_detail::AdminHistoryOperation::Kind::SnapshotRepair);
}

asio::awaitable<void> handle_api_admin_mirror_rebuild(
    HttpContext& ctx, AppServices services) {
    return admin_route_detail::start_admin_history_request(
        ctx, std::move(services), admin_route_detail::AdminHistoryOperation::Kind::MirrorRebuild);
}

asio::awaitable<void> handle_api_admin_history_migration(
    HttpContext& ctx, AppServices services) {
    return admin_route_detail::start_admin_history_request(
        ctx, std::move(services), admin_route_detail::AdminHistoryOperation::Kind::Migration);
}

asio::awaitable<void> handle_api_admin_orphan_resolution(
    HttpContext& ctx, AppServices services) {
    return admin_route_detail::start_admin_history_request(
        ctx, std::move(services),
        admin_route_detail::AdminHistoryOperation::Kind::OrphanResolution);
}
