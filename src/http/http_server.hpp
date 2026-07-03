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
#include "../../picohttpparser.h"
#include "../common/logger.hpp"
#include "response.hpp"
#include "http_context.hpp"
#include "http_pool.hpp"
#include "upstream_manager.hpp"

using namespace std::chrono_literals;

// 转发响应：保存下游返回的 headers，最终透传给客户端
struct ProxyResponse {
    int status_code;
    std::string status_text;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct HeaderParseState {
    std::optional<size_t> content_length;
    bool duplicate_content_length = false;
    bool invalid_content_length = false;
    bool is_chunked = false;
    bool has_transfer_encoding = false;
    bool connection_close = false;
    bool connection_keep_alive = false;
};

struct HeaderTokens {
    bool close = false;
    bool keep_alive = false;
};

// HttpServer
// 10MB body 上限
static constexpr size_t kMaxBodySize = 10 * 1024 * 1024;

class HttpServer {
public:
    HttpServer(asio::io_context& ioc, unsigned short port)
        : ioc_(ioc), acceptor_(ioc, {asio::ip::tcp::v4(), port}), running_(true),
          upman_(ioc) {}

    void route(const std::string& path, Handler handler) {
        routes_[path] = std::move(handler);
    }

    UpstreamManager& upstreams() { return upman_; }

    void stop() {
        if (!running_.exchange(false)) return;
        std::error_code ec;
        acceptor_.cancel(ec);
        acceptor_.close(ec);
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
    // ConnGuard：RAII 归还连接，异常路径自动标记 bad
    struct ConnGuard {
        std::shared_ptr<HttpPool::State> pool_state_;
        std::unique_ptr<HttpPool::HttpConn> conn_;
        bool good_ = true;

        ConnGuard(HttpPool* pool, std::unique_ptr<HttpPool::HttpConn> conn)
            : pool_state_(pool->state()), conn_(std::move(conn)) {}

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

    // 带超时的 async_read_some；超时返回 0
    asio::awaitable<std::size_t> read_with_timeout(
        asio::ip::tcp::socket& sock,
        char* buf, std::size_t size, std::chrono::milliseconds timeout) {
        auto ex = co_await asio::this_coro::executor;
        auto timed_out = std::make_shared<bool>(false);
        asio::steady_timer timer(ex);
        timer.expires_after(timeout);
        timer.async_wait([timed_out, &sock](asio::error_code ec) {
            if (!ec) {
                *timed_out = true;
                asio::error_code ignored;
                sock.cancel(ignored);
            }
        });

        asio::error_code ec;
        auto n = co_await sock.async_read_some(
            asio::buffer(buf, size), asio::redirect_error(asio::use_awaitable, ec));
        timer.cancel();
        if (*timed_out || ec) {
            co_return 0;
        }
        co_return n;
    }

    asio::awaitable<bool> write_with_timeout(
        asio::ip::tcp::socket& sock, const std::string& data, std::chrono::milliseconds timeout) {
        auto ex = co_await asio::this_coro::executor;
        auto timed_out = std::make_shared<bool>(false);
        asio::steady_timer timer(ex);
        timer.expires_after(timeout);
        timer.async_wait([timed_out, &sock](asio::error_code ec) {
            if (!ec) {
                *timed_out = true;
                asio::error_code ignored;
                sock.cancel(ignored);
            }
        });

        asio::error_code ec;
        co_await asio::async_write(
            sock, asio::buffer(data), asio::redirect_error(asio::use_awaitable, ec));
        timer.cancel();
        co_return !*timed_out && !ec;
    }

    static std::optional<size_t> parse_hex_size_line(const std::string& line) {
        auto end = line.find(';');
        std::string_view digits(line.data(), end == std::string::npos ? line.size() : end);
        while (!digits.empty() && std::isspace(static_cast<unsigned char>(digits.front()))) {
            digits.remove_prefix(1);
        }
        while (!digits.empty() && std::isspace(static_cast<unsigned char>(digits.back()))) {
            digits.remove_suffix(1);
        }
        if (digits.empty()) return std::nullopt;

        size_t value = 0;
        for (char c : digits) {
            unsigned int d = 0;
            if (c >= '0' && c <= '9') d = static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<unsigned int>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<unsigned int>(c - 'A' + 10);
            else return std::nullopt;
            if (value > (std::numeric_limits<size_t>::max() - d) / 16) {
                return std::nullopt;
            }
            value = value * 16 + d;
        }
        return value;
    }

    static std::string trim_copy(std::string_view s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
            s.remove_prefix(1);
        }
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
            s.remove_suffix(1);
        }
        return std::string(s);
    }

