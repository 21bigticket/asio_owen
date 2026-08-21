#include "admin_history_operation.hpp"

#include <exception>

#include "../../http/response.hpp"

namespace admin_route_detail {

void AdminHistoryOperation::read_detail(int64_t version, DetailCallback callback) {
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

void AdminHistoryOperation::run_command(std::vector<std::string> args, ReplyCallback callback) {
    auto self = shared_from_this();
    dispatch_redis_command(
        services_, executor_, std::move(args), std::move(callback),
        [self](std::exception_ptr ep) { self->fail(ep); });
}

void AdminHistoryOperation::bad_request(const std::string& message) {
    ctx_.status_code = 400;
    ctx_.response_body = resp_err(PARAM_ERROR, message);
    complete();
}

void AdminHistoryOperation::not_found() {
    ctx_.status_code = 404;
    ctx_.response_body = resp_err(404, "history version not found");
    complete();
}

void AdminHistoryOperation::redis_failed(const std::string& message) {
    ctx_.status_code = 500;
    ctx_.response_body = resp_err(DB_ERROR, message);
    complete();
}

void AdminHistoryOperation::inconsistent(const std::string& message) {
    ctx_.status_code = 409;
    ctx_.response_body = json_resp(409, message);
    complete();
}

void AdminHistoryOperation::fail(std::exception_ptr ep) noexcept {
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

void AdminHistoryOperation::complete() noexcept {
    executor_guard_.release();
    complete_admin_request(completed_, executor_, completion_);
}

}  // namespace admin_route_detail
