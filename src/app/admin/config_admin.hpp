#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include "../../common/config.hpp"
#include "../../db/redis_pool.hpp"
#include "../../http/response.hpp"
#include "../../http/upstream_manager.hpp"
#include "../../security/jwt_auth.hpp"
#include "../../security/principal.hpp"
#include "../../security/security_rules.hpp"
#include "../app_config.hpp"
#include "../config_sync_service.hpp"
#include "config_history.hpp"
#include "generated/admin_login_html.hpp"
#include "generated/admin_settings_html.hpp"

namespace config_admin {

inline constexpr std::string_view kVersionKey = "asio_owen:config:version";
inline constexpr std::string_view kFilesKey = "asio_owen:config:files";
inline constexpr std::string_view kAuditKey = "asio_owen:config:audit";
inline constexpr std::string_view kStagingKey = "asio_owen:config:files:staging";
inline constexpr std::string_view kMachinesKey = "asio_owen:config:machines";
inline constexpr std::string_view kAdminIssuer = "asio-owen-admin";
inline constexpr size_t kMaxLoginFieldBytes = 256;

struct ManagedFile {
    std::string name;
    std::string content;
};

struct SaveRequest {
    int64_t base_version = 0;
    std::string reason;
    std::vector<ManagedFile> files;
};

struct ParseResult {
    bool ok = false;
    SaveRequest request;
    std::string error;
};

struct LoginRequest {
    std::string username;
    std::string password;
};

struct LoginParseResult {
    bool ok = false;
    LoginRequest request;
    std::string error;
};

struct RollbackRequest {
    int64_t base_version = 0;
    int64_t target_version = 0;
    std::string reason;
};

struct RollbackParseResult {
    bool ok = false;
    RollbackRequest request;
    std::string error;
};

struct RepairRequest {
    int64_t version = 0;
    std::string reason;
};

struct RepairParseResult {
    bool ok = false;
    RepairRequest request;
    std::string error;
};

struct OrphanResolutionRequest {
    int64_t current_version = 0;
    int64_t target_version = 0;
    std::string action;
    std::string reason;
    std::string confirmation;
};

struct OrphanResolutionParseResult {
    bool ok = false;
    OrphanResolutionRequest request;
    std::string error;
};

class JsonReader {
public:
    explicit JsonReader(std::string_view input) : input_(input) {}

    LoginParseResult parse_login_request() {
        LoginParseResult result;
        if (!consume('{')) return login_fail("expected object");
        bool saw_username = false;
        bool saw_password = false;
        while (true) {
            skip_ws();
            if (consume('}')) break;
            auto key = parse_string();
            if (!key) return login_fail("expected object key");
            if (!consume(':')) return login_fail("expected ':'");
            if (*key == "username") {
                auto value = parse_string();
                if (!value || value->empty() ||
                    value->size() > kMaxLoginFieldBytes) {
                    return login_fail("invalid username");
                }
                result.request.username = std::move(*value);
                saw_username = true;
            } else if (*key == "password") {
                auto value = parse_string();
                if (!value || value->empty() ||
                    value->size() > kMaxLoginFieldBytes) {
                    return login_fail("invalid password");
                }
                result.request.password = std::move(*value);
                saw_password = true;
            } else if (!skip_value()) {
                return login_fail("invalid JSON value");
            }
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) break;
            return login_fail("expected ',' or '}'");
        }
        skip_ws();
        if (pos_ != input_.size()) return login_fail("trailing data");
        if (!saw_username) return login_fail("missing username");
        if (!saw_password) return login_fail("missing password");
        result.ok = true;
        return result;
    }

