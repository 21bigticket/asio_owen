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
#include "config_admin.hpp"

namespace config_admin {

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

ParseResult parse_save_request(std::string_view body) {
    return JsonReader(body).parse_save_request();
}

LoginParseResult parse_login_request(std::string_view body) {
    return JsonReader(body).parse_login_request();
}

RollbackParseResult parse_rollback_request(std::string_view body) {
    return JsonReader(body).parse_rollback_request();
}

RepairParseResult parse_repair_request(std::string_view body) {
    return JsonReader(body).parse_repair_request();
}

OrphanResolutionParseResult parse_orphan_resolution_request(
    std::string_view body) {
    return JsonReader(body).parse_orphan_resolution_request();
}

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

}  // namespace config_admin
