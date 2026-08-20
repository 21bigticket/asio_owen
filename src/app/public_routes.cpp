#include "public_routes.hpp"

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
#include "config_sync_service.hpp"
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
        [](const std::exception_ptr& ep) {
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

}  // namespace

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
                [](const std::exception_ptr& ep, const RedisPool::Reply& reply) {
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

asio::awaitable<void> api_ready(HttpContext& ctx, AppServices services) {
    const bool draining = services.draining_state &&
        services.draining_state->load(std::memory_order_acquire);

    ConfigSyncService::State sync_state;
    const bool sync_required = services.config_sync.enabled;
    const bool sync_ready = !sync_required || [&]() {
        sync_state = ConfigSyncService::load_state(services.config_base);
        return sync_state.exists && sync_state.status == "ok";
    }();
    const bool history_ready = !services.config_history_service ||
        !services.config_history_service->inconsistent();
    const bool ready = !draining && sync_ready && history_ready;

    std::ostringstream data;
    data << "{\"ready\":" << (ready ? "true" : "false")
         << ",\"draining\":" << (draining ? "true" : "false")
         << ",\"config_sync\":{\"required\":"
         << (sync_required ? "true" : "false")
         << ",\"healthy\":" << (sync_ready ? "true" : "false")
         << ",\"synced_version\":" << sync_state.synced_version
         << ",\"status\":\"" << json_escape(
             sync_required ? sync_state.status : "disabled") << "\"}"
         << ",\"history\":{\"healthy\":"
         << (history_ready ? "true" : "false") << "}";
    if (services.config_history_service) {
        const auto stats = services.config_history_service->stats();
        data << ",\"history_max_version\":" << stats.max_observed_version;
    }
    data << "}";

    ctx.response_headers.emplace_back("Content-Type", "application/json");
    ctx.status_code = ready ? 200 : 503;
    ctx.response_body = json_resp(ready ? 0 : 503,
        ready ? "ready" : "not ready", data.str());
    co_return;
}

asio::awaitable<void> api_metrics(HttpContext& ctx, AppServices services) {
    ConfigSyncService::State sync_state;
    if (services.config_sync.enabled) {
        sync_state = ConfigSyncService::load_state(services.config_base);
    }
    const auto history_stats = services.config_history_service ?
        services.config_history_service->stats() : ConfigHistoryService::Stats{};

    std::ostringstream data;
    data << "{\"draining\":"
         << (services.draining_state &&
                 services.draining_state->load(std::memory_order_acquire) ?
             "true" : "false")
         << ",\"config_sync\":{\"enabled\":"
         << (services.config_sync.enabled ? "true" : "false")
         << ",\"version\":" << sync_state.synced_version
         << ",\"status\":\"" << json_escape(
             services.config_sync.enabled ? sync_state.status : "disabled") << "\"}"
         << ",\"history\":{\"checks\":" << history_stats.checks
         << ",\"inconsistent_checks\":" << history_stats.inconsistent_checks
         << ",\"gc_deleted\":" << history_stats.gc_deleted
         << ",\"gc_failures\":" << history_stats.gc_failures
         << ",\"max_observed_version\":" << history_stats.max_observed_version
         << ",\"inconsistent\":"
         << (history_stats.inconsistent ? "true" : "false") << "}"
         << ",\"admin_auth\":{\"attempts\":"
         << (services.admin_metrics ? services.admin_metrics->auth_attempts.load(std::memory_order_relaxed) : 0)
         << ",\"rejected\":"
         << (services.admin_metrics ? services.admin_metrics->auth_rejected.load(std::memory_order_relaxed) : 0)
         << ",\"login_failures\":"
         << (services.admin_metrics ? services.admin_metrics->login_failures.load(std::memory_order_relaxed) : 0)
         << ",\"login_successes\":"
         << (services.admin_metrics ? services.admin_metrics->login_successes.load(std::memory_order_relaxed) : 0)
         << ",\"in_flight\":"
         << (services.admin_metrics ? services.admin_metrics->auth_in_flight.load(std::memory_order_relaxed) : 0)
         << ",\"work_rejections\":"
         << (services.admin_metrics ? services.admin_metrics->auth_work_rejections.load(std::memory_order_relaxed) : 0)
         << ",\"locked_ips\":"
         << (services.admin_metrics ? services.admin_metrics->throttle_locked_ips.load(std::memory_order_relaxed) : 0)
         << ",\"drain_started_ns\":"
         << (services.admin_metrics ? services.admin_metrics->drain_started_ns.load(std::memory_order_relaxed) : 0)
         << ",\"drain_completed_ns\":"
         << (services.admin_metrics ? services.admin_metrics->drain_completed_ns.load(std::memory_order_relaxed) : 0)
         << ",\"drain_duration_ns\":"
         << (services.admin_metrics ? services.admin_metrics->drain_duration_ns.load(std::memory_order_relaxed) : 0)
         << ",\"drain_timeouts\":"
         << (services.admin_metrics ? services.admin_metrics->drain_timeout_count.load(std::memory_order_relaxed) : 0)
         << ",\"forced_stops\":"
         << (services.admin_metrics ? services.admin_metrics->forced_stop_count.load(std::memory_order_relaxed) : 0)
         << "}"
         << ",\"config_workers\":{\"jobs\":"
         << (services.runtime && services.runtime->config_metrics ? services.runtime->config_metrics->file_worker_jobs.load(std::memory_order_relaxed) : 0)
         << ",\"fallbacks\":"
         << (services.runtime && services.runtime->config_metrics ? services.runtime->config_metrics->file_worker_fallbacks.load(std::memory_order_relaxed) : 0)
         << ",\"busy\":"
         << (services.runtime && services.runtime->config_metrics ? services.runtime->config_metrics->file_worker_busy.load(std::memory_order_relaxed) : 0)
         << ",\"queue_delay_us_total\":"
         << (services.runtime && services.runtime->config_metrics ? services.runtime->config_metrics->file_worker_queue_delay_us_total.load(std::memory_order_relaxed) : 0)
         << ",\"queue_delay_us_max\":"
         << (services.runtime && services.runtime->config_metrics ? services.runtime->config_metrics->file_worker_queue_delay_us_max.load(std::memory_order_relaxed) : 0)
         << ",\"scan_us_total\":"
         << (services.runtime && services.runtime->config_metrics ? services.runtime->config_metrics->file_scan_duration_us_total.load(std::memory_order_relaxed) : 0)
         << ",\"scan_us_max\":"
         << (services.runtime && services.runtime->config_metrics ? services.runtime->config_metrics->file_scan_duration_us_max.load(std::memory_order_relaxed) : 0)
         << ",\"files_scanned\":"
         << (services.runtime && services.runtime->config_metrics ? services.runtime->config_metrics->files_scanned.load(std::memory_order_relaxed) : 0)
         << ",\"bytes_read\":"
         << (services.runtime && services.runtime->config_metrics ? services.runtime->config_metrics->file_bytes_read.load(std::memory_order_relaxed) : 0)
         << "}"
         << ",\"http_pool\":\""
         << json_escape(services.upstreams ? services.upstreams->pool_stats() : "")
         << "\"}";

    ctx.response_headers.emplace_back("Content-Type", "application/json");
    ctx.response_headers.emplace_back("Cache-Control", "no-store");
    ctx.status_code = 200;
    ctx.response_body = resp_ok(data.str());
    co_return;
}

asio::awaitable<void> api_build(HttpContext& ctx) {
    ctx.response_headers.emplace_back("Content-Type", "application/json");
    ctx.status_code = 200;
    ctx.response_body = "{\"code\":0,\"build\":\"asio_owen\"}";
    co_return;
}
