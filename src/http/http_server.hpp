#pragma once
#include <asio.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <atomic>
#include <memory>
#include <variant>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <limits>
#include <cctype>
#include <cmath>
#include <regex>
#include "../../picohttpparser.h"
#include "../common/logger.hpp"
#include "response.hpp"
#include "http_context.hpp"
#include "http_pool.hpp"
#include "upstream_manager.hpp"
#include "json_transform.hpp"
#include "http_protocol.hpp"
#include "http_io.hpp"
#include "http_body_reader.hpp"
#include "response_builder.hpp"
#include "proxy_forwarder.hpp"
#include "client_session.hpp"
#include "../security/security_rules.hpp"

using namespace std::chrono_literals;

class HttpServer {
public:
    HttpServer(asio::io_context& ioc, unsigned short port)
        : ioc_(ioc), acceptor_(ioc, {asio::ip::tcp::v4(), port}),
          state_(std::make_shared<HttpServerState>(ioc)) {}

    void route(const std::string& path, Handler handler) {
        state_->routes[path] = std::move(handler);
    }

    UpstreamManager& upstreams() { return state_->upstreams; }

    unsigned short port() const { return acceptor_.local_endpoint().port(); }

    // Inject security rules
    void set_security_rules(SecurityRules* rules) { state_->security_rules = rules; }

    void stop() {
        if (!state_->running.exchange(false)) return;
        std::error_code ec;
        acceptor_.cancel(ec);
        acceptor_.close(ec);
    }

    asio::awaitable<void> start() {
        LOG_INFO("HTTP server listening on port ", acceptor_.local_endpoint().port());
        while (state_->running) {
            try {
                auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
                if (!state_->running) break;
                auto session = std::make_shared<ClientSession>(state_);
                co_spawn(ioc_, session->run(std::move(socket)),
                    [session](std::exception_ptr ep) {
                        if (!ep) return;
                        try {
                            std::rethrow_exception(ep);
                        } catch (const std::exception& e) {
                            LOG_WARN("Client session failed: ", e.what());
                        }
                    });
            } catch (const std::exception& e) {
                if (state_->running) LOG_ERROR("Accept error: ", e.what());
            }
        }
        LOG_INFO("HTTP server stopped");
    }

private:
    asio::io_context& ioc_;
    asio::ip::tcp::acceptor acceptor_;
    std::shared_ptr<HttpServerState> state_;
};