    ParseResult parse_save_request() {
        ParseResult result;
        if (!consume('{')) return fail("expected object");
        bool saw_base_version = false;
        bool saw_files = false;
        while (true) {
            skip_ws();
            if (consume('}')) break;
            auto key = parse_string();
            if (!key) return fail("expected object key");
            if (!consume(':')) return fail("expected ':'");
            if (*key == "base_version") {
                auto value = parse_int64();
                if (!value || *value < 0) return fail("invalid base_version");
                result.request.base_version = *value;
                saw_base_version = true;
            } else if (*key == "reason") {
                auto value = parse_string();
                if (!value) return fail("invalid reason");
                result.request.reason = std::move(*value);
            } else if (*key == "files") {
                auto files = parse_files_array();
                if (!files) return fail("invalid files array");
                result.request.files = std::move(*files);
                saw_files = true;
            } else if (!skip_value()) {
                return fail("invalid JSON value");
            }
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) break;
            return fail("expected ',' or '}'");
        }
        skip_ws();
        if (pos_ != input_.size()) return fail("trailing data");
        if (!saw_base_version) return fail("missing base_version");
        if (!saw_files) return fail("missing files");
        result.ok = true;
        return result;
    }

    RollbackParseResult parse_rollback_request() {
        RollbackParseResult result;
        if (!consume('{')) return rollback_fail("expected object");
        bool saw_base = false;
        bool saw_target = false;
        bool saw_reason = false;
        while (true) {
            skip_ws();
            if (consume('}')) break;
            auto key = parse_string();
            if (!key) return rollback_fail("expected object key");
            if (!consume(':')) return rollback_fail("expected ':'");
            if (*key == "base_version") {
                auto value = parse_int64();
                if (!value || *value < 0) {
                    return rollback_fail("invalid base_version");
                }
                result.request.base_version = *value;
                saw_base = true;
            } else if (*key == "target_version") {
                auto value = parse_int64();
                if (!value || *value <= 0) {
                    return rollback_fail("invalid target_version");
                }
                result.request.target_version = *value;
                saw_target = true;
            } else if (*key == "reason") {
                auto value = parse_string();
                if (!value || value->empty()) {
                    return rollback_fail("reason is required");
                }
                result.request.reason = std::move(*value);
                saw_reason = true;
            } else if (!skip_value()) {
                return rollback_fail("invalid JSON value");
            }
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) break;
            return rollback_fail("expected ',' or '}'");
        }
        skip_ws();
        if (pos_ != input_.size()) return rollback_fail("trailing data");
        if (!saw_base) return rollback_fail("missing base_version");
        if (!saw_target) return rollback_fail("missing target_version");
        if (!saw_reason) return rollback_fail("missing reason");
        result.ok = true;
        return result;
    }

    RepairParseResult parse_repair_request() {
        RepairParseResult result;
        if (!consume('{')) return repair_fail("expected object");
        bool saw_version = false;
        bool saw_reason = false;
        while (true) {
            skip_ws();
            if (consume('}')) break;
            auto key = parse_string();
            if (!key) return repair_fail("expected object key");
            if (!consume(':')) return repair_fail("expected ':'");
            if (*key == "version") {
                auto value = parse_int64();
                if (!value || *value <= 0) return repair_fail("invalid version");
                result.request.version = *value;
                saw_version = true;
            } else if (*key == "reason") {
                auto value = parse_string();
                if (!value || value->empty()) return repair_fail("reason is required");
                result.request.reason = std::move(*value);
                saw_reason = true;
            } else if (!skip_value()) {
                return repair_fail("invalid JSON value");
            }
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) break;
            return repair_fail("expected ',' or '}'");
        }
        skip_ws();
        if (pos_ != input_.size()) return repair_fail("trailing data");
        if (!saw_version) return repair_fail("missing version");
        if (!saw_reason) return repair_fail("missing reason");
        result.ok = true;
        return result;
    }

    OrphanResolutionParseResult parse_orphan_resolution_request() {
        OrphanResolutionParseResult result;
        if (!consume('{')) return orphan_fail("expected object");
        bool saw_current = false;
        bool saw_target = false;
        bool saw_action = false;
        bool saw_reason = false;
        bool saw_confirmation = false;
        while (true) {
            skip_ws();
            if (consume('}')) break;
            auto key = parse_string();
            if (!key) return orphan_fail("expected object key");
            if (!consume(':')) return orphan_fail("expected ':'");
            if (*key == "current_version") {
                auto value = parse_int64();
                if (!value || *value < 0) return orphan_fail("invalid current_version");
                result.request.current_version = *value;
                saw_current = true;
            } else if (*key == "target_version") {
                auto value = parse_int64();
                if (!value || *value <= 0) return orphan_fail("invalid target_version");
                result.request.target_version = *value;
                saw_target = true;
            } else if (*key == "action") {
                auto value = parse_string();
                if (!value || (*value != "restore-version" &&
                               *value != "delete-orphan")) {
                    return orphan_fail("invalid action");
                }
                result.request.action = std::move(*value);
                saw_action = true;
            } else if (*key == "reason") {
                auto value = parse_string();
                if (!value || value->empty()) return orphan_fail("reason is required");
                result.request.reason = std::move(*value);
                saw_reason = true;
            } else if (*key == "confirmation") {
                auto value = parse_string();
                if (!value || value->empty()) {
                    return orphan_fail("confirmation is required");
                }
                result.request.confirmation = std::move(*value);
                saw_confirmation = true;
            } else if (!skip_value()) {
                return orphan_fail("invalid JSON value");
            }
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) break;
            return orphan_fail("expected ',' or '}'");
        }
        skip_ws();
        if (pos_ != input_.size()) return orphan_fail("trailing data");
        if (!saw_current || !saw_target || !saw_action ||
            !saw_reason || !saw_confirmation) {
            return orphan_fail("missing orphan resolution field");
        }
        const std::string expected = result.request.action == "restore-version" ?
            "RESTORE_VERSION_POINTER" : "DELETE_UNPUBLISHED_ORPHAN";
        if (result.request.confirmation != expected) {
            return orphan_fail("confirmation text does not match action");
        }
        result.ok = true;
        return result;
    }

