#pragma once

#include <asio.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <chrono>
#include <memory>
#include <string>

enum class IoStatus {
    Success,
    Timeout,
    PeerClosed,
    SysError
};

struct IoResult {
    IoStatus status = IoStatus::Success;
    std::size_t bytes = 0;
    asio::error_code ec;

    bool ok() const { return status == IoStatus::Success; }
};

inline std::string io_status_name(IoStatus status) {
    switch (status) {
        case IoStatus::Success: return "success";
        case IoStatus::Timeout: return "timeout";
        case IoStatus::PeerClosed: return "peer_closed";
        case IoStatus::SysError: return "sys_error";
    }
    return "unknown";
}

inline IoStatus classify_read_error(const asio::error_code& ec, bool timed_out, std::size_t n) {
    if (timed_out) return IoStatus::Timeout;
    if (!ec && n > 0) return IoStatus::Success;
    if (!ec && n == 0) return IoStatus::PeerClosed;
    if (ec == asio::error::eof || ec == asio::error::connection_reset) {
        return IoStatus::PeerClosed;
    }
    return IoStatus::SysError;
}

inline IoStatus classify_write_error(const asio::error_code& ec, bool timed_out) {
    if (timed_out) return IoStatus::Timeout;
    if (!ec) return IoStatus::Success;
    if (ec == asio::error::eof || ec == asio::error::connection_reset ||
        ec == asio::error::broken_pipe) {
        return IoStatus::PeerClosed;
    }
    return IoStatus::SysError;
}

inline asio::awaitable<IoResult> read_with_timeout(
    asio::ip::tcp::socket& sock,
    char* buf,
    std::size_t size,
    std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0) {
        asio::error_code ec;
        auto n = co_await sock.async_read_some(
            asio::buffer(buf, size), asio::redirect_error(asio::use_awaitable, ec));
        co_return IoResult{classify_read_error(ec, false, n), n, ec};
    }

    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer timer(ex);
    timer.expires_after(timeout);

    auto [order, read_ec, n, timer_ec] = co_await asio::experimental::make_parallel_group(
        [&](auto token) {
            return sock.async_read_some(asio::buffer(buf, size), token);
        },
        [&](auto token) {
            return timer.async_wait(token);
        }
    ).async_wait(asio::experimental::wait_for_one(), asio::use_awaitable);

    if (order[0] == 1 && !timer_ec) {
        co_return IoResult{IoStatus::Timeout, 0, asio::error::operation_aborted};
    }
    co_return IoResult{classify_read_error(read_ec, false, n), n, read_ec};
}

inline asio::awaitable<IoResult> write_with_timeout(
    asio::ip::tcp::socket& sock,
    const std::string& data,
    std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0) {
        asio::error_code ec;
        co_await asio::async_write(
            sock, asio::buffer(data), asio::redirect_error(asio::use_awaitable, ec));
        co_return IoResult{classify_write_error(ec, false), ec ? 0 : data.size(), ec};
    }

    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer timer(ex);
    timer.expires_after(timeout);

    auto [order, write_ec, bytes, timer_ec] = co_await asio::experimental::make_parallel_group(
        [&](auto token) {
            return asio::async_write(sock, asio::buffer(data), token);
        },
        [&](auto token) {
            return timer.async_wait(token);
        }
    ).async_wait(asio::experimental::wait_for_one(), asio::use_awaitable);

    if (order[0] == 1 && !timer_ec) {
        co_return IoResult{IoStatus::Timeout, 0, asio::error::operation_aborted};
    }
    co_return IoResult{classify_write_error(write_ec, false), write_ec ? 0 : bytes, write_ec};
}
