#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../db/redis_pool.hpp"
#include "app_config.hpp"

class ConfigSyncServiceImpl;

struct ConfigSyncRuntimeMetrics {
    std::atomic<uint64_t> file_worker_jobs{0};
    std::atomic<uint64_t> file_worker_fallbacks{0};
    std::atomic<uint64_t> file_worker_busy{0};
    std::atomic<uint64_t> file_worker_queue_delay_us_total{0};
    std::atomic<uint64_t> file_worker_queue_delay_us_max{0};
    std::atomic<uint64_t> file_scan_duration_us_total{0};
    std::atomic<uint64_t> file_scan_duration_us_max{0};
    std::atomic<uint64_t> files_scanned{0};
    std::atomic<uint64_t> file_bytes_read{0};
};

class ConfigSyncService {
public:
    using Reply = RedisPool::Reply;
    using Completion = std::function<void(bool)>;
    using CommandCompletion = std::function<void(Reply)>;
    using Command = std::function<void(std::vector<std::string>, CommandCompletion)>;

    struct ValidationResult {
        bool ok = true;
        std::string reason;
    };

    struct State {
        bool exists = false;
        int64_t synced_version = 0;
        std::string status = "ok";
        std::map<std::string, std::string> managed_files;
        std::map<std::string, std::string> last_ok;
        std::map<std::string, std::string> failures;
    };

    ConfigSyncService(asio::io_context& ioc, RedisPool& redis,
        std::filesystem::path config_base, ConfigSyncConfig cfg,
        AppConfig running_app_cfg, asio::thread_pool* file_workers = nullptr,
        std::shared_ptr<ConfigSyncRuntimeMetrics> metrics = nullptr);
    ConfigSyncService(asio::io_context& ioc, Command command,
        std::filesystem::path config_base, ConfigSyncConfig cfg,
        AppConfig running_app_cfg, asio::thread_pool* file_workers = nullptr,
        std::shared_ptr<ConfigSyncRuntimeMetrics> metrics = nullptr);
    ~ConfigSyncService();

    ConfigSyncService(const ConfigSyncService&) = delete;
    ConfigSyncService& operator=(const ConfigSyncService&) = delete;

    void start();
    void stop();
    void sync_once_for_test(Completion completion);

    static bool blocking_first_pull(const std::filesystem::path& config_base,
        const ConfigSyncConfig& sync_cfg, RedisPool::Config redis_cfg,
        const AppConfig& running_app_cfg);
    static ConfigSyncConfig normalize_config(ConfigSyncConfig cfg);
    static bool is_never_sync_file(const std::string& name);
    static bool is_valid_managed_filename(const std::string& name);
    static ValidationResult validate_managed_file(
        const std::string& name, const std::string& content);
    static bool has_reserved_admin_rule(const std::string& content);
    static bool is_reserved_admin_path(std::string path);
    static bool contains_section(const std::string& content,
        const std::string& target);
    static std::string content_hash(const std::string& content);
    static State load_state(const std::filesystem::path& config_base);
    static bool write_state(const std::filesystem::path& config_base,
        const State& state);

private:
    std::shared_ptr<ConfigSyncServiceImpl> impl_;
};
