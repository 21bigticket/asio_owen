#include "routes.hpp"

#include <asio/co_spawn.hpp>
#include <asio/this_coro.hpp>

#include <atomic>
#include <chrono>
#include <ctime>
#include <map>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <type_traits>
#include <unordered_map>

#include "admin/config_admin.hpp"
#include "combo_deadline.hpp"
#include "config_history_service.hpp"
#include "../db/mysql_result_json.hpp"
#include "../http/response.hpp"

namespace {

class PoolComboBackend final : public ComboBackend {
public:
    PoolComboBackend(MysqlPool* mysql, RedisPool* redis) : mysql_(mysql), redis_(redis) {}

    asio::awaitable<std::string> get_cache() override {
        auto reply = co_await redis_->get("cache:user:1");
        co_return reply.ok ? reply.str : "";
    }

    asio::awaitable<MysqlPool::Result> query() override {
        co_return co_await mysql_->execute("SELECT 'from_mysql' AS name");
    }

private:
    MysqlPool* mysql_;
    RedisPool* redis_;
};

asio::awaitable<std::optional<MysqlPool::Result>> execute_combo_with_deadline(
    asio::awaitable<MysqlPool::Result> query, ComboQueryPermit permit,
    std::chrono::milliseconds deadline) {
    co_return co_await await_combo_query_with_deadline(
        std::move(query), std::move(permit), deadline,
        [](std::exception_ptr ep) {
            MysqlPool::Result result;
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    result = {false, e.what(), {}};
                } catch (...) {
                    result = {false, "unknown MySQL exception", {}};
                }
            }
            return result;
        });
}

asio::awaitable<void> api_mysql(HttpContext& ctx, AppServices services) {
    auto res = co_await services.mysql->execute("SELECT * FROM sys_dict_type LIMIT 20");
    ctx.response_headers.emplace_back("Content-Type", "application/json");
    if (!res.ok) {
        ctx.status_code = 500;
        ctx.response_body = resp_err(DB_ERROR, res.error);
    } else {
        ctx.status_code = 200;
        ctx.response_body = resp_ok(res.json);
    }
}

asio::awaitable<void> api_redis(HttpContext& ctx, AppServices services) {
    try {
        auto g = co_await services.redis->get("demo_key");
        ctx.response_headers.emplace_back("Content-Type", "application/json");
        if (!g.ok) {
            ctx.status_code = 500;
            ctx.response_body = resp_err(DB_ERROR, g.error);
        } else {
            ctx.status_code = 200;
            ctx.response_body = resp_ok_str(g.str);
        }
    } catch (const std::exception& e) {
        ctx.status_code = 500;
        ctx.response_body = resp_err(DB_ERROR, e.what());
    }
}

void set_json(HttpContext& ctx) {
    if (get_header_value(ctx.response_headers, "Content-Type").empty()) {
        ctx.response_headers.emplace_back("Content-Type", "application/json");
    }
    if (get_header_value(ctx.response_headers, "Cache-Control").empty()) {
        ctx.response_headers.emplace_back("Cache-Control", "no-store");
    }
}

asio::awaitable<RedisPool::Reply> run_redis_command(
    AppServices services, std::vector<std::string> args) {
    if (services.redis_command) {
        return services.redis_command(std::move(args));
    }
    return services.redis->cmd_argv(std::move(args));
}

bool redis_command_available(const AppServices& services) {
    return static_cast<bool>(services.redis_command) || services.redis != nullptr;
}

class AdminLoginThrottle {
public:
    bool locked(const std::string& client_ip) {
        std::lock_guard<std::mutex> lock(mu_);
        sweep_expired_locked();
        auto it = entries_.find(client_ip);
        return it != entries_.end() && it->second.locked_until > Clock::now();
    }

    void record_failure(const std::string& client_ip) {
        std::lock_guard<std::mutex> lock(mu_);
        sweep_expired_locked();
        auto& entry = entries_[client_ip];
        ++entry.failures;
        if (entry.failures >= kMaxFailures) {
            entry.locked_until = Clock::now() + kLockDuration;
        }
    }

    void record_success(const std::string& client_ip) {
        std::lock_guard<std::mutex> lock(mu_);
        entries_.erase(client_ip);
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr int kMaxFailures = 5;
    static constexpr auto kLockDuration = std::chrono::minutes(15);

    struct Entry {
        int failures = 0;
        Clock::time_point locked_until{};
    };

    void sweep_expired_locked() {
        if (entries_.size() <= kMaxEntries) {
            return;
        }
        const auto now = Clock::now();
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.locked_until <= now) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
        while (entries_.size() > kMaxEntries) {
            entries_.erase(entries_.begin());
        }
    }

