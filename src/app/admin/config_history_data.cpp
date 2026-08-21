#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <openssl/evp.h>

#include "../../http/response.hpp"
#include "../app_config.hpp"
#include "config_history.hpp"

namespace config_history {

struct DigestContextFree {
    void operator()(EVP_MD_CTX* ctx) const { EVP_MD_CTX_free(ctx); }
};

bool digest_update_u64(EVP_MD_CTX* ctx, uint64_t value) {
    std::array<unsigned char, 8> bytes{};
    for (int i = 7; i >= 0; --i) {
        bytes[static_cast<size_t>(i)] = static_cast<unsigned char>(value & 0xffu);
        value >>= 8;
    }
    return EVP_DigestUpdate(ctx, bytes.data(), bytes.size()) == 1;
}

std::optional<std::string> content_sha256(
    const std::map<std::string, std::string>& files) {
    std::unique_ptr<EVP_MD_CTX, DigestContextFree> ctx(EVP_MD_CTX_new());
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        return std::nullopt;
    }
    for (const auto& [name, content] : files) {
        if (!digest_update_u64(ctx.get(), static_cast<uint64_t>(name.size())) ||
            EVP_DigestUpdate(ctx.get(), name.data(), name.size()) != 1 ||
            !digest_update_u64(ctx.get(), static_cast<uint64_t>(content.size())) ||
            EVP_DigestUpdate(ctx.get(), content.data(), content.size()) != 1) {
            return std::nullopt;
        }
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &digest_size) != 1) {
        return std::nullopt;
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.resize(static_cast<size_t>(digest_size) * 2);
    for (size_t i = 0; i < digest_size; ++i) {
        result[i * 2] = hex[digest[i] >> 4];
        result[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    return result;
}

std::optional<std::string> validate_snapshot(
    const std::map<std::string, std::string>& files,
    const ConfigHistoryConfig& cfg,
    SnapshotInfo& info,
    bool reject_empty) {
    info = {};
    if (reject_empty && files.empty()) {
        return "files must not be empty";
    }
    if (files.size() > cfg.max_files) {
        return "managed file count exceeds limit " +
            std::to_string(cfg.max_files);
    }
    info.file_count = files.size();
    for (const auto& [name, content] : files) {
        if (content.size() > cfg.max_file_bytes) {
            return name + ": file content exceeds limit " +
                std::to_string(cfg.max_file_bytes) + " bytes";
        }
        if (content.size() > cfg.max_snapshot_bytes -
                std::min(info.total_bytes, cfg.max_snapshot_bytes)) {
            return "snapshot content exceeds limit " +
                std::to_string(cfg.max_snapshot_bytes) + " bytes";
        }
        info.total_bytes += content.size();
    }
    auto hash = content_sha256(files);
    if (!hash) return "failed to calculate snapshot SHA-256";
    info.content_sha256 = std::move(*hash);
    return std::nullopt;
}

std::string snapshot_key(int64_t version) {
    return std::string(kSnapshotPrefix) + std::to_string(version);
}

std::string metadata_json(
    int64_t version,
    int64_t base_version,
    int64_t timestamp,
    std::string_view user,
    std::string_view action,
    std::string_view reason,
    const SnapshotInfo& info,
    std::optional<int64_t> rollback_from) {
    std::ostringstream out;
    out << "{\"version\":" << version
        << ",\"base_version\":" << base_version
        << ",\"ts\":" << timestamp
        << ",\"user\":\"" << json_escape(std::string(user))
        << "\",\"action\":\"" << json_escape(std::string(action))
        << "\",\"reason\":\"" << json_escape(std::string(reason))
        << "\",\"file_count\":" << info.file_count
        << ",\"total_bytes\":" << info.total_bytes
        << ",\"content_sha256\":\"" << info.content_sha256
        << "\",\"rollback_from\":";
    if (rollback_from) out << *rollback_from;
    else out << "null";
    out << "}";
    return out.str();
}

// JSON is the source document and field is its lookup key; both are string views.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::optional<std::string> json_string_field(
    std::string_view json, std::string_view field) {
    const std::string needle = "\"" + std::string(field) + "\"";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) return std::nullopt;
    ++pos;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' ||
            json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') return std::nullopt;
    ++pos;
    std::string value;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '"') return value;
        if (c != '\\') {
            value.push_back(c);
            continue;
        }
        if (pos >= json.size()) return std::nullopt;
        char escaped = json[pos++];
        switch (escaped) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: return std::nullopt;
        }
    }
    return std::nullopt;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

