#include "admin_route_support.hpp"

#include <asio/co_spawn.hpp>

#include <atomic>
#include <limits>
#include <type_traits>

#include "config_admin.hpp"
#include "../config_history_service.hpp"
#include "../../http/response.hpp"

namespace admin_route_detail {

class AdminRequestOperation : public std::enable_shared_from_this<AdminRequestOperation> {
public:
    using StartAction = std::function<void(AdminRequestOperation&)>;

    AdminRequestOperation(HttpContext& ctx, AppServices services,
                          asio::any_io_executor executor,
                          std::function<void()> completion, StartAction start_action)
        : ctx_(ctx),
          services_(std::move(services)),
          executor_(std::move(executor)),
          executor_guard_(executor_),
          completion_(std::move(completion)),
          start_action_(std::move(start_action)) {}

    void start() {
        auto self = shared_from_this();
        dispatch_admin_work(services_,
            [self]() { self->start_blocking(); },
            [self](const std::exception_ptr& ep) { self->fail(ep); });
    }

    void start_config() {
        if (ctx_.method == "GET") {
            read_config_version(0);
        } else if (ctx_.method == "POST") {
            save_config();
        } else {
            method_not_allowed(ctx_, "GET, POST");
            complete();
        }
    }

    void start_machines_request() {
        start_machines();
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
            start_action_(*this);
        } catch (...) {
            fail(std::current_exception());
        }
    }

    using ReplyCallback = std::function<void(RedisPool::Reply)>;

    void read_config_version(int attempt) {
        auto self = shared_from_this();
        run_command({"GET", std::string(config_admin::kVersionKey)},
            [self, attempt](const RedisPool::Reply& reply) {
                if (!reply.ok) {
                    self->ctx_.status_code = 500;
                    self->ctx_.response_body = resp_err(DB_ERROR, reply.error);
                    self->complete();
                    return;
                }
                auto version = config_admin::redis_version(reply);
                if (!version) {
                    self->invalid_version();
                    return;
                }
                self->read_config_files(*version, attempt);
            });
    }

    void read_config_files(int64_t version, int attempt) {
        auto self = shared_from_this();
        run_command({"HGETALL", config_history::snapshot_key(version)},
            [self, version, attempt](const RedisPool::Reply& reply) {
                if (!reply.ok) {
                    self->ctx_.status_code = 500;
                    self->ctx_.response_body = resp_err(DB_ERROR, reply.error);
                    self->complete();
                    return;
                }
                auto files = config_admin::parse_hgetall(reply);
                if (!files) {
                    self->ctx_.status_code = 500;
                    self->ctx_.response_body = resp_err(
                        SERVER_ERROR, "files hash returned malformed data");
                    self->complete();
                    return;
                }
                auto snapshot = std::make_shared<std::map<std::string, std::string>>(
                    std::move(*files));
                if (snapshot->empty()) {
                    self->read_config_mirror(version, attempt);
                    return;
                }
                self->verify_config_hash(
                    version, attempt, std::move(snapshot), false);
            });
    }

    void read_config_mirror(int64_t version, int attempt) {
        auto self = shared_from_this();
        run_command({"HGETALL", std::string(config_admin::kFilesKey)},
            [self, version, attempt](const RedisPool::Reply& reply) {
                if (!reply.ok) {
                    self->ctx_.status_code = 500;
                    self->ctx_.response_body = resp_err(DB_ERROR, reply.error);
                    self->complete();
                    return;
                }
                auto files = config_admin::parse_hgetall(reply);
                if (!files || files->empty()) {
                    self->ctx_.status_code = 409;
                    self->ctx_.response_body = json_resp(
                        409, "current history snapshot is missing");
                    self->complete();
                    return;
                }
                auto mirror = std::make_shared<std::map<std::string, std::string>>(
                    std::move(*files));
                if (self->services_.config_sync.history.read_mode == "compat") {
                    self->verify_config_version(
                        version, attempt, std::move(mirror), true);
                    return;
                }
                self->verify_config_hash(version, attempt, std::move(mirror), true);
            });
    }

