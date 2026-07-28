#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <optional>
#include <thread>

#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>

#include "http/http_io.hpp"

namespace {

using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

struct ConnectedSockets {
    tcp::socket client;
    tcp::socket server;
    bool accepted = false;

    explicit ConnectedSockets(asio::io_context& ioc) : client(ioc), server(ioc) {
        tcp::acceptor acceptor(ioc, {tcp::v4(), 0});
        asio::error_code accept_ec;
        acceptor.async_accept(server, [&](const asio::error_code& ec) { accept_ec = ec; });
        client.connect(acceptor.local_endpoint());
        ioc.run();
        accepted = !accept_ec;
        ioc.restart();
    }
};

TEST(HttpIoDeadline, TimesOutCurrentRead) {
    asio::io_context ioc;
    ConnectedSockets sockets(ioc);
    ASSERT_TRUE(sockets.accepted);
    std::optional<IoResult> result;

    auto strand = asio::make_strand(ioc);
    asio::co_spawn(strand, [&]() -> asio::awaitable<void> {
        OperationDeadline deadline(co_await asio::this_coro::executor);
        std::array<char, 8> buffer{};
        result = co_await read_with_timeout(
            sockets.server, buffer.data(), buffer.size(), 15ms, deadline);
    }, asio::detached);

    ioc.run();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, IoStatus::Timeout);
}

TEST(HttpIoDeadline, CompletedReadDoesNotCancelNextOperation) {
    asio::io_context ioc;
    ConnectedSockets sockets(ioc);
    ASSERT_TRUE(sockets.accepted);
    std::array<char, 1> first{};
    std::array<char, 1> second{};
    std::optional<IoResult> first_result;
    std::optional<IoResult> second_result;

    asio::write(sockets.client, asio::buffer("a", 1));
    std::thread sender([&]() {
        std::this_thread::sleep_for(20ms);
        asio::error_code ec;
        asio::write(sockets.client, asio::buffer("b", 1), ec);
    });

    auto strand = asio::make_strand(ioc);
    asio::co_spawn(strand, [&]() -> asio::awaitable<void> {
        OperationDeadline deadline(co_await asio::this_coro::executor);
        first_result = co_await read_with_timeout(
            sockets.server, first.data(), first.size(), 100ms, deadline);
        second_result = co_await read_with_timeout(
            sockets.server, second.data(), second.size(), 100ms, deadline);
    }, asio::detached);

    std::thread worker([&]() { ioc.run(); });
    ioc.run();
    worker.join();
    sender.join();

    ASSERT_TRUE(first_result.has_value());
    ASSERT_TRUE(second_result.has_value());
    EXPECT_EQ(first_result->status, IoStatus::Success);
    EXPECT_EQ(second_result->status, IoStatus::Success);
    EXPECT_EQ(first[0], 'a');
    EXPECT_EQ(second[0], 'b');
}

TEST(HttpIoDeadline, CompletedWriteDoesNotCancelNextRead) {
    asio::io_context ioc;
    ConnectedSockets sockets(ioc);
    std::array<char, 1> server_read{};
    std::optional<IoResult> write_result;
    std::optional<IoResult> read_result;

    std::thread client([&]() {
        std::array<char, 1> received{};
        asio::error_code ec;
        asio::read(sockets.client, asio::buffer(received), ec);
        if (!ec && received[0] == 'a') {
            asio::write(sockets.client, asio::buffer("b", 1), ec);
        }
    });

    auto strand = asio::make_strand(ioc);
    asio::co_spawn(strand, [&]() -> asio::awaitable<void> {
        OperationDeadline deadline(co_await asio::this_coro::executor);
        write_result = co_await write_with_timeout(sockets.server, "a", 100ms, deadline);
        read_result = co_await read_with_timeout(
            sockets.server, server_read.data(), server_read.size(), 100ms, deadline);
    }, asio::detached);

    std::thread worker([&]() { ioc.run(); });
    ioc.run();
    worker.join();
    client.join();

    ASSERT_TRUE(write_result.has_value());
    ASSERT_TRUE(read_result.has_value());
    EXPECT_EQ(write_result->status, IoStatus::Success);
    EXPECT_EQ(read_result->status, IoStatus::Success);
    EXPECT_EQ(server_read[0], 'b');
}

}  // namespace