private:
    LoginParseResult login_fail(std::string message) const {
        LoginParseResult result;
        result.error = std::move(message);
        return result;
    }

    ParseResult fail(std::string message) const {
        ParseResult result;
        result.error = std::move(message);
        return result;
    }

    RollbackParseResult rollback_fail(std::string message) const {
        RollbackParseResult result;
        result.error = std::move(message);
        return result;
    }

    RepairParseResult repair_fail(std::string message) const {
        RepairParseResult result;
        result.error = std::move(message);
        return result;
    }

    OrphanResolutionParseResult orphan_fail(std::string message) const {
        OrphanResolutionParseResult result;
        result.error = std::move(message);
        return result;
    }

    void skip_ws() {
        while (pos_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[pos_]))) {
            ++pos_;
        }
    }

    bool consume(char expected) {
        skip_ws();
        if (pos_ >= input_.size() || input_[pos_] != expected) return false;
        ++pos_;
        return true;
    }

    static int hex_value(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp <= 0x7f) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }

    std::optional<std::string> parse_string() {
        skip_ws();
        if (pos_ >= input_.size() || input_[pos_] != '"') return std::nullopt;
        ++pos_;
        std::string out;
        while (pos_ < input_.size()) {
            char c = input_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) return std::nullopt;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= input_.size()) return std::nullopt;
            char esc = input_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > input_.size()) return std::nullopt;
                    uint32_t cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        int hv = hex_value(input_[pos_++]);
                        if (hv < 0) return std::nullopt;
                        cp = (cp << 4) | static_cast<uint32_t>(hv);
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<int64_t> parse_int64() {
        skip_ws();
        auto begin = pos_;
        if (pos_ < input_.size() && input_[pos_] == '-') ++pos_;
        auto digits = pos_;
        while (pos_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
            ++pos_;
        }
        if (digits == pos_) return std::nullopt;
        int64_t value = 0;
        auto [end, ec] = std::from_chars(
            input_.data() + begin, input_.data() + pos_, value);
        if (ec != std::errc{} || end != input_.data() + pos_) return std::nullopt;
        return value;
    }

    std::optional<std::vector<ManagedFile>> parse_files_array() {
        if (!consume('[')) return std::nullopt;
        std::vector<ManagedFile> files;
        while (true) {
            skip_ws();
            if (consume(']')) return files;
            auto file = parse_file_object();
            if (!file) return std::nullopt;
            files.push_back(std::move(*file));
            skip_ws();
            if (consume(',')) continue;
            if (consume(']')) return files;
            return std::nullopt;
        }
    }

    std::optional<ManagedFile> parse_file_object() {
        if (!consume('{')) return std::nullopt;
        ManagedFile file;
        bool saw_name = false;
        bool saw_content = false;
        while (true) {
            skip_ws();
            if (consume('}')) break;
            auto key = parse_string();
            if (!key) return std::nullopt;
            if (!consume(':')) return std::nullopt;
            if (*key == "name") {
                auto value = parse_string();
                if (!value) return std::nullopt;
                file.name = std::move(*value);
                saw_name = true;
            } else if (*key == "content") {
                auto value = parse_string();
                if (!value) return std::nullopt;
                file.content = std::move(*value);
                saw_content = true;
            } else if (!skip_value()) {
                return std::nullopt;
            }
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) break;
            return std::nullopt;
        }
        if (!saw_name || !saw_content) return std::nullopt;
        return file;
    }

    bool skip_value() {
        skip_ws();
        if (pos_ >= input_.size()) return false;
        char c = input_[pos_];
        if (c == '"') return parse_string().has_value();
        if (c == '{') {
            ++pos_;
            skip_ws();
            if (consume('}')) return true;
            while (true) {
                if (!parse_string()) return false;
                if (!consume(':')) return false;
                if (!skip_value()) return false;
                skip_ws();
                if (consume(',')) continue;
                return consume('}');
            }
        }
        if (c == '[') {
            ++pos_;
            skip_ws();
            if (consume(']')) return true;
            while (true) {
                if (!skip_value()) return false;
                skip_ws();
                if (consume(',')) continue;
                return consume(']');
            }
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-') {
            auto begin = pos_;
            if (c == '-') ++pos_;
            while (pos_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                ++pos_;
            }
            if (begin == pos_ || (input_[begin] == '-' && begin + 1 == pos_)) return false;
            if (pos_ < input_.size() && input_[pos_] == '.') {
                ++pos_;
                auto frac = pos_;
                while (pos_ < input_.size() &&
                       std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                    ++pos_;
                }
                if (frac == pos_) return false;
            }
            return true;
        }
        if (input_.substr(pos_, 4) == "true") {
            pos_ += 4;
            return true;
        }
        if (input_.substr(pos_, 5) == "false") {
            pos_ += 5;
            return true;
        }
        if (input_.substr(pos_, 4) == "null") {
            pos_ += 4;
            return true;
        }
        return false;
    }

    std::string_view input_;
    size_t pos_ = 0;
};