    void verify_config_hash(
        int64_t version, int attempt,
        std::shared_ptr<std::map<std::string, std::string>> files,
        bool degraded) {
        auto self = shared_from_this();
        run_command({"HGET", std::string(config_history::kMetaKey),
                     std::to_string(version)},
            [self, version, attempt, files = std::move(files), degraded](
                const RedisPool::Reply& reply) mutable {
                auto expected = reply.ok && reply.type == "string" ?
                    config_history::json_string_field(
                        reply.str, "content_sha256") : std::nullopt;
                auto actual = config_history::content_sha256(*files);
                if (!expected || !actual || *expected != *actual) {
                    self->ctx_.status_code = 409;
                    self->ctx_.response_body = json_resp(
                        409, "current history snapshot hash is inconsistent");
                    self->complete();
                    return;
                }
                self->verify_config_version(
                    version, attempt, std::move(files), degraded);
            });
    }

    void verify_config_version(
        int64_t version, int attempt,
        std::shared_ptr<std::map<std::string, std::string>> files,
        bool degraded = false) {
        auto self = shared_from_this();
        run_command({"GET", std::string(config_admin::kVersionKey)},
            [self, version, attempt, files = std::move(files), degraded](
                const RedisPool::Reply& reply) {
                auto version_after = config_admin::redis_version(reply);
                if (!version_after) {
                    self->invalid_version();
                    return;
                }
                if (*version_after != version) {
                    if (attempt == 0) {
                        self->read_config_version(1);
                    } else {
                        self->ctx_.status_code = 409;
                        self->ctx_.response_body = json_resp(
                            409, "version changed during read, retry");
                        self->complete();
                    }
                    return;
                }
                self->ctx_.status_code = 200;
                self->ctx_.response_body = resp_ok(
                    config_admin::files_json(version, *files, degraded));
                self->complete();
            });
    }

    void save_config() {
        if (services_.config_history_service &&
            services_.config_history_service->inconsistent()) {
            ctx_.status_code = 409;
            ctx_.response_body = json_resp(
                409, "history is inconsistent; save, rollback and GC are frozen");
            complete();
            return;
        }
        auto parsed = config_admin::parse_save_request(ctx_.body);
        if (!parsed.ok) {
            ctx_.status_code = 400;
            ctx_.response_body = resp_err(PARAM_ERROR, parsed.error);
            complete();
            return;
        }
        if (auto error = config_admin::validate_file_set(parsed.request.files, true)) {
            ctx_.status_code = 400;
            ctx_.response_body = resp_err(PARAM_ERROR, *error);
            complete();
            return;
        }
        const auto& history_cfg = services_.config_sync.history;
        if (parsed.request.reason.size() > history_cfg.max_reason_bytes) {
            ctx_.status_code = 400;
            ctx_.response_body = resp_err(PARAM_ERROR,
                "reason exceeds " + std::to_string(history_cfg.max_reason_bytes) +
                " bytes");
            complete();
            return;
        }
        if (parsed.request.base_version == std::numeric_limits<int64_t>::max()) {
            ctx_.status_code = 400;
            ctx_.response_body = resp_err(PARAM_ERROR, "base_version is too large");
            complete();
            return;
        }
        std::map<std::string, std::string> snapshot;
        for (const auto& file : parsed.request.files) {
            snapshot.emplace(file.name, file.content);
        }
        config_history::SnapshotInfo snapshot_info;
        if (auto error = config_history::validate_snapshot(
                snapshot, history_cfg, snapshot_info)) {
            ctx_.status_code = 400;
            ctx_.response_body = resp_err(PARAM_ERROR, *error);
            complete();
            return;
        }
        if (snapshot_info.file_count >= history_cfg.warn_files ||
            snapshot_info.total_bytes >= history_cfg.warn_snapshot_bytes) {
            LOG_WARN("config history snapshot near capacity: files=",
                snapshot_info.file_count, ", bytes=", snapshot_info.total_bytes);
        }
        for (const auto& file : parsed.request.files) {
            if (file.content.size() >= history_cfg.warn_file_bytes) {
                LOG_WARN("config history file near capacity: name=", file.name,
                    ", bytes=", file.content.size());
            }
        }
        if (services_.config_base.empty()) {
            ctx_.status_code = 500;
            ctx_.response_body = resp_err(SERVER_ERROR, "config base is unavailable");
            complete();
            return;
        }
        if (auto error = config_admin::dry_run_config_set(
                services_.config_base, parsed.request.files)) {
            ctx_.status_code = 400;
            ctx_.response_body = resp_err(PARAM_ERROR, "dry-run rejected: " + *error);
            complete();
            return;
        }

        const int64_t new_version = parsed.request.base_version + 1;
        const int64_t timestamp = static_cast<int64_t>(std::time(nullptr));
        const std::string user = ctx_.admin_principal ?
            (ctx_.admin_principal->username.empty() ?
                ctx_.admin_principal->subject : ctx_.admin_principal->username) :
            "insecure";
        const std::string meta = config_history::metadata_json(
            new_version, parsed.request.base_version, timestamp, user, "save",
            parsed.request.reason, snapshot_info);

        std::vector<std::string> args{
            "EVAL", config_admin::save_script(), "9",
            std::string(config_admin::kVersionKey),
            std::string(config_admin::kFilesKey),
            std::string(config_admin::kAuditKey),
            std::string(config_admin::kStagingKey),
            std::string(config_history::kMetaKey),
            std::string(config_history::kIndexKey),
            config_history::snapshot_key(new_version),
            std::string(config_history::kSnapshotStagingKey),
            config_history::snapshot_key(parsed.request.base_version),
            std::to_string(parsed.request.base_version),
            config_admin::audit_json(
                ctx_.admin_principal ? &*ctx_.admin_principal : nullptr,
                parsed.request.base_version, new_version, "save",
                parsed.request.reason, parsed.request.files),
            meta,
            std::to_string(history_cfg.max_files),
            std::to_string(history_cfg.max_file_bytes),
            std::to_string(history_cfg.max_snapshot_bytes)
        };
        for (const auto& file : parsed.request.files) {
            args.push_back(file.name);
            args.push_back(file.content);
        }

        auto self = shared_from_this();
        run_command(std::move(args), [self](const RedisPool::Reply& reply) {
            self->handle_save_reply(reply);
        });
    }