std::optional<int64_t> parse_int64(std::string_view value) {
    if (value.empty()) return std::nullopt;
    int64_t parsed = 0;
    auto [end, ec] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<SnapshotRecord> parse_detail_elements(
    const std::vector<std::string>& elements) {
    if (elements.size() < 2 || (elements.size() - 2) % 2 != 0) {
        return std::nullopt;
    }
    auto current = parse_int64(elements[0]);
    if (!current || *current < 0 || elements[1].empty() ||
        elements[1] == "(nil)") {
        return std::nullopt;
    }
    SnapshotRecord record;
    record.current_version = *current;
    record.meta_json = elements[1];
    for (size_t i = 2; i < elements.size(); i += 2) {
        record.files[elements[i]] = elements[i + 1];
    }
    if (record.files.empty()) return std::nullopt;
    return record;
}

std::string history_list_json(
    const std::vector<std::string>& elements, int64_t current_version) {
    std::ostringstream out;
    out << "{\"current_version\":" << current_version << ",\"versions\":[";
    bool first = true;
    std::optional<int64_t> next_before;
    for (size_t i = 0; i + 1 < elements.size(); i += 2) {
        auto version = parse_int64(elements[i]);
        if (!version || elements[i + 1].empty() || elements[i + 1] == "(nil)") {
            continue;
        }
        if (!first) out << ",";
        first = false;
        out << elements[i + 1];
        next_before = *version;
    }
    out << "],\"next_before\":";
    if (next_before) out << *next_before;
    else out << "null";
    out << "}";
    return out.str();
}

std::string detail_json(int64_t version, const SnapshotRecord& record) {
    std::ostringstream out;
    out << "{\"version\":" << version
        << ",\"current_version\":" << record.current_version
        << ",\"meta\":" << record.meta_json << ",\"files\":[";
    bool first = true;
    for (const auto& [name, content] : record.files) {
        if (!first) out << ",";
        first = false;
        out << "{\"name\":\"" << json_escape(name)
            << "\",\"content\":\"" << json_escape(content) << "\"}";
    }
    out << "]}";
    return out.str();
}

std::vector<std::string> split_text_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t begin = 0;
    while (begin < text.size()) {
        auto end = text.find('\n', begin);
        if (end == std::string::npos) {
            lines.push_back(text.substr(begin));
            return lines;
        }
        lines.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
    if (!text.empty() && text.back() == '\n') lines.emplace_back();
    return lines;
}

// The three strings have fixed roles in the generated diff.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::string coarse_text_diff(
    const std::string& name,
    const std::string& from,
    const std::string& to) {
    auto old_lines = split_text_lines(from);
    auto new_lines = split_text_lines(to);
    size_t prefix = 0;
    while (prefix < old_lines.size() && prefix < new_lines.size() &&
           old_lines[prefix] == new_lines[prefix]) {
        ++prefix;
    }
    size_t old_suffix = old_lines.size();
    size_t new_suffix = new_lines.size();
    while (old_suffix > prefix && new_suffix > prefix &&
           old_lines[old_suffix - 1] == new_lines[new_suffix - 1]) {
        --old_suffix;
        --new_suffix;
    }
    std::ostringstream out;
    out << "--- " << name << "\n+++ " << name << "\n";
    const size_t context_begin = prefix > 2 ? prefix - 2 : 0;
    for (size_t i = context_begin; i < prefix; ++i) {
        out << " " << old_lines[i] << "\n";
    }
    for (size_t i = prefix; i < old_suffix; ++i) {
        out << "-" << old_lines[i] << "\n";
    }
    for (size_t i = prefix; i < new_suffix; ++i) {
        out << "+" << new_lines[i] << "\n";
    }
    const size_t context_end = std::min(old_lines.size(), old_suffix + 2);
    for (size_t i = old_suffix; i < context_end; ++i) {
        out << " " << old_lines[i] << "\n";
    }
    return out.str();
}
// NOLINTEND(bugprone-easily-swappable-parameters)

bool looks_sensitive(const std::string& content) {
    std::string lowered = content;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static constexpr std::array<std::string_view, 7> needles = {
        "[mysql]", "pass", "password", "secret", "token",
        "private_key", "jwt_private"
    };
    for (auto needle : needles) {
        if (lowered.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::optional<std::string> diff_json(
    int64_t from_version,
    const SnapshotRecord& from,
    int64_t to_version,
    const SnapshotRecord& to,
    size_t max_bytes) {
    std::ostringstream out;
    out << "{\"from\":" << from_version << ",\"to\":" << to_version
        << ",\"changes\":[";
    bool first = true;
    std::set<std::string> names;
    for (const auto& [name, _] : from.files) names.insert(name);
    for (const auto& [name, _] : to.files) names.insert(name);
    for (const auto& name : names) {
        auto old_it = from.files.find(name);
        auto new_it = to.files.find(name);
        if (old_it != from.files.end() && new_it != to.files.end() &&
            old_it->second == new_it->second) {
            continue;
        }
        if (!first) out << ",";
        first = false;
        out << "{\"name\":\"" << json_escape(name) << "\",\"type\":\"";
        if (old_it == from.files.end()) {
            out << "added\",\"content\":\"" << json_escape(new_it->second)
                << "\",\"sensitive\":"
                << (looks_sensitive(new_it->second) ? "true" : "false");
        } else if (new_it == to.files.end()) {
            out << "deleted\",\"content\":\"" << json_escape(old_it->second)
                << "\",\"sensitive\":"
                << (looks_sensitive(old_it->second) ? "true" : "false");
        } else {
            auto diff = coarse_text_diff(name, old_it->second, new_it->second);
            out << "modified\",\"diff\":\"" << json_escape(diff)
                << "\",\"sensitive\":"
                << (looks_sensitive(old_it->second) || looks_sensitive(new_it->second)
                    ? "true" : "false");
        }
        out << "}";
        if (static_cast<size_t>(out.tellp()) > max_bytes) return std::nullopt;
    }
    out << "]}";
    auto result = out.str();
    if (result.size() > max_bytes) return std::nullopt;
    return result;
}

}  // namespace config_history
