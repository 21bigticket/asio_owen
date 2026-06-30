#pragma once
#include <asio.hpp>
#include <string>
#include <unordered_map>
#include <functional>
#include <atomic>
#include "../../picohttpparser.h"
#include "../common/logger.hpp"
#include "response.hpp"

class HttpServer {
public:
    using Handler = std::function<asio::awaitable<std::string>(const std::string& body, const std::string& path)>;

    HttpServer(asio::io_context& ioc, unsigned short port)
        : ioc_(ioc), acceptor_(ioc, {asio::ip::tcp::v4(), port}), running_(true) {}

    void route(const std::string& path, Handler handler) {
        routes_[path] = std::move(handler);
    }

    void stop() {
        running_ = false;
        acceptor_.cancel();
    }

    asio::awaitable<void> start() {
        LOG_INFO("HTTP server listening on port ", acceptor_.local_endpoint().port());
        while (running_) {
            try {
                auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
                if (!running_) break;
                co_spawn(ioc_, handle_connection(std::move(socket)), asio::detached);
            } catch (const std::exception& e) {
                if (running_) LOG_ERROR("Accept error: ", e.what());
            }
        }
        LOG_INFO("HTTP server stopped");
    }

private:
    asio::awaitable<void> handle_connection(asio::ip::tcp::socket socket) {
        try {
            char buf[8192];
            // 复用连接，Keep-Alive 循环处理多个请求
            while (running_) {
                std::size_t n = co_await socket.async_read_some(asio::buffer(buf), asio::use_awaitable);

                const char* method, *path;
                int minor_version;
                struct phr_header headers[32];
                size_t num_headers = 32;

                size_t method_len, path_len;
                int pret = phr_parse_request(buf, n, &method, &method_len, &path, &path_len,
                    &minor_version, headers, &num_headers, 0);

                if (pret < 0) {
                    break;
                }

                std::string path_str(path, path_len);
                std::string body(buf + pret, n - pret);

                std::string resp_body;
                auto it = routes_.find(path_str);
                if (it != routes_.end()) {
                    resp_body = co_await it->second(body, path_str);
                } else {
                    resp_body = resp_err(NOT_FOUND, "not found");
                }

                std::string resp = "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: " + std::to_string(resp_body.size()) +
                    "\r\n\r\n" + resp_body;

                co_await asio::async_write(socket, asio::buffer(resp), asio::use_awaitable);
            }
        } catch (const std::exception& e) {
            LOG_WARN("Connection error: ", e.what());
        }
    }

    asio::io_context& ioc_;
    asio::ip::tcp::acceptor acceptor_;
    std::unordered_map<std::string, Handler> routes_;
    std::atomic<bool> running_;
};
