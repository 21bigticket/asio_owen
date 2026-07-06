#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <cctype>

// Normalized path result
struct NormalizedPath {
    std::string path;   // normalized path (without query string)
};

// percent-decode, but preserve %2F/%2f as literals (do not decode as path separator)
inline std::string percent_decode(std::string_view s) {
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            // preserve %2F/%2f (do not decode, keep as literal)
            bool is_slash = (s[i+1] == '2') &&
                            (s[i+2] == 'f' || s[i+2] == 'F');
            if (is_slash) {
                result += s[i];
                result += s[i+1];
                result += s[i+2];
                i += 2;
                continue;
            }
            // normal percent-decode for other sequences
            unsigned char uc1 = static_cast<unsigned char>(s[i+1]);
            unsigned char uc2 = static_cast<unsigned char>(s[i+2]);
            int hi = std::isxdigit(uc1) ? (std::isdigit(uc1)
                ? uc1 - '0' : std::tolower(uc1) - 'a' + 10) : -1;
            int lo = std::isxdigit(uc2) ? (std::isdigit(uc2)
                ? uc2 - '0' : std::tolower(uc2) - 'a' + 10) : -1;
            if (hi >= 0 && lo >= 0) {
                result += static_cast<char>((hi << 4) | lo);
                i += 2;
            } else {
                // invalid percent-encoding (e.g. %GG), preserve original char
                result += s[i];
            }
        } else {
            result += s[i];
        }
    }
    return result;
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
    std::string decoded = percent_decode(path_only);
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
