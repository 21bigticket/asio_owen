#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <cctype>

// Normalized path result
struct NormalizedPath {
    std::string path;   // normalized path (without query string)
    bool valid = true;
    std::string error;
};

inline bool is_hex_char(unsigned char c) {
    return std::isxdigit(c) != 0;
}

inline int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    return std::tolower(c) - 'a' + 10;
}

// Percent-decode path bytes. Encoded slash/backslash/NUL and residual percent
// encodings are rejected so security checks see the same path shape as upstreams.
inline NormalizedPath percent_decode(std::string_view s) {
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            unsigned char uc1 = static_cast<unsigned char>(s[i+1]);
            unsigned char uc2 = static_cast<unsigned char>(s[i+2]);
            if (!is_hex_char(uc1) || !is_hex_char(uc2)) {
                return {"", false, "invalid percent encoding"};
            }
            char decoded = static_cast<char>((hex_value(uc1) << 4) | hex_value(uc2));
            if (decoded == '\0' || decoded == '/' || decoded == '\\') {
                return {"", false, "unsafe encoded path separator or nul"};
            }
            result += decoded;
            i += 2;
        } else {
            if (s[i] == '\0' || s[i] == '\\') {
                return {"", false, "unsafe path character"};
            }
            result += s[i];
        }
    }
    if (result.find('%') != std::string::npos) {
        return {"", false, "residual percent encoding"};
    }
    return {std::move(result), true, ""};
}

// Normalize path: percent-decode + resolve dot-segments + collapse slashes + optional lowercase
// Input is the path portion parsed by picohttpparser (no host, no query string)
// case_sensitive=false (default): normalize to lowercase for uniform ACL matching
// case_sensitive=true: preserve original case
inline NormalizedPath normalize_path(std::string_view path_only, bool case_sensitive = false) {
    // Strip query string before normalization
    auto qpos = path_only.find('?');
    if (qpos != std::string_view::npos) {
        path_only = path_only.substr(0, qpos);
    }

    // 1. percent-decode (%2F preserved as literal)
    auto decoded_result = percent_decode(path_only);
    if (!decoded_result.valid) {
        return decoded_result;
    }
    std::string& decoded = decoded_result.path;
    if (decoded.empty()) {
        return {"/"};
    }

    // 2. collapse // to /, split into segments
    std::vector<std::string_view> segments;
    size_t start = 0;
    for (size_t i = 0; i <= decoded.size(); ++i) {
        if (i == decoded.size() || decoded[i] == '/') {
            if (i > start) {
                auto seg = std::string_view(decoded.data() + start, i - start);
                // skip "." (current directory)
                if (seg == ".") {
                    // skip
                } else if (seg == "..") {
                    // parent directory, pop previous segment
                    if (!segments.empty()) {
                        segments.pop_back();
                    }
                } else {
                    segments.push_back(seg);
                }
            }
            start = i + 1;
        }
    }

    // 3. reassemble path
    std::string normalized;
    normalized.reserve(decoded.size() + 1);
    normalized += "/";
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) normalized += "/";
        // normalize to lowercase (unless case-sensitive mode)
        for (auto c : segments[i]) {
            auto ch = static_cast<unsigned char>(c);
            normalized += case_sensitive ? static_cast<char>(ch)
                : static_cast<char>(std::tolower(ch));
        }
    }

    return {std::move(normalized)};
}
