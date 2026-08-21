#pragma once

#include <asio/any_io_executor.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "admin_route_support.hpp"
#include "config_admin.hpp"
#include "config_history.hpp"

namespace admin_route_detail {

class AdminHistoryOperation : public std::enable_shared_from_this<AdminHistoryOperation> {
public:
    using StartAction = std::function<void(AdminHistoryOperation&)>;

    AdminHistoryOperation(HttpContext& ctx, AppServices services,
                          asio::any_io_executor executor,
                          std::function<void()> completion,
                          StartAction start_action);

    void start();
    void start_history_list();
    void start_history_path();
    void start_rollback_request();
    void start_snapshot_repair();
    void start_mirror_rebuild();
    void start_history_migration();
    void start_orphan_request();

private:
    struct RepairPlan {
        bool compat_only = false;
        bool read_history_snapshot = false;
        bool synthesize_metadata = false;
        bool use_config_staging = false;
        std::string action;
        std::string (*script)() = nullptr;
    };

    using ReplyCallback = std::function<void(RedisPool::Reply)>;
    using DetailCallback =
        std::function<void(std::optional<config_history::SnapshotRecord>)>;

    void start_blocking() noexcept;
    static std::string path_only(std::string_view path);
    static std::optional<std::string> query_value(
        std::string_view path, std::string_view key);
    static bool valid_record_hash(const config_history::SnapshotRecord& record);

    void start_list();
    void start_path();
    void start_detail(int64_t version);
    void start_diff(int64_t from_version);

    void start_rollback();
    void prepare_rollback(std::optional<config_history::SnapshotRecord> record);
    void submit_rollback(
        std::vector<config_admin::ManagedFile> files,
        const config_history::SnapshotRecord& source,
        const config_history::SnapshotInfo& info);
    void handle_rollback_reply(const RedisPool::Reply& reply, bool sensitive);

    void start_repair();
    void read_repair_mirror();
    void read_repair_meta(
        std::map<std::string, std::string> files,
        std::string mirror_hash);
    void submit_repair(
        const std::map<std::string, std::string>& files,
        std::string meta, std::string script, std::string action);
    void handle_repair_reply(
        const RedisPool::Reply& reply, const std::string& action);
    void finish_repair_success(const std::string& action, int64_t result);

    void start_orphan_resolution();
    void prepare_orphan_resolution(RedisPool::Reply reply);
    std::string orphan_audit(
        std::string_view action,
        const std::map<std::string, std::string>& files) const;
    void submit_restore_version(
        const std::map<std::string, std::string>& files,
        const std::string& meta);
    void submit_delete_orphan(
        const std::map<std::string, std::string>& files,
        const std::string& meta, bool indexed, bool snapshot_exists);
    void handle_orphan_resolution_reply(
        const RedisPool::Reply& reply, const std::string& action);

    void read_detail(int64_t version, DetailCallback callback);
    void run_command(std::vector<std::string> args, ReplyCallback callback);
    void bad_request(const std::string& message);
    void not_found();
    void redis_failed(const std::string& message);
    void inconsistent(const std::string& message);
    void fail(std::exception_ptr ep) noexcept;
    void complete() noexcept;

    HttpContext& ctx_;
    AppServices services_;
    asio::any_io_executor executor_;
    AdminExecutorGuard executor_guard_;
    std::function<void()> completion_;
    StartAction start_action_;
    RepairPlan repair_plan_;
    config_admin::RollbackRequest rollback_request_;
    config_admin::RepairRequest repair_request_;
    config_admin::OrphanResolutionRequest orphan_request_;
    bool restart_required_ = false;
    std::atomic<bool> completed_{false};
};

}  // namespace admin_route_detail
