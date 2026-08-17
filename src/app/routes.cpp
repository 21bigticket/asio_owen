#include "routes.hpp"

#include <asio/co_spawn.hpp>
#include <asio/this_coro.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <type_traits>
#include <unordered_map>

#include "admin/config_admin.hpp"
#include "combo_deadline.hpp"
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
    if (!get_header_value(ctx.response_headers, "Content-Type").empty()) return;
    ctx.response_headers.emplace_back("Content-Type", "application/json");
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
    bool locked(const std::string& client_ip, const std::string& username) {
        std::lock_guard<std::mutex> lock(mu_);
        sweep_expired_locked();
        auto it = entries_.find(key(client_ip, username));
        return it != entries_.end() && it->second.locked_until > Clock::now();
    }

    void record_failure(const std::string& client_ip, const std::string& username) {
        std::lock_guard<std::mutex> lock(mu_);
        sweep_expired_locked();
        auto& entry = entries_[key(client_ip, username)];
        ++entry.failures;
        if (entry.failures >= kMaxFailures) {
            entry.locked_until = Clock::now() + kLockDuration;
        }
    }

    void record_success(const std::string& client_ip, const std::string& username) {
        std::lock_guard<std::mutex> lock(mu_);
        entries_.erase(key(client_ip, username));
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr int kMaxFailures = 5;
    static constexpr auto kLockDuration = std::chrono::minutes(15);

    struct Entry {
        int failures = 0;
        Clock::time_point locked_until{};
    };

    static std::string key(const std::string& client_ip, const std::string& username) {
        return client_ip + '\n' + username;
    }

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

void handle_api_admin_login_sync(HttpContext& ctx, const AppServices& services) {
    set_json(ctx);
    if (ctx.method != "POST") {
        method_not_allowed(ctx, "POST");
        return;
    }

    std::string admin_config_error;
    auto current_admin = load_effective_admin_config(services, &admin_config_error);
    if (!current_admin ||
        !config_admin::admin_configured(*current_admin, services.config_base)) {
        if (!admin_config_error.empty()) {
            LOG_WARN("admin login config reload failed: ", admin_config_error);
        }
        ctx.status_code = 503;
        ctx.response_body = resp_err(SERVER_ERROR, "admin API is not configured");
        return;
    }

    auto parsed = config_admin::parse_login_request(ctx.body);
    if (!parsed.ok) {
        ctx.status_code = 400;
        ctx.response_body = resp_err(PARAM_ERROR, parsed.error);
        return;
    }

    const auto client_ip = ctx.client_ip.empty() ? std::string("unknown") : ctx.client_ip;
    auto& throttle = admin_login_throttle();
    if (throttle.locked(client_ip, parsed.request.username)) {
        LOG_WARN("admin login rejected: locked username=", parsed.request.username,
            ", ip=", client_ip);
        admin_login_failed(ctx);
        return;
    }

    if (!config_admin::verify_admin_password(
            *current_admin, parsed.request.username, parsed.request.password)) {
        throttle.record_failure(client_ip, parsed.request.username);
        LOG_WARN("admin login failed: username=", parsed.request.username,
            ", ip=", client_ip);
        admin_login_failed(ctx);
        return;
    }

    auto issued = config_admin::issue_admin_token(
        *current_admin, parsed.request.username, services.config_base);
    if (!issued) {
        ctx.status_code = 503;
        ctx.response_body = resp_err(SERVER_ERROR, "admin token signer is unavailable");
        return;
    }

    throttle.record_success(client_ip, parsed.request.username);
    LOG_INFO("admin login succeeded: username=", parsed.request.username,
        ", ip=", client_ip);
    ctx.status_code = 200;
    ctx.response_body = resp_ok("{\"token\":\"" + json_escape(issued->token) +
        "\",\"expires_in\":" + std::to_string(issued->expires_in) + "}");
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

private:
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
        run_command({"HGETALL", std::string(config_admin::kFilesKey)},
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
                self->verify_config_version(
                    version, attempt,
                    std::make_shared<std::map<std::string, std::string>>(
                        std::move(*files)));
            });
    }

    void verify_config_version(
        int64_t version, int attempt,
        std::shared_ptr<std::map<std::string, std::string>> files) {
        auto self = shared_from_this();
        run_command({"GET", std::string(config_admin::kVersionKey)},
            [self, version, attempt, files = std::move(files)](
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
                    config_admin::files_json(version, *files));
                self->complete();
            });
    }

    void save_config() {
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

        std::vector<std::string> args{
            "EVAL", config_admin::save_script(), "4",
            std::string(config_admin::kVersionKey),
            std::string(config_admin::kFilesKey),
            std::string(config_admin::kAuditKey),
            std::string(config_admin::kStagingKey),
            std::to_string(parsed.request.base_version),
            config_admin::audit_json(
                ctx_.admin_principal ? &*ctx_.admin_principal : nullptr,
                parsed.request.base_version, parsed.request.files)
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
                    try {
                        if (ep) reply = redis_exception_reply(ep);
                        callback(std::move(reply));
                    } catch (...) {
                        self->fail(std::current_exception());
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
    handle_api_admin_login_sync(ctx, services);
    co_return;
}

asio::awaitable<void> handle_api_admin_config(HttpContext& ctx, AppServices services) {
    return start_admin_request(
        ctx, std::move(services), AdminRequestOperation::Kind::Config);
}

asio::awaitable<void> handle_api_admin_machines(HttpContext& ctx, AppServices services) {
    return start_admin_request(
        ctx, std::move(services), AdminRequestOperation::Kind::Machines);
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
}
