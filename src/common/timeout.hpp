#pragma once
#include <asio.hpp>
#include <chrono>
#include <optional>

template<typename T>
auto with_timeout(asio::steady_timer& timer, 
                  asio::awaitable<T> op,
                  std::chrono::milliseconds ms)
    -> asio::awaitable<std::optional<T>>
{
    auto ex = co_await asio::this_coro::executor;
    bool completed = false;
    std::optional<T> result;

    timer.expires_after(ms);

    co_spawn(ex, [&]() -> asio::awaitable<void> {
        try {
            result = co_await std::move(op);
        } catch (const std::exception& e) {
            LOG_WARN("Timeout operation error: ", e.what());
        } catch (...) {
            LOG_WARN("Timeout operation unknown error");
        }
        completed = true;
        timer.cancel();
    }, asio::detached);

    std::error_code ec;
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));

    co_return completed ? result : std::nullopt;
}