inline ParseResult parse_save_request(std::string_view body) {
    return JsonReader(body).parse_save_request();
}

inline LoginParseResult parse_login_request(std::string_view body) {
    return JsonReader(body).parse_login_request();
}

inline RollbackParseResult parse_rollback_request(std::string_view body) {
    return JsonReader(body).parse_rollback_request();
}

inline RepairParseResult parse_repair_request(std::string_view body) {
    return JsonReader(body).parse_repair_request();
}

inline OrphanResolutionParseResult parse_orphan_resolution_request(
    std::string_view body) {
    return JsonReader(body).parse_orphan_resolution_request();
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

inline std::string base64url_encode(const unsigned char* data, size_t len) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len * 4 + 2) / 3);
    uint32_t bits = 0;
    int bit_count = 0;
    for (size_t i = 0; i < len; ++i) {
        bits = (bits << 8) | data[i];
        bit_count += 8;
        while (bit_count >= 6) {
            bit_count -= 6;
            out.push_back(alphabet[(bits >> bit_count) & 0x3f]);
        }
    }
    if (bit_count > 0) {
        out.push_back(alphabet[(bits << (6 - bit_count)) & 0x3f]);
    }
    return out;
}

inline std::string base64url_encode(std::string_view data) {
    return base64url_encode(
        reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

inline std::optional<std::vector<unsigned char>> base64url_decode(std::string_view input) {
    std::vector<unsigned char> out;
    out.reserve(input.size() * 3 / 4);
    uint32_t bits = 0;
    int bit_count = 0;
    for (char c : input) {
        if (c == '=') break;
        int value = -1;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '-') value = 62;
        else if (c == '_') value = 63;
        else return std::nullopt;
        bits = (bits << 6) | static_cast<uint32_t>(value);
        bit_count += 6;
        if (bit_count >= 8) {
            bit_count -= 8;
            out.push_back(static_cast<unsigned char>((bits >> bit_count) & 0xff));
        }
    }
    return out;
}

inline std::optional<std::vector<unsigned char>> pbkdf2_sha256(
    std::string_view password,
    const std::vector<unsigned char>& salt,
    int iterations,
    size_t output_len) {
    if (iterations <= 0 || iterations > 10000000 || output_len == 0) {
        return std::nullopt;
    }
    std::vector<unsigned char> out(output_len);
    if (PKCS5_PBKDF2_HMAC(
            password.data(), static_cast<int>(password.size()),
            salt.data(), static_cast<int>(salt.size()),
            iterations, EVP_sha256(), static_cast<int>(out.size()),
            out.data()) != 1) {
        return std::nullopt;
    }
    return out;
}

inline std::string pbkdf2_sha256_hash_string(
    std::string_view password,
    const std::vector<unsigned char>& salt,
    int iterations = 100000,
    size_t output_len = 32) {
    auto hash = pbkdf2_sha256(password, salt, iterations, output_len);
    if (!hash) return {};
    return "pbkdf2_sha256$" + std::to_string(iterations) + "$" +
        base64url_encode(salt.data(), salt.size()) + "$" +
        base64url_encode(hash->data(), hash->size());
}

inline bool split_password_hash(
    std::string_view encoded,
    int& iterations,
    std::vector<unsigned char>& salt,
    std::vector<unsigned char>& expected) {
    std::array<std::string_view, 4> parts{};
    size_t pos = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        auto next = encoded.find('$', pos);
        if (i + 1 == parts.size()) {
            if (next != std::string_view::npos) return false;
            parts[i] = encoded.substr(pos);
        } else {
            if (next == std::string_view::npos) return false;
            parts[i] = encoded.substr(pos, next - pos);
            pos = next + 1;
        }
    }
    if (parts[0] != "pbkdf2_sha256") return false;
    auto parsed_iter = parse_int64(parts[1]);
    if (!parsed_iter || *parsed_iter <= 0 || *parsed_iter > 10000000) {
        return false;
    }
    auto parsed_salt = base64url_decode(parts[2]);
    auto parsed_hash = base64url_decode(parts[3]);
    if (!parsed_salt || !parsed_hash || parsed_salt->empty() ||
        parsed_hash->empty()) {
        return false;
    }
    iterations = static_cast<int>(*parsed_iter);
    salt = std::move(*parsed_salt);
    expected = std::move(*parsed_hash);
    return true;
}

