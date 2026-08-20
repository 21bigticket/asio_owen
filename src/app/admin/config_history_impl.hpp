#pragma once

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

#ifdef CONFIG_HISTORY_OUT_OF_LINE
#define inline
#endif

namespace config_history {

struct DigestContextFree {
    void operator()(EVP_MD_CTX* ctx) const { EVP_MD_CTX_free(ctx); }
};

inline bool digest_update_u64(EVP_MD_CTX* ctx, uint64_t value) {
    std::array<unsigned char, 8> bytes{};
    for (int i = 7; i >= 0; --i) {
        bytes[static_cast<size_t>(i)] = static_cast<unsigned char>(value & 0xffu);
        value >>= 8;
    }
    return EVP_DigestUpdate(ctx, bytes.data(), bytes.size()) == 1;
}

inline std::optional<std::string> content_sha256(
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

inline std::optional<std::string> validate_snapshot(
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

inline std::string snapshot_key(int64_t version) {
    return std::string(kSnapshotPrefix) + std::to_string(version);
}

inline std::string metadata_json(
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

inline std::optional<std::string> json_string_field(
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

inline std::optional<int64_t> parse_int64(std::string_view value) {
    if (value.empty()) return std::nullopt;
    int64_t parsed = 0;
    auto [end, ec] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

inline std::optional<SnapshotRecord> parse_detail_elements(
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

inline std::string history_list_json(
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

inline std::string detail_json(int64_t version, const SnapshotRecord& record) {
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

inline std::vector<std::string> split_text_lines(const std::string& text) {
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

inline std::string coarse_text_diff(
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

inline bool looks_sensitive(const std::string& content) {
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

inline std::optional<std::string> diff_json(
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

inline std::string list_script() {
    return R"(
local cur = tonumber(redis.call('GET', KEYS[1]) or '')
local before = tonumber(ARGV[1])
local limit = tonumber(ARGV[2])
if cur == nil or before == nil or limit == nil or limit < 1 then return {'__error__', 'invalid'} end
local max_score = '+inf'
if before > 0 then max_score = '(' .. tostring(before) end
local versions = redis.call('ZREVRANGEBYSCORE', KEYS[2], max_score, '-inf', 'LIMIT', 0, limit)
local out = {tostring(cur)}
local metas = {}
if #versions > 0 then metas = redis.call('HMGET', KEYS[3], unpack(versions)) end
for i, version in ipairs(versions) do
  local numeric = tonumber(version)
  local meta = metas[i]
  if numeric ~= nil and numeric <= cur and meta and
     redis.call('EXISTS', ARGV[3] .. version) == 1 then
    table.insert(out, version)
    table.insert(out, meta)
  end
end
return out
)";
}

inline std::string detail_script() {
    return R"(
local cur = tonumber(redis.call('GET', KEYS[1]) or '')
local version = tonumber(ARGV[1])
if cur == nil or version == nil or version > cur then return {} end
local member = tostring(version)
local meta = redis.call('HGET', KEYS[2], member)
if not meta or not redis.call('ZSCORE', KEYS[3], member) or
   redis.call('EXISTS', KEYS[4]) ~= 1 then return {} end
local files = redis.call('HGETALL', KEYS[4])
if #files == 0 then return {} end
local out = {tostring(cur), meta}
for _, value in ipairs(files) do table.insert(out, value) end
return out
)";
}

inline std::string save_script() {
    return R"(
if (#ARGV - 6) % 2 ~= 0 then return -2 end
if #ARGV < 8 then return -2 end
local base = tonumber(ARGV[1])
local max_files = tonumber(ARGV[4])
local max_file_bytes = tonumber(ARGV[5])
local max_total_bytes = tonumber(ARGV[6])
if base == nil or max_files == nil or max_file_bytes == nil or max_total_bytes == nil then return -2 end
local file_count = (#ARGV - 6) / 2
if file_count < 1 or file_count > max_files then return -8 end
local total_bytes = 0
for i = 7, #ARGV, 2 do
  local file_bytes = string.len(ARGV[i + 1])
  if file_bytes > max_file_bytes then return -8 end
  total_bytes = total_bytes + file_bytes
  if total_bytes > max_total_bytes then return -8 end
end
local expected = {'string', 'hash', 'list', 'hash', 'hash', 'zset', 'hash', 'hash', 'hash'}
for i = 1, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return -3 end
end
local cur = tonumber(redis.call('GET', KEYS[1]) or '0')
if cur == nil then return -4 end
if cur ~= base then return -1 end
local top = redis.call('ZREVRANGE', KEYS[6], 0, 0)
if cur > 0 then
  local current_member = tostring(cur)
  if top[1] == nil or tonumber(top[1]) ~= cur or
     not redis.call('HGET', KEYS[5], current_member) or
     redis.call('EXISTS', KEYS[9]) ~= 1 then return -5 end
elseif top[1] ~= nil or redis.call('ZCARD', KEYS[6]) ~= 0 or
   redis.call('HLEN', KEYS[5]) ~= 0 then
  return -5
end
local newv = base + 1
local member = tostring(newv)
if redis.call('EXISTS', KEYS[7]) ~= 0 or
   redis.call('HEXISTS', KEYS[5], member) ~= 0 or
   redis.call('ZSCORE', KEYS[6], member) ~= false then return -6 end
redis.call('DEL', KEYS[8])
redis.call('HSET', KEYS[8], unpack(ARGV, 7, #ARGV))
redis.call('RENAME', KEYS[8], KEYS[7])
redis.call('HSET', KEYS[5], member, ARGV[3])
redis.call('ZADD', KEYS[6], newv, member)
redis.call('DEL', KEYS[4])
redis.call('HSET', KEYS[4], unpack(ARGV, 7, #ARGV))
redis.call('RENAME', KEYS[4], KEYS[2])
local published = redis.call('INCR', KEYS[1])
if published ~= newv then return -7 end
pcall(function()
  redis.call('LPUSH', KEYS[3], ARGV[2])
  redis.call('LTRIM', KEYS[3], 0, 199)
end)
return published
)";
}

inline std::string rollback_script() {
    return R"(
if (#ARGV - 8) % 2 ~= 0 then return -2 end
if #ARGV < 10 then return -2 end
local base = tonumber(ARGV[1])
local max_files = tonumber(ARGV[4])
local max_file_bytes = tonumber(ARGV[5])
local max_total_bytes = tonumber(ARGV[6])
local source_version = tonumber(ARGV[8])
if base == nil or max_files == nil or max_file_bytes == nil or
   max_total_bytes == nil or source_version == nil then return -2 end
local file_count = (#ARGV - 8) / 2
if file_count < 1 or file_count > max_files then return -8 end
local total_bytes = 0
for i = 9, #ARGV, 2 do
  local file_bytes = string.len(ARGV[i + 1])
  if file_bytes > max_file_bytes then return -8 end
  total_bytes = total_bytes + file_bytes
  if total_bytes > max_total_bytes then return -8 end
end
local expected = {'string', 'hash', 'list', 'hash', 'hash', 'zset', 'hash', 'hash', 'hash', 'hash'}
for i = 1, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return -3 end
end
local cur = tonumber(redis.call('GET', KEYS[1]) or '0')
if cur == nil then return -4 end
if cur ~= base then return -1 end
local top = redis.call('ZREVRANGE', KEYS[6], 0, 0)
if cur > 0 then
  local current_member = tostring(cur)
  if top[1] == nil or tonumber(top[1]) ~= cur or
     not redis.call('HGET', KEYS[5], current_member) or
     redis.call('EXISTS', KEYS[10]) ~= 1 then return -5 end
elseif top[1] ~= nil or redis.call('ZCARD', KEYS[6]) ~= 0 or
   redis.call('HLEN', KEYS[5]) ~= 0 then
  return -5
end
local source_member = tostring(source_version)
if source_version > cur or redis.call('EXISTS', KEYS[9]) ~= 1 or
   redis.call('HGET', KEYS[5], source_member) ~= ARGV[7] or
   not redis.call('ZSCORE', KEYS[6], source_member) or
   redis.call('HLEN', KEYS[9]) ~= file_count then return -9 end
for i = 9, #ARGV, 2 do
  if redis.call('HGET', KEYS[9], ARGV[i]) ~= ARGV[i + 1] then return -9 end
end
local newv = base + 1
local member = tostring(newv)
if redis.call('EXISTS', KEYS[7]) ~= 0 or
   redis.call('HEXISTS', KEYS[5], member) ~= 0 or
   redis.call('ZSCORE', KEYS[6], member) ~= false then return -6 end
redis.call('DEL', KEYS[8])
redis.call('HSET', KEYS[8], unpack(ARGV, 9, #ARGV))
redis.call('RENAME', KEYS[8], KEYS[7])
redis.call('HSET', KEYS[5], member, ARGV[3])
redis.call('ZADD', KEYS[6], newv, member)
redis.call('DEL', KEYS[4])
redis.call('HSET', KEYS[4], unpack(ARGV, 9, #ARGV))
redis.call('RENAME', KEYS[4], KEYS[2])
local published = redis.call('INCR', KEYS[1])
if published ~= newv then return -7 end
pcall(function()
  redis.call('LPUSH', KEYS[3], ARGV[2])
  redis.call('LTRIM', KEYS[3], 0, 199)
end)
return published
)";
}

inline std::string snapshot_repair_script() {
    return R"(
if (#ARGV - 6) % 2 ~= 0 then return -2 end
if #ARGV < 8 then return -2 end
local version = tonumber(ARGV[1])
local max_files = tonumber(ARGV[4])
local max_file_bytes = tonumber(ARGV[5])
local max_total_bytes = tonumber(ARGV[6])
if version == nil or max_files == nil or max_file_bytes == nil or max_total_bytes == nil then return -2 end
local file_count = (#ARGV - 6) / 2
if file_count < 1 or file_count > max_files then return -8 end
local total_bytes = 0
for i = 7, #ARGV, 2 do
  local file_bytes = string.len(ARGV[i + 1])
  if file_bytes > max_file_bytes then return -8 end
  total_bytes = total_bytes + file_bytes
  if total_bytes > max_total_bytes then return -8 end
end
local expected = {'string', 'hash', 'hash', 'zset', 'hash', 'hash', 'list'}
for i = 1, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return -3 end
end
local cur = tonumber(redis.call('GET', KEYS[1]) or '')
if cur == nil then return -4 end
if cur ~= version then return -1 end
local top = redis.call('ZREVRANGE', KEYS[4], 0, 0)
if top[1] ~= nil then
  local high = tonumber(top[1])
  if high == nil or high > cur then return -5 end
end
local member = tostring(version)
if redis.call('EXISTS', KEYS[5]) ~= 0 then return -6 end
if redis.call('HGET', KEYS[3], member) ~= ARGV[2] or
   not redis.call('ZSCORE', KEYS[4], member) or
   redis.call('HLEN', KEYS[2]) ~= file_count then return -9 end
for i = 7, #ARGV, 2 do
  if redis.call('HGET', KEYS[2], ARGV[i]) ~= ARGV[i + 1] then return -9 end
end
redis.call('DEL', KEYS[6])
redis.call('HSET', KEYS[6], unpack(ARGV, 7, #ARGV))
redis.call('RENAME', KEYS[6], KEYS[5])
pcall(function()
  redis.call('LPUSH', KEYS[7], ARGV[3])
  redis.call('LTRIM', KEYS[7], 0, 199)
end)
return version
)";
}

inline std::string mirror_rebuild_script() {
    return R"(
if (#ARGV - 6) % 2 ~= 0 then return -2 end
if #ARGV < 8 then return -2 end
local version = tonumber(ARGV[1])
local max_files = tonumber(ARGV[4])
local max_file_bytes = tonumber(ARGV[5])
local max_total_bytes = tonumber(ARGV[6])
if version == nil or max_files == nil or max_file_bytes == nil or max_total_bytes == nil then return -2 end
local file_count = (#ARGV - 6) / 2
if file_count < 1 or file_count > max_files then return -8 end
local expected = {'string', 'hash', 'hash', 'zset', 'hash', 'hash', 'list'}
for i = 1, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return -3 end
end
local cur = tonumber(redis.call('GET', KEYS[1]) or '')
if cur == nil then return -4 end
if cur ~= version then return -1 end
local top = redis.call('ZREVRANGE', KEYS[4], 0, 0)
if top[1] ~= nil then
  local high = tonumber(top[1])
  if high == nil or high > cur then return -5 end
end
local member = tostring(version)
if redis.call('EXISTS', KEYS[5]) ~= 1 or
   redis.call('HGET', KEYS[3], member) ~= ARGV[2] or
   not redis.call('ZSCORE', KEYS[4], member) or
   redis.call('HLEN', KEYS[5]) ~= file_count then return -9 end
local total_bytes = 0
for i = 7, #ARGV, 2 do
  local file_bytes = string.len(ARGV[i + 1])
  if file_bytes > max_file_bytes then return -8 end
  total_bytes = total_bytes + file_bytes
  if total_bytes > max_total_bytes then return -8 end
  if redis.call('HGET', KEYS[5], ARGV[i]) ~= ARGV[i + 1] then return -9 end
end
redis.call('DEL', KEYS[6])
redis.call('HSET', KEYS[6], unpack(ARGV, 7, #ARGV))
redis.call('RENAME', KEYS[6], KEYS[2])
pcall(function()
  redis.call('LPUSH', KEYS[7], ARGV[3])
  redis.call('LTRIM', KEYS[7], 0, 199)
end)
return version
)";
}

inline std::string migration_script() {
    return R"(
if (#ARGV - 6) % 2 ~= 0 then return -2 end
if #ARGV < 8 then return -2 end
local version = tonumber(ARGV[1])
local max_files = tonumber(ARGV[4])
local max_file_bytes = tonumber(ARGV[5])
local max_total_bytes = tonumber(ARGV[6])
if version == nil or max_files == nil or max_file_bytes == nil or max_total_bytes == nil then return -2 end
local file_count = (#ARGV - 6) / 2
if file_count < 1 or file_count > max_files then return -8 end
local expected = {'string', 'hash', 'hash', 'zset', 'hash', 'hash', 'list'}
for i = 1, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return -3 end
end
local cur = tonumber(redis.call('GET', KEYS[1]) or '')
if cur == nil then return -4 end
if cur ~= version then return -1 end
local top = redis.call('ZREVRANGE', KEYS[4], 0, 0)
if top[1] ~= nil then
  local high = tonumber(top[1])
  if high == nil or high > cur then return -5 end
end
local member = tostring(version)
if redis.call('ZCARD', KEYS[4]) ~= 0 or redis.call('HLEN', KEYS[3]) ~= 0 or
   redis.call('EXISTS', KEYS[5]) ~= 0 or
   redis.call('HEXISTS', KEYS[3], member) ~= 0 or
   redis.call('ZSCORE', KEYS[4], member) ~= false then return -6 end
if redis.call('HLEN', KEYS[2]) ~= file_count then return -9 end
local total_bytes = 0
for i = 7, #ARGV, 2 do
  local file_bytes = string.len(ARGV[i + 1])
  if file_bytes > max_file_bytes then return -8 end
  total_bytes = total_bytes + file_bytes
  if total_bytes > max_total_bytes then return -8 end
  if redis.call('HGET', KEYS[2], ARGV[i]) ~= ARGV[i + 1] then return -9 end
end
redis.call('DEL', KEYS[6])
redis.call('HSET', KEYS[6], unpack(ARGV, 7, #ARGV))
redis.call('RENAME', KEYS[6], KEYS[5])
redis.call('HSET', KEYS[3], member, ARGV[2])
redis.call('ZADD', KEYS[4], version, member)
pcall(function()
  redis.call('LPUSH', KEYS[7], ARGV[3])
  redis.call('LTRIM', KEYS[7], 0, 199)
end)
return version
)";
}

inline std::string orphan_inspect_script() {
    return R"(
local expected = {'string', 'zset', 'hash', 'hash', 'hash'}
for i = 1, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return {'__error__'} end
end
local current = tonumber(redis.call('GET', KEYS[1]) or '0')
local target = tonumber(ARGV[1])
if current == nil or target == nil or target <= current then return {'__error__'} end
local top = redis.call('ZREVRANGE', KEYS[2], 0, 0)
local index_high = tonumber(top[1] or '0')
if index_high == nil then return {'__error__'} end
local machine_high = 0
for _, value in ipairs(redis.call('HVALS', KEYS[5])) do
  local version = tonumber(string.match(value, '^([^|]+)'))
  if version ~= nil and version > machine_high then machine_high = version end
end
local member = tostring(target)
local indexed = redis.call('ZSCORE', KEYS[2], member) and '1' or '0'
local meta = redis.call('HGET', KEYS[3], member) or '__missing__'
local snapshot_exists = redis.call('EXISTS', KEYS[4]) == 1 and '1' or '0'
local out = {tostring(current), tostring(index_high), tostring(machine_high),
  indexed, meta, snapshot_exists}
if snapshot_exists == '1' then
  for _, value in ipairs(redis.call('HGETALL', KEYS[4])) do table.insert(out, value) end
end
return out
)";
}

inline std::string restore_version_script() {
    return R"(
if (#ARGV - 7) % 2 ~= 0 or #ARGV < 9 then return -2 end
local expected_current = tonumber(ARGV[1])
local target = tonumber(ARGV[2])
local max_files = tonumber(ARGV[5])
local max_file_bytes = tonumber(ARGV[6])
local max_total_bytes = tonumber(ARGV[7])
if expected_current == nil or target == nil or max_files == nil or
   max_file_bytes == nil or max_total_bytes == nil then return -2 end
local expected = {'string', 'hash', 'zset', 'hash', 'hash', 'hash', 'list', 'hash'}
for i = 1, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return -3 end
end
local current = tonumber(redis.call('GET', KEYS[1]) or '0')
if current == nil then return -4 end
if current ~= expected_current then return -1 end
if target <= current then return -5 end
local top = redis.call('ZREVRANGE', KEYS[3], 0, 0)
if tonumber(top[1] or '') ~= target then return -5 end
local member = tostring(target)
local file_count = (#ARGV - 7) / 2
if file_count < 1 or file_count > max_files or
   redis.call('EXISTS', KEYS[5]) ~= 1 or
   redis.call('HGET', KEYS[4], member) ~= ARGV[3] or
   not redis.call('ZSCORE', KEYS[3], member) or
   redis.call('HLEN', KEYS[5]) ~= file_count then return -9 end
local total_bytes = 0
for i = 8, #ARGV, 2 do
  local file_bytes = string.len(ARGV[i + 1])
  total_bytes = total_bytes + file_bytes
  if file_bytes > max_file_bytes or total_bytes > max_total_bytes or
     redis.call('HGET', KEYS[5], ARGV[i]) ~= ARGV[i + 1] then return -9 end
end
for _, value in ipairs(redis.call('HVALS', KEYS[8])) do
  local version = tonumber(string.match(value, '^([^|]+)'))
  if version ~= nil and version > target then return -5 end
end
redis.call('DEL', KEYS[6])
redis.call('HSET', KEYS[6], unpack(ARGV, 8, #ARGV))
redis.call('RENAME', KEYS[6], KEYS[2])
redis.call('SET', KEYS[1], target)
pcall(function()
  redis.call('LPUSH', KEYS[7], ARGV[4])
  redis.call('LTRIM', KEYS[7], 0, 199)
end)
return target
)";
}

inline std::string delete_orphan_script() {
    return R"(
if (#ARGV - 6) % 2 ~= 0 then return -2 end
local expected_current = tonumber(ARGV[1])
local target = tonumber(ARGV[2])
local expected_indexed = ARGV[4]
local expected_snapshot = ARGV[5]
if expected_current == nil or target == nil or
   (expected_indexed ~= '0' and expected_indexed ~= '1') or
   (expected_snapshot ~= '0' and expected_snapshot ~= '1') then return -2 end
local expected = {'string', 'zset', 'hash', 'hash', 'list', 'hash'}
for i = 1, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return -3 end
end
local current = tonumber(redis.call('GET', KEYS[1]) or '0')
if current == nil then return -4 end
if current ~= expected_current then return -1 end
if target ~= current + 1 then return -5 end
for _, value in ipairs(redis.call('HVALS', KEYS[6])) do
  local version = tonumber(string.match(value, '^([^|]+)'))
  if version ~= nil and version > current then return -5 end
end
local member = tostring(target)
local indexed = redis.call('ZSCORE', KEYS[2], member) and '1' or '0'
local meta = redis.call('HGET', KEYS[3], member) or '__missing__'
local snapshot = redis.call('EXISTS', KEYS[4]) == 1 and '1' or '0'
if indexed ~= expected_indexed or meta ~= ARGV[3] or snapshot ~= expected_snapshot then
  return -9
end
if indexed == '0' and meta == '__missing__' and snapshot == '0' then return -9 end
local file_count = (#ARGV - 6) / 2
if snapshot == '1' then
  if file_count < 1 or redis.call('HLEN', KEYS[4]) ~= file_count then return -9 end
  for i = 7, #ARGV, 2 do
    if redis.call('HGET', KEYS[4], ARGV[i]) ~= ARGV[i + 1] then return -9 end
  end
elseif file_count ~= 0 then return -9 end
redis.call('UNLINK', KEYS[4])
redis.call('HDEL', KEYS[3], member)
redis.call('ZREM', KEYS[2], member)
pcall(function()
  redis.call('LPUSH', KEYS[5], ARGV[6])
  redis.call('LTRIM', KEYS[5], 0, 199)
end)
return 1
)";
}

inline std::string seed_script() {
    return R"(
if (#ARGV - 4) % 2 ~= 0 then return -2 end
local max_files = tonumber(ARGV[2])
local max_file_bytes = tonumber(ARGV[3])
local max_total_bytes = tonumber(ARGV[4])
if max_files == nil or max_file_bytes == nil or max_total_bytes == nil then return -2 end
if redis.call('TYPE', KEYS[1]).ok ~= 'none' then return 0 end
local expected = {'none', 'hash', 'hash', 'hash', 'zset', 'hash', 'hash'}
for i = 2, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return -3 end
end
local file_count = (#ARGV - 4) / 2
if file_count > max_files then return -8 end
local total_bytes = 0
for i = 5, #ARGV, 2 do
  local file_bytes = string.len(ARGV[i + 1])
  if file_bytes > max_file_bytes then return -8 end
  total_bytes = total_bytes + file_bytes
  if total_bytes > max_total_bytes then return -8 end
end
if file_count == 0 then
  if redis.call('ZCARD', KEYS[5]) ~= 0 or
     redis.call('HLEN', KEYS[4]) ~= 0 or
     redis.call('EXISTS', KEYS[6]) ~= 0 then return -6 end
  redis.call('DEL', KEYS[2])
  redis.call('SET', KEYS[1], 1)
  return 1
end
if redis.call('ZCARD', KEYS[5]) ~= 0 or
   redis.call('HLEN', KEYS[4]) ~= 0 or
   redis.call('EXISTS', KEYS[6]) ~= 0 then return -6 end
redis.call('DEL', KEYS[7])
redis.call('HSET', KEYS[7], unpack(ARGV, 5, #ARGV))
redis.call('RENAME', KEYS[7], KEYS[6])
redis.call('HSET', KEYS[4], '1', ARGV[1])
redis.call('ZADD', KEYS[5], 1, '1')
redis.call('DEL', KEYS[3])
redis.call('HSET', KEYS[3], unpack(ARGV, 5, #ARGV))
redis.call('RENAME', KEYS[3], KEYS[2])
redis.call('SET', KEYS[1], 1)
return 1
)";
}

}  // namespace config_history

#ifdef CONFIG_HISTORY_OUT_OF_LINE
#undef inline
#endif
