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

bool ConfigSyncServiceImpl::startup_config_differs(const AppConfig& current) const {
    return current.server_port != running_app_cfg_.server_port ||
           current.io_threads != running_app_cfg_.io_threads ||
           current.max_client_connections !=
               running_app_cfg_.max_client_connections ||
           current.fd_reserve != running_app_cfg_.fd_reserve ||
           ConfigSyncServiceImpl::mysql_config_differs(current.mysql, running_app_cfg_.mysql) ||
           ConfigSyncServiceImpl::redis_config_differs(current.redis, running_app_cfg_.redis);
}

bool ConfigSyncServiceImpl::mysql_config_differs(
    const MysqlPool::Config& a, const MysqlPool::Config& b) {
    return a.host != b.host || a.port != b.port || a.user != b.user ||
           a.pass != b.pass || a.db != b.db || a.min_size != b.min_size ||
           a.max_size != b.max_size || a.max_idle_sec != b.max_idle_sec ||
           a.connect_timeout_ms != b.connect_timeout_ms ||
           a.read_timeout_ms != b.read_timeout_ms ||
           a.query_timeout_ms != b.query_timeout_ms ||
           a.acquire_timeout_ms != b.acquire_timeout_ms ||
           a.keepalive_sec != b.keepalive_sec ||
           a.worker_threads != b.worker_threads ||
           a.max_creating != b.max_creating;
}

bool ConfigSyncServiceImpl::redis_config_differs(
    const RedisPool::Config& a, const RedisPool::Config& b) {
    return a.host != b.host || a.port != b.port || a.db != b.db ||
           a.connect_timeout_ms != b.connect_timeout_ms ||
           a.cmd_timeout_ms != b.cmd_timeout_ms || a.mode != b.mode ||
           a.min_size != b.min_size || a.max_size != b.max_size ||
           a.max_idle_sec != b.max_idle_sec ||
           a.worker_threads != b.worker_threads ||
           a.max_creating != b.max_creating ||
           a.acquire_timeout_ms != b.acquire_timeout_ms;
}

std::filesystem::path ConfigSyncServiceImpl::config_dir() const {
    return config_base_ / "config.d";
}

// Name and content are fixed, distinct fields of the managed file record.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool ConfigSyncServiceImpl::write_managed_file_if_changed(
    const std::string& name, const std::string& content) const {
    std::error_code ec;
    std::filesystem::create_directories(ConfigSyncServiceImpl::config_dir(), ec);
    if (ec) return false;

    auto path = ConfigSyncServiceImpl::config_dir() / name;
    std::string existing;
    if (ConfigSyncServiceImpl::read_file(path, existing) && existing == content) {
        return true;
    }
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out << content;
    }
    return std::rename(tmp.string().c_str(), path.string().c_str()) == 0;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

std::optional<std::map<std::string, std::string>> ConfigSyncServiceImpl::parse_hgetall(
    const Reply& reply) {
    if (reply.type != "array") return std::map<std::string, std::string>{};
    if (reply.elements.size() % 2 != 0) return std::nullopt;
    std::map<std::string, std::string> result;
    for (size_t i = 0; i < reply.elements.size(); i += 2) {
        result[reply.elements[i]] = reply.elements[i + 1];
    }
    return result;
}

std::optional<int64_t> ConfigSyncServiceImpl::script_integer(const Reply& reply) {
    if (reply.type == "integer") return reply.integer;
    if (reply.type == "string") return ConfigSyncServiceImpl::parse_int64(reply.str);
    return std::nullopt;
}

