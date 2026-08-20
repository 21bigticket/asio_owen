#include "../routes.hpp"
#include "admin_route_support.hpp"

#include <asio/co_spawn.hpp>
#include <asio/this_coro.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <ctime>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <type_traits>
#include <unordered_map>

#include <openssl/rand.h>

#include "config_admin.hpp"
#include "admin_credential_store.hpp"
#include "../config_history_service.hpp"
#include "../../http/response.hpp"

namespace admin_route_detail {

void set_json(HttpContext& ctx) {
    if (get_header_value(ctx.response_headers, "Content-Type").empty()) {
        ctx.response_headers.emplace_back("Content-Type", "application/json");
    }
    if (get_header_value(ctx.response_headers, "Cache-Control").empty()) {
        ctx.response_headers.emplace_back("Cache-Control", "no-store");
    }
}

void dispatch_admin_work(
    const AppServices& services,
    std::function<void()> work,
    std::function<void(std::exception_ptr)> failure) noexcept {
    auto invoke_failure = [failure = std::move(failure)](
                               std::exception_ptr ep) mutable noexcept {
        try {
            failure(std::move(ep));
        } catch (...) {
        }
    };
    auto invoke = [work = std::move(work), invoke_failure]() mutable noexcept {
        try {
            work();
        } catch (...) {
            invoke_failure(std::current_exception());
        }
    };
    if (!services.admin_auth_workers) {
        invoke();
        return;
    }
    try {
        asio::post(*services.admin_auth_workers, std::move(invoke));
    } catch (...) {
        invoke_failure(std::current_exception());
    }
}

asio::awaitable<RedisPool::Reply> run_redis_command(
    AppServices services, std::vector<std::string> args) {
    // Must be a coroutine (not a plain function returning the inner
    // awaitable): a coroutine lambda stored in services.redis_command keeps
    // its captures in the closure object inside this by-value parameter.
    // If this function returned the lazy awaitable and exited, the closure
    // would be destroyed before the awaitable resumes and reads its
    // captures — a use-after-free. Awaiting here keeps `services` (and the
    // closure) alive in this frame until the inner awaitable completes.
    if (services.redis_command) {
        co_return co_await services.redis_command(std::move(args));
    }
    co_return co_await services.redis->cmd_argv(std::move(args));
}

bool redis_command_available(const AppServices& services) {
    return static_cast<bool>(services.redis_command) || services.redis != nullptr;
}

namespace {

RedisPool::Reply redis_exception_reply(const std::exception_ptr& ep) {
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

}  // namespace

void dispatch_redis_command(
    const AppServices& services,
    // The executor is passed by value to match the public helper contract.
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    asio::any_io_executor executor,
    std::vector<std::string> args,
    std::function<void(RedisPool::Reply)> callback,
    std::function<void(std::exception_ptr)> failure) {
    auto* workers = services.admin_auth_workers;
    auto failure_handler =
        std::make_shared<std::function<void(std::exception_ptr)>>(std::move(failure));
    try {
        auto command = run_redis_command(services, std::move(args));
        asio::co_spawn(executor, std::move(command),
            [workers, callback = std::move(callback), failure_handler](
                const std::exception_ptr& ep, RedisPool::Reply reply) mutable {
                if (ep) reply = redis_exception_reply(ep);
                auto invoke = [callback = std::move(callback),
                               failure_handler,
                               reply = std::move(reply)]() mutable {
                    try {
                        callback(std::move(reply));
                    } catch (...) {
                        (*failure_handler)(std::current_exception());
                    }
                };
                if (!workers) {
                    invoke();
                    return;
                }
                try {
                    asio::post(*workers, std::move(invoke));
                } catch (...) {
                    (*failure_handler)(std::current_exception());
                }
            });
    } catch (...) {
        (*failure_handler)(std::current_exception());
    }
}

void complete_admin_request(
    std::atomic<bool>& completed,
    const asio::any_io_executor& executor,
    std::function<void()>& completion) noexcept {
    if (completed.exchange(true, std::memory_order_acq_rel)) return;
    auto owned_completion = std::make_shared<std::function<void()>>(
        std::move(completion));
    try {
        asio::post(executor, [owned_completion]() mutable { (*owned_completion)(); });
    } catch (...) {
        try {
            (*owned_completion)();
        } catch (...) {
        }
    }
}

class AdminAuthWorkPermit {
public:
    AdminAuthWorkPermit(AdminAuthWorkLimiter& limiter,
                        std::shared_ptr<AdminRuntimeMetrics> metrics)
        : limiter_(&limiter), metrics_(std::move(metrics)) {
        if (metrics_) metrics_->auth_in_flight.fetch_add(1, std::memory_order_relaxed);
    }

