#pragma once

#include <asio/executor_work_guard.hpp>

#include <atomic>
#include <optional>

#include "../routes.hpp"

namespace admin_route_detail {

// Keeps the request executor from running out of work while an Admin
// operation is in flight on the worker pool. Without this guard, an
// io_context that only hosts the awaiting coroutine can drain (and be
// destroyed) while the operation is still bouncing between the pool and
// the executor — the operation then posts into a dead executor (UB).
// The operation holds the guard from construction until completion.
class AdminExecutorGuard {
public:
    explicit AdminExecutorGuard(const asio::any_io_executor& executor)
        : guard_(asio::make_work_guard(executor)) {}

    void release() noexcept { guard_.reset(); }

private:
    std::optional<asio::executor_work_guard<asio::any_io_executor>> guard_;
};

void dispatch_admin_work(
    const AppServices& services,
    std::function<void()> work,
    std::function<void(std::exception_ptr)> failure) noexcept;
asio::awaitable<RedisPool::Reply> run_redis_command(
    AppServices services, std::vector<std::string> args);
bool redis_command_available(const AppServices& services);
bool authorize_admin(HttpContext& ctx, const AppServices& services);
void method_not_allowed(HttpContext& ctx, std::string allow);
void dispatch_redis_command(
    const AppServices& services,
    asio::any_io_executor executor,
    std::vector<std::string> args,
    std::function<void(RedisPool::Reply)> callback,
    std::function<void(std::exception_ptr)> failure);
void complete_admin_request(
    std::atomic<bool>& completed,
    const asio::any_io_executor& executor,
    std::function<void()>& completion) noexcept;

}  // namespace admin_route_detail