    static constexpr size_t kMaxEntries = 4096;
    std::mutex mu_;
    std::unordered_map<std::string, Entry> entries_;
};

AdminLoginThrottle& admin_login_throttle() {
    static AdminLoginThrottle throttle;
    return throttle;
}

class AdminAuthWorkLimiter {
public:
    bool try_acquire() {
        size_t current = in_flight_.load(std::memory_order_relaxed);
        while (current < kMaxInFlight) {
            if (in_flight_.compare_exchange_weak(
                    current, current + 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void release() {
        in_flight_.fetch_sub(1, std::memory_order_acq_rel);
    }

private:
    static constexpr size_t kMaxInFlight = 16;
    std::atomic<size_t> in_flight_{0};
};

AdminAuthWorkLimiter& admin_auth_work_limiter() {
    static AdminAuthWorkLimiter limiter;
    return limiter;
}

class AdminAuthWorkPermit {
public:
    explicit AdminAuthWorkPermit(AdminAuthWorkLimiter& limiter)
        : limiter_(&limiter) {}

    AdminAuthWorkPermit(const AdminAuthWorkPermit&) = delete;
    AdminAuthWorkPermit& operator=(const AdminAuthWorkPermit&) = delete;

    ~AdminAuthWorkPermit() {
        if (limiter_) limiter_->release();
    }

private:
    AdminAuthWorkLimiter* limiter_;
};

std::optional<AdminConfig> load_effective_admin_config(
    const AppServices& services,
    std::string* error = nullptr) {
    auto current_admin = config_admin::load_local_admin_config(
        services.config_base, error);
    if (current_admin) {
        return current_admin;
    }
    if (services.config_base.empty()) {
        return services.config_sync.admin;
    }
    return std::nullopt;
}

bool authorize_admin(HttpContext& ctx, const AppServices& services) {
    set_json(ctx);
    std::string admin_config_error;
    auto current_admin = load_effective_admin_config(services, &admin_config_error);
    if (!current_admin) {
        if (!admin_config_error.empty()) {
            LOG_WARN("admin auth config reload failed: ", admin_config_error);
        }
        ctx.status_code = 503;
        ctx.response_body = resp_err(SERVER_ERROR, "admin API is not configured");
        return false;
    }
    const auto& admin = *current_admin;
    if (admin.insecure_no_auth) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_acq_rel)) {
            LOG_ERROR("SECURITY WARNING: admin API authentication is disabled by "
                "admin.insecure_no_auth=true");
        }
        ctx.admin_principal.reset();
        return true;
    }
    if (!config_admin::admin_configured(admin, services.config_base)) {
        ctx.status_code = 503;
        ctx.response_body = resp_err(SERVER_ERROR, "admin API is not configured");
        return false;
    }
    auto principal = config_admin::verify_admin_token(
        admin, ctx.get_header("Authorization"), services.config_base);
    if (!principal) {
        ctx.status_code = 401;
        ctx.response_body = resp_err(401, "admin token required");
        return false;
    }
    if (!has_role(*principal, "admin")) {
        ctx.status_code = 403;
        ctx.response_body = resp_err(403, "admin role required");
        return false;
    }
    ctx.admin_principal = std::move(*principal);
    return true;
}

void method_not_allowed(HttpContext& ctx, std::string allow) {
    set_json(ctx);
    ctx.status_code = 405;
    ctx.response_headers.emplace_back("Allow", std::move(allow));
    ctx.response_body = resp_err(405, "method not allowed");
}

void admin_login_failed(HttpContext& ctx) {
    ctx.status_code = 401;
    ctx.response_body = resp_err(401, "invalid credentials");
}

asio::awaitable<void> handle_api_admin_login_impl(
    HttpContext& ctx, AppServices services) {
    set_json(ctx);
    if (ctx.method != "POST") {
        method_not_allowed(ctx, "POST");
        co_return;
    }

    std::string admin_config_error;
    auto current_admin = load_effective_admin_config(services, &admin_config_error);
    if (!current_admin || current_admin->accounts.empty() ||
        current_admin->jwt_private_key.empty() ||
        current_admin->jwt_public_key.empty()) {
        if (!admin_config_error.empty()) {
            LOG_WARN("admin login config reload failed: ", admin_config_error);
        }
        ctx.status_code = 503;
        ctx.response_body = resp_err(SERVER_ERROR, "admin API is not configured");
        co_return;
    }

    auto parsed = config_admin::parse_login_request(ctx.body);
    if (!parsed.ok) {
        ctx.status_code = 400;
        ctx.response_body = resp_err(PARAM_ERROR, parsed.error);
        co_return;
    }

    const auto client_ip = ctx.client_ip.empty() ? std::string("unknown") : ctx.client_ip;
    auto& throttle = admin_login_throttle();
    if (throttle.locked(client_ip)) {
        LOG_WARN("admin login rejected: locked username=", parsed.request.username,
            ", ip=", client_ip);
        admin_login_failed(ctx);
        co_return;
    }

    auto& work_limiter = admin_auth_work_limiter();
    if (!work_limiter.try_acquire()) {
        ctx.status_code = 429;
        ctx.response_headers.emplace_back("Retry-After", "1");
        ctx.response_body = resp_err(429, "too many login attempts");
        co_return;
    }
    AdminAuthWorkPermit permit(work_limiter);
    if (services.admin_auth_workers) {
        co_await asio::post(*services.admin_auth_workers, asio::use_awaitable);
    }

    if (!config_admin::admin_configured(*current_admin, services.config_base)) {
        ctx.status_code = 503;
        ctx.response_body = resp_err(SERVER_ERROR, "admin API is not configured");
        co_return;
    }
    if (!config_admin::verify_admin_password(
            *current_admin, parsed.request.username, parsed.request.password)) {
        throttle.record_failure(client_ip);
        LOG_WARN("admin login failed: username=", parsed.request.username,
            ", ip=", client_ip);
        admin_login_failed(ctx);
        co_return;
    }

    auto issued = config_admin::issue_admin_token(
        *current_admin, parsed.request.username, services.config_base);
    if (!issued) {
        ctx.status_code = 503;
        ctx.response_body = resp_err(SERVER_ERROR, "admin token signer is unavailable");
        co_return;
    }

    throttle.record_success(client_ip);
    LOG_INFO("admin login succeeded: username=", parsed.request.username,
        ", ip=", client_ip);
    ctx.status_code = 200;
    ctx.response_body = resp_ok("{\"token\":\"" + json_escape(issued->token) +
        "\",\"expires_in\":" + std::to_string(issued->expires_in) + "}");
    co_return;
}

class AdminRequestOperation : public std::enable_shared_from_this<AdminRequestOperation> {
public:
    enum class Kind { Config, Machines };