inline bool verify_admin_password(
    const AdminConfig& admin,
    const std::string& username,
    const std::string& password) {
    const AdminAccountConfig* account = nullptr;
    for (const auto& item : admin.accounts) {
        if (item.username == username) {
            account = &item;
            break;
        }
    }

    int iterations = 100000;
    std::vector<unsigned char> salt{
        'a', 's', 'i', 'o', '_', 'o', 'w', 'e', 'n', '_',
        'a', 'd', 'm', 'i', 'n', '_', 'd', 'u', 'm', 'm', 'y'
    };
    std::vector<unsigned char> expected(32, 0);
    bool valid_account_hash = false;
    if (account) {
        valid_account_hash = split_password_hash(
            account->password_hash, iterations, salt, expected);
    }
    auto actual = pbkdf2_sha256(password, salt, iterations, expected.size());
    if (!actual || actual->size() != expected.size()) return false;
    bool matched =
        CRYPTO_memcmp(actual->data(), expected.data(), expected.size()) == 0;
    return valid_account_hash && matched;
}

inline std::optional<std::string> load_pem_or_file(
    const std::string& value,
    const std::filesystem::path& base = {}) {
    if (value.empty()) return std::nullopt;
    if (value.find("-----BEGIN") != std::string::npos) return value;
    std::filesystem::path path(value);
    if (!base.empty() && path.is_relative()) {
        path = base / path;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    auto loaded = ss.str();
    if (loaded.find("-----BEGIN") == std::string::npos) return std::nullopt;
    return loaded;
}

inline std::optional<AdminConfig> load_local_admin_config(
    const std::filesystem::path& config_base,
    std::string* error = nullptr) {
    if (config_base.empty()) {
        if (error) *error = "config base is unavailable";
        return std::nullopt;
    }

    Config cfg;
    constexpr std::array<std::string_view, 2> local_files = {
        "12-config-sync.ini", "99-local.ini"
    };
    bool loaded_any = false;
    for (auto name : local_files) {
        auto path = config_base / "config.d" / std::string(name);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            continue;
        }
        if (!cfg.load_file(path)) {
            if (error) *error = "failed to parse " + std::string(name);
            return std::nullopt;
        }
        loaded_any = true;
    }
    if (!loaded_any) {
        if (error) *error = "local admin config not found";
        return std::nullopt;
    }

    try {
        return admin_config_from(cfg);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

inline bool admin_configured(
    const AdminConfig& admin,
    const std::filesystem::path& base = {}) {
    return !admin.accounts.empty() &&
           load_pem_or_file(admin.jwt_private_key, base).has_value() &&
           load_pem_or_file(admin.jwt_public_key, base).has_value();
}

struct PkeyFree {
    void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};

inline std::optional<std::string> sign_rs256(
    std::string_view data,
    const std::string& private_key_pem) {
    std::unique_ptr<BIO, detail::BioFree> bio(
        BIO_new_mem_buf(private_key_pem.data(),
            static_cast<int>(private_key_pem.size())));
    if (!bio) return std::nullopt;
    std::unique_ptr<EVP_PKEY, PkeyFree> pkey(
        PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
    if (!pkey) return std::nullopt;

    std::unique_ptr<EVP_MD_CTX, detail::MdCtxFree> ctx(EVP_MD_CTX_new());
    if (!ctx) return std::nullopt;
    if (EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr,
            pkey.get()) != 1 ||
        EVP_DigestSignUpdate(ctx.get(), data.data(), data.size()) != 1) {
        return std::nullopt;
    }
    size_t sig_len = 0;
    if (EVP_DigestSignFinal(ctx.get(), nullptr, &sig_len) != 1 ||
        sig_len == 0) {
        return std::nullopt;
    }
    std::string signature(sig_len, '\0');
    if (EVP_DigestSignFinal(ctx.get(),
            reinterpret_cast<unsigned char*>(signature.data()),
            &sig_len) != 1) {
        return std::nullopt;
    }
    signature.resize(sig_len);
    return signature;
}

struct IssuedToken {
    std::string token;
    int expires_in = 0;
};

inline std::optional<std::string> admin_auth_version(
    const AdminConfig& admin,
    std::string_view username) {
    const AdminAccountConfig* account = nullptr;
    for (const auto& item : admin.accounts) {
        if (item.username == username) {
            account = &item;
            break;
        }
    }
    if (!account || account->password_hash.empty()) return std::nullopt;
    const std::string material =
        std::string(username) + "\n" + account->password_hash;
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;
    if (EVP_Digest(material.data(), material.size(), digest.data(),
            &digest_len, EVP_sha256(), nullptr) != 1 || digest_len == 0) {
        return std::nullopt;
    }
    return base64url_encode(digest.data(), digest_len);
}

inline std::optional<IssuedToken> issue_admin_token(
    const AdminConfig& admin,
    const std::string& username,
    const std::filesystem::path& base = {}) {
    auto private_key = load_pem_or_file(admin.jwt_private_key, base);
    if (!private_key) return std::nullopt;
    auto auth_version = admin_auth_version(admin, username);
    if (!auth_version) return std::nullopt;
    const int ttl_sec = std::max(1, admin.token_ttl_min) * 60;
    const auto now = static_cast<int64_t>(std::time(nullptr));
    const auto exp = now + ttl_sec;
    const std::string header = R"({"alg":"RS256","typ":"JWT"})";
    std::ostringstream payload;
    payload << "{\"iss\":\"" << kAdminIssuer
            << "\",\"sub\":\"" << json_escape(username)
            << "\",\"name\":\"" << json_escape(username)
            << "\",\"roles\":[\"admin\"],\"av\":\""
            << *auth_version << "\",\"iat\":" << now
            << ",\"exp\":" << exp << "}";
    auto signing_input = base64url_encode(header) + "." +
        base64url_encode(payload.str());
    auto signature = sign_rs256(signing_input, *private_key);
    if (!signature) return std::nullopt;
    return IssuedToken{
        .token = signing_input + "." + base64url_encode(*signature),
        .expires_in = ttl_sec
    };
}

inline std::optional<Principal> verify_admin_token(
    const AdminConfig& admin,
    const std::string& auth_header,
    const std::filesystem::path& base = {}) {
    auto public_key = load_pem_or_file(admin.jwt_public_key, base);
    if (!public_key) return std::nullopt;
    try {
        JWTAuth auth("admin-rs256-unused-secret",
            std::string(kAdminIssuer), "RS256", *public_key);
        auto claims = auth.verify(auth_header);
        if (!claims) return std::nullopt;
        const std::string username = claims->username.empty() ?
            claims->subject : claims->username;
        auto expected_auth_version = admin_auth_version(admin, username);
        if (!expected_auth_version) return std::nullopt;
        static constexpr std::string_view bearer = "Bearer ";
        if (auth_header.size() <= bearer.size() ||
            !std::equal(bearer.begin(), bearer.end(), auth_header.begin(),
                [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                        std::tolower(static_cast<unsigned char>(b));
                })) {
            return std::nullopt;
        }
        auto decoded = jwt::decode(auth_header.substr(bearer.size()));
        if (!decoded.has_payload_claim("av") ||
            decoded.get_payload_claim("av").as_string() !=
                *expected_auth_version) {
            return std::nullopt;
        }
        Principal principal{
            .subject = claims->subject,
            .username = claims->username,
            .roles = claims->roles
        };
        return principal;
    } catch (const std::exception& e) {
        LOG_WARN("admin JWT verifier unavailable: ", e.what());
        return std::nullopt;
    }
}

inline std::optional<int64_t> redis_integer(const RedisPool::Reply& reply) {
    if (reply.type == "integer") return reply.integer;
    if (reply.type == "string") return parse_int64(reply.str);
    return std::nullopt;
}

inline std::optional<int64_t> redis_version(const RedisPool::Reply& reply) {
    if (!reply.ok) return std::nullopt;
    if (reply.type == "nil") return 0;
    auto parsed = redis_integer(reply);
    if (!parsed || *parsed < 0) return std::nullopt;
    return parsed;
}

inline std::optional<std::map<std::string, std::string>> parse_hgetall(
    const RedisPool::Reply& reply) {
    if (!reply.ok || reply.type != "array" || reply.elements.size() % 2 != 0) {
        return std::nullopt;
    }
    std::map<std::string, std::string> result;
    for (size_t i = 0; i < reply.elements.size(); i += 2) {
        result[reply.elements[i]] = reply.elements[i + 1];
    }
    return result;
}

inline bool restart_required(const std::string& content) {
    return ConfigSyncService::contains_section(content, "server") ||
           ConfigSyncService::contains_section(content, "mysql");
}

inline std::optional<std::string> validate_file_set(
    const std::vector<ManagedFile>& files, bool reject_empty) {
    if (reject_empty && files.empty()) {
        return "files must not be empty";
    }
    std::set<std::string> seen;
    for (const auto& file : files) {
        if (!seen.insert(file.name).second) {
            return "duplicate file: " + file.name;
        }
        auto validation = ConfigSyncService::validate_managed_file(file.name, file.content);
        if (!validation.ok) {
            return file.name + ": " + validation.reason;
        }
    }
    return std::nullopt;
}

inline bool write_file(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out << content;
    return true;
}

class TempConfigDir {
public:
    TempConfigDir() {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        base_ = std::filesystem::temp_directory_path() /
            ("asio_owen_admin_dry_run_" + std::to_string(getpid()) +
             "_" + std::to_string(now));
        std::filesystem::create_directories(base_ / "config.d");
    }

    ~TempConfigDir() {
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
    }

    const std::filesystem::path& base() const { return base_; }

private:
    std::filesystem::path base_;
};

inline std::optional<std::string> dry_run_config_set(
    const std::filesystem::path& config_base,
    const std::vector<ManagedFile>& files) {
    try {
        TempConfigDir temp;
        constexpr std::array<std::string_view, 3> never_sync = {
            "11-redis.ini", "12-config-sync.ini", "99-local.ini"
        };
        for (auto name : never_sync) {
            auto src = config_base / "config.d" / std::string(name);
            std::error_code ec;
            if (std::filesystem::exists(src, ec) && !ec) {
                std::filesystem::copy_file(
                    src, temp.base() / "config.d" / std::string(name),
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                    return "failed to copy local " + std::string(name);
                }
            }
        }
        for (const auto& file : files) {
            if (!write_file(temp.base() / "config.d" / file.name, file.content)) {
                return "failed to write staged file " + file.name;
            }
        }

        Config cfg;
        if (!cfg.load(temp.base())) {
            return "staged config does not parse";
        }
        SecurityRules::validate_config_for_staging(cfg);
        asio::io_context ioc;
        UpstreamManager upstreams(ioc);
        (void)upstreams.prepare_reload(cfg, http_pool_config_from(cfg));
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "unknown dry-run error";
    }
    return std::nullopt;
}

inline std::string files_json(int64_t version,
                              const std::map<std::string, std::string>& files,
                              bool degraded = false) {
    std::ostringstream out;
    out << "{\"version\":" << version
        << ",\"degraded\":" << (degraded ? "true" : "false")
        << ",\"files\":[";
    bool first = true;
    for (const auto& [name, content] : files) {
        if (!first) out << ",";
        first = false;
        out << "{\"name\":\"" << json_escape(name)
            << "\",\"content\":\"" << json_escape(content)
            << "\",\"restart_required\":"
            << (restart_required(content) ? "true" : "false") << "}";
    }
    out << "]}";
    return out.str();
}

inline std::vector<std::string> split_pipe_fields(const std::string& value) {
    std::vector<std::string> fields;
    size_t pos = 0;
    while (pos <= value.size()) {
        auto next = value.find('|', pos);
        if (next == std::string::npos) {
            fields.push_back(value.substr(pos));
            break;
        }
        fields.push_back(value.substr(pos, next - pos));
        pos = next + 1;
    }
    return fields;
}

inline std::string machines_json(const std::map<std::string, std::string>& machines) {
    std::ostringstream out;
    out << "{\"machines\":[";
    bool first = true;
    for (const auto& [name, value] : machines) {
        auto fields = split_pipe_fields(value);
        if (!first) out << ",";
        first = false;
        out << "{\"machine\":\"" << json_escape(name) << "\"";
        if (fields.size() >= 4) {
            out << ",\"version\":\"" << json_escape(fields[0])
                << "\",\"ts\":\"" << json_escape(fields[1])
                << "\",\"pid\":\"" << json_escape(fields[2])
                << "\",\"status\":\"" << json_escape(fields[3]) << "\"";
            if (fields.size() >= 5) {
                out << ",\"detail\":\"" << json_escape(fields[4]) << "\"";
            }
        } else {
            out << ",\"raw\":\"" << json_escape(value) << "\"";
        }
        out << "}";
    }
    out << "]}";
    return out.str();
}

inline std::string audit_json(const Principal* principal, int64_t base_version,
                              int64_t new_version,
                              std::string_view action,
                              std::string_view reason,
                              const std::vector<ManagedFile>& files) {
    std::ostringstream out;
    out << "{\"ts\":" << static_cast<int64_t>(std::time(nullptr))
        << ",\"user\":\""
        << json_escape(principal ? (principal->username.empty()
                ? principal->subject : principal->username) : "insecure")
        << "\",\"base_version\":" << base_version
        << ",\"new_version\":" << new_version
        << ",\"action\":\"" << json_escape(std::string(action))
        << "\",\"reason\":\"" << json_escape(std::string(reason)) << "\""
        << ",\"files\":[";
    for (size_t i = 0; i < files.size(); ++i) {
        if (i > 0) out << ",";
        out << "\"" << json_escape(files[i].name) << "\"";
    }
    out << "]}";
    return out.str();
}

inline std::string save_script() {
    return config_history::save_script();
}

inline std::string admin_login_html() {
    return std::string(generated_assets::admin_login_html);
}

inline std::string admin_settings_html() {
    return std::string(generated_assets::admin_settings_html);
}

}  // namespace config_admin