    static std::optional<size_t> parse_decimal_size(std::string_view s) {
        s = std::string_view(trim_copy(s));
        if (s.empty()) return std::nullopt;
        size_t value = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return std::nullopt;
            size_t d = static_cast<size_t>(c - '0');
            if (value > (std::numeric_limits<size_t>::max() - d) / 10) {
                return std::nullopt;
            }
            value = value * 10 + d;
        }
        return value;
    }

    static HeaderTokens split_connection_tokens(std::string_view value, std::unordered_set<std::string>* out = nullptr) {
        HeaderTokens tokens;
        size_t start = 0;
        while (start <= value.size()) {
            auto comma = value.find(',', start);
            auto end = comma == std::string_view::npos ? value.size() : comma;
            auto token = to_lower(trim_copy(value.substr(start, end - start)));
            if (!token.empty()) {
                if (token == "close") tokens.close = true;
                if (token == "keep-alive") tokens.keep_alive = true;
                if (out) out->insert(std::move(token));
            }
            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }
        return tokens;
    }

    static void parse_header_line_into(
        const std::string& line,
        std::vector<std::pair<std::string, std::string>>& out,
        HeaderParseState& state) {
        auto colon = line.find(':');
        if (colon == std::string::npos) return;
        auto k = trim_copy(std::string_view(line.data(), colon));
        auto v = trim_copy(std::string_view(line.data() + colon + 1, line.size() - colon - 1));
        out.emplace_back(k, v);
        auto lk = to_lower(k);
        auto lv = to_lower(v);
        if (lk == "content-length") {
            auto parsed = parse_decimal_size(v);
            if (!parsed) {
                state.invalid_content_length = true;
            } else if (state.content_length && *state.content_length != *parsed) {
                state.duplicate_content_length = true;
            } else {
                state.content_length = *parsed;
            }
        } else if (lk == "transfer-encoding") {
            state.has_transfer_encoding = true;
            if (lv.find("chunked") != std::string::npos) {
                state.is_chunked = true;
            }
        } else if (lk == "connection") {
            auto tokens = split_connection_tokens(v);
            if (tokens.close) state.connection_close = true;
            if (tokens.keep_alive) state.connection_keep_alive = true;
        }
    }

    static void parse_header_fields(
        std::string header_lines,
        std::vector<std::pair<std::string, std::string>>& out,
        HeaderParseState& state) {
        size_t pos = 0;
        while ((pos = header_lines.find("\r\n")) != std::string::npos) {
            auto line = header_lines.substr(0, pos);
            parse_header_line_into(line, out, state);
            header_lines.erase(0, pos + 2);
        }
        if (!header_lines.empty()) {
            parse_header_line_into(header_lines, out, state);
        }
    }

