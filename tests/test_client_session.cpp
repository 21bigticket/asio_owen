#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>

#include "http/http_server.hpp"
#include "http/http_protocol.hpp"

namespace {

using tcp = asio::ip::tcp;

constexpr int kServerStartTimeoutMs = 500;
constexpr int kClientReadTimeoutMs = 2000;

Config make_upstream_config(const std::string& name, const std::string& host, int port) {
    auto path = std::filesystem::temp_directory_path() /
        ("asio_owen_client_session_" +
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

// Like make_upstream_config, but also writes the [gateway] transform switch.
Config make_gateway_upstream_config(const std::string& name, const std::string& host,
                                    int port, bool snake_to_camel) {
    auto path = std::filesystem::temp_directory_path() /
        ("asio_owen_client_session_gw_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ini");
    {
        std::ofstream out(path);
        out << "[upstream]\n";
        out << name << " = " << host << ":" << port << "\n";
        out << "[gateway]\n";
        out << "json_keys_snake_to_camel = " << (snake_to_camel ? "true" : "false") << "\n";
    }
    Config cfg;
    cfg.load_file(path);
    std::filesystem::remove(path);
    return cfg;
}

struct TempDirGuard {
    std::filesystem::path path;

    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

TempDirGuard make_temp_security_dir() {
    auto path = std::filesystem::temp_directory_path() /
        ("asio_owen_client_security_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path / "config.d");
    return TempDirGuard{std::move(path)};
}

Config make_security_config(const std::filesystem::path& base, bool cors_enabled,
                            const std::string& allowed_origin,
                            bool include_upstream = false) {
    {
        std::ofstream out(base / "config.d" / "00-test.ini", std::ios::trunc);
        out << "[security]\n"
            << "jwt_disabled = true\n"
            << "[cors]\n"
            << "enabled = " << (cors_enabled ? "true" : "false") << "\n";
        if (cors_enabled) {
            out << "allowed_origins = " << allowed_origin << "\n";
        }
        out << "[rate_limit]\n"
            << "snapshot_path = "
            << (base / "rate_limit.bin").string()
            << "\n";
        if (include_upstream) {
            out << "[upstream]\n"
                << "dead = 127.0.0.1:1\n";
        }
    }
    Config cfg;
    EXPECT_TRUE(cfg.load(base));
    return cfg;
}

// Send raw bytes to the server and read everything until EOF/short read.
// Used for tests that don't depend on Content-Length framing for the response.
struct ClientExchange {
    std::string request;
    bool shutdown_write_after_send = false;
};

std::string exchange_with_server(unsigned short port, const ClientExchange& ex) {
    asio::io_context ioc;
    tcp::socket client(ioc);
    client.connect({asio::ip::make_address("127.0.0.1"), port});

    asio::write(client, asio::buffer(ex.request));
    if (ex.shutdown_write_after_send) {
        asio::error_code ec;
        client.shutdown(tcp::socket::shutdown_send, ec);
    }

    std::string response;
    asio::error_code ec;
    asio::read(client, asio::dynamic_buffer(response), ec);

    asio::error_code ignore;
    client.close(ignore);
    return response;
}

// Read until either the response body is fully received (Content-Length) or a
// short timeout fires. Returns once Content-Length bytes have been read.
std::string read_response_with_timeout(unsigned short port, const std::string& request) {
    asio::io_context ioc;
    tcp::socket client(ioc);
    client.connect({asio::ip::make_address("127.0.0.1"), port});

    asio::write(client, asio::buffer(request));

    std::string response;
    asio::error_code ec;
    asio::read(client, asio::dynamic_buffer(response), ec);

    asio::error_code ignore;
    client.close(ignore);
    return response;
}

// Minimal mock upstream: binds an ephemeral port, drains each request's
// headers, then answers with the same canned response. Serves at most
// max_responses requests; after that its io_context runs out of work and the
// thread exits on its own.
class MockUpstream {
public:
    unsigned short start(std::string canned_response, int max_responses = 1) {
        canned_ = std::move(canned_response);
        remaining_ = max_responses;
        acceptor_ = std::make_unique<tcp::acceptor>(
            ioc_, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
        const unsigned short port = acceptor_->local_endpoint().port();
        do_accept();
        runner_ = std::thread([this] { ioc_.run(); });
        return port;
    }

    ~MockUpstream() {
        if (runner_.joinable()) {
            // Cancels a still-pending accept so run() can return.
            asio::post(ioc_, [this] {
                asio::error_code ec;
                acceptor_->close(ec);
            });
            runner_.join();
        }
    }

    MockUpstream() = default;
    MockUpstream(const MockUpstream&) = delete;
    MockUpstream& operator=(const MockUpstream&) = delete;

private:
    void do_accept() {
        acceptor_->async_accept([this](asio::error_code ec, tcp::socket peer) {
            if (ec) return;
            peer_ = std::move(peer);
            request_.clear();
            read_request();
        });
    }

    void read_request() {
        peer_.async_read_some(asio::buffer(buf_),
            [this](asio::error_code ec, size_t n) {
                request_.append(buf_.data(), n);
                if (!ec && request_.find("\r\n\r\n") == std::string::npos) {
                    read_request();
                    return;
                }
                write_response();
            });
    }

    void write_response() {
        asio::async_write(peer_, asio::buffer(canned_),
            [this](asio::error_code, size_t) {
                asio::error_code ignore;
                peer_.shutdown(tcp::socket::shutdown_both, ignore);
                peer_.close(ignore);
                if (--remaining_ > 0) do_accept();
            });
    }

    asio::io_context ioc_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    tcp::socket peer_{ioc_};
    std::array<char, 1024> buf_{};
    std::string request_;
    std::string canned_;
    int remaining_ = 0;
    std::thread runner_;
};

class ClientSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<HttpServer>(ioc_, 0);
        register_default_routes();
    }

    void TearDown() override {
        if (server_) server_->stop();
        ioc_.stop();
        if (runner_.joinable()) runner_.join();
    }

    void start_server() {
        co_spawn(ioc_, server_->start(), asio::detached);
        runner_ = std::thread([this]() { ioc_.run(); });
        // Give the acceptor a moment to actually start.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    unsigned short port() const { return server_->port(); }

    HttpServer& server() { return *server_; }

    void register_default_routes() {
        server_->route("/api/ping", [](HttpContext& ctx) -> asio::awaitable<void> {
            ctx.response_headers.emplace_back("Content-Type", "text/plain");
            ctx.status_code = 200;
            ctx.response_body = "pong";
            co_return;
        });
        server_->route("/api/echo", [](HttpContext& ctx) -> asio::awaitable<void> {
            ctx.response_headers.emplace_back("Content-Type", "text/plain");
            ctx.status_code = 200;
            ctx.response_body = ctx.body;
            co_return;
        });
    }

private:
    asio::io_context ioc_;
    std::unique_ptr<HttpServer> server_;
    std::thread runner_;
};

// ============== Happy paths ==============

TEST_F(ClientSessionTest, GetLocalRouteReturns200) {
    start_server();
    auto resp = read_response_with_timeout(port(),
        "GET /api/ping HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    EXPECT_TRUE(resp.rfind("HTTP/1.1 200", 0) == 0) << resp;
    EXPECT_NE(resp.find("pong"), std::string::npos) << resp;
}

TEST_F(ClientSessionTest, QueryStringStillMatchesExactLocalRoute) {
    start_server();
    auto resp = read_response_with_timeout(port(),
        "GET /api/ping?limit=20 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    EXPECT_TRUE(resp.rfind("HTTP/1.1 200", 0) == 0) << resp;
    EXPECT_NE(resp.find("pong"), std::string::npos) << resp;
}

TEST_F(ClientSessionTest, LongestPrefixRouteWinsAndIgnoresQueryString) {
    server().route_prefix("/api/admin/config/history/",
        [](HttpContext& ctx) -> asio::awaitable<void> {
            ctx.status_code = 200;
            ctx.response_body = "general-history";
            co_return;
        });
    server().route_prefix("/api/admin/config/history/7/",
        [](HttpContext& ctx) -> asio::awaitable<void> {
            ctx.status_code = 200;
            ctx.response_body = "version-seven";
            co_return;
        });
    start_server();
    auto resp = read_response_with_timeout(port(),
        "GET /api/admin/config/history/7/diff?to=8 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    EXPECT_TRUE(resp.rfind("HTTP/1.1 200", 0) == 0) << resp;
    EXPECT_NE(resp.find("version-seven"), std::string::npos) << resp;
    EXPECT_EQ(resp.find("general-history"), std::string::npos) << resp;
}

TEST_F(ClientSessionTest, PostContentLengthBodyEchoed) {
    start_server();
    auto resp = read_response_with_timeout(port(),
        "POST /api/echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "Connection: close\r\n"
        "\r\n"
        "hello");

    EXPECT_TRUE(resp.rfind("HTTP/1.1 200", 0) == 0) << resp;
    EXPECT_NE(resp.find("\r\n\r\nhello"), std::string::npos) << resp;
}

TEST_F(ClientSessionTest, PostChunkedBodyAggregated) {
    start_server();
    auto resp = read_response_with_timeout(port(),
        "POST /api/echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n"
        "3\r\nfoo\r\n"
        "3\r\nbar\r\n"
        "0\r\n\r\n");

    EXPECT_TRUE(resp.rfind("HTTP/1.1 200", 0) == 0) << resp;
    EXPECT_NE(resp.find("\r\n\r\nfoobar"), std::string::npos) << resp;
}

TEST_F(ClientSessionTest, KeepAliveMultipleRequestsSameConnection) {
    start_server();
    asio::io_context ioc;
    tcp::socket client(ioc);
    client.connect({asio::ip::make_address("127.0.0.1"), port()});

    std::string request =
        "GET /api/ping HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    for (int i = 0; i < 3; ++i) {
        asio::write(client, asio::buffer(request));

        // Read response: status line + headers + body. Body is "pong" (4 bytes).
        std::string buf;
        asio::error_code ec;
        while (buf.find("\r\n\r\n") == std::string::npos) {
            char tmp[256];
            size_t n = client.read_some(asio::buffer(tmp), ec);
            if (ec) break;
            buf.append(tmp, n);
        }
        // Read remaining 4 bytes of body if not already in buf.
        size_t header_end = buf.find("\r\n\r\n");
        ASSERT_NE(header_end, std::string::npos);
        std::string body_part = buf.substr(header_end + 4);
        while (body_part.size() < 4) {
            char tmp[256];
            size_t n = client.read_some(asio::buffer(tmp), ec);
            if (ec) break;
            body_part.append(tmp, n);
        }
        EXPECT_EQ(body_part.substr(0, 4), "pong") << "iteration " << i;
    }

    asio::error_code ignore;
    client.close(ignore);
}

TEST_F(ClientSessionTest, ConnectionCloseHeaderTerminatesAfterResponse) {
    start_server();
    asio::io_context ioc;
    tcp::socket client(ioc);
    client.connect({asio::ip::make_address("127.0.0.1"), port()});

    std::string request =
        "GET /api/ping HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";
    asio::write(client, asio::buffer(request));

    std::string response;
    asio::error_code ec;
    asio::read(client, asio::dynamic_buffer(response), ec);
    EXPECT_TRUE(ec == asio::error::eof || !ec) << "ec=" << ec.message();

    asio::error_code ignore;
    client.close(ignore);
}

TEST_F(ClientSessionTest, Http10WithoutKeepAliveClosesConnection) {
    start_server();
    asio::io_context ioc;
    tcp::socket client(ioc);
    client.connect({asio::ip::make_address("127.0.0.1"), port()});

    std::string request =
        "GET /api/ping HTTP/1.0\r\n"
        "Host: localhost\r\n"
        "\r\n";
    asio::write(client, asio::buffer(request));

    std::string response;
    asio::error_code ec;
    asio::read(client, asio::dynamic_buffer(response), ec);
    EXPECT_TRUE(ec == asio::error::eof || !ec);
    EXPECT_NE(response.find("pong"), std::string::npos);

    asio::error_code ignore;
    client.close(ignore);
}

// ============== Error paths ==============

TEST_F(ClientSessionTest, UnknownPathReturns404) {
    start_server();
    auto resp = read_response_with_timeout(port(),
        "GET /no-such-path HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    EXPECT_TRUE(resp.rfind("HTTP/1.1 404", 0) == 0) << resp;
}

TEST_F(ClientSessionTest, DuplicateContentLengthReturns400) {
    start_server();
    auto resp = read_response_with_timeout(port(),
        "GET /api/ping HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 0\r\n"
        "Content-Length: 1\r\n"
        "Connection: close\r\n"
        "\r\n");

    EXPECT_TRUE(resp.rfind("HTTP/1.1 400", 0) == 0) << resp;
}

TEST_F(ClientSessionTest, OversizedHeaderReturns431) {
    start_server();
    std::string request =
        "GET /api/ping HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Huge: ";
    // kMaxHeaderSize = 64KB; pad past it.
    request.append(70 * 1024, 'a');
    request += "\r\nConnection: close\r\n\r\n";

    auto resp = read_response_with_timeout(port(), request);
    EXPECT_TRUE(resp.rfind("HTTP/1.1 431", 0) == 0) << resp;
}

TEST_F(ClientSessionTest, ProxyUpstreamFailureReturns502) {
    // Configure an upstream pointing at a closed port.
    HttpPool::Config pool_cfg;
    pool_cfg.connect_timeout_ms = 100;
    pool_cfg.read_timeout_ms = 200;
    pool_cfg.request_timeout_ms = 200;
    auto upstream_cfg = make_upstream_config("dead", "127.0.0.1", 1);
    server().upstreams().reload(upstream_cfg, pool_cfg);
    start_server();

    auto resp = read_response_with_timeout(port(),
        "GET /dead/path HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    EXPECT_TRUE(resp.rfind("HTTP/1.1 502", 0) == 0) << resp;
}

// ============== Gateway snake_to_camel switch (proxy happy path) ==============

TEST_F(ClientSessionTest, ProxyJsonResponseSnakeToCamelAppliedWhenEnabled) {
    const std::string body = R"({"user_name":"a","nested":{"deep_key":1}})";
    MockUpstream mock;
    auto upstream_port = mock.start(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + body);
    auto cfg = make_gateway_upstream_config("mock", "127.0.0.1", upstream_port, true);
    server().upstreams().reload(cfg, HttpPool::Config{});
    start_server();

    auto resp = read_response_with_timeout(port(),
        "GET /mock/path HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    EXPECT_TRUE(resp.rfind("HTTP/1.1 200", 0) == 0) << resp;
    EXPECT_NE(resp.find("\"userName\":\"a\""), std::string::npos) << resp;
    EXPECT_NE(resp.find("\"deepKey\":1"), std::string::npos) << resp;
    EXPECT_EQ(resp.find("user_name"), std::string::npos) << resp;
    EXPECT_EQ(resp.find("deep_key"), std::string::npos) << resp;
}

TEST_F(ClientSessionTest, ProxyJsonResponseSnakeToCamelDisabledByConfig) {
    const std::string body = R"({"user_name":"a","nested":{"deep_key":1}})";
    MockUpstream mock;
    auto upstream_port = mock.start(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + body);
    auto cfg = make_gateway_upstream_config("mock", "127.0.0.1", upstream_port, false);
    server().upstreams().reload(cfg, HttpPool::Config{});
    start_server();

    auto resp = read_response_with_timeout(port(),
        "GET /mock/path HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    EXPECT_TRUE(resp.rfind("HTTP/1.1 200", 0) == 0) << resp;
    EXPECT_NE(resp.find("\"user_name\":\"a\""), std::string::npos) << resp;
    EXPECT_NE(resp.find("\"deep_key\":1"), std::string::npos) << resp;
    EXPECT_EQ(resp.find("userName"), std::string::npos) << resp;
    EXPECT_EQ(resp.find("deepKey"), std::string::npos) << resp;
}

TEST_F(ClientSessionTest, ReloadBetweenSecurityCheckAndRouteRefreshesCorsPolicy) {
    auto temp_dir = make_temp_security_dir();
    auto old_cfg = make_security_config(
        temp_dir.path, true, "https://old.example.test");
    SecurityRules rules;
    rules.load_from_config(old_cfg);
    server().set_security_rules(&rules);

    std::mutex hook_mu;
    std::condition_variable hook_cv;
    bool first_check_entered = false;
    bool release_first_check = false;
    int hook_calls = 0;
    server().set_after_initial_security_check_for_test([&] {
        std::unique_lock lock(hook_mu);
        if (++hook_calls != 1) {
            return;
        }
        first_check_entered = true;
        hook_cv.notify_all();
        hook_cv.wait(lock, [&] { return release_first_check; });
    });
    start_server();

    std::string response;
    std::exception_ptr client_error;
    std::thread client([&] {
        try {
            response = read_response_with_timeout(port(),
                "GET /dead/path HTTP/1.1\r\n"
                "Host: localhost\r\n"
                "Origin: https://old.example.test\r\n"
                "Connection: close\r\n"
                "\r\n");
        } catch (...) {
            client_error = std::current_exception();
        }
    });

    bool reached_hook = false;
    {
        std::unique_lock lock(hook_mu);
        reached_hook = hook_cv.wait_for(lock, std::chrono::seconds(1), [&] {
            return first_check_entered;
        });
    }

    if (!reached_hook) {
        {
            std::lock_guard lock(hook_mu);
            release_first_check = true;
        }
        hook_cv.notify_all();
        client.join();
        server().set_security_rules(nullptr);
        FAIL() << "request did not reach post-security-check hook";
        return;
    }

    auto new_cfg = make_security_config(
        temp_dir.path, true, "https://new.example.test", true);
    HttpPool::Config pool_cfg;
    pool_cfg.connect_timeout_ms = 100;
    auto prepared_security = rules.prepare_reload(new_cfg);
    auto prepared_upstreams = server().upstreams().prepare_reload(new_cfg, pool_cfg);
    rules.publish_reload(std::move(prepared_security));
    server().upstreams().publish_reload(std::move(prepared_upstreams));

    {
        std::lock_guard lock(hook_mu);
        release_first_check = true;
    }
    hook_cv.notify_all();
    client.join();
    server().set_security_rules(nullptr);

    ASSERT_EQ(client_error, nullptr);
    EXPECT_TRUE(response.rfind("HTTP/1.1 502", 0) == 0) << response;
    EXPECT_EQ(response.find("Access-Control-Allow-Origin:"), std::string::npos)
        << response;
    EXPECT_EQ(response.find("https://old.example.test"), std::string::npos) << response;
    EXPECT_EQ(response.find("https://new.example.test"), std::string::npos) << response;
}

}  // namespace
