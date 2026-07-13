#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>

#include "http/http_server.hpp"

namespace {

using tcp = asio::ip::tcp;

Config make_upstream_config(const std::string& name, const std::string& host, int port) {
    auto path = std::filesystem::temp_directory_path() /
        ("asio_owen_proxy_framing_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ini");
    {
        std::ofstream out(path);
        out << "[upstream]\n";
        out << name << " = " << host << ":" << port << "\n";
    }
    Config cfg;
    cfg.load_file(path);
    std::filesystem::remove(path);
    return cfg;
}

asio::awaitable<bool> read_request_headers(tcp::socket& socket) {
    asio::error_code ec;
    std::string request;
    std::array<char, 1024> buf{};
    while (request.find("\r\n\r\n") == std::string::npos) {
        auto n = co_await socket.async_read_some(
            asio::buffer(buf), asio::redirect_error(asio::use_awaitable, ec));
        if (ec) co_return false;
        request.append(buf.data(), n);
    }
    co_return true;
}

asio::awaitable<void> serve_upstream_once(tcp::acceptor& acceptor, std::string response) {
    asio::error_code ec;
    auto socket = co_await acceptor.async_accept(
        asio::redirect_error(asio::use_awaitable, ec));
    if (ec) co_return;

    if (!co_await read_request_headers(socket)) co_return;

    co_await asio::async_write(
        socket, asio::buffer(response), asio::redirect_error(asio::use_awaitable, ec));
    socket.shutdown(tcp::socket::shutdown_both, ec);
    socket.close(ec);
}

asio::awaitable<void> serve_upstream_stalls_after_response(
    tcp::acceptor& acceptor,
    std::string response,
    std::chrono::milliseconds stall_for) {
    asio::error_code ec;
    auto socket = co_await acceptor.async_accept(
        asio::redirect_error(asio::use_awaitable, ec));
    if (ec) co_return;

    if (!co_await read_request_headers(socket)) co_return;

    co_await asio::async_write(
        socket, asio::buffer(response), asio::redirect_error(asio::use_awaitable, ec));
    asio::steady_timer timer(co_await asio::this_coro::executor);
    timer.expires_after(stall_for);
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    socket.shutdown(tcp::socket::shutdown_both, ec);
    socket.close(ec);
}

asio::awaitable<void> serve_oversized_then_ok_on_new_connection(
    tcp::acceptor& acceptor,
    std::atomic<int>& accepted_count) {
    asio::error_code ec;
    auto first = co_await acceptor.async_accept(
        asio::redirect_error(asio::use_awaitable, ec));
    if (ec) co_return;
    ++accepted_count;
    if (!co_await read_request_headers(first)) co_return;

    std::string oversized_response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "hello";
    co_await asio::async_write(
        first, asio::buffer(oversized_response), asio::redirect_error(asio::use_awaitable, ec));
    if (ec) co_return;

    auto second = co_await acceptor.async_accept(
        asio::redirect_error(asio::use_awaitable, ec));
    if (ec) co_return;
    ++accepted_count;
    if (!co_await read_request_headers(second)) co_return;

    std::string ok_response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 2\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "ok";
    co_await asio::async_write(
        second, asio::buffer(ok_response), asio::redirect_error(asio::use_awaitable, ec));
    second.shutdown(tcp::socket::shutdown_both, ec);
    second.close(ec);
    first.close(ec);
}

std::string send_proxy_request(unsigned short port) {
    asio::io_context client_ioc;
    tcp::socket client(client_ioc);
    client.connect({asio::ip::make_address("127.0.0.1"), port});

    std::string request =
        "GET /svc/test HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";
    asio::write(client, asio::buffer(request));

    std::string response;
    asio::error_code ec;
    asio::read(client, asio::dynamic_buffer(response), ec);
    return response;
}

template <typename SpawnUpstream>
std::string proxy_request_with(SpawnUpstream spawn_upstream, int read_timeout_ms = 1000) {
    asio::io_context ioc;
    tcp::acceptor upstream_acceptor(ioc, {tcp::v4(), 0});

    HttpPool::Config pool_cfg;
    pool_cfg.max_body_size = 4;
    pool_cfg.connect_timeout_ms = 1000;
    pool_cfg.read_timeout_ms = read_timeout_ms;
    pool_cfg.request_timeout_ms = 1000;

    HttpServer server(ioc, 0);
    auto upstream_cfg = make_upstream_config(
        "svc", "127.0.0.1", upstream_acceptor.local_endpoint().port());
    server.upstreams().reload(upstream_cfg, pool_cfg);

    spawn_upstream(ioc, upstream_acceptor);
    co_spawn(ioc, server.start(), asio::detached);

    std::thread runner([&]() {
        ioc.run();
    });

    std::string response = send_proxy_request(server.port());
    asio::error_code ec;

    server.stop();
    upstream_acceptor.close(ec);
    ioc.stop();
    if (runner.joinable()) runner.join();

    return response;
}

std::string proxy_request_for(std::string upstream_response) {
    return proxy_request_with([response = std::move(upstream_response)](
        asio::io_context& ioc, tcp::acceptor& upstream_acceptor) mutable {
        co_spawn(ioc, serve_upstream_once(upstream_acceptor, std::move(response)),
            asio::detached);
    });
}

void expect_bad_gateway_without_truncated_body(const std::string& response) {
    EXPECT_TRUE(response.rfind("HTTP/1.1 502", 0) == 0) << response;
    EXPECT_NE(response.find("Bad Gateway"), std::string::npos) << response;
    EXPECT_EQ(response.find("hello"), std::string::npos) << response;
}

}  // namespace

