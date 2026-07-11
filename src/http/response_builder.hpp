#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "http_context.hpp"
#include "http_protocol.hpp"

inline std::string_view reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "OK";
    }
}

inline bool http_response_has_no_body(std::string_view method, int status) {
    return method == "HEAD" || status == 204 || status == 304 ||
        (status >= 100 && status < 200);
}

inline std::string build_error_response(
    int status, std::string_view reason, std::string_view body) {
    std::string resp = "HTTP/1.1 ";
    resp += std::to_string(status);
    resp += " ";
    resp += reason;
    resp += "\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: ";
    resp += std::to_string(body.size());
    resp += "\r\n\r\n";
    resp += body;
    return resp;
}

inline std::string build_downstream_response(
    const HttpContext& ctx,
    std::string_view method,
    bool proxy_response) {
    std::string resp = "HTTP/1.1 ";
    resp += std::to_string(ctx.status_code);
    resp += " ";
    if (!ctx.response_status_text.empty()) {
        resp += ctx.response_status_text;
    } else {
        resp += reason_phrase(ctx.status_code);
    }
    resp += "\r\n";

    bool has_content_type = false;
    if (!ctx.response_headers.empty()) {
        if (proxy_response) {
            std::vector<std::string> filtered = {
                "connection", "keep-alive", "proxy-authenticate",
                "proxy-authorization", "te", "trailer",
                "transfer-encoding", "upgrade"
            };
            add_connection_tokens(ctx.response_headers, filtered);
            // For responses with a body, filter out the upstream Content-Length
            // because the body may have been transformed (json_keys_snake_to_camel)
            // and the upstream CL would be incorrect. We write our own below.
            // For HEAD responses, preserve upstream CL per RFC 7231.
            if (!http_response_has_no_body(method, ctx.status_code)) {
                filtered.push_back("content-length");
            }
            for (auto& [k, v] : ctx.response_headers) {
                if (contains_header_name(filtered, k)) continue;
                resp += k + ": " + v + "\r\n";
                if (header_iequals(k, "content-type")) has_content_type = true;
            }
        } else {
            for (auto& [k, v] : ctx.response_headers) {
                if (header_iequals(k, "content-length") || is_hop_by_hop_header(k)) continue;
                resp += k + ": " + v + "\r\n";
                if (header_iequals(k, "content-type")) has_content_type = true;
            }
        }
    }

    bool no_body = http_response_has_no_body(method, ctx.status_code);
    // For no-body responses (HEAD/204/304), preserve upstream Content-Length
    // if present (RFC 7231 requires HEAD to match GET). For responses with a
    // body, the gateway may have transformed it (e.g. json_keys_snake_to_camel),
    // so we must always write our own Content-Length based on the final body size.
    bool has_upstream_cl = false;
    if (proxy_response && no_body) {
        for (auto& [k, v] : ctx.response_headers) {
            if (header_iequals(k, "content-length")) {
                has_upstream_cl = true;
                break;
            }
        }
    }
    if (!has_content_type && !no_body) {
        resp += "Content-Type: application/json\r\n";
    }
    if (ctx.status_code >= 400) {
        resp += "Connection: close\r\n";
        resp += "X-Asio-Owen-Status-Source: ";
        resp += proxy_response ? "proxy\r\n" : "local\r\n";
    }

    if (!has_upstream_cl) {
        resp += "Content-Length: ";
        resp += no_body ? "0" : std::to_string(ctx.response_body.size());
        resp += "\r\n";
    }
    resp += "\r\n";
    if (!no_body) resp += ctx.response_body;
    return resp;
}
