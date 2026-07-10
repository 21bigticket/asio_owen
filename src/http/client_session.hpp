#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>

#include <asio.hpp>

#include "../../picohttpparser.h"
#include "../common/logger.hpp"
#include "../security/security_rules.hpp"
#include "http_body_reader.hpp"
#include "http_context.hpp"
#include "http_io.hpp"
#include "http_protocol.hpp"
#include "json_transform.hpp"
#include "proxy_forwarder.hpp"
#include "response_builder.hpp"
#include "upstream_manager.hpp"

using namespace std::chrono_literals;

struct HttpServerState {
    explicit HttpServerState(asio::io_context& ioc) : upstreams(ioc) {}

    std::unordered_map<std::string, Handler> routes;
    std::atomic<bool> running{true};
    UpstreamManager upstreams;
    SecurityRules* security_rules = nullptr;
};

class ClientSession {
public:
    explicit ClientSession(std::shared_ptr<HttpServerState> state)
        : state_(std::move(state)) {}

    asio::awaitable<void> run(asio::ip::tcp::socket socket) {
        try {
            char buf[kClientReadBufferSize];
            std::string client_preread;
            auto client_timeout = 30s;

            while (state_->running) {
                while (client_preread.find("\r\n\r\n") == std::string::npos) {
                    auto read = co_await read_with_timeout(socket, buf, sizeof(buf), client_timeout);
                    if (!read.ok()) {
                        if (client_preread.empty()) {
                            LOG_DEBUG("Client read stopped before sending data: status=",
                                io_status_name(read.status), ", error=", read.ec.message());
                        } else {
                            LOG_WARN("Client read failed mid-header: status=",
                                io_status_name(read.status),
                                ", error=", read.ec.message(),
                                ", preread=", client_preread.size());
                        }
                        co_return;
                    }
                    client_preread.append(buf, read.bytes);
                    if (client_preread.size() > kMaxHeaderSize) {
                        LOG_WARN("Client request header too large");
                        co_await write_simple_error(socket, 431, "Request Header Fields Too Large",
                            "{\"code\":431,\"msg\":\"request header too large\"}");
                        co_return;
                    }
                }

                const char* method, *path;
                int minor_version;
                struct phr_header headers[128];
                size_t num_headers = 128;

                size_t method_len, path_len;
                int pret = phr_parse_request(client_preread.data(), client_preread.size(),
                    &method, &method_len, &path, &path_len,
                    &minor_version, headers, &num_headers, 0);

                if (pret < 0) {
                    LOG_WARN("Client request parse failed: pret=", pret,
                        ", bytes=", client_preread.size(),
                        ", preview=", sanitize_body_preview(client_preread));
                    co_await write_simple_error(socket, 400, "Bad Request",
                        "{\"code\":400,\"msg\":\"bad request\"}");
                    co_return;
                }
                std::string body_buffer = client_preread.substr(pret);

                std::string path_str(path, path_len);
                std::string method_str(method, method_len);

                HttpContext ctx;
                ctx.method = method_str;
                ctx.path = path_str;

                HeaderParseState request_header_state;
                for (size_t i = 0; i < num_headers; ++i) {
                    std::string_view name(headers[i].name, headers[i].name_len);
                    std::string_view value(headers[i].value, headers[i].value_len);
                    update_header_state(name, value, request_header_state);
                    ctx.headers.emplace_back(std::string(name), std::string(value));
                }
                client_preread.clear();

                LOG_DEBUG("Client request parsed: method=", method_str,
                    ", path=", path_str,
                    ", http_minor=", minor_version,
                    ", header_count=", ctx.headers.size(),
                    ", initial_body_buffer=", body_buffer.size(),
                    ", headers=[", describe_headers(ctx.headers), "]");

                bool handled = false;
                bool proxy_response = false;

                std::string& preread = body_buffer;
                if (request_header_state.invalid_content_length ||
                    request_header_state.duplicate_content_length ||
                    (request_header_state.has_transfer_encoding && !request_header_state.is_chunked)) {
                    LOG_INFO("Reject request framing: method=", method_str,
                        ", path=", path_str,
                        ", invalid_cl=", request_header_state.invalid_content_length,
                        ", duplicate_cl=", request_header_state.duplicate_content_length,
                        ", has_te=", request_header_state.has_transfer_encoding,
                        ", is_chunked=", request_header_state.is_chunked,
                        ", headers=[", describe_headers(ctx.headers), "]");
                    ctx.status_code = 400;
                    ctx.response_body = "{\"code\":400,\"msg\":\"invalid request framing\"}";
                    handled = true;
                } else if (request_header_state.is_chunked) {
                    auto body_result = co_await read_chunked_body(socket, preread, kMaxBodySize, client_timeout);
                    if (!body_result.ok()) {
                        LOG_INFO("Reject invalid chunked body: method=", method_str,
                            ", path=", path_str,
                            ", status=", static_cast<int>(body_result.status),
                            ", error=", body_result.ec.message(),
                            ", headers=[", describe_headers(ctx.headers), "]");
                        if (body_result.status == BodyReadStatus::TooLarge) {
                            ctx.status_code = 413;
                            ctx.response_body = "{\"code\":413,\"msg\":\"body too large\"}";
                        } else if (body_result.status == BodyReadStatus::Timeout) {
                            ctx.status_code = 408;
                            ctx.response_body = "{\"code\":408,\"msg\":\"request timeout\"}";
                        } else {
                            ctx.status_code = 400;
                            ctx.response_body = "{\"code\":400,\"msg\":\"invalid chunked body\"}";
                        }
                        handled = true;
                    } else {
                        ctx.body = std::move(body_result.body);
                    }
                    client_preread = std::move(preread);
                } else if (request_header_state.content_length) {
                    size_t content_length = *request_header_state.content_length;
                    auto body_result = co_await read_content_length_body(
                        socket, preread, content_length, kMaxBodySize, client_timeout);
                    if (!body_result.ok()) {
                        LOG_INFO("Reject request body read failed: method=", method_str,
                            ", path=", path_str,
                            ", content_length=", content_length,
                            ", status=", static_cast<int>(body_result.status),
                            ", error=", body_result.ec.message());
                        if (body_result.status == BodyReadStatus::TooLarge) {
                            ctx.status_code = 413;
                            ctx.response_body = "{\"code\":413,\"msg\":\"body too large\"}";
                        } else if (body_result.status == BodyReadStatus::Timeout) {
                            ctx.status_code = 408;
                            ctx.response_body = "{\"code\":408,\"msg\":\"request timeout\"}";
                        } else {
                            ctx.status_code = 400;
                            ctx.response_body = "{\"code\":400,\"msg\":\"invalid request body\"}";
                        }
                        handled = true;
                    } else {
                        ctx.body = std::move(body_result.body);
                    }
                    client_preread = std::move(preread);
                } else {
                    ctx.body.clear();
                    client_preread = std::move(preread);
                }

                LOG_DEBUG("Client request body ready: method=", method_str,
                    ", path=", path_str,
                    ", body_size=", ctx.body.size(),
                    ", next_preread=", client_preread.size(),
                    ", body_preview=", sanitize_body_preview(ctx.body));

                if (!handled && state_->security_rules) {
                    std::string xff = ctx.get_header("X-Forwarded-For");
                    std::string auth = ctx.get_header("Authorization");
                    auto result = state_->security_rules->check(
                        socket, method_str, path_str, xff, auth);
                    if (result.status_code != 0) {
                        ctx.status_code = result.status_code;
                        ctx.response_body = "{\"code\":"
                            + std::to_string(result.status_code)
                            + ",\"msg\":\"" + result.reason + "\"}";
                        if (result.status_code == 429 && result.retry_after_ms > 0) {
                            int retry_sec = static_cast<int>(
                                std::ceil(result.retry_after_ms / 1000.0));
                            ctx.response_headers.emplace_back(
                                "Retry-After", std::to_string(retry_sec));
                        }
                        handled = true;
                        {
                            static std::atomic<int64_t> last_log_ms{0};
                            int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
                            int64_t last = last_log_ms.load(std::memory_order_relaxed);
                            if (now - last > 30000 &&
                                last_log_ms.compare_exchange_strong(last, now)) {
                                LOG_WARN("Security check rejected: method=", method_str,
                                    ", path=", path_str,
                                    ", status=", result.status_code,
                                    ", reason=", result.reason);
                            }
                        }
                    }
                }

                auto it = state_->routes.find(path_str);
                if (!handled && it != state_->routes.end()) {
                    co_await it->second(ctx);
                    handled = true;
                }

                if (!handled) {
                    auto upstream = state_->upstreams.route(path_str);
                    if (upstream) {
                        const auto& cfg = upstream->config;
                        auto pool = upstream->pool;
                        LOG_DEBUG("Proxy route matched: method=", method_str,
                            ", path=", path_str,
                            ", upstream_path=", upstream->upstream_path,
                            ", upstream=", cfg.host, ":", cfg.port,
                            ", body_size=", ctx.body.size(),
                            ", headers=[", describe_headers(ctx.headers), "]");
                        try {
                            for (int attempt = 0; attempt < 2 && !handled; ++attempt) {
                                auto conn_opt = co_await pool->acquire(cfg.host, cfg.port);
                                if (!conn_opt) {
                                    ctx.status_code = 503;
                                    ctx.response_body = "{\"code\":503,\"msg\":\"no available connection\"}";
                                    handled = true;
                                    break;
                                }

                                ConnGuard guard(pool, std::move(conn_opt));
                                auto& conn = guard.conn();
                                bool can_retry_stale_idle = conn.reused_from_idle && attempt == 0;

                                LOG_DEBUG("Proxy request: method=", method_str,
                                    ", path=", path_str,
                                    ", upstream_path=", upstream->upstream_path,
                                    ", upstream=", cfg.host, ":", cfg.port,
                                    ", body_size=", ctx.body.size());

                                auto forward_req = build_proxy_request(
                                    method_str, upstream->upstream_path, cfg, ctx, request_header_state,
                                    pool->cfg().send_keep_alive_header);
                                LOG_DEBUG("Proxy upstream request built: upstream_path=",
                                    upstream->upstream_path,
                                    ", bytes=", forward_req.size(),
                                    ", body_size=", ctx.body.size(),
                                    ", body_preview=", sanitize_body_preview(ctx.body));

                                auto write_result = co_await write_with_timeout(
                                    conn.socket, forward_req,
                                    std::chrono::milliseconds(pool->cfg().request_timeout_ms));

                                if (!write_result.ok()) {
                                    asio::error_code ec;
                                    conn.socket.cancel(ec);
                                    guard.set_bad();
                                    LOG_WARN("Proxy upstream write failed: method=", method_str,
                                        ", upstream_path=", upstream->upstream_path,
                                        ", upstream=", cfg.host, ":", cfg.port,
                                        ", reused=", conn.reused_from_idle,
                                        ", attempt=", attempt + 1,
                                        ", status=", io_status_name(write_result.status),
                                        ", error=", write_result.ec.message(),
                                        ", pool_stats={", pool->stats(), "}");
                                    if (can_retry_stale_idle) {
                                        LOG_INFO("Proxy retry after stale idle write failure: method=", method_str,
                                            ", upstream_path=", upstream->upstream_path,
                                            ", pool_stats={", pool->stats(), "}");
                                        continue;
                                    }
                                    ctx.status_code = 504;
                                    ctx.response_body = "{\"code\":504,\"msg\":\"upstream write failed\"}";
                                    handled = true;
                                } else {
                                    auto proxy_resp = co_await read_proxy_response(
                                        conn, pool->cfg(), method_str == "HEAD");
                                    bool response_failed = !proxy_resp.error.empty();
                                    if (response_failed) {
                                        LOG_INFO("Proxy upstream response failed: reason=", proxy_resp.error,
                                            ", method=", method_str,
                                            ", upstream_path=", upstream->upstream_path,
                                            ", reused=", conn.reused_from_idle,
                                            ", attempt=", attempt + 1,
                                            ", pool_stats={", pool->stats(), "}");
                                    }
                                    if (response_failed && can_retry_stale_idle) {
                                        guard.set_bad();
                                        LOG_INFO("Proxy retry after stale idle read failure: method=", method_str,
                                            ", upstream_path=", upstream->upstream_path,
                                            ", pool_stats={", pool->stats(), "}");
                                        continue;
                                    }
                                    if (response_failed) {
                                        guard.set_bad();
                                        ctx.status_code = 502;
                                        ctx.response_status_text = "Bad Gateway";
                                        ctx.response_body = "{\"code\":502,\"msg\":\"Bad Gateway\"}";
                                        ctx.response_headers.clear();
                                        proxy_response = true;
                                        handled = true;
                                        continue;
                                    }
                                    ctx.status_code = proxy_resp.status_code;
                                    ctx.response_status_text = std::move(proxy_resp.status_text);
                                    ctx.response_body = std::move(proxy_resp.body);
                                    ctx.response_headers = std::move(proxy_resp.headers);
                                    proxy_response = true;

                                    if (!ctx.response_body.empty()) {
                                        auto ct = get_header_value(ctx.response_headers, "content-type");
                                        if (!ct.empty() && ct.find("application/json") != std::string::npos) {
                                            json_keys_snake_to_camel(ctx.response_body);
                                        }
                                    }

                                    LOG_DEBUG("Proxy forwarded: method=", method_str,
                                        ", path=", path_str,
                                        ", upstream_path=", upstream->upstream_path,
                                        ", upstream=", cfg.host, ":", cfg.port,
                                        ", status=", ctx.status_code,
                                        ", request_body_size=", ctx.body.size(),
                                        ", response_body_size=", ctx.response_body.size());

                                    if (conn.connection_close) guard.set_bad();
                                    handled = true;
                                }
                            }
                        } catch (const std::exception& e) {
                            LOG_WARN("Proxy forward error: ", e.what(),
                                ", pool_stats={", pool->stats(), "}");
                            ctx.status_code = 502;
                            ctx.response_body = "{\"code\":502,\"msg\":\"Bad Gateway\"}";
                            handled = true;
                        }
                    }
                }

                if (!handled) {
                    ctx.status_code = 404;
                    ctx.response_body = "{\"code\":404,\"msg\":\"not found\"}";
                }

                if (ctx.status_code >= 400) {
                    LOG_WARN("Client error response: method=", method_str,
                        ", path=", path_str,
                        ", status=", ctx.status_code,
                        ", proxy_response=", proxy_response,
                        ", response_body_size=", ctx.response_body.size(),
                        ", body_preview=", sanitize_body_preview(ctx.response_body));
                }

                auto resp = build_downstream_response(ctx, method_str, proxy_response);

                asio::error_code write_ec;
                co_await asio::async_write(socket, asio::buffer(resp),
                    asio::redirect_error(asio::use_awaitable, write_ec));
                if (write_ec) {
                    LOG_WARN("Client response write failed: method=", method_str,
                        ", path=", path_str,
                        ", status=", ctx.status_code,
                        ", error=", write_ec.message(),
                        ", response_bytes=", resp.size());
                    co_return;
                }

                if (ctx.status_code == 400 || ctx.status_code == 408 || ctx.status_code == 413) {
                    co_return;
                }

                auto client_conn = ctx.get_header("Connection");
                auto client_conn_tokens = split_connection_tokens(client_conn);
                if (minor_version == 0 && !client_conn_tokens.keep_alive) {
                    break;
                }
                if (client_conn_tokens.close) break;
            }
        } catch (const std::system_error& e) {
            LOG_WARN("Connection system_error: ", e.what());
            std::string body = "{\"code\":500,\"msg\":\"connection error\"}";
            auto resp = build_error_response(500, "Internal Server Error", body);
            asio::error_code ec;
            asio::write(socket, asio::buffer(resp), ec);
        } catch (const std::exception& e) {
            LOG_WARN("Connection error: ", e.what());
            std::string body = "{\"code\":500,\"msg\":\"internal server error\"}";
            auto resp = build_error_response(500, "Internal Server Error", body);
            asio::error_code ec;
            asio::write(socket, asio::buffer(resp), ec);
        }
    }

private:
    asio::awaitable<void> write_simple_error(
        asio::ip::tcp::socket& socket, int status, std::string_view reason,
        std::string_view body) {
        auto resp = build_error_response(status, reason, body);
        asio::error_code ec;
        co_await asio::async_write(
            socket, asio::buffer(resp), asio::redirect_error(asio::use_awaitable, ec));
    }

    std::shared_ptr<HttpServerState> state_;
};