    void handle_save_reply(const RedisPool::Reply& reply) {
        if (!reply.ok) {
            ctx_.status_code = 500;
            ctx_.response_body = resp_err(DB_ERROR, reply.error);
            complete();
            return;
        }
        auto code = config_admin::redis_integer(reply);
        if (!code) {
            ctx_.status_code = 500;
            ctx_.response_body = resp_err(
                SERVER_ERROR, "save script returned non-integer");
            complete();
            return;
        }
        if (*code > 0) {
            ctx_.status_code = 200;
            ctx_.response_body = resp_ok(
                "{\"version\":" + std::to_string(*code) + "}");
            complete();
            return;
        }
        if (*code == -1) {
            read_conflict_version();
            return;
        }

        ctx_.status_code = 500;
        if (*code == -2) {
            ctx_.response_body = resp_err(
                SERVER_ERROR, "save script rejected invalid argv");
        } else if (*code == -3) {
            ctx_.response_body = resp_err(
                SERVER_ERROR, "config Redis key type is invalid");
        } else if (*code == -4) {
            ctx_.response_body = resp_err(SERVER_ERROR,
                "version key content is not numeric, manual repair required");
        } else if (*code == -5) {
            ctx_.status_code = 409;
            ctx_.response_body = json_resp(409,
                "history is inconsistent; save, rollback and GC are frozen");
        } else if (*code == -6) {
            ctx_.status_code = 409;
            ctx_.response_body = json_resp(409,
                "history target already exists; manual repair required");
        } else if (*code == -7) {
            ctx_.response_body = resp_err(SERVER_ERROR,
                "version publish returned an unexpected value");
        } else if (*code == -8) {
            ctx_.status_code = 400;
            ctx_.response_body = resp_err(PARAM_ERROR,
                "snapshot exceeds configured capacity limits");
        } else {
            ctx_.response_body = resp_err(SERVER_ERROR,
                "save script returned unexpected code " + std::to_string(*code));
        }
        complete();
    }

