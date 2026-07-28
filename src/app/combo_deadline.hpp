#pragma once

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/concurrent_channel.hpp>
#include <asio/redirect_error.hpp>
#include <asio/this_coro.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <utility>

#include "combo_query_limiter.hpp"

template <typename Result>
struct ComboDeadlineMessage {
    bool timed_out = false;
    std::optional<Result> result;
};

template <typename Result>
using ComboDeadlineChannel = asio::experimental::concurrent_channel<
    asio::any_io_executor, void(asio::error_code, ComboDeadlineMessage<Result>)>;

template <typename Result>
asio::awaitable<void> send_combo_deadline(
    std::shared_ptr<ComboDeadlineChannel<Result>> channel,
    std::shared_ptr<asio::steady_timer> timer,
    std::shared_ptr<ComboQueryState> state) {
    asio::error_code ec;
    co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
    if (!ec && state->claim_timeout()) {
        channel->try_send(asio::error_code{}, ComboDeadlineMessage<Result>{true, std::nullopt});
    }
}

template <typename Result, typename ErrorResultFactory>
asio::awaitable<std::optional<Result>> await_combo_query_with_deadline(
    asio::awaitable<Result> query, ComboQueryPermit permit,
    std::chrono::milliseconds deadline, ErrorResultFactory make_error_result) {
    auto ex = co_await asio::this_coro::executor;
    auto channel = std::make_shared<ComboDeadlineChannel<Result>>(ex, 1);
    auto timer = std::make_shared<asio::steady_timer>(ex);
    auto state = std::make_shared<ComboQueryState>(std::move(permit));
    timer->expires_after(deadline);

    asio::co_spawn(ex, std::move(query),
        [channel, state, make_error_result = std::move(make_error_result)](
            std::exception_ptr ep, Result result) mutable {
            if (ep) result = make_error_result(ep);
            if (state->claim_query()) {
                channel->try_send(asio::error_code{},
                    ComboDeadlineMessage<Result>{false, std::move(result)});
            }
        });
    asio::co_spawn(ex, send_combo_deadline<Result>(channel, timer, state), asio::detached);

    auto message = co_await channel->async_receive(asio::use_awaitable);
    if (message.timed_out) co_return std::nullopt;
    timer->cancel();
    co_return std::move(message.result);
}
