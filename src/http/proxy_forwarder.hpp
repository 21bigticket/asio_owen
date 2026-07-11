#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <asio.hpp>

#include "../common/logger.hpp"
#include "http_body_reader.hpp"
#include "http_context.hpp"
#include "http_io.hpp"
#include "http_pool.hpp"
#include "http_protocol.hpp"
#include "upstream_manager.hpp"

struct ProxyResponse {
    int status_code = 502;
    std::string status_text;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string error;
};

// ConnGuard: RAII connection return, auto-mark bad on exception paths.
// Holds a shared_ptr to HttpPool to keep the pool alive during hot-reload.
struct ConnGuard {
    std::shared_ptr<HttpPool> pool_holder_;
    std::shared_ptr<HttpPool::State> pool_state_;
    std::unique_ptr<HttpPool::HttpConn> conn_;
    bool good_ = true;

    ConnGuard(std::shared_ptr<HttpPool> pool, std::unique_ptr<HttpPool::HttpConn> conn)
        : pool_holder_(std::move(pool)), pool_state_(pool_holder_->state()), conn_(std::move(conn)) {}

    ConnGuard(const ConnGuard&) = delete;
    ConnGuard& operator=(const ConnGuard&) = delete;
    ConnGuard(ConnGuard&&) = delete;
    ConnGuard& operator=(ConnGuard&&) = delete;

    ~ConnGuard() {
        if (!conn_) return;
        HttpPool::untrack_active(pool_state_, conn_.get());
        if (good_) {
            HttpPool::release(pool_state_, std::move(conn_));
        } else {
            HttpPool::release_bad(pool_state_, std::move(conn_));
        }
    }

    void set_bad() { good_ = false; }
    HttpPool::HttpConn& conn() { return *conn_; }
};

inline std::string build_proxy_request(
    const std::string& method,
    const std::string& upstream_path,
    const UpstreamManager::UpstreamConfig& cfg,
    const HttpContext& ctx,
    const HeaderParseState& request_header_state,
    bool send_keep_alive_header = false) {
    std::string forward_req = method + " " + upstream_path + " HTTP/1.1\r\n";
    forward_req += "Host: " + cfg.host + ":" + std::to_string(cfg.port) + "\r\n";

    // Expect is consumed at the gateway boundary. We deliberately do not implement
    // 100-continue, so forwarding it would make upstream wait on a handshake we own.
    std::vector<std::string> filtered = {
        "connection", "proxy-connection", "proxy-authenticate",
        "proxy-authorization", "te", "trailer",
        "transfer-encoding", "upgrade",
        "host", "accept-encoding", "expect"
    };
    add_connection_tokens(ctx.headers, filtered);

    bool forwarding_transfer_encoding = false;
    for (auto& [k, v] : ctx.headers) {
        // Reject headers with CR, LF, or NUL to prevent HTTP request smuggling.
        for (char c : v) {
            if (c == '\r' || c == '\n' || c == '\0') {
                LOG_WARN("Rejecting request with control char in header ", k,
                    " from path ", ctx.path);
                return "";  // caller must treat empty request as error
            }
        }
        if (header_iequals(k, "transfer-encoding")) {
            forwarding_transfer_encoding = true;
            break;
        }
    }

    if (request_header_state.is_chunked) {
        forward_req += "Content-Length: " + std::to_string(ctx.body.size()) + "\r\n";
    }
    for (auto& [k, v] : ctx.headers) {
        if (header_iequals(k, "transfer-encoding") && request_header_state.is_chunked) {
            continue;
        }
        if (header_iequals(k, "transfer-encoding")) {
            forward_req += k + ": " + v + "\r\n";
            continue;
        }
        if (forwarding_transfer_encoding && header_iequals(k, "content-length")) {
            continue;
        }
        if (contains_header_name(filtered, k)) {
            continue;
        }
        forward_req += k + ": " + v + "\r\n";
    }

    if (send_keep_alive_header) {
        forward_req += "Connection: keep-alive\r\n";
    }
    forward_req += "\r\n";
    forward_req += ctx.body;
    return forward_req;
}

