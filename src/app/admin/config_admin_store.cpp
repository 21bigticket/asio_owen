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

std::optional<int64_t> redis_integer(const RedisPool::Reply& reply) {
    if (reply.type == "integer") return reply.integer;
    if (reply.type == "string") return parse_int64(reply.str);
    return std::nullopt;
}

std::optional<int64_t> redis_version(const RedisPool::Reply& reply) {
    if (!reply.ok) return std::nullopt;
    if (reply.type == "nil") return 0;
    auto parsed = redis_integer(reply);
    if (!parsed || *parsed < 0) return std::nullopt;
    return parsed;
}

std::optional<std::map<std::string, std::string>> parse_hgetall(
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

bool restart_required(const std::string& content) {
    return ConfigSyncService::contains_section(content, "server") ||
           ConfigSyncService::contains_section(content, "mysql");
}

std::optional<std::string> validate_file_set(
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

bool write_file(const std::filesystem::path& path, const std::string& content) {
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

std::optional<std::string> dry_run_config_set(
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

std::string files_json(int64_t version,
                              const std::map<std::string, std::string>& files,
                              bool degraded) {
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

std::vector<std::string> split_pipe_fields(const std::string& value) {
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

std::string machines_json(const std::map<std::string, std::string>& machines) {
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

std::string audit_json(const Principal* principal, int64_t base_version,
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

std::string save_script() {
    return config_history::save_script();
}

std::string admin_login_html() {
    return std::string(generated_assets::admin_login_html);
}

std::string admin_settings_html() {
    return std::string(generated_assets::admin_settings_html);
}

}  // namespace config_admin