TEST(ProxyFraming, ContentLengthBodyTooLargeReturns502) {
    auto response = proxy_request_for(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "hello");

    expect_bad_gateway_without_truncated_body(response);
}

TEST(ProxyFraming, ChunkedBodyTooLargeReturns502) {
    auto response = proxy_request_for(
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n");

    expect_bad_gateway_without_truncated_body(response);
}

TEST(ProxyFraming, ChunkedControlLineTooLargeReturns502) {
    std::string upstream_response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n";
    upstream_response += std::string(kMaxChunkControlLineSize + 1, '1');

    auto response = proxy_request_for(std::move(upstream_response));

    EXPECT_TRUE(response.rfind("HTTP/1.1 502", 0) == 0) << response;
    EXPECT_NE(response.find("Bad Gateway"), std::string::npos) << response;
}

TEST(ProxyFraming, EofFramedBodyTooLargeReturns502) {
    auto response = proxy_request_for(
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "hello");

    expect_bad_gateway_without_truncated_body(response);
}

TEST(ProxyFraming, EofFramedReadTimeoutReturns502) {
    auto response = proxy_request_with([](asio::io_context& ioc, tcp::acceptor& upstream_acceptor) {
        co_spawn(ioc, serve_upstream_stalls_after_response(
            upstream_acceptor,
            "HTTP/1.1 200 OK\r\n"
            "Connection: close\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "hi",
            std::chrono::milliseconds(200)),
            asio::detached);
    }, 50);

    EXPECT_TRUE(response.rfind("HTTP/1.1 502", 0) == 0) << response;
    EXPECT_NE(response.find("Bad Gateway"), std::string::npos) << response;
    EXPECT_EQ(response.find("hi"), std::string::npos) << response;
}

TEST(ProxyFraming, OversizedUpstreamConnectionIsNotReused) {
    asio::io_context ioc;
    tcp::acceptor upstream_acceptor(ioc, {tcp::v4(), 0});
    std::atomic<int> accepted_count{0};

    HttpPool::Config pool_cfg;
    pool_cfg.max_body_size = 4;
    pool_cfg.connect_timeout_ms = 1000;
    pool_cfg.read_timeout_ms = 200;
    pool_cfg.request_timeout_ms = 1000;

    HttpServer server(ioc, 0);
    auto upstream_cfg = make_upstream_config(
        "svc", "127.0.0.1", upstream_acceptor.local_endpoint().port());
    server.upstreams().reload(upstream_cfg, pool_cfg);
    auto route = server.upstreams().route("/svc/test");
    ASSERT_TRUE(route.has_value());
    auto pool_state = route->pool->state();

    co_spawn(ioc, serve_oversized_then_ok_on_new_connection(upstream_acceptor, accepted_count),
        asio::detached);
    co_spawn(ioc, server.start(), asio::detached);

    std::thread runner([&]() {
        ioc.run();
    });

    auto first_response = send_proxy_request(server.port());
    expect_bad_gateway_without_truncated_body(first_response);

    auto second_response = send_proxy_request(server.port());
    EXPECT_TRUE(second_response.rfind("HTTP/1.1 200", 0) == 0) << second_response;
    EXPECT_NE(second_response.find("\r\n\r\nok"), std::string::npos) << second_response;

    EXPECT_EQ(accepted_count.load(), 2);
    EXPECT_EQ(pool_state->acquire_created.load(std::memory_order_relaxed), 2u);
    EXPECT_EQ(pool_state->acquire_reused.load(std::memory_order_relaxed), 0u);
    EXPECT_GE(pool_state->released_bad.load(std::memory_order_relaxed), 1u);

    asio::error_code ec;
    server.stop();
    upstream_acceptor.close(ec);
    ioc.stop();
    if (runner.joinable()) runner.join();
}