inline asio::awaitable<ProxyResponse> read_proxy_response(
    HttpPool::HttpConn& conn, const HttpPool::Config& pool_cfg, bool request_is_head) {

    ProxyResponse resp;

    std::string buf = std::move(conn.read_buffer);

    while (buf.find("\r\n\r\n") == std::string::npos) {
        if (buf.size() > kMaxHeaderSize) {
            resp.error = "upstream_response_header_too_large";
            conn.connection_close = true;
            co_return resp;
        }
        char tmp[kHttpIoBufferSize];
        auto read = co_await read_with_timeout(conn.socket, tmp, sizeof(tmp),
            std::chrono::milliseconds(pool_cfg.read_timeout_ms));
        if (!read.ok()) {
            LOG_INFO("Proxy response header read failed: status=", io_status_name(read.status),
                ", error=", read.ec.message());
            resp.error = "upstream_response_header_read_failed";
            conn.connection_close = true;
            co_return resp;
        }
        buf.append(tmp, read.bytes);
    }

    auto header_end = buf.find("\r\n\r\n");
    std::string header_part = buf.substr(0, header_end);
    std::string body_rest = buf.substr(header_end + 4);

    auto first_line_end = header_part.find("\r\n");
    if (first_line_end == std::string::npos) {
        resp.error = "upstream_response_missing_status_line";
        conn.connection_close = true;
        co_return resp;
    }
    auto status_line = header_part.substr(0, first_line_end);
    auto sp1 = status_line.find(' ');
    auto sp2 = status_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        LOG_INFO("Proxy response invalid status line: ", sanitize_body_preview(status_line));
        resp.error = "upstream_response_invalid_status_line";
        conn.connection_close = true;
        co_return resp;
    }
    auto status_code = parse_decimal_size(status_line.substr(sp1 + 1, sp2 - sp1 - 1));
    if (!status_code || *status_code > 999) {
        LOG_INFO("Proxy response invalid status code: status_line=",
            sanitize_body_preview(status_line));
        resp.error = "upstream_response_invalid_status_code";
        conn.connection_close = true;
        co_return resp;
    }
    resp.status_code = static_cast<int>(*status_code);
    resp.status_text = status_line.substr(sp2 + 1);

    int upstream_minor_version = status_line.rfind("HTTP/1.0", 0) == 0 ? 0 : 1;
    HeaderParseState header_state;
    auto hdr = header_part.substr(first_line_end + 2, header_end - first_line_end - 2);
    parse_header_fields(hdr, resp.headers, header_state);
    LOG_DEBUG("Proxy response header parsed: status_line=", status_line,
        ", status=", resp.status_code,
        ", headers=[", describe_headers(resp.headers), "]",
        ", invalid_cl=", header_state.invalid_content_length,
        ", duplicate_cl=", header_state.duplicate_content_length,
        ", has_te=", header_state.has_transfer_encoding,
        ", is_chunked=", header_state.is_chunked,
        ", content_length=",
        (header_state.content_length ? std::to_string(*header_state.content_length) : "none"),
        ", body_rest=", body_rest.size());
    if (header_state.invalid_content_length || header_state.duplicate_content_length ||
        (header_state.has_transfer_encoding && !header_state.is_chunked)) {
        LOG_INFO("Proxy response framing invalid: status=", resp.status_code,
            ", upstream body preview=", sanitize_body_preview(body_rest));
        resp.error = "upstream_response_invalid_framing";
        conn.connection_close = true;
        co_return resp;
    }

    conn.connection_close = header_state.connection_close ||
        (upstream_minor_version == 0 && !header_state.connection_keep_alive);

    bool response_has_no_body = request_is_head || resp.status_code == 204 ||
        resp.status_code == 304 || (resp.status_code >= 100 && resp.status_code < 200);
    if (response_has_no_body) {
        resp.body.clear();
        if (!body_rest.empty()) {
            LOG_INFO("Proxy response expected no body but received preread bytes: status=",
                resp.status_code, ", bytes=", body_rest.size());
            conn.connection_close = true;
        }
        co_return resp;
    }

    if (header_state.is_chunked) {
        auto body_result = co_await read_chunked_body(conn.socket, body_rest,
            pool_cfg.max_body_size, std::chrono::milliseconds(pool_cfg.read_timeout_ms));
        if (!body_result.ok()) {
            LOG_INFO("Proxy response chunked read failed: status=", resp.status_code,
                ", body_rest=", body_rest.size(),
                ", read_status=", static_cast<int>(body_result.status),
                ", error=", body_result.ec.message());
            resp.error = body_result.status == BodyReadStatus::TooLarge ?
                "upstream_response_body_too_large" : "upstream_response_chunked_read_failed";
            conn.connection_close = true;
            co_return resp;
        }
        resp.body = std::move(body_result.body);
        conn.read_buffer = std::move(body_rest);
    } else if (header_state.content_length) {
        size_t content_length = *header_state.content_length;
        auto body_result = co_await read_content_length_body(conn.socket, body_rest,
            content_length, pool_cfg.max_body_size,
            std::chrono::milliseconds(pool_cfg.read_timeout_ms));
        if (!body_result.ok()) {
            LOG_INFO("Proxy response content-length read failed: status=", resp.status_code,
                ", content_length=", content_length,
                ", max=", pool_cfg.max_body_size,
                ", read_status=", static_cast<int>(body_result.status),
                ", error=", body_result.ec.message());
            resp.error = body_result.status == BodyReadStatus::TooLarge ?
                "upstream_response_body_too_large" : "upstream_response_body_read_failed";
            resp.body.clear();
            conn.connection_close = true;
            co_return resp;
        }
        resp.body = std::move(body_result.body);
        conn.read_buffer = std::move(body_rest);
    } else {
        conn.connection_close = true;
        resp.body = std::move(body_rest);
        if (resp.body.size() > pool_cfg.max_body_size) {
            LOG_INFO("Proxy response eof-framed body too large before read: status=", resp.status_code,
                ", size=", resp.body.size(),
                ", max=", pool_cfg.max_body_size);
            resp.error = "upstream_response_body_too_large";
            resp.body.clear();
            co_return resp;
        }
        char tmp[kHttpIoBufferSize];
        while (true) {
            auto read = co_await read_with_timeout(conn.socket, tmp, sizeof(tmp),
                std::chrono::milliseconds(pool_cfg.read_timeout_ms));
            if (!read.ok()) {
                if (read.status != IoStatus::PeerClosed) {
                    LOG_INFO("Proxy response eof-framed read stopped: status=",
                        io_status_name(read.status), ", error=", read.ec.message());
                    resp.error = "upstream_response_body_read_failed";
                    resp.body.clear();
                    conn.connection_close = true;
                    co_return resp;
                }
                break;
            }
            resp.body.append(tmp, read.bytes);
            if (resp.body.size() > pool_cfg.max_body_size) {
                LOG_INFO("Proxy response eof-framed body too large: status=", resp.status_code,
                    ", size=", resp.body.size(),
                    ", max=", pool_cfg.max_body_size);
                resp.error = "upstream_response_body_too_large";
                resp.body.clear();
                conn.connection_close = true;
                co_return resp;
            }
        }
    }

    LOG_DEBUG("Proxy response body ready: status=", resp.status_code,
        ", body_size=", resp.body.size(),
        ", connection_close=", conn.connection_close,
        ", read_buffer=", conn.read_buffer.size(),
        ", body_preview=", sanitize_body_preview(resp.body));
    co_return resp;
}
