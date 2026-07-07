#pragma once

#include <asio.hpp>
#include <chrono>
#include <optional>
#include <string>

#include "http_io.hpp"
#include "http_protocol.hpp"

enum class BodyReadStatus {
    Success,
    Timeout,
    PeerClosed,
    TooLarge,
    InvalidChunk,
    SysError
};

struct BodyReadResult {
    BodyReadStatus status = BodyReadStatus::Success;
    std::string body;
    asio::error_code ec;

    bool ok() const { return status == BodyReadStatus::Success; }
};

inline BodyReadStatus body_status_from_io(const IoResult& result) {
    switch (result.status) {
        case IoStatus::Success: return BodyReadStatus::Success;
        case IoStatus::Timeout: return BodyReadStatus::Timeout;
        case IoStatus::PeerClosed: return BodyReadStatus::PeerClosed;
        case IoStatus::SysError: return BodyReadStatus::SysError;
    }
    return BodyReadStatus::SysError;
}

inline asio::awaitable<std::optional<std::string>> consume_line(
    asio::ip::tcp::socket& sock,
    std::string& buffer,
    std::chrono::milliseconds timeout,
    asio::error_code* out_ec = nullptr,
    BodyReadStatus* out_status = nullptr) {
    while (true) {
        auto pos = buffer.find("\r\n");
        if (pos != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 2);
            co_return line;
        }
        char tmp[kHttpIoBufferSize];
        auto read = co_await read_with_timeout(sock, tmp, sizeof(tmp), timeout);
        if (!read.ok()) {
            if (out_ec) *out_ec = read.ec;
            if (out_status) *out_status = body_status_from_io(read);
            co_return std::nullopt;
        }
        buffer.append(tmp, read.bytes);
    }
}

inline asio::awaitable<BodyReadStatus> consume_exact(
    asio::ip::tcp::socket& sock,
    std::string& buffer,
    std::string& out,
    size_t len,
    std::chrono::milliseconds timeout,
    asio::error_code* out_ec = nullptr) {
    while (len > 0) {
        if (!buffer.empty()) {
            size_t take = std::min(len, buffer.size());
            out.append(buffer, 0, take);
            buffer.erase(0, take);
            len -= take;
            continue;
        }
        char tmp[kHttpIoBufferSize];
        auto read = co_await read_with_timeout(sock, tmp, sizeof(tmp), timeout);
        if (!read.ok()) {
            if (out_ec) *out_ec = read.ec;
            co_return body_status_from_io(read);
        }
        buffer.append(tmp, read.bytes);
    }
    co_return BodyReadStatus::Success;
}

inline asio::awaitable<BodyReadResult> read_content_length_body(
    asio::ip::tcp::socket& sock,
    std::string& preread,
    size_t content_length,
    size_t max_body_size,
    std::chrono::milliseconds timeout) {
    BodyReadResult result;
    if (content_length > max_body_size) {
        // Do not drain an oversized body. The caller must stop reusing this
        // connection because unread body bytes may still be on the wire.
        result.status = BodyReadStatus::TooLarge;
        co_return result;
    }

    if (preread.size() >= content_length) {
        result.body = preread.substr(0, content_length);
        preread.erase(0, content_length);
        co_return result;
    }

    result.body = std::move(preread);
    preread.clear();
    size_t remaining = content_length - result.body.size();
    asio::error_code ec;
    auto status = co_await consume_exact(sock, preread, result.body, remaining, timeout, &ec);
    if (status != BodyReadStatus::Success) {
        result.status = status;
        result.ec = ec;
        result.body.clear();
        co_return result;
    }
    co_return result;
}

inline asio::awaitable<BodyReadResult> read_chunked_body(
    asio::ip::tcp::socket& sock,
    std::string& preread,
    size_t max_body_size,
    std::chrono::milliseconds timeout) {
    BodyReadResult result;
    while (true) {
        asio::error_code ec;
        BodyReadStatus line_status = BodyReadStatus::Success;
        auto line = co_await consume_line(sock, preread, timeout, &ec, &line_status);
        if (!line) {
            result.status = line_status;
            result.ec = ec;
            co_return result;
        }

        auto chunk_size = parse_hex_size_line(*line);
        if (!chunk_size) {
            result.status = BodyReadStatus::InvalidChunk;
            co_return result;
        }

        if (*chunk_size == 0) {
            while (true) {
                auto trailer = co_await consume_line(sock, preread, timeout, &ec, &line_status);
                if (!trailer) {
                    result.status = line_status;
                    result.ec = ec;
                    co_return result;
                }
                if (trailer->empty()) {
                    co_return result;
                }
            }
        }

        if (*chunk_size > max_body_size ||
            result.body.size() > max_body_size - *chunk_size) {
            result.status = BodyReadStatus::TooLarge;
            result.body.clear();
            co_return result;
        }

        std::string tmp;
        auto status = co_await consume_exact(sock, preread, tmp, *chunk_size + 2, timeout, &ec);
        if (status != BodyReadStatus::Success) {
            result.status = status;
            result.ec = ec;
            result.body.clear();
            co_return result;
        }
        if (tmp.size() < *chunk_size + 2 || tmp[*chunk_size] != '\r' || tmp[*chunk_size + 1] != '\n') {
            result.status = BodyReadStatus::InvalidChunk;
            result.body.clear();
            co_return result;
        }
        result.body.append(tmp.data(), *chunk_size);
    }
}
