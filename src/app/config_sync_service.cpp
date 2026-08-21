#include "config_sync_service_impl.hpp"

ConfigSyncService::ConfigSyncService(
    asio::io_context& ioc, RedisPool& redis, std::filesystem::path config_base,
    ConfigSyncConfig cfg, AppConfig running_app_cfg,
    asio::thread_pool* file_workers, std::shared_ptr<ConfigSyncRuntimeMetrics> metrics)
    : impl_(std::make_shared<ConfigSyncServiceImpl>(
          ioc, redis, std::move(config_base), std::move(cfg),
          std::move(running_app_cfg), file_workers, std::move(metrics))) {}

ConfigSyncService::ConfigSyncService(
    asio::io_context& ioc, Command command, std::filesystem::path config_base,
    ConfigSyncConfig cfg, AppConfig running_app_cfg,
    asio::thread_pool* file_workers, std::shared_ptr<ConfigSyncRuntimeMetrics> metrics)
    : impl_(std::make_shared<ConfigSyncServiceImpl>(
          ioc, std::move(command), std::move(config_base), std::move(cfg),
          std::move(running_app_cfg), file_workers, std::move(metrics))) {}

ConfigSyncService::~ConfigSyncService() = default;

void ConfigSyncService::start() { impl_->start(); }
void ConfigSyncService::stop() { impl_->stop(); }
void ConfigSyncService::sync_once_for_test(Completion completion) {
    impl_->sync_once_for_test(std::move(completion));
}
bool ConfigSyncService::blocking_first_pull(
    const std::filesystem::path& config_base,
    const ConfigSyncConfig& sync_cfg, RedisPool::Config redis_cfg,
    const AppConfig& running_app_cfg) {
    return ConfigSyncServiceImpl::blocking_first_pull(
        config_base, sync_cfg, std::move(redis_cfg), running_app_cfg);
}
ConfigSyncConfig ConfigSyncService::normalize_config(ConfigSyncConfig cfg) {
    return ConfigSyncServiceImpl::normalize_config(std::move(cfg));
}
bool ConfigSyncService::is_never_sync_file(const std::string& name) {
    return ConfigSyncServiceImpl::is_never_sync_file(name);
}
bool ConfigSyncService::is_valid_managed_filename(const std::string& name) {
    return ConfigSyncServiceImpl::is_valid_managed_filename(name);
}
ConfigSyncService::ValidationResult ConfigSyncService::validate_managed_file(
    const std::string& name, const std::string& content) {
    auto result = ConfigSyncServiceImpl::validate_managed_file(name, content);
    return {result.ok, std::move(result.reason)};
}
bool ConfigSyncService::has_reserved_admin_rule(const std::string& content) {
    return ConfigSyncServiceImpl::has_reserved_admin_rule(content);
}
bool ConfigSyncService::is_reserved_admin_path(std::string path) {
    return ConfigSyncServiceImpl::is_reserved_admin_path(std::move(path));
}
bool ConfigSyncService::contains_section(const std::string& content,
                                         const std::string& target) {
    return ConfigSyncServiceImpl::contains_section(content, target);
}
std::string ConfigSyncService::content_hash(const std::string& content) {
    return ConfigSyncServiceImpl::content_hash(content);
}
ConfigSyncService::State ConfigSyncService::load_state(
    const std::filesystem::path& config_base) {
    auto state = ConfigSyncServiceImpl::load_state(config_base);
    State result;
    result.exists = state.exists;
    result.synced_version = state.synced_version;
    result.status = std::move(state.status);
    result.managed_files = std::move(state.managed_files);
    result.last_ok = std::move(state.last_ok);
    result.failures = std::move(state.failures);
    return result;
}
bool ConfigSyncService::write_state(const std::filesystem::path& config_base,
                                    const State& state) {
    ConfigSyncServiceImpl::State impl_state;
    impl_state.exists = state.exists;
    impl_state.synced_version = state.synced_version;
    impl_state.status = state.status;
    impl_state.managed_files = state.managed_files;
    impl_state.last_ok = state.last_ok;
    impl_state.failures = state.failures;
    return ConfigSyncServiceImpl::write_state(config_base, impl_state);
}