bool ConfigSyncServiceImpl::collect_local_managed_files(
    const std::filesystem::path& config_base,
    std::map<std::string, std::string>& files,
    std::vector<std::string>& errors) const {
    const auto started = std::chrono::steady_clock::now();
    auto dir = config_base / "config.d";
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        errors.push_back("config.d not found");
        return false;
    }
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            errors.push_back("failed to scan config.d");
            return false;
        }
        if (entry.is_regular_file(ec) && entry.path().extension() == ".ini") {
            auto name = entry.path().filename().string();
            if (!ConfigSyncServiceImpl::is_never_sync_file(name)) {
                paths.push_back(entry.path());
            }
        }
        ec.clear();
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
        auto name = path.filename().string();
        std::string content;
        if (!ConfigSyncServiceImpl::read_file(path, content)) {
            errors.push_back("failed to read " + name);
            continue;
        }
        auto validation = ConfigSyncServiceImpl::validate_managed_file(name, content);
        if (!validation.ok) {
            errors.push_back(name + ": " + validation.reason);
            continue;
        }
        files[name] = std::move(content);
    }
    if (metrics_) {
        metrics_->files_scanned.fetch_add(files.size(), std::memory_order_relaxed);
        uint64_t bytes = 0;
        for (const auto& [unused_name, value] : files) bytes += value.size();
        metrics_->file_bytes_read.fetch_add(bytes, std::memory_order_relaxed);
        const auto elapsed = static_cast<uint64_t>(std::max<int64_t>(0,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count()));
        metrics_->file_scan_duration_us_total.fetch_add(elapsed, std::memory_order_relaxed);
        auto old = metrics_->file_scan_duration_us_max.load(std::memory_order_relaxed);
        while (old < elapsed && !metrics_->file_scan_duration_us_max.compare_exchange_weak(
                   old, elapsed, std::memory_order_relaxed)) {
        }
    }
    return errors.empty();
}

std::set<std::string> ConfigSyncServiceImpl::collect_local_managed_file_names(
    const std::filesystem::path& config_base) {
    std::set<std::string> names;
    auto dir = config_base / "config.d";
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return names;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) return names;
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".ini") {
            ec.clear();
            continue;
        }
        auto name = entry.path().filename().string();
        if (!ConfigSyncServiceImpl::is_never_sync_file(name) && is_valid_managed_filename(name)) {
            names.insert(std::move(name));
        }
        ec.clear();
    }
    return names;
}

bool ConfigSyncServiceImpl::read_file(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::filesystem::path ConfigSyncServiceImpl::state_path(const std::filesystem::path& config_base) {
    return config_base / ".config-sync-state";
}

std::vector<std::string> ConfigSyncServiceImpl::split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
}

void ConfigSyncServiceImpl::trim(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
}

void ConfigSyncServiceImpl::strip_cr(std::string& s) {
    if (!s.empty() && s.back() == '\r') s.pop_back();
}

void ConfigSyncServiceImpl::to_lower_in_place(std::string& s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
}

std::optional<int64_t> ConfigSyncServiceImpl::parse_int64(const std::string& value) {
    if (value.empty()) return std::nullopt;
    int64_t parsed = 0;
    auto [end, ec] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

std::string ConfigSyncServiceImpl::machine_name() const {
    if (!cfg_.machine_name.empty()) return cfg_.machine_name;
    char host[256]{};
    if (gethostname(host, sizeof(host) - 1) == 0 && host[0] != '\0') {
        return host;
    }
    return "unknown";
}

int ConfigSyncServiceImpl::effective_drain_timeout_ms() const {
    int cmd_ms = running_app_cfg_.redis.cmd_timeout_ms <= 0 ?
        30000 : running_app_cfg_.redis.cmd_timeout_ms;
    return cmd_ms + 500;
}

void ConfigSyncServiceImpl::warn_limited(const std::string& message) {
    const auto now = std::chrono::steady_clock::now();
    bool should_log = false;
    {
        std::lock_guard lock(warn_mu_);
        if (now - last_warn_ > std::chrono::seconds(30)) {
            last_warn_ = now;
            should_log = true;
        }
    }
    if (should_log) {
        LOG_WARN(message);
    }
}

std::string ConfigSyncServiceImpl::seed_script() {
    return config_history::seed_script();
}