    asio::awaitable<std::optional<std::string>> consume_line(
        asio::ip::tcp::socket& sock, std::string& buffer, std::chrono::milliseconds timeout) {
        while (true) {
            auto pos = buffer.find("\r\n");
            if (pos != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 2);
                co_return line;
            }
            char tmp[4096];
            auto n = co_await read_with_timeout(sock, tmp, sizeof(tmp), timeout);
            if (!n) co_return std::nullopt;
            buffer.append(tmp, n);
        }
    }

    asio::awaitable<bool> consume_exact(
        asio::ip::tcp::socket& sock, std::string& buffer, std::string& out,
        size_t len, std::chrono::milliseconds timeout) {
        while (len > 0) {
            if (!buffer.empty()) {
                size_t take = std::min(len, buffer.size());
                out.append(buffer, 0, take);
                buffer.erase(0, take);
                len -= take;
                continue;
            }
            char tmp[4096];
            auto n = co_await read_with_timeout(sock, tmp, sizeof(tmp), timeout);
            if (!n) co_return false;
            buffer.append(tmp, n);
        }
        co_return true;
    }

    asio::awaitable<bool> read_chunked_stream(
        asio::ip::tcp::socket& sock, std::string& buffer, std::string& out,
        size_t max_body_size, std::chrono::milliseconds timeout) {
        out.clear();
        while (true) {
            auto line = co_await consume_line(sock, buffer, timeout);
            if (!line) co_return false;
            out += *line;
            out += "\r\n";

            auto chunk_size = parse_hex_size_line(*line);
            if (!chunk_size) co_return false;
            if (*chunk_size > max_body_size || out.size() + *chunk_size + 2 > max_body_size) {
                co_return false;
            }

            if (*chunk_size == 0) {
                while (true) {
                    auto trailer = co_await consume_line(sock, buffer, timeout);
                    if (!trailer) co_return false;
                    out += *trailer;
                    out += "\r\n";
                    if (trailer->empty()) {
                        co_return true;
                    }
                    if (out.size() > max_body_size) {
                        co_return false;
                    }
                }
            }

            if (!co_await consume_exact(sock, buffer, out, *chunk_size + 2, timeout)) {
                co_return false;
            }
            if (out.size() < 2 || out[out.size() - 2] != '\r' || out[out.size() - 1] != '\n') {
                co_return false;
            }
        }
    }

    // 读取下游响应：返回状态码、headers、body；多读的字节存入 conn.read_buffer
    asio::awaitable<ProxyResponse> read_proxy_response(
        HttpPool::HttpConn& conn, const HttpPool::Config& pool_cfg) {

        ProxyResponse resp;
        resp.status_code = 502;  // 默认 Bad Gateway

        // 优先消费 read_buffer（上次多读的字节）
        std::string buf = std::move(conn.read_buffer);

        // 读头部直到 \r\n\r\n
        while (buf.find("\r\n\r\n") == std::string::npos) {
            char tmp[4096];
            auto nr = co_await read_with_timeout(conn.socket, tmp, sizeof(tmp),
                std::chrono::milliseconds(pool_cfg.read_timeout_ms));
            if (!nr) {
                // 超时返回
                co_return resp;
            }
            buf.append(tmp, nr);
        }

        auto header_end = buf.find("\r\n\r\n");
        std::string header_part = buf.substr(0, header_end);
        std::string body_rest = buf.substr(header_end + 4);

        // 解析状态行
        auto first_line_end = header_part.find("\r\n");
        if (first_line_end == std::string::npos) {
            conn.connection_close = true;
            co_return resp;
        }
        auto status_line = header_part.substr(0, first_line_end);
        auto sp1 = status_line.find(' ');
        auto sp2 = status_line.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) {
            conn.connection_close = true;
            co_return resp;
        }
        resp.status_code = std::stoi(status_line.substr(sp1 + 1, sp2 - sp1 - 1));
        resp.status_text = status_line.substr(sp2 + 1);

        int upstream_minor_version = status_line.rfind("HTTP/1.0", 0) == 0 ? 0 : 1;
        HeaderParseState header_state;
        auto hdr = header_part.substr(first_line_end + 2, header_end - first_line_end - 2);
        parse_header_fields(std::move(hdr), resp.headers, header_state);
        if (header_state.invalid_content_length || header_state.duplicate_content_length ||
            (header_state.has_transfer_encoding && !header_state.is_chunked)) {
            conn.connection_close = true;
            co_return resp;
        }

        conn.connection_close = header_state.connection_close ||
            (upstream_minor_version == 0 && !header_state.connection_keep_alive);

        // 读 body
        if (header_state.is_chunked) {
            if (!co_await read_chunked_stream(conn.socket, body_rest, resp.body,
                    pool_cfg.max_body_size, std::chrono::milliseconds(pool_cfg.read_timeout_ms))) {
                conn.connection_close = true;
                co_return resp;
            }
            conn.read_buffer = std::move(body_rest);
        } else if (header_state.content_length) {
            size_t content_length = *header_state.content_length;
            resp.body = std::move(body_rest);
            if (resp.body.size() >= content_length) {
                // 多读的字节存回 read_buffer
                if (resp.body.size() > content_length) {
                    conn.read_buffer = resp.body.substr(content_length);
                    resp.body.resize(content_length);
                }
            } else {
                size_t remaining = content_length - resp.body.size();
                while (remaining > 0) {
                    char tmp[4096];
                    auto nr = co_await read_with_timeout(conn.socket, tmp, sizeof(tmp),
                        std::chrono::milliseconds(pool_cfg.read_timeout_ms));
                    if (!nr) {
                        conn.connection_close = true;
                        break;
                    }
                    if (nr > remaining) {
                        resp.body.append(tmp, remaining);
                        conn.read_buffer.assign(tmp + remaining, nr - remaining);
                        remaining = 0;
                    } else {
                        resp.body.append(tmp, nr);
                        remaining -= nr;
                    }
                    if (resp.body.size() > pool_cfg.max_body_size) {
                        conn.connection_close = true;
                        break;
                    }
                }
            }
            if (resp.body.size() > pool_cfg.max_body_size) {
                conn.connection_close = true;
                resp.body.resize(pool_cfg.max_body_size);
            }
        } else {
            // 无 CL 无 TE：按 RFC 7230 §3.3.3 判定 body
            // - 1xx/204/304: 必须没有 body
            // - 其他：连接关闭或 Connection: close 时读到 EOF
            if (resp.status_code == 204 || resp.status_code == 304 ||
                (resp.status_code >= 100 && resp.status_code < 200)) {
                // 无 body
                resp.body = std::string();
                // 如果 body_rest 有多读（实际不应有），存回 read_buffer
                if (!body_rest.empty()) {
                    conn.read_buffer = std::move(body_rest);
                }
            } else {
                // 读到连接关闭（仅 Connection: close 或 HTTP/1.0）
                conn.connection_close = true;
                resp.body = std::move(body_rest);
                char tmp[4096];
                while (true) {
                    auto nr = co_await read_with_timeout(conn.socket, tmp, sizeof(tmp),
                        std::chrono::milliseconds(pool_cfg.read_timeout_ms));
                    if (!nr) break;
                    resp.body.append(tmp, nr);
                    if (resp.body.size() > pool_cfg.max_body_size) {
                        conn.connection_close = true;
                        resp.body.resize(pool_cfg.max_body_size);
                        break;
                    }
                }
            }
        }

        co_return resp;
    }

    static std::string to_lower(std::string_view s) {
        std::string out; out.reserve(s.size());
        for (char c : s) out += std::tolower(static_cast<unsigned char>(c));
        return out;
    }

    static const std::unordered_set<std::string>& hop_by_hop_headers() {
        static const std::unordered_set<std::string> headers = {
            "connection", "keep-alive", "proxy-authenticate",
            "proxy-authorization", "te", "trailer",
            "transfer-encoding", "upgrade"
        };
        return headers;
    }

    static void add_connection_tokens(
        const std::vector<std::pair<std::string, std::string>>& headers,
        std::unordered_set<std::string>& filtered) {
        for (auto& [k, v] : headers) {
            if (to_lower(k) != "connection") continue;
            split_connection_tokens(v, &filtered);
        }
    }

    asio::awaitable<void> handle_connection(asio::ip::tcp::socket socket) {
        try {
            char buf[8192];
            std::string client_preread;
            auto client_timeout = 30s;

            while (running_) {
                while (client_preread.find("\r\n\r\n") == std::string::npos) {
                    auto n = co_await read_with_timeout(socket, buf, sizeof(buf), client_timeout);
                    if (!n) co_return;
                    client_preread.append(buf, n);
                    if (client_preread.size() > 64 * 1024) {
                        co_return;
                    }
                }

                const char* method, *path;
                int minor_version;
                struct phr_header headers[32];
                size_t num_headers = 32;

                size_t method_len, path_len;
                int pret = phr_parse_request(client_preread.data(), client_preread.size(),
                    &method, &method_len, &path, &path_len,
                    &minor_version, headers, &num_headers, 0);

                if (pret < 0) break;
                std::string body_buffer = client_preread.substr(pret);
                client_preread.clear();

                std::string path_str(path, path_len);
                std::string method_str(method, method_len);

                // 构造 HttpContext
                HttpContext ctx;
                ctx.method = method_str;
                ctx.path = path_str;

                for (size_t i = 0; i < num_headers; ++i) {
                    ctx.headers.emplace_back(
                        std::string(headers[i].name, headers[i].name_len),
                        std::string(headers[i].value, headers[i].value_len));
                }

                HeaderParseState request_header_state;
                for (auto& [k, v] : ctx.headers) {
                    std::vector<std::pair<std::string, std::string>> ignored_headers;
                    std::string line;
                    line.reserve(k.size() + v.size() + 2);
                    line.append(k).append(": ").append(v);
                    parse_header_line_into(line, ignored_headers, request_header_state);
                }

                // 路由 + 处理（先确定 handled，再解析 body 避免重复劳动）
                bool handled = false;

                // 先试本地路由
                auto it = routes_.find(path_str);
                if (it != routes_.end()) {
                    co_await it->second(ctx);
                    handled = true;
                }

                // 再试代理路由 — 需要读取 body 转发
                if (!handled) {
                    // 读取 body
                    std::string& preread = body_buffer;
                    if (request_header_state.invalid_content_length ||
                        request_header_state.duplicate_content_length ||
                        (request_header_state.has_transfer_encoding && !request_header_state.is_chunked)) {
                        ctx.status_code = 400;
                        ctx.response_body = "{\"code\":400,\"msg\":\"invalid request framing\"}";
                        handled = true;
                    } else if (request_header_state.is_chunked) {
                        if (!co_await read_chunked_stream(socket, preread, ctx.body,
                                kMaxBodySize, client_timeout)) {
                            ctx.status_code = 400;
                            ctx.response_body = "{\"code\":400,\"msg\":\"invalid chunked body\"}";
                            handled = true;
                        }
                        client_preread = std::move(preread);
                    } else if (request_header_state.content_length) {
                        size_t content_length = *request_header_state.content_length;
                        if (content_length > kMaxBodySize) {
                            ctx.status_code = 413;
                            ctx.response_body = "{\"code\":413,\"msg\":\"body too large\"}";
                            handled = true;
                        } else if (preread.size() >= content_length) {
                            ctx.body = preread.substr(0, content_length);
                            client_preread = preread.substr(content_length);
                        } else {
                            ctx.body = preread;
                            size_t remaining = content_length - ctx.body.size();
                            while (remaining > 0) {
                                auto nr = co_await read_with_timeout(socket, buf, sizeof(buf), client_timeout);
                                if (!nr) {
                                    ctx.status_code = 408;
                                    ctx.response_body = "{\"code\":408,\"msg\":\"request timeout\"}";
                                    handled = true;
                                    break;
                                }
                                if (nr > remaining) {
                                    ctx.body.append(buf, remaining);
                                    client_preread.assign(buf + remaining, nr - remaining);
                                    remaining = 0;
                                } else {
                                    ctx.body.append(buf, nr);
                                    remaining -= nr;
                                }
                            }
                        }
                    } else {
                        ctx.body = preread;
                    }
                }

                // 代理路由（需要 body 已解析）
                if (!handled) {
                    auto upstream = upman_.route(path_str);
                    if (upstream) {
                        auto& [cfg, pool] = *upstream;
                        try {
                            auto conn_opt = co_await pool->acquire(cfg.host, cfg.port);
                            if (conn_opt) {
                                ConnGuard guard(pool, std::move(conn_opt));
                                auto& conn = guard.conn();

                                // 构造转发请求
                                std::string forward_req = method_str + " " + path_str + " HTTP/1.1\r\n";
                                forward_req += "Host: " + cfg.host + ":" + std::to_string(cfg.port) + "\r\n";

                                std::unordered_set<std::string> filtered = hop_by_hop_headers();
                                filtered.insert("host");
                                add_connection_tokens(ctx.headers, filtered);
                                bool forwarding_transfer_encoding = false;
                                for (auto& [k, v] : ctx.headers) {
                                    if (to_lower(k) == "transfer-encoding") {
                                        forwarding_transfer_encoding = true;
                                        break;
                                    }
                                }

                                for (auto& [k, v] : ctx.headers) {
                                    auto lk = to_lower(k);
                                    // transfer-encoding: chunked 需要保留，让下游正确解析 raw chunked body
                                    if (lk == "transfer-encoding") {
                                        forward_req += k + ": " + v + "\r\n";
                                        continue;
                                    }
                                    if (forwarding_transfer_encoding && lk == "content-length") continue;
                                    if (filtered.find(lk) != filtered.end()) continue;
                                    forward_req += k + ": " + v + "\r\n";
                                }
                                asio::error_code remote_ec;
                                auto remote_ep = socket.remote_endpoint(remote_ec);
                                std::string client_ip = remote_ec ? "" : remote_ep.address().to_string();
                                if (!client_ip.empty()) {
                                    auto existing_xff = ctx.get_header("X-Forwarded-For");
                                    forward_req += "X-Forwarded-For: ";
                                    if (!existing_xff.empty()) {
                                        forward_req += existing_xff + ", ";
                                    }
                                    forward_req += client_ip + "\r\n";
                                }
                                forward_req += "X-Forwarded-Proto: http\r\n";
                                forward_req += "Via: 1.1 asio_owen\r\n";

                                forward_req += "\r\n" + ctx.body;

                                // 发送请求（带超时）
                                auto write_ok = co_await write_with_timeout(
                                    conn.socket, forward_req,
                                    std::chrono::milliseconds(pool->cfg().request_timeout_ms));

                                if (!write_ok) {
                                    // 超时，取消 socket 上的 pending IO
                                    asio::error_code ec;
                                    conn.socket.cancel(ec);
                                    guard.set_bad();
                                    ctx.status_code = 504;
                                    ctx.response_body = "{\"code\":504,\"msg\":\"upstream write timeout\"}";
                                    handled = true;
                                } else {
                                    auto proxy_resp = co_await read_proxy_response(conn, pool->cfg());
                                    ctx.status_code = proxy_resp.status_code;
                                    ctx.response_status_text = std::move(proxy_resp.status_text);
                                    ctx.response_body = std::move(proxy_resp.body);
                                    ctx.response_headers = std::move(proxy_resp.headers);

                                    if (conn.connection_close) guard.set_bad();
                                    handled = true;
                                }
                            } else {
                                ctx.status_code = 503;
                                ctx.response_body = "{\"code\":503,\"msg\":\"no available connection\"}";
                                handled = true;
                            }
                        } catch (const std::exception& e) {
                            LOG_WARN("Proxy forward error: ", e.what());
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

                // 构建响应
                std::string resp = "HTTP/1.1 ";
                resp += std::to_string(ctx.status_code);
                resp += " ";
                if (!ctx.response_status_text.empty()) resp += ctx.response_status_text;
                else if (ctx.status_code == 200) resp += "OK";
                else if (ctx.status_code == 400) resp += "Bad Request";
                else if (ctx.status_code == 404) resp += "Not Found";
                else if (ctx.status_code == 408) resp += "Request Timeout";
                else if (ctx.status_code == 413) resp += "Payload Too Large";
                else if (ctx.status_code == 502) resp += "Bad Gateway";
                else if (ctx.status_code == 503) resp += "Service Unavailable";
                else if (ctx.status_code == 504) resp += "Gateway Timeout";
                else resp += "OK";
                resp += "\r\n";

                // 透传 headers（过滤 hop-by-hop；Content-Length 和 Transfer-Encoding 由我们控制）
                bool has_content_type = false;
                if (!ctx.response_headers.empty()) {
                    std::unordered_set<std::string> filtered = hop_by_hop_headers();
                    add_connection_tokens(ctx.response_headers, filtered);
                    filtered.insert("content-length");
                    for (auto& [k, v] : ctx.response_headers) {
                        auto lk = to_lower(k);
                        if (filtered.find(lk) != filtered.end()) continue;
                        resp += k + ": " + v + "\r\n";
                        if (lk == "content-type") has_content_type = true;
                    }
                }
                if (!has_content_type) {
                    resp += "Content-Type: application/json\r\n";
                }

                resp += "Content-Length: " + std::to_string(ctx.response_body.size()) + "\r\n";
                resp += "\r\n" + ctx.response_body;

                co_await asio::async_write(socket, asio::buffer(resp), asio::use_awaitable);

                // 客户端 Connection: close 则断开
                auto client_conn = ctx.get_header("Connection");
                auto client_conn_tokens = split_connection_tokens(client_conn);
                if (minor_version == 0 && !client_conn_tokens.keep_alive) {
                    break;
                }
                if (client_conn_tokens.close) break;
            }
        } catch (const std::system_error& e) {
            LOG_DEBUG("Connection closed: ", e.what());
        } catch (const std::exception& e) {
            LOG_WARN("Connection error: ", e.what());
        }
    }

    asio::io_context& ioc_;
    asio::ip::tcp::acceptor acceptor_;
    std::unordered_map<std::string, Handler> routes_;
    std::atomic<bool> running_;
    UpstreamManager upman_;
};
