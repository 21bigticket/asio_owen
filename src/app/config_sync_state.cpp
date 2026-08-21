#include "config_sync_service_impl.hpp"

#include <algorithm>
#include <asio/co_spawn.hpp>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#include <unistd.h>

#include "../common/config.hpp"
#include "../common/logger.hpp"
#include "admin/config_history.hpp"

bool ConfigSyncServiceImpl::is_never_sync_file(const std::string& name) {
    return name == "11-redis.ini" ||
           name == "12-config-sync.ini" ||
           name == "99-local.ini";
}

bool ConfigSyncServiceImpl::is_valid_managed_filename(const std::string& name) {
    if (name.size() < 7) return false;
    if (!std::isdigit(static_cast<unsigned char>(name[0])) ||
        !std::isdigit(static_cast<unsigned char>(name[1])) ||
        name[2] != '-') {
        return false;
    }
    if (name.size() < 5 || name.substr(name.size() - 4) != ".ini") {
        return false;
    }
    for (size_t i = 3; i + 4 < name.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (!(std::islower(c) || std::isdigit(c) || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

// Filename and contents are distinct fields even though both are strings.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
ConfigSyncServiceImpl::ValidationResult ConfigSyncServiceImpl::validate_managed_file(
    const std::string& name, const std::string& content) {
    if (ConfigSyncServiceImpl::is_never_sync_file(name)) {
        return {false, "never-sync file is not Redis-managed"};
    }
    if (!ConfigSyncServiceImpl::is_valid_managed_filename(name)) {
        return {false, "invalid managed filename"};
    }
    if (ConfigSyncServiceImpl::contains_section(content, "redis")) {
        return {false, "managed file contains [redis] section"};
    }
    if (ConfigSyncServiceImpl::contains_section(content, "admin")) {
        return {false, "managed file contains [admin] section"};
    }
    if (ConfigSyncServiceImpl::contains_section(content, "config_sync")) {
        return {false, "managed file contains [config_sync] section"};
    }
    if (ConfigSyncServiceImpl::contains_section(content, "config_history")) {
        return {false, "managed file contains [config_history] section"};
    }
    if (ConfigSyncServiceImpl::has_reserved_admin_rule(content)) {
        return {false, "managed file touches reserved admin path"};
    }
    return {};
}
// NOLINTEND(bugprone-easily-swappable-parameters)

bool ConfigSyncServiceImpl::has_reserved_admin_rule(const std::string& content) {
    std::string section;
    for (auto line : ConfigSyncServiceImpl::split_lines(content)) {
        ConfigSyncServiceImpl::strip_cr(line);
        ConfigSyncServiceImpl::trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            ConfigSyncServiceImpl::trim(section);
            ConfigSyncServiceImpl::to_lower_in_place(section);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        ConfigSyncServiceImpl::trim(key);
        ConfigSyncServiceImpl::trim(value);
        if (section == "auth_whitelist") {
            if (ConfigSyncServiceImpl::is_reserved_admin_path(key) || is_reserved_admin_path(value)) {
                return true;
            }
        } else if (section == "path_blacklist") {
            if (ConfigSyncServiceImpl::is_reserved_admin_path(key)) {
                return true;
            }
        }
    }
    return false;
}

bool ConfigSyncServiceImpl::is_reserved_admin_path(std::string path) {
    ConfigSyncServiceImpl::trim(path);
    if (path.empty() || path.front() != '/') return false;
    ConfigSyncServiceImpl::to_lower_in_place(path);
    return path == "/admin" || path.rfind("/admin/", 0) == 0 ||
           path == "/api/admin" || path.rfind("/api/admin/", 0) == 0;
}

// The content document and section name have intentionally different roles.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool ConfigSyncServiceImpl::contains_section(const std::string& content, const std::string& target) {
    std::string lowered_target = target;
    ConfigSyncServiceImpl::to_lower_in_place(lowered_target);
    for (auto line : ConfigSyncServiceImpl::split_lines(content)) {
        ConfigSyncServiceImpl::strip_cr(line);
        ConfigSyncServiceImpl::trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            auto section = line.substr(1, line.size() - 2);
            ConfigSyncServiceImpl::trim(section);
            ConfigSyncServiceImpl::to_lower_in_place(section);
            if (section == lowered_target) return true;
        }
    }
    return false;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

std::string ConfigSyncServiceImpl::content_hash(const std::string& content) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : content) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << hash;
    return oss.str();
}

ConfigSyncServiceImpl::State ConfigSyncServiceImpl::load_state(
    const std::filesystem::path& config_base) {
    State state;
    auto path = ConfigSyncServiceImpl::state_path(config_base);
    std::ifstream in(path);
    if (!in.is_open()) return state;
    state.exists = true;
    std::string line;
    while (std::getline(in, line)) {
        ConfigSyncServiceImpl::strip_cr(line);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        auto value = line.substr(eq + 1);
        if (key == "synced_version") {
            auto parsed = ConfigSyncServiceImpl::parse_int64(value);
            if (parsed) state.synced_version = *parsed;
        } else if (key == "status") {
            state.status = value;
        } else if (key.rfind("file.", 0) == 0) {
            state.managed_files[key.substr(5)] = value;
        } else if (key.rfind("last_ok.", 0) == 0) {
            state.last_ok[key.substr(8)] = value;
        } else if (key.rfind("failure.", 0) == 0) {
            state.failures[key.substr(8)] = value;
        }
    }
    return state;
}

bool ConfigSyncServiceImpl::write_state(const std::filesystem::path& config_base, const State& state) {
    auto path = ConfigSyncServiceImpl::state_path(config_base);
    auto tmp = path;
    tmp += ".tmp";

    std::ostringstream serialized;
    serialized << "synced_version=" << state.synced_version << "\n";
    serialized << "status=" << state.status << "\n";
    for (const auto& [name, hash] : state.managed_files) {
        serialized << "file." << name << "=" << hash << "\n";
    }
    for (const auto& [name, hash] : state.last_ok) {
        serialized << "last_ok." << name << "=" << hash << "\n";
    }
    for (const auto& [name, reason] : state.failures) {
        serialized << "failure." << name << "=" << reason << "\n";
    }
    const std::string content = serialized.str();

    {
        std::ifstream current(path, std::ios::binary);
        if (current.is_open()) {
            std::ostringstream existing;
            existing << current.rdbuf();
            if (current.good() || current.eof()) {
                if (existing.str() == content) return true;
            }
        }
    }
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out << content;
    }
    return std::rename(tmp.string().c_str(), path.string().c_str()) == 0;
}
