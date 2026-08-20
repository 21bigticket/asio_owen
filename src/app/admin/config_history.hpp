#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../app_config.hpp"

namespace config_history {

inline constexpr std::string_view kMetaKey = "asio_owen:config:history:meta";
inline constexpr std::string_view kIndexKey = "asio_owen:config:history:index";
inline constexpr std::string_view kSnapshotPrefix = "asio_owen:config:history:";
inline constexpr std::string_view kSnapshotStagingKey = "asio_owen:config:history:staging";

struct SnapshotInfo {
    size_t file_count = 0;
    size_t total_bytes = 0;
    std::string content_sha256;
};

struct SnapshotRecord {
    int64_t current_version = 0;
    std::string meta_json;
    std::map<std::string, std::string> files;
};

std::optional<std::string> content_sha256(
    const std::map<std::string, std::string>& files);
std::optional<std::string> validate_snapshot(
    const std::map<std::string, std::string>& files,
    const ConfigHistoryConfig& cfg, SnapshotInfo& info,
    bool reject_empty = true);
std::string snapshot_key(int64_t version);
std::string metadata_json(
    int64_t version, int64_t base_version, int64_t timestamp,
    std::string_view user, std::string_view action, std::string_view reason,
    const SnapshotInfo& info,
    std::optional<int64_t> rollback_from = std::nullopt);
std::optional<std::string> json_string_field(
    std::string_view json, std::string_view field);
std::optional<int64_t> parse_int64(std::string_view value);
std::optional<SnapshotRecord> parse_detail_elements(
    const std::vector<std::string>& elements);
std::string history_list_json(
    const std::vector<std::string>& elements, int64_t current_version);
std::string detail_json(int64_t version, const SnapshotRecord& record);
std::optional<std::string> diff_json(
    int64_t from_version, const SnapshotRecord& from,
    int64_t to_version, const SnapshotRecord& to, size_t max_bytes);
bool looks_sensitive(const std::string& content);

std::string list_script();
std::string detail_script();
std::string save_script();
std::string rollback_script();
std::string snapshot_repair_script();
std::string mirror_rebuild_script();
std::string migration_script();
std::string orphan_inspect_script();
std::string restore_version_script();
std::string delete_orphan_script();
std::string seed_script();

}  // namespace config_history