    AdminAuthWorkPermit(const AdminAuthWorkPermit&) = delete;
    AdminAuthWorkPermit& operator=(const AdminAuthWorkPermit&) = delete;

    ~AdminAuthWorkPermit() {
        if (limiter_) limiter_->release();
        if (metrics_) metrics_->auth_in_flight.fetch_sub(1, std::memory_order_relaxed);
    }

private:
    AdminAuthWorkLimiter* limiter_;
    std::shared_ptr<AdminRuntimeMetrics> metrics_;
};

std::optional<AdminConfig> load_effective_admin_config(
    const AppServices& services,
    std::string* error = nullptr) {
    if (services.admin_credentials) {
        auto snapshot = services.admin_credentials->snapshot(
            services.config_base.empty() ? &services.config_sync.admin : nullptr,
            error);
        if (!snapshot->loaded) return std::nullopt;
        return snapshot->config;
    }
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
    if (services.admin_metrics) {
        services.admin_metrics->auth_attempts.fetch_add(1, std::memory_order_relaxed);
    }
    set_json(ctx);
    std::string admin_config_error;
    auto current_admin = load_effective_admin_config(services, &admin_config_error);
    if (!current_admin) {
        if (services.admin_metrics) services.admin_metrics->auth_rejected.fetch_add(1, std::memory_order_relaxed);
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
    const bool configured = services.admin_credentials ?
        services.admin_credentials->configured(
            services.config_base.empty() ? &services.config_sync.admin : nullptr,
            &admin_config_error) :
        config_admin::admin_configured(admin, services.config_base);
    if (!configured) {
        if (services.admin_metrics) services.admin_metrics->auth_rejected.fetch_add(1, std::memory_order_relaxed);
        ctx.status_code = 503;
        ctx.response_body = resp_err(SERVER_ERROR, "admin API is not configured");
        return false;
    }
    auto principal = services.admin_credentials ?
        services.admin_credentials->verify(
            ctx.get_header("Authorization"),
            services.config_base.empty() ? &services.config_sync.admin : nullptr,
            &admin_config_error) :
        config_admin::verify_admin_token(
            admin, ctx.get_header("Authorization"), services.config_base);
    if (!principal) {
        if (services.admin_metrics) services.admin_metrics->auth_rejected.fetch_add(1, std::memory_order_relaxed);
        ctx.status_code = 401;
        ctx.response_body = resp_err(401, "admin token required");
        return false;
    }
    if (!has_role(*principal, "admin")) {
        if (services.admin_metrics) services.admin_metrics->auth_rejected.fetch_add(1, std::memory_order_relaxed);
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

std::optional<std::string> csp_nonce() {
    std::array<unsigned char, 18> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        return std::nullopt;
    }
    return config_admin::base64url_encode(bytes.data(), bytes.size());
}

std::string add_csp_nonce(std::string html, std::string_view nonce) {
    const std::string style = "<style nonce=\"" + std::string(nonce) + "\">";
    const std::string script = "<script nonce=\"" + std::string(nonce) + "\">";
    size_t pos = 0;
    while ((pos = html.find("<style>", pos)) != std::string::npos) {
        html.replace(pos, 7, style);
        pos += style.size();
    }
    pos = 0;
    while ((pos = html.find("<script>", pos)) != std::string::npos) {
        html.replace(pos, 8, script);
        pos += script.size();
    }
    return html;
}

void set_admin_page_csp(HttpContext& ctx, std::string_view nonce) {
    ctx.response_headers.emplace_back("Content-Security-Policy",
        "default-src 'self'; script-src 'self' 'nonce-" + std::string(nonce) +
        "'; style-src 'self' 'nonce-" + std::string(nonce) +
        "'; connect-src 'self'; img-src 'self' data:; base-uri 'none'; "
        "frame-ancestors 'none'; form-action 'self'");
}

asio::awaitable<void> handle_api_admin_login_impl(
    HttpContext& ctx, AppServices services) {
    if (services.admin_metrics) {
        services.admin_metrics->auth_attempts.fetch_add(1, std::memory_order_relaxed);
    }
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
    auto throttle = services.admin_login_throttle;
    if (!throttle) {
        ctx.status_code = 503;
        ctx.response_body = resp_err(SERVER_ERROR, "admin login runtime unavailable");
        co_return;
    }
    if (throttle->locked(client_ip)) {
        if (services.admin_metrics) {
            services.admin_metrics->auth_rejected.fetch_add(1, std::memory_order_relaxed);
            services.admin_metrics->login_failures.fetch_add(1, std::memory_order_relaxed);
        }
        LOG_WARN("admin login rejected: locked username=", parsed.request.username,
            ", ip=", client_ip);
        admin_login_failed(ctx);
        co_return;
    }

    auto work_limiter = services.admin_auth_limiter;
    if (!work_limiter || !work_limiter->try_acquire()) {
        if (services.admin_metrics) services.admin_metrics->auth_work_rejections.fetch_add(1, std::memory_order_relaxed);
        if (services.admin_metrics) services.admin_metrics->auth_rejected.fetch_add(1, std::memory_order_relaxed);
        ctx.status_code = 429;
        ctx.response_headers.emplace_back("Retry-After", "1");
        ctx.response_body = resp_err(429, "too many login attempts");
        co_return;
    }
    AdminAuthWorkPermit permit(*work_limiter, services.admin_metrics);
    if (services.admin_auth_workers) {
        co_await asio::post(*services.admin_auth_workers, asio::use_awaitable);
    }

    const bool configured = services.admin_credentials ?
        services.admin_credentials->configured(
            services.config_base.empty() ? &services.config_sync.admin : nullptr,
            &admin_config_error) :
        config_admin::admin_configured(*current_admin, services.config_base);
    if (!configured) {
        if (services.admin_metrics) services.admin_metrics->auth_rejected.fetch_add(1, std::memory_order_relaxed);
        ctx.status_code = 503;
        ctx.response_body = resp_err(SERVER_ERROR, "admin API is not configured");
        co_return;
    }
    if (!config_admin::verify_admin_password(
            *current_admin, parsed.request.username, parsed.request.password)) {
        if (services.admin_metrics) services.admin_metrics->login_failures.fetch_add(1, std::memory_order_relaxed);
        throttle->record_failure(client_ip);
        LOG_WARN("admin login failed: username=", parsed.request.username,
            ", ip=", client_ip);
        admin_login_failed(ctx);
        co_return;
    }

    auto issued = services.admin_credentials ?
        services.admin_credentials->issue(
            parsed.request.username,
            services.config_base.empty() ? &services.config_sync.admin : nullptr,
            &admin_config_error) :
        config_admin::issue_admin_token(
            *current_admin, parsed.request.username, services.config_base);
    if (!issued) {
        if (services.admin_metrics) services.admin_metrics->auth_rejected.fetch_add(1, std::memory_order_relaxed);
        ctx.status_code = 503;
        ctx.response_body = resp_err(SERVER_ERROR, "admin token signer is unavailable");
        co_return;
    }

    throttle->record_success(client_ip);
    if (services.admin_metrics) {
        services.admin_metrics->throttle_locked_ips.store(
            throttle->locked_entries(), std::memory_order_relaxed);
    }
    if (services.admin_metrics) services.admin_metrics->login_successes.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO("admin login succeeded: username=", parsed.request.username,
        ", ip=", client_ip);
    ctx.status_code = 200;
    ctx.response_body = resp_ok("{\"token\":\"" + json_escape(issued->token) +
        "\",\"expires_in\":" + std::to_string(issued->expires_in) + "}");
    co_return;
}

}  // namespace admin_route_detail

asio::awaitable<void> handle_api_admin_login(HttpContext& ctx, AppServices services) {
    return admin_route_detail::handle_api_admin_login_impl(ctx, std::move(services));
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
    auto nonce = admin_route_detail::csp_nonce();
    if (!nonce) {
        ctx.status_code = 500;
        ctx.response_body = "admin page unavailable";
        co_return;
    }
    admin_route_detail::set_admin_page_csp(ctx, *nonce);
    ctx.response_body = admin_route_detail::add_csp_nonce(
        config_admin::admin_login_html(), *nonce);
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
    auto nonce = admin_route_detail::csp_nonce();
    if (!nonce) {
        ctx.status_code = 500;
        ctx.response_body = "admin page unavailable";
        co_return;
    }
    admin_route_detail::set_admin_page_csp(ctx, *nonce);
    ctx.response_body = admin_route_detail::add_csp_nonce(
        config_admin::admin_settings_html(), *nonce);
    co_return;
}