    AdminRequestOperation(HttpContext& ctx, AppServices services,
                          asio::any_io_executor executor,
                          std::function<void()> completion, Kind kind)
        : ctx_(ctx),
          services_(std::move(services)),
          executor_(std::move(executor)),
          completion_(std::move(completion)),
          kind_(kind) {}

    void start() noexcept {
        if (services_.admin_auth_workers) {
            auto self = shared_from_this();
            try {
                asio::post(*services_.admin_auth_workers,
                    [self]() { self->start_blocking(); });
            } catch (...) {
                fail(std::current_exception());
            }
            return;
        }
        start_blocking();
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
            if (kind_ == Kind::Machines) {
                start_machines();
            } else if (ctx_.method == "GET") {
                read_config_version(0);
            } else if (ctx_.method == "POST") {
                save_config();
            } else {
                method_not_allowed(ctx_, "GET, POST");
                complete();
            }
        } catch (...) {
            fail(std::current_exception());
        }
    }

    using ReplyCallback = std::function<void(RedisPool::Reply)>;

    void read_config_version(int attempt) {
        auto self = shared_from_this();
        run_command({"GET", std::string(config_admin::kVersionKey)},
            [self, attempt](RedisPool::Reply reply) {
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
            [self, version, attempt](RedisPool::Reply reply) {
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
            [self, version, attempt](RedisPool::Reply reply) {
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
                RedisPool::Reply reply) mutable {
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
                RedisPool::Reply reply) {
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
        run_command(std::move(args), [self](RedisPool::Reply reply) {
            self->handle_save_reply(std::move(reply));
        });
    }

    void handle_save_reply(RedisPool::Reply reply) {
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
            [self](RedisPool::Reply reply) {
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
            [self](RedisPool::Reply reply) {
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
        try {
            auto command = run_redis_command(services_, std::move(args));
            asio::co_spawn(executor_, std::move(command),
                [self, callback = std::move(callback)](
                    std::exception_ptr ep, RedisPool::Reply reply) mutable {
                    if (ep) reply = redis_exception_reply(ep);
                    auto invoke =
                        [self, callback = std::move(callback),
                         reply = std::move(reply)]() mutable {
                            try {
                                callback(std::move(reply));
                            } catch (...) {
                                self->fail(std::current_exception());
                            }
                        };
                    if (self->services_.admin_auth_workers) {
                        try {
                            asio::post(*self->services_.admin_auth_workers,
                                std::move(invoke));
                        } catch (...) {
                            self->fail(std::current_exception());
                        }
                    } else {
                        invoke();
                    }
                });
        } catch (...) {
            fail(std::current_exception());
        }
    }

    static RedisPool::Reply redis_exception_reply(std::exception_ptr ep) {
        RedisPool::Reply reply;
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

    void invalid_version() {
        ctx_.status_code = 500;
        ctx_.response_body = resp_err(SERVER_ERROR,
            "version key content is not numeric, manual repair required");
        complete();
    }

    void fail(std::exception_ptr ep) noexcept {
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
        if (completed_.exchange(true, std::memory_order_acq_rel)) return;
        auto completion = std::make_shared<std::function<void()>>(
            std::move(completion_));
        try {
            asio::post(executor_, [completion]() mutable { (*completion)(); });
        } catch (...) {
            try {
                (*completion)();
            } catch (...) {
            }
        }
    }

    HttpContext& ctx_;
    AppServices services_;
    asio::any_io_executor executor_;
    std::function<void()> completion_;
    Kind kind_;
    std::atomic<bool> completed_{false};
};

asio::awaitable<void> start_admin_request(
    HttpContext& ctx, AppServices services, AdminRequestOperation::Kind kind) {
    asio::use_awaitable_t<> token;
    return asio::async_initiate<asio::use_awaitable_t<>, void()>(
        [&ctx, services = std::move(services), kind](auto handler) mutable {
            asio::any_io_executor executor = asio::get_associated_executor(handler);
            using HandlerType = std::decay_t<decltype(handler)>;
            auto handler_ptr = std::make_shared<HandlerType>(std::move(handler));
            auto completion = [handler_ptr]() mutable { (*handler_ptr)(); };
            auto operation = std::make_shared<AdminRequestOperation>(
                ctx, std::move(services), std::move(executor),
                std::move(completion), kind);
            operation->start();
        },
        token);
}

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
          completion_(std::move(completion)),
          kind_(kind) {}

    void start() noexcept {
        if (services_.admin_auth_workers) {
            auto self = shared_from_this();
            try {
                asio::post(*services_.admin_auth_workers,
                    [self]() { self->start_blocking(); });
            } catch (...) {
                fail(std::current_exception());
            }
            return;
        }
        start_blocking();
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

    void start_list() {
        if (ctx_.method != "GET") {
            method_not_allowed(ctx_, "GET");
            complete();
            return;
        }
        int64_t before = 0;
        size_t limit = services_.config_sync.history.history_page_size;
        if (auto value = query_value(ctx_.path, "before")) {
            auto parsed = config_history::parse_int64(*value);
            if (!parsed || *parsed <= 0) {
                bad_request("invalid before cursor");
                return;
            }
            before = *parsed;
        }
        if (auto value = query_value(ctx_.path, "limit")) {
            auto parsed = config_history::parse_int64(*value);
            if (!parsed || *parsed <= 0 ||
                static_cast<uint64_t>(*parsed) >
                    services_.config_sync.history.history_page_size_max) {
                bad_request("invalid history limit");
                return;
            }
            limit = static_cast<size_t>(*parsed);
        }
        auto self = shared_from_this();
        run_command({"EVAL", config_history::list_script(), "3",
                     std::string(config_admin::kVersionKey),
                     std::string(config_history::kIndexKey),
                     std::string(config_history::kMetaKey),
                     std::to_string(before), std::to_string(limit),
                     std::string(config_history::kSnapshotPrefix)},
            [self](RedisPool::Reply reply) {
                if (!reply.ok) {
                    self->redis_failed(reply.error);
                    return;
                }
                if (reply.type != "array" || reply.elements.empty()) {
                    self->inconsistent("history list returned malformed data");
                    return;
                }
                auto current = config_history::parse_int64(reply.elements.front());
                if (!current || *current < 0) {
                    self->inconsistent("history version is not numeric");
                    return;
                }
                std::vector<std::string> rows(
                    reply.elements.begin() + 1, reply.elements.end());
                if (rows.size() % 2 != 0) {
                    self->inconsistent("history list returned malformed rows");
                    return;
                }
                self->ctx_.status_code = 200;
                self->ctx_.response_body = resp_ok(
                    config_history::history_list_json(rows, *current));
                self->complete();
            });
    }

    void start_path() {
        if (ctx_.method != "GET") {
            method_not_allowed(ctx_, "GET");
            complete();
            return;
        }
        static constexpr std::string_view prefix =
            "/api/admin/config/history/";
        const std::string path = path_only(ctx_.path);
        if (path.rfind(prefix, 0) != 0) {
            not_found();
            return;
        }
        std::string suffix = path.substr(prefix.size());
        bool diff = false;
        static constexpr std::string_view diff_suffix = "/diff";
        if (suffix.size() > diff_suffix.size() &&
            suffix.compare(suffix.size() - diff_suffix.size(),
                diff_suffix.size(), diff_suffix) == 0) {
            diff = true;
            suffix.resize(suffix.size() - diff_suffix.size());
        }
        auto version = config_history::parse_int64(suffix);
        if (!version || *version <= 0) {
            not_found();
            return;
        }
        if (diff) start_diff(*version);
        else start_detail(*version);
    }

    void start_detail(int64_t version) {
        auto self = shared_from_this();
        read_detail(version,
            [self, version](
                std::optional<config_history::SnapshotRecord> record) {
                if (!record) {
                    self->not_found();
                    return;
                }
                if (!valid_record_hash(*record)) {
                    self->inconsistent("history snapshot hash mismatch");
                    return;
                }
                self->ctx_.status_code = 200;
                self->ctx_.response_body = resp_ok(
                    config_history::detail_json(version, *record));
                self->complete();
            });
    }

    void start_diff(int64_t from_version) {
        auto requested_to = query_value(ctx_.path, "to");
        std::optional<int64_t> to_version;
        if (requested_to) {
            to_version = config_history::parse_int64(*requested_to);
            if (!to_version || *to_version <= 0) {
                bad_request("invalid diff target version");
                return;
            }
        }
        auto self = shared_from_this();
        read_detail(from_version,
            [self, from_version, to_version](
                std::optional<config_history::SnapshotRecord> from) mutable {
                if (!from) {
                    self->not_found();
                    return;
                }
                if (!valid_record_hash(*from)) {
                    self->inconsistent("history source hash mismatch");
                    return;
                }
                const int64_t target = to_version.value_or(from->current_version);
                if (target == from_version) {
                    auto result = config_history::diff_json(
                        from_version, *from, target, *from,
                        self->services_.config_sync.history.max_diff_response_bytes);
                    self->ctx_.status_code = 200;
                    self->ctx_.response_body = resp_ok(result.value_or(
                        "{\"from\":" + std::to_string(from_version) +
                        ",\"to\":" + std::to_string(target) +
                        ",\"changes\":[]}"));
                    self->complete();
                    return;
                }
                auto saved_from = std::make_shared<config_history::SnapshotRecord>(
                    std::move(*from));
                self->read_detail(target,
                    [self, from_version, target, saved_from](
                        std::optional<config_history::SnapshotRecord> to) {
                        if (!to) {
                            self->not_found();
                            return;
                        }
                        if (!valid_record_hash(*to)) {
                            self->inconsistent("history target hash mismatch");
                            return;
                        }
                        auto result = config_history::diff_json(
                            from_version, *saved_from, target, *to,
                            self->services_.config_sync.history.max_diff_response_bytes);
                        if (!result) {
                            self->ctx_.status_code = 413;
                            self->ctx_.response_body = resp_err(
                                413, "diff response exceeds configured limit");
                            self->complete();
                            return;
                        }
                        self->ctx_.status_code = 200;
                        self->ctx_.response_body = resp_ok(*result);
                        self->complete();
                    });
            });
    }

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

    void start_orphan_resolution() {
        if (ctx_.method != "POST") {
            method_not_allowed(ctx_, "POST");
            complete();
            return;
        }
        auto parsed = config_admin::parse_orphan_resolution_request(ctx_.body);
        if (!parsed.ok) {
            bad_request(parsed.error);
            return;
        }
        if (parsed.request.reason.size() >
            services_.config_sync.history.max_reason_bytes) {
            bad_request("reason exceeds configured limit");
            return;
        }
        orphan_request_ = std::move(parsed.request);
        auto self = shared_from_this();
        run_command({"EVAL", config_history::orphan_inspect_script(), "5",
                     std::string(config_admin::kVersionKey),
                     std::string(config_history::kIndexKey),
                     std::string(config_history::kMetaKey),
                     config_history::snapshot_key(orphan_request_.target_version),
                     std::string(config_admin::kMachinesKey),
                     std::to_string(orphan_request_.target_version)},
            [self](RedisPool::Reply reply) {
                self->prepare_orphan_resolution(std::move(reply));
            });
    }

    void prepare_orphan_resolution(RedisPool::Reply reply) {
        if (!reply.ok || reply.type != "array" || reply.elements.size() < 6 ||
            (reply.elements.size() - 6) % 2 != 0 ||
            reply.elements.front() == "__error__") {
            inconsistent("orphan state is missing, malformed or changed");
            return;
        }
        auto current = config_history::parse_int64(reply.elements[0]);
        auto index_high = config_history::parse_int64(reply.elements[1]);
        auto machine_high = config_history::parse_int64(reply.elements[2]);
        if (!current || !index_high || !machine_high ||
            *current != orphan_request_.current_version) {
            inconsistent("orphan state changed; inspect again");
            return;
        }
        const bool indexed = reply.elements[3] == "1";
        const std::string meta = reply.elements[4];
        const bool snapshot_exists = reply.elements[5] == "1";
        if ((reply.elements[3] != "0" && !indexed) ||
            (reply.elements[5] != "0" && !snapshot_exists)) {
            inconsistent("orphan inspection returned malformed flags");
            return;
        }
        std::map<std::string, std::string> files;
        for (size_t i = 6; i < reply.elements.size(); i += 2) {
            files[reply.elements[i]] = reply.elements[i + 1];
        }
        const int64_t observed = services_.config_history_service ?
            services_.config_history_service->stats().max_observed_version : 0;
        if (orphan_request_.action == "restore-version") {
            if (!indexed || meta == "__missing__" || !snapshot_exists ||
                files.empty() || *index_high != orphan_request_.target_version ||
                *machine_high > orphan_request_.target_version ||
                observed > orphan_request_.target_version) {
                inconsistent("target is not a complete trusted high-water snapshot");
                return;
            }
            config_history::SnapshotRecord record;
            record.meta_json = meta;
            record.files = files;
            if (!valid_record_hash(record)) {
                inconsistent("orphan snapshot hash does not match metadata");
                return;
            }
            config_history::SnapshotInfo info;
            if (auto error = config_history::validate_snapshot(
                    files, services_.config_sync.history, info)) {
                inconsistent(*error);
                return;
            }
            submit_restore_version(std::move(files), meta);
            return;
        }
        if (orphan_request_.target_version != *current + 1 ||
            *machine_high > *current || observed > *current ||
            (!indexed && meta == "__missing__" && !snapshot_exists)) {
            inconsistent("delete refused: published-version evidence exists or target is not current+1");
            return;
        }
        submit_delete_orphan(
            std::move(files), meta, indexed, snapshot_exists);
    }

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

    void read_detail(int64_t version, DetailCallback callback) {
        auto self = shared_from_this();
        run_command({"EVAL", config_history::detail_script(), "4",
                     std::string(config_admin::kVersionKey),
                     std::string(config_history::kMetaKey),
                     std::string(config_history::kIndexKey),
                     config_history::snapshot_key(version),
                     std::to_string(version)},
            [self, callback = std::move(callback)](RedisPool::Reply reply) mutable {
                if (!reply.ok) {
                    self->redis_failed(reply.error);
                    return;
                }
                if (reply.type != "array") {
                    self->inconsistent("history detail returned malformed data");
                    return;
                }
                if (reply.elements.empty()) {
                    callback(std::nullopt);
                    return;
                }
                auto record = config_history::parse_detail_elements(reply.elements);
                if (!record) {
                    self->inconsistent("history detail returned malformed fields");
                    return;
                }
                for (const auto& [_, content] : record->files) {
                    if (config_admin::restart_required(content)) {
                        self->restart_required_ = true;
                        break;
                    }
                }
                callback(std::move(record));
            });
    }

    void run_command(std::vector<std::string> args, ReplyCallback callback) {
        auto self = shared_from_this();
        try {
            auto command = run_redis_command(services_, std::move(args));
            asio::co_spawn(executor_, std::move(command),
                [self, callback = std::move(callback)](
                    std::exception_ptr ep, RedisPool::Reply reply) mutable {
                    if (ep) reply = redis_exception_reply(ep);
                    auto invoke =
                        [self, callback = std::move(callback),
                         reply = std::move(reply)]() mutable {
                            try {
                                callback(std::move(reply));
                            } catch (...) {
                                self->fail(std::current_exception());
                            }
                        };
                    if (self->services_.admin_auth_workers) {
                        try {
                            asio::post(*self->services_.admin_auth_workers,
                                std::move(invoke));
                        } catch (...) {
                            self->fail(std::current_exception());
                        }
                    } else {
                        invoke();
                    }
                });
        } catch (...) {
            fail(std::current_exception());
        }
    }

    static RedisPool::Reply redis_exception_reply(std::exception_ptr ep) {
        RedisPool::Reply reply;
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

    void bad_request(const std::string& message) {
        ctx_.status_code = 400;
        ctx_.response_body = resp_err(PARAM_ERROR, message);
        complete();
    }

    void not_found() {
        ctx_.status_code = 404;
        ctx_.response_body = resp_err(404, "history version not found");
        complete();
    }

    void redis_failed(const std::string& message) {
        ctx_.status_code = 500;
        ctx_.response_body = resp_err(DB_ERROR, message);
        complete();
    }

    void inconsistent(const std::string& message) {
        ctx_.status_code = 409;
        ctx_.response_body = json_resp(409, message);
        complete();
    }

    void fail(std::exception_ptr ep) noexcept {
        std::string message = "unknown admin history request exception";
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
        if (completed_.exchange(true, std::memory_order_acq_rel)) return;
        auto completion = std::make_shared<std::function<void()>>(
            std::move(completion_));
        try {
            asio::post(executor_, [completion]() mutable { (*completion)(); });
        } catch (...) {
            try {
                (*completion)();
            } catch (...) {
            }
        }
    }

    HttpContext& ctx_;
    AppServices services_;
    asio::any_io_executor executor_;
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

}  // namespace

std::shared_ptr<ComboBackend> make_pool_combo_backend(MysqlPool* mysql, RedisPool* redis) {
    return std::make_shared<PoolComboBackend>(mysql, redis);
}

asio::awaitable<void> handle_api_combo(HttpContext& ctx, AppServices services) {
    auto redis_ret = co_await services.combo_backend->get_cache();

    std::string data;
    if (!redis_ret.empty()) {
        data = redis_ret;
    } else {
        auto permit = services.combo_query_limiter->try_acquire();
        if (!permit) {
            ctx.response_headers.emplace_back("Content-Type", "application/json");
            ctx.status_code = 503;
            ctx.response_body = resp_err(DB_ERROR, "too many in-flight MySQL queries");
            co_return;
        }
        auto mysql_ret = co_await execute_combo_with_deadline(
            services.combo_backend->query(), std::move(*permit),
            std::chrono::milliseconds(services.combo_deadline_ms));
        if (!mysql_ret) {
            ctx.response_headers.emplace_back("Content-Type", "application/json");
            ctx.status_code = 504;
            ctx.response_body = resp_err(DB_ERROR, "MySQL query deadline exceeded");
            co_return;
        }
        if (!mysql_ret->ok) {
            ctx.response_headers.emplace_back("Content-Type", "application/json");
            ctx.status_code = 500;
            ctx.response_body = resp_err(DB_ERROR, mysql_ret->error);
            co_return;
        }
        auto parsed = extract_first_string_value(mysql_ret->json);
        if (!parsed) {
            ctx.response_headers.emplace_back("Content-Type", "application/json");
            ctx.status_code = 500;
            ctx.response_body = resp_err(DB_ERROR, "invalid MySQL response");
            co_return;
        }
        data = std::move(*parsed);
        auto ex = co_await asio::this_coro::executor;
        if (!services.redis) {
            LOG_WARN("combo cache SET skipped: Redis service unavailable");
        } else {
            auto redis = services.redis;
            asio::co_spawn(ex,
                redis->cmd_argv({"SET", "cache:user:1", data, "EX", "300"}),
                [redis](std::exception_ptr ep, RedisPool::Reply reply) {
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    LOG_WARN("combo cache SET failed: ", e.what());
                } catch (...) {
                    LOG_WARN("combo cache SET failed with an unknown exception");
                }
                return;
            }
            if (!reply.ok) {
                LOG_WARN("combo cache SET failed: ", reply.error);
            }
        });
        }
    }

    ctx.response_headers.emplace_back("Content-Type", "application/json");
    ctx.status_code = 200;
    ctx.response_body = resp_ok_str(data);
}

asio::awaitable<void> api_health(HttpContext& ctx) {
    ctx.response_headers.emplace_back("Content-Type", "application/json");
    ctx.status_code = 200;
    ctx.response_body = resp_ok_str("running");
    co_return;
}

asio::awaitable<void> api_build(HttpContext& ctx) {
    ctx.response_headers.emplace_back("Content-Type", "application/json");
    ctx.status_code = 200;
    ctx.response_body = "{\"code\":0,\"build\":\"asio_owen\"}";
    co_return;
}

asio::awaitable<void> handle_api_admin_login(HttpContext& ctx, AppServices services) {
    return handle_api_admin_login_impl(ctx, std::move(services));
}

asio::awaitable<void> handle_api_admin_config(HttpContext& ctx, AppServices services) {
    return start_admin_request(
        ctx, std::move(services), AdminRequestOperation::Kind::Config);
}

asio::awaitable<void> handle_api_admin_machines(HttpContext& ctx, AppServices services) {
    return start_admin_request(
        ctx, std::move(services), AdminRequestOperation::Kind::Machines);
}

asio::awaitable<void> handle_api_admin_history(HttpContext& ctx, AppServices services) {
    return start_admin_history_request(
        ctx, std::move(services), AdminHistoryOperation::Kind::List);
}

asio::awaitable<void> handle_api_admin_history_path(
    HttpContext& ctx, AppServices services) {
    return start_admin_history_request(
        ctx, std::move(services), AdminHistoryOperation::Kind::Path);
}

asio::awaitable<void> handle_api_admin_rollback(HttpContext& ctx, AppServices services) {
    return start_admin_history_request(
        ctx, std::move(services), AdminHistoryOperation::Kind::Rollback);
}

asio::awaitable<void> handle_api_admin_snapshot_repair(
    HttpContext& ctx, AppServices services) {
    return start_admin_history_request(
        ctx, std::move(services), AdminHistoryOperation::Kind::SnapshotRepair);
}

asio::awaitable<void> handle_api_admin_mirror_rebuild(
    HttpContext& ctx, AppServices services) {
    return start_admin_history_request(
        ctx, std::move(services), AdminHistoryOperation::Kind::MirrorRebuild);
}

asio::awaitable<void> handle_api_admin_history_migration(
    HttpContext& ctx, AppServices services) {
    return start_admin_history_request(
        ctx, std::move(services), AdminHistoryOperation::Kind::Migration);
}

asio::awaitable<void> handle_api_admin_orphan_resolution(
    HttpContext& ctx, AppServices services) {
    return start_admin_history_request(
        ctx, std::move(services), AdminHistoryOperation::Kind::OrphanResolution);
}

asio::awaitable<void> handle_admin_page(HttpContext& ctx) {
    if (ctx.method != "GET" && ctx.method != "HEAD") {
        ctx.status_code = 405;
        ctx.response_headers.emplace_back("Allow", "GET, HEAD");
        ctx.response_body = "method not allowed";
        co_return;
    }
    ctx.status_code = 200;
    ctx.response_headers.emplace_back("Content-Type", "text/html; charset=utf-8");
    ctx.response_headers.emplace_back("Cache-Control", "no-store");
    ctx.response_headers.emplace_back("Content-Security-Policy",
        "default-src 'self'; script-src 'self' 'unsafe-inline'; "
        "style-src 'self' 'unsafe-inline'; connect-src 'self'; "
        "img-src 'self' data:; base-uri 'none'; frame-ancestors 'none'; "
        "form-action 'self'");
    ctx.response_body = config_admin::admin_login_html();
    co_return;
}

asio::awaitable<void> handle_admin_settings_page(HttpContext& ctx) {
    if (ctx.method != "GET" && ctx.method != "HEAD") {
        ctx.status_code = 405;
        ctx.response_headers.emplace_back("Allow", "GET, HEAD");
        ctx.response_body = "method not allowed";
        co_return;
    }
    ctx.status_code = 200;
    ctx.response_headers.emplace_back("Content-Type", "text/html; charset=utf-8");
    ctx.response_headers.emplace_back("Cache-Control", "no-store");
    ctx.response_headers.emplace_back("Content-Security-Policy",
        "default-src 'self'; script-src 'self' 'unsafe-inline'; "
        "style-src 'self' 'unsafe-inline'; connect-src 'self'; "
        "img-src 'self' data:; base-uri 'none'; frame-ancestors 'none'; "
        "form-action 'self'");
    ctx.response_body = config_admin::admin_settings_html();
    co_return;
}

void register_routes(HttpServer& server, AppServices services) {
    server.route("/api/health", api_health);
    server.route("/api/build", api_build);
    server.route("/admin", handle_admin_page);
    server.route("/admin/login", handle_admin_page);
    server.route("/admin/settings", handle_admin_settings_page);
    server.route("/api/redis", [services](HttpContext& ctx) {
        return api_redis(ctx, services);
    });
    server.route("/api/mysql", [services](HttpContext& ctx) {
        return api_mysql(ctx, services);
    });
    server.route("/api/combo", [services](HttpContext& ctx) {
        return handle_api_combo(ctx, services);
    });
    server.route("/api/admin/login", [services](HttpContext& ctx) {
        return handle_api_admin_login(ctx, services);
    });
    server.route("/api/admin/config", [services](HttpContext& ctx) {
        return handle_api_admin_config(ctx, services);
    });
    server.route("/api/admin/config/machines", [services](HttpContext& ctx) {
        return handle_api_admin_machines(ctx, services);
    });
    server.route("/api/admin/config/history", [services](HttpContext& ctx) {
        return handle_api_admin_history(ctx, services);
    });
    server.route_prefix("/api/admin/config/history/", [services](HttpContext& ctx) {
        return handle_api_admin_history_path(ctx, services);
    });
    server.route("/api/admin/config/rollback", [services](HttpContext& ctx) {
        return handle_api_admin_rollback(ctx, services);
    });
    server.route("/api/admin/config/history/repair-snapshot", [services](HttpContext& ctx) {
        return handle_api_admin_snapshot_repair(ctx, services);
    });
    server.route("/api/admin/config/history/rebuild-mirror", [services](HttpContext& ctx) {
        return handle_api_admin_mirror_rebuild(ctx, services);
    });
    server.route("/api/admin/config/history/migrate", [services](HttpContext& ctx) {
        return handle_api_admin_history_migration(ctx, services);
    });
    server.route("/api/admin/config/history/resolve-orphan", [services](HttpContext& ctx) {
        return handle_api_admin_orphan_resolution(ctx, services);
    });
}
