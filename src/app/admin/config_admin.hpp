#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config_admin_types.hpp"
#include "../app_config.hpp"
#include "../../db/redis_pool.hpp"
#include "../../security/jwt_auth.hpp"
#include "../../security/principal.hpp"

namespace config_admin {

inline constexpr std::string_view kVersionKey = "asio_owen:config:version";
inline constexpr std::string_view kFilesKey = "asio_owen:config:files";
inline constexpr std::string_view kAuditKey = "asio_owen:config:audit";
inline constexpr std::string_view kStagingKey = "asio_owen:config:files:staging";
inline constexpr std::string_view kMachinesKey = "asio_owen:config:machines";
inline constexpr std::string_view kAdminIssuer = "asio-owen-admin";
inline constexpr size_t kMaxLoginFieldBytes = 256;

ParseResult parse_save_request(std::string_view body);
LoginParseResult parse_login_request(std::string_view body);
RollbackParseResult parse_rollback_request(std::string_view body);
RepairParseResult parse_repair_request(std::string_view body);
OrphanResolutionParseResult parse_orphan_resolution_request(std::string_view body);
std::optional<int64_t> parse_int64(std::string_view value);

std::string base64url_encode(const unsigned char* data, size_t len);
std::string base64url_encode(std::string_view data);
std::optional<std::vector<unsigned char>> base64url_decode(std::string_view input);
std::optional<std::vector<unsigned char>> pbkdf2_sha256(
    std::string_view password, const std::vector<unsigned char>& salt,
    int iterations, size_t output_len);
std::string pbkdf2_sha256_hash_string(
    std::string_view password, const std::vector<unsigned char>& salt,
    int iterations = 100000, size_t output_len = 32);
bool split_password_hash(std::string_view encoded, int& iterations,
    std::vector<unsigned char>& salt, std::vector<unsigned char>& expected);
bool verify_admin_password(const AdminConfig& admin,
    const std::string& username, const std::string& password);

std::optional<std::string> load_pem_or_file(
    const std::string& value, const std::filesystem::path& base = {});
std::optional<AdminConfig> load_local_admin_config(
    const std::filesystem::path& config_base, std::string* error = nullptr);
bool admin_configured(const AdminConfig& admin,
    const std::filesystem::path& base = {});

std::optional<IssuedToken> issue_admin_token_with_pem(
    const AdminConfig& admin, const std::string& username,
    std::string_view private_key_pem);
std::optional<IssuedToken> issue_admin_token(
    const AdminConfig& admin, const std::string& username,
    const std::filesystem::path& base = {});
std::optional<Principal> verify_admin_token_with_auth(
    const AdminConfig& admin, const std::string& auth_header, const JWTAuth& auth);
std::optional<Principal> verify_admin_token_with_pem(
    const AdminConfig& admin, const std::string& auth_header,
    std::string_view public_key_pem);
std::optional<Principal> verify_admin_token(
    const AdminConfig& admin, const std::string& auth_header,
    const std::filesystem::path& base = {});

std::optional<int64_t> redis_integer(const RedisPool::Reply& reply);
std::optional<int64_t> redis_version(const RedisPool::Reply& reply);
std::optional<std::map<std::string, std::string>> parse_hgetall(
    const RedisPool::Reply& reply);
bool restart_required(const std::string& content);
std::optional<std::string> validate_file_set(
    const std::vector<ManagedFile>& files, bool reject_empty);
std::optional<std::string> dry_run_config_set(
    const std::filesystem::path& config_base,
    const std::vector<ManagedFile>& files);
std::string files_json(int64_t version,
    const std::map<std::string, std::string>& files, bool degraded = false);
std::string machines_json(const std::map<std::string, std::string>& machines);
std::string audit_json(const Principal* principal, int64_t base_version,
    int64_t new_version, std::string_view action, std::string_view reason,
    const std::vector<ManagedFile>& files);
std::string save_script();
std::string admin_login_html();
std::string admin_settings_html();

}  // namespace config_admin
