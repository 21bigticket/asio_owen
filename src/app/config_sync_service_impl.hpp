#pragma once

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "config_sync_service.hpp"

class ConfigSyncServiceImpl : public std::enable_shared_from_this<ConfigSyncServiceImpl> {
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

    ConfigSyncServiceImpl(
        asio::io_context& ioc, RedisPool& redis,
        std::filesystem::path config_base, ConfigSyncConfig cfg,
        AppConfig running_app_cfg, asio::thread_pool* file_workers = nullptr,
        std::shared_ptr<ConfigSyncRuntimeMetrics> metrics = nullptr);
    ConfigSyncServiceImpl(
        asio::io_context& ioc, Command command,
        std::filesystem::path config_base, ConfigSyncConfig cfg,
        AppConfig running_app_cfg, asio::thread_pool* file_workers = nullptr,
        std::shared_ptr<ConfigSyncRuntimeMetrics> metrics = nullptr);

    void start();
    void stop();
    void sync_once_for_test(Completion completion);
    static bool blocking_first_pull(
        const std::filesystem::path& config_base,
        const ConfigSyncConfig& sync_cfg, RedisPool::Config redis_cfg,
        const AppConfig& running_app_cfg);
    static ConfigSyncConfig normalize_config(ConfigSyncConfig cfg);
    static bool is_never_sync_file(const std::string& name);
    static bool is_valid_managed_filename(const std::string& name);
    static ValidationResult validate_managed_file(
        const std::string& name, const std::string& content);
    static bool has_reserved_admin_rule(const std::string& content);
    static bool is_reserved_admin_path(std::string path);
    static bool contains_section(
        const std::string& content, const std::string& target);
    static std::string content_hash(const std::string& content);
    static State load_state(const std::filesystem::path& config_base);
    static bool write_state(
        const std::filesystem::path& config_base, const State& state);

private:
    struct VersionRead {
        enum class Status { Value, Missing, Invalid, Error };
        Status status = Status::Error;
        int64_t value = 0;
        std::string error;
    };

    class TickCompletion {
    public:
        explicit TickCompletion(std::shared_ptr<ConfigSyncServiceImpl> service);
        ~TickCompletion();
        void finish() noexcept;

    private:
        std::shared_ptr<ConfigSyncServiceImpl> service_;
        std::atomic<bool> finished_{false};
    };

    static constexpr std::string_view kVersionKey = "asio_owen:config:version";
    static constexpr std::string_view kFilesKey = "asio_owen:config:files";
    static constexpr std::string_view kStagingKey = "asio_owen:config:files:staging";
    static constexpr std::string_view kMachinesKey = "asio_owen:config:machines";

    void schedule_after(std::chrono::milliseconds delay);
    void launch_tick();
    void finish_tick();
    void sync_once(Completion completion);
    void read_remote_files(int64_t remote_version, Completion completion);
    void handle_missing_snapshot(int64_t remote_version, Completion completion);
    void read_current_mirror(
        int64_t remote_version, bool require_meta_hash, Completion completion);
    void verify_snapshot_hash(
        int64_t remote_version,
        std::shared_ptr<std::map<std::string, std::string>> files,
        std::string fallback_detail, Completion completion);
    void record_history_failure(
        int64_t remote_version, std::string detail, Completion completion);
    void verify_remote_version(
        int64_t remote_version,
        std::shared_ptr<std::map<std::string, std::string>> remote_files,
        std::string forced_partial_detail, Completion completion);
    static VersionRead read_version_reply(const Reply& reply);

    void seed_if_eligible(Completion completion);
    bool has_seed_eligibility(const State& state, std::string& reason) const;
    bool apply_remote_files(
        int64_t version,
        const std::map<std::string, std::string>& remote_files,
        const State& previous_state,
        std::map<std::string, std::string>& failures,
        const std::string& forced_partial_detail = {});
    static std::string heartbeat_payload(
        int64_t version, std::string_view status,
        const std::map<std::string, std::string>* failures = nullptr);
    void heartbeat(std::string value, std::function<void()> completion);

    void run_command(
        std::vector<std::string> args, CommandCompletion completion);
    void dispatch_command_completion(
        std::shared_ptr<CommandCompletion> done,
        std::shared_ptr<std::atomic<bool>> callback_started, Reply reply);
    static Reply exception_reply(const std::exception_ptr& ep);

    void warn_startup_drift_once(int64_t version);
    bool startup_config_differs(const AppConfig& current) const;
    static bool mysql_config_differs(
        const MysqlPool::Config& a, const MysqlPool::Config& b);
    static bool redis_config_differs(
        const RedisPool::Config& a, const RedisPool::Config& b);
    std::filesystem::path config_dir() const;
    bool write_managed_file_if_changed(
        const std::string& name, const std::string& content) const;
    static std::optional<std::map<std::string, std::string>> parse_hgetall(
        const Reply& reply);
    static std::optional<int64_t> script_integer(const Reply& reply);
    bool collect_local_managed_files(
        const std::filesystem::path& config_base,
        std::map<std::string, std::string>& files,
        std::vector<std::string>& errors) const;
    static std::set<std::string> collect_local_managed_file_names(
        const std::filesystem::path& config_base);
    static bool read_file(
        const std::filesystem::path& path, std::string& out);
    static std::filesystem::path state_path(
        const std::filesystem::path& config_base);
    static std::vector<std::string> split_lines(const std::string& text);
    static void trim(std::string& s);
    static void strip_cr(std::string& s);
    static void to_lower_in_place(std::string& s);
    static std::optional<int64_t> parse_int64(const std::string& value);
    std::string machine_name() const;
    int effective_drain_timeout_ms() const;
    void warn_limited(const std::string& message);
    static std::string seed_script();

    asio::io_context& ioc_;
    asio::steady_timer timer_;
    Command command_;
    std::filesystem::path config_base_;
    ConfigSyncConfig cfg_;
    AppConfig running_app_cfg_;
    asio::thread_pool* file_workers_ = nullptr;
    std::shared_ptr<ConfigSyncRuntimeMetrics> metrics_;
    std::atomic<bool> running_{false};
    std::mutex in_flight_mu_;
    std::condition_variable in_flight_cv_;
    int in_flight_ = 0;
    std::mutex warn_mu_;
    std::chrono::steady_clock::time_point last_warn_{};
    std::mutex drift_mu_;
    std::set<int64_t> drift_warned_versions_;
};
