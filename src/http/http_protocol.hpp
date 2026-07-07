#pragma once

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

static constexpr size_t kHttpIoBufferSize = 4096;
static constexpr size_t kClientReadBufferSize = 8192;
static constexpr size_t kMaxHeaderSize = 64 * 1024;
static constexpr size_t kMaxBodySize = 10 * 1024 * 1024;

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

struct HeaderListTokens {
    bool has_token = false;
    bool last_is_token = false;
};

inline std::string_view trim_view(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

inline std::string trim_copy(std::string_view s) {
    return std::string(trim_view(s));
}

inline std::optional<size_t> parse_decimal_size(std::string_view s) {
    auto trimmed = trim_view(s);
    if (trimmed.empty()) return std::nullopt;
    size_t value = 0;
    for (char c : trimmed) {
        if (c < '0' || c > '9') return std::nullopt;
        size_t d = static_cast<size_t>(c - '0');
        if (value > (std::numeric_limits<size_t>::max() - d) / 10) {
            return std::nullopt;
        }
        value = value * 10 + d;
    }
    return value;
}

inline std::optional<size_t> parse_hex_size_line(std::string_view line) {
    auto end = line.find(';');
    auto digits = trim_view(line.substr(0, end == std::string_view::npos ? line.size() : end));
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

inline bool header_iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

inline std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

inline HeaderTokens split_connection_tokens(std::string_view value, std::vector<std::string>* out = nullptr) {
    HeaderTokens tokens;
    size_t start = 0;
    while (start <= value.size()) {
        auto comma = value.find(',', start);
        auto end = comma == std::string_view::npos ? value.size() : comma;
        auto token = trim_view(value.substr(start, end - start));
        if (!token.empty()) {
            if (header_iequals(token, "close")) tokens.close = true;
            if (header_iequals(token, "keep-alive")) tokens.keep_alive = true;
            if (out) out->push_back(std::string(token));
        }
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return tokens;
}

inline HeaderListTokens parse_header_list_token(std::string_view value, std::string_view expected) {
    HeaderListTokens result;
    size_t start = 0;
    while (start <= value.size()) {
        auto comma = value.find(',', start);
        auto end = comma == std::string_view::npos ? value.size() : comma;
        auto token = trim_view(value.substr(start, end - start));
        if (!token.empty()) {
            bool matched = header_iequals(token, expected);
            result.has_token = result.has_token || matched;
            result.last_is_token = matched;
        }
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return result;
}

inline void update_header_state(std::string_view k, std::string_view v, HeaderParseState& state) {
    k = trim_view(k);
    v = trim_view(v);
    if (header_iequals(k, "content-length")) {
        auto parsed = parse_decimal_size(v);
        if (!parsed) {
            state.invalid_content_length = true;
        } else if (state.content_length && *state.content_length != *parsed) {
            state.duplicate_content_length = true;
        } else {
            state.content_length = *parsed;
        }
    } else if (header_iequals(k, "transfer-encoding")) {
        state.has_transfer_encoding = true;
        auto te = parse_header_list_token(v, "chunked");
        if (te.last_is_token) {
            state.is_chunked = true;
        }
    } else if (header_iequals(k, "connection")) {
        auto tokens = split_connection_tokens(v);
        if (tokens.close) state.connection_close = true;
        if (tokens.keep_alive) state.connection_keep_alive = true;
    }
}

inline void parse_header_pair_into(
    std::string_view key,
    std::string_view value,
    std::vector<std::pair<std::string, std::string>>* out,
    HeaderParseState& state) {
    auto k = trim_copy(key);
    auto v = trim_copy(value);
    if (out) {
        out->emplace_back(k, v);
    }
    update_header_state(k, v, state);
}

inline void parse_header_line_into(
    std::string_view line,
    std::vector<std::pair<std::string, std::string>>& out,
    HeaderParseState& state) {
    auto colon = line.find(':');
    if (colon == std::string_view::npos) return;
    parse_header_pair_into(line.substr(0, colon), line.substr(colon + 1), &out, state);
}

inline void parse_header_fields(
    std::string_view header_lines,
    std::vector<std::pair<std::string, std::string>>& out,
    HeaderParseState& state) {
    size_t start = 0;
    while (start <= header_lines.size()) {
        auto pos = header_lines.find("\r\n", start);
        auto end = pos == std::string_view::npos ? header_lines.size() : pos;
        if (end > start) {
            parse_header_line_into(header_lines.substr(start, end - start), out, state);
        }
        if (pos == std::string_view::npos) break;
        start = pos + 2;
    }
}

inline bool is_hop_by_hop_header(std::string_view k) {
    return header_iequals(k, "connection") ||
        header_iequals(k, "keep-alive") ||
        header_iequals(k, "proxy-authenticate") ||
        header_iequals(k, "proxy-authorization") ||
        header_iequals(k, "te") ||
        header_iequals(k, "trailer") ||
        header_iequals(k, "transfer-encoding") ||
        header_iequals(k, "upgrade");
}

inline void add_connection_tokens(
    const std::vector<std::pair<std::string, std::string>>& headers,
    std::vector<std::string>& filtered) {
    for (auto& [k, v] : headers) {
        if (!header_iequals(k, "connection")) continue;
        split_connection_tokens(v, &filtered);
    }
}

inline bool contains_header_name(
    const std::vector<std::string>& names, std::string_view name) {
    for (auto& candidate : names) {
        if (header_iequals(candidate, name)) return true;
    }
    return false;
}

inline std::string describe_headers(
    const std::vector<std::pair<std::string, std::string>>& headers) {
    std::string out;
    for (auto& [k, v] : headers) {
        if (!out.empty()) out += ", ";
        out += k;
        out += "(len=";
        out += std::to_string(v.size());
        if (header_iequals(k, "authorization") || header_iequals(k, "cookie") ||
            header_iequals(k, "set-cookie")) {
            out += ",redacted";
        }
        out += ")";
    }
    return out;
}

inline std::string sanitize_header_value(std::string_view key, std::string_view value) {
    if (header_iequals(key, "authorization") || header_iequals(key, "cookie") ||
        header_iequals(key, "set-cookie")) {
        return "<redacted len=" + std::to_string(value.size()) + ">";
    }
    constexpr size_t kMaxLogValue = 160;
    if (value.size() <= kMaxLogValue) {
        return std::string(value);
    }
    return std::string(value.substr(0, kMaxLogValue)) +
        "...<truncated len=" + std::to_string(value.size()) + ">";
}

inline std::string sanitize_body_preview(std::string_view body) {
    constexpr size_t kMaxPreview = 512;
    std::string out;
    size_t n = std::min(body.size(), kMaxPreview);
    out.reserve(n + 32);
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(body[i]);
        if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else if (std::isprint(c)) out += static_cast<char>(c);
        else out += '.';
    }
    if (body.size() > kMaxPreview) {
        out += "...<truncated len=" + std::to_string(body.size()) + ">";
    }
    return out;
}
