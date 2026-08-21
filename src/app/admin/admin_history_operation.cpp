#include "admin_history_operation.hpp"

#include <asio/async_result.hpp>
#include <asio/use_awaitable.hpp>

#include <type_traits>
#include <utility>

#include "../config_history_service.hpp"
#include "../../http/response.hpp"

namespace admin_route_detail {

AdminHistoryOperation::AdminHistoryOperation(
    HttpContext& ctx, AppServices services, asio::any_io_executor executor,
    std::function<void()> completion, StartAction start_action)
    : ctx_(ctx), services_(std::move(services)), executor_(std::move(executor)),
      executor_guard_(executor_), completion_(std::move(completion)),
      start_action_(std::move(start_action)) {}

void AdminHistoryOperation::start() {
    auto self = shared_from_this();
    dispatch_admin_work(services_,
        [self]() { self->start_blocking(); },
        [self](const std::exception_ptr& ep) { self->fail(ep); });
}

void AdminHistoryOperation::start_history_list() { start_list(); }
void AdminHistoryOperation::start_history_path() { start_path(); }

void AdminHistoryOperation::start_rollback_request() {
    if (services_.config_history_service &&
        services_.config_history_service->inconsistent()) {
        ctx_.status_code = 409;
        ctx_.response_body = json_resp(
            409, "history is inconsistent; rollback is frozen");
        complete();
        return;
    }
    start_rollback();
}

void AdminHistoryOperation::start_snapshot_repair() {
    repair_plan_ = RepairPlan{
        .action = "snapshot-repair",
        .script = &config_history::snapshot_repair_script
    };
    start_repair();
}

void AdminHistoryOperation::start_mirror_rebuild() {
    repair_plan_ = RepairPlan{
        .read_history_snapshot = true,
        .use_config_staging = true,
        .action = "mirror-rebuild",
        .script = &config_history::mirror_rebuild_script
    };
    start_repair();
}

void AdminHistoryOperation::start_history_migration() {
    repair_plan_ = RepairPlan{
        .compat_only = true,
        .synthesize_metadata = true,
        .action = "migration",
        .script = &config_history::migration_script
    };
    start_repair();
}

void AdminHistoryOperation::start_orphan_request() {
    start_orphan_resolution();
}

void AdminHistoryOperation::start_blocking() noexcept {
    try {
        if (!authorize_admin(ctx_, services_)) {
            complete();
            return;
        }
        if (!redis_command_available(services_)) {
            ctx_.status_code = 503;
            ctx_.response_body = resp_err(
                SERVER_ERROR, "Redis service unavailable");
            complete();
            return;
        }
        start_action_(*this);
    } catch (...) {
        fail(std::current_exception());
    }
}

std::string AdminHistoryOperation::path_only(std::string_view path) {
    const auto pos = path.find('?');
    return std::string(path.substr(0, pos));
}

std::optional<std::string> AdminHistoryOperation::query_value(
    std::string_view path, std::string_view key) {
    const auto query = path.find('?');
    if (query == std::string_view::npos) return std::nullopt;
    size_t pos = query + 1;
    while (pos <= path.size()) {
        auto end = path.find('&', pos);
        if (end == std::string_view::npos) end = path.size();
        const auto eq = path.find('=', pos);
        if (eq != std::string_view::npos && eq < end &&
            path.substr(pos, eq - pos) == key) {
            return std::string(path.substr(eq + 1, end - eq - 1));
        }
        if (end == path.size()) break;
        pos = end + 1;
    }
    return std::nullopt;
}

bool AdminHistoryOperation::valid_record_hash(
    const config_history::SnapshotRecord& record) {
    const auto expected = config_history::json_string_field(
        record.meta_json, "content_sha256");
    const auto actual = config_history::content_sha256(record.files);
    return expected && actual && *expected == *actual;
}

asio::awaitable<void> start_admin_history_request(
    HttpContext& ctx, AppServices services,
    AdminHistoryOperation::StartAction start_action) {
    asio::use_awaitable_t<> token;
    return asio::async_initiate<asio::use_awaitable_t<>, void()>(
        [&ctx, services = std::move(services),
         start_action = std::move(start_action)](auto handler) mutable {
            asio::any_io_executor executor = asio::get_associated_executor(handler);
            using HandlerType = std::decay_t<decltype(handler)>;
            auto handler_ptr = std::make_shared<HandlerType>(std::move(handler));
            auto completion = [handler_ptr]() mutable { (*handler_ptr)(); };
            auto operation = std::make_shared<AdminHistoryOperation>(
                ctx, std::move(services), std::move(executor),
                std::move(completion), std::move(start_action));
            operation->start();
        }, token);
}

}  // namespace admin_route_detail

asio::awaitable<void> handle_api_admin_history(HttpContext& ctx, AppServices services) {
    return admin_route_detail::start_admin_history_request(
        ctx, std::move(services),
        [](auto& operation) { operation.start_history_list(); });
}

asio::awaitable<void> handle_api_admin_history_path(
    HttpContext& ctx, AppServices services) {
    return admin_route_detail::start_admin_history_request(
        ctx, std::move(services),
        [](auto& operation) { operation.start_history_path(); });
}

asio::awaitable<void> handle_api_admin_rollback(HttpContext& ctx, AppServices services) {
    return admin_route_detail::start_admin_history_request(
        ctx, std::move(services),
        [](auto& operation) { operation.start_rollback_request(); });
}

asio::awaitable<void> handle_api_admin_snapshot_repair(
    HttpContext& ctx, AppServices services) {
    return admin_route_detail::start_admin_history_request(
        ctx, std::move(services),
        [](auto& operation) { operation.start_snapshot_repair(); });
}

asio::awaitable<void> handle_api_admin_mirror_rebuild(
    HttpContext& ctx, AppServices services) {
    return admin_route_detail::start_admin_history_request(
        ctx, std::move(services),
        [](auto& operation) { operation.start_mirror_rebuild(); });
}

asio::awaitable<void> handle_api_admin_history_migration(
    HttpContext& ctx, AppServices services) {
    return admin_route_detail::start_admin_history_request(
        ctx, std::move(services),
        [](auto& operation) { operation.start_history_migration(); });
}

asio::awaitable<void> handle_api_admin_orphan_resolution(
    HttpContext& ctx, AppServices services) {
    return admin_route_detail::start_admin_history_request(
        ctx, std::move(services),
        [](auto& operation) { operation.start_orphan_request(); });
}
