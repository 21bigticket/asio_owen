#pragma once
#include <cstdio>
#include <string>
#include <sstream>
#include <string_view>

enum HttpCode {
    OK = 0,
    PARAM_ERROR = 400,
    UNAUTHORIZED = 401,
    NOT_FOUND = 404,
    SERVER_ERROR = 500,
    DB_ERROR = 501,
    TIMEOUT = 504,
};

// Escape a string for safe inclusion inside a JSON string literal.
// Covers " \ and the RFC 8259 mandatory control-char escapes (U+0000..U+001F).
inline std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                        static_cast<unsigned int>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Build a JSON envelope: {"code":N,"msg":"<escaped>","data":<raw>}.
// `data` is spliced verbatim — callers must pass already-serialized JSON.
// `msg` is escaped because it routinely holds DB/Redis/exception text.
inline std::string json_resp(int code, const std::string& msg, const std::string& data = "null") {
    std::ostringstream oss;
    oss << R"({"code":)" << code
        << R"(,"msg":")" << json_escape(msg) << R"(")"
        << R"(,"data":)" << data
        << "}";
    return oss.str();
}

inline std::string resp_ok(const std::string& data = "null") {
    return json_resp(OK, "ok", data);
}

// Wrap a string-typed result as a JSON string value inside the envelope.
// `data` is treated as opaque text and escaped.
inline std::string resp_ok_str(const std::string& data) {
    std::ostringstream oss;
    oss << "\"" << json_escape(data) << "\"";
    return json_resp(OK, "ok", oss.str());
}

inline std::string resp_err(int code, const std::string& msg) {
    return json_resp(code, msg);
}