    void read_conflict_version() {
        auto self = shared_from_this();
        run_command({"GET", std::string(config_admin::kVersionKey)},
            [self](const RedisPool::Reply& reply) {
                auto current_version = config_admin::redis_version(reply);
                std::string data = current_version ?
                    "{\"current_version\":" + std::to_string(*current_version) + "}" :
                    "{\"current_version\":null}";
                self->ctx_.status_code = 409;
                self->ctx_.response_body = json_resp(
                    409, "version conflict", data);
                self->complete();
            });
    }

    void start_machines() {
        if (ctx_.method != "GET") {
            method_not_allowed(ctx_, "GET");
            complete();
            return;
        }
        auto self = shared_from_this();
        run_command({"HGETALL", std::string(config_admin::kMachinesKey)},
            [self](const RedisPool::Reply& reply) {
                if (!reply.ok) {
                    self->ctx_.status_code = 500;
                    self->ctx_.response_body = resp_err(DB_ERROR, reply.error);
                    self->complete();
                    return;
                }
                auto machines = config_admin::parse_hgetall(reply);
                if (!machines) {
                    self->ctx_.status_code = 500;
                    self->ctx_.response_body = resp_err(
                        SERVER_ERROR, "machines hash returned malformed data");
                    self->complete();
                    return;
                }
                self->ctx_.status_code = 200;
                self->ctx_.response_body = resp_ok(
                    config_admin::machines_json(*machines));
                self->complete();
            });
    }

    void run_command(std::vector<std::string> args, ReplyCallback callback) {
        auto self = shared_from_this();
        dispatch_redis_command(
            services_, executor_, std::move(args), std::move(callback),
            [self](const std::exception_ptr& ep) { self->fail(ep); });
    }

    void invalid_version() {
        ctx_.status_code = 500;
        ctx_.response_body = resp_err(SERVER_ERROR,
            "version key content is not numeric, manual repair required");
        complete();
    }

    void fail(const std::exception_ptr& ep) noexcept {
        std::string message = "unknown admin request exception";
        if (ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                message = e.what();
            } catch (...) {
            }
        }
        try {
            ctx_.status_code = 500;
            ctx_.response_body = resp_err(SERVER_ERROR, message);
        } catch (...) {
        }
        complete();
    }

    void complete() noexcept {
        executor_guard_.release();
        complete_admin_request(completed_, executor_, completion_);
    }

    HttpContext& ctx_;
    AppServices services_;
    asio::any_io_executor executor_;
    AdminExecutorGuard executor_guard_;
    std::function<void()> completion_;
    StartAction start_action_;
    std::atomic<bool> completed_{false};
};

asio::awaitable<void> start_admin_request(
    HttpContext& ctx, AppServices services,
    AdminRequestOperation::StartAction start_action) {
    asio::use_awaitable_t<> token;
    return asio::async_initiate<asio::use_awaitable_t<>, void()>(
        [&ctx, services = std::move(services),
         start_action = std::move(start_action)](auto handler) mutable {
            asio::any_io_executor executor = asio::get_associated_executor(handler);
            using HandlerType = std::decay_t<decltype(handler)>;
            auto handler_ptr = std::make_shared<HandlerType>(std::move(handler));
            auto completion = [handler_ptr]() mutable { (*handler_ptr)(); };
            auto operation = std::make_shared<AdminRequestOperation>(
                ctx, std::move(services), std::move(executor),
                std::move(completion), std::move(start_action));
            operation->start();
        },
        token);
}

}  // namespace admin_route_detail

asio::awaitable<void> handle_api_admin_config(HttpContext& ctx, AppServices services) {
    return admin_route_detail::start_admin_request(
        ctx, std::move(services),
        [](auto& operation) { operation.start_config(); });
}

asio::awaitable<void> handle_api_admin_machines(HttpContext& ctx, AppServices services) {
    return admin_route_detail::start_admin_request(
        ctx, std::move(services),
        [](auto& operation) { operation.start_machines_request(); });
}
