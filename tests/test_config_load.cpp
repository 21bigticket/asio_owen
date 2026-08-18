#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "common/config.hpp"
#include "app/app_config.hpp"

namespace {

std::filesystem::path make_temp_config_dir() {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto base = std::filesystem::temp_directory_path() /
        ("asio_owen_config_test_" + std::to_string(now));
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base / "config.d");
    return base;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
}

}  // namespace

TEST(ConfigLoad, LoadsConfigDFromBaseDirectory) {
    auto base = make_temp_config_dir();
    write_file(base / "config.d" / "00-server.ini",
        "[server]\n"
        "port = 8081\n");

    Config cfg;
    ASSERT_TRUE(cfg.load(base));
    EXPECT_EQ(cfg.get_int("server", "port", 0), 8081);

    std::filesystem::remove_all(base);
}

TEST(ConfigLoad, LaterFilesOverrideEarlierFiles) {
    auto base = make_temp_config_dir();
    write_file(base / "config.d" / "00-server.ini",
        "[server]\n"
        "port = 8081\n");
    write_file(base / "config.d" / "99-local.ini",
        "[server]\n"
        "port = 9090\n");

    Config cfg;
    ASSERT_TRUE(cfg.load(base));
    EXPECT_EQ(cfg.get_int("server", "port", 0), 9090);

    std::filesystem::remove_all(base);
}

TEST(ConfigLoad, ParsesBoolValues) {
    auto base = make_temp_config_dir();
    write_file(base / "config.d" / "21-http_pool.ini",
        "[http_pool]\n"
        "send_keep_alive_header = true\n"
        "disabled = 0\n");

    Config cfg;
    ASSERT_TRUE(cfg.load(base));
    EXPECT_TRUE(cfg.get_bool("http_pool", "send_keep_alive_header", false));
    EXPECT_FALSE(cfg.get_bool("http_pool", "disabled", true));
    EXPECT_TRUE(cfg.get_bool("http_pool", "missing", true));

    std::filesystem::remove_all(base);
}

TEST(ConfigLoad, LegacyHistoryAutoMigrationDefaultsOnAndCanBeDisabled) {
    auto default_base = make_temp_config_dir();
    write_file(default_base / "config.d" / "12-config-sync.ini",
        "[config_history]\nread_mode = required\n");
    Config default_cfg;
    ASSERT_TRUE(default_cfg.load(default_base));
    EXPECT_TRUE(app_config_from(default_cfg).config_sync.history.auto_migrate_legacy);
    std::filesystem::remove_all(default_base);

    auto disabled_base = make_temp_config_dir();
    write_file(disabled_base / "config.d" / "12-config-sync.ini",
        "[config_history]\n"
        "read_mode = required\n"
        "auto_migrate_legacy = false\n");
    Config disabled_cfg;
    ASSERT_TRUE(disabled_cfg.load(disabled_base));
    EXPECT_FALSE(app_config_from(disabled_cfg)
        .config_sync.history.auto_migrate_legacy);
    std::filesystem::remove_all(disabled_base);
}

TEST(ConfigLoad, ParsesDownstreamWriteTimeout) {
    auto base = make_temp_config_dir();
    write_file(base / "config.d" / "00-server.ini",
        "[server]\n"
        "downstream_write_timeout_ms = 1234\n");

    Config cfg;
    ASSERT_TRUE(cfg.load(base));
    auto app = app_config_from(cfg);
    EXPECT_EQ(app.downstream_write_timeout_ms, 1234);

    std::filesystem::remove_all(base);
}

TEST(ConfigLoad, ParsesClientHeaderReadTimeout) {
    auto base = make_temp_config_dir();
    write_file(base / "config.d" / "00-server.ini",
        "[server]\n"
        "client_header_read_timeout_ms = 4321\n");

    Config cfg;
    ASSERT_TRUE(cfg.load(base));
    auto app = app_config_from(cfg);
    EXPECT_EQ(app.client_header_read_timeout_ms, 4321);

    std::filesystem::remove_all(base);
}

TEST(ConfigLoad, ParsesComboDeadlineAndInFlightLimit) {
    auto base = make_temp_config_dir();
    write_file(base / "config.d" / "00-server.ini",
        "[server]\n"
        "combo_deadline_ms = 250\n"
        "combo_max_in_flight_queries = 12\n");

    Config cfg;
    ASSERT_TRUE(cfg.load(base));
    auto app = app_config_from(cfg);
    EXPECT_EQ(app.combo_deadline_ms, 250);
    EXPECT_EQ(app.combo_max_in_flight_queries, 12u);

    std::filesystem::remove_all(base);
}

TEST(ConfigLoad, ClampsInvalidComboLimits) {
    auto base = make_temp_config_dir();
    write_file(base / "config.d" / "00-server.ini",
        "[server]\n"
        "combo_deadline_ms = 0\n"
        "combo_max_in_flight_queries = 0\n");

    Config cfg;
    ASSERT_TRUE(cfg.load(base));
    auto app = app_config_from(cfg);
    EXPECT_EQ(app.combo_deadline_ms, 1);
    EXPECT_EQ(app.combo_max_in_flight_queries, 1u);

    std::filesystem::remove_all(base);
}

TEST(ConfigLoad, ParsesMysqlQueryTimeout) {
    auto base = make_temp_config_dir();
    write_file(base / "config.d" / "10-mysql.ini",
        "[mysql]\n"
        "query_timeout_ms = 2500\n"
        "acquire_timeout_ms = 1200\n");

    Config cfg;
    ASSERT_TRUE(cfg.load(base));
    auto app = app_config_from(cfg);
    EXPECT_EQ(app.mysql.query_timeout_ms, 2500);
    EXPECT_EQ(app.mysql.acquire_timeout_ms, 1200);

    std::filesystem::remove_all(base);
}

TEST(ConfigLoad, ParsesRedisWorkerPoolConfig) {
    auto base = make_temp_config_dir();
    write_file(base / "config.d" / "11-redis.ini",
        "[redis]\n"
        "mode = worker\n"
        "db = 2\n"
        "min_size = 2\n"
        "max_size = 9\n"
        "max_idle_sec = 77\n"
        "worker_threads = 3\n"
        "max_creating = 2\n"
        "acquire_timeout_ms = 456\n");

    Config cfg;
    ASSERT_TRUE(cfg.load(base));
    auto app = app_config_from(cfg);
    EXPECT_EQ(app.redis.mode, RedisPool::Mode::Worker);
    EXPECT_EQ(app.redis.db, 2);
    EXPECT_EQ(app.redis.min_size, 2u);
    EXPECT_EQ(app.redis.max_size, 9u);
    EXPECT_EQ(app.redis.max_idle_sec, 77);
    EXPECT_EQ(app.redis.worker_threads, 3u);
    EXPECT_EQ(app.redis.max_creating, 2u);
    EXPECT_EQ(app.redis.acquire_timeout_ms, 456);

    std::filesystem::remove_all(base);
}

TEST(ConfigLoad, RejectsMalformedLineWithoutEquals) {
    auto base = make_temp_config_dir();
    write_file(base / "config.d" / "00-bad.ini",
        "[server]\n"
        "port = 8081\n"
        "this line has no equals sign\n");

    Config cfg;
    EXPECT_FALSE(cfg.load(base));

    std::filesystem::remove_all(base);
}

TEST(ConfigLoad, RejectsMalformedSectionHeader) {
    auto base = make_temp_config_dir();
    write_file(base / "config.d" / "00-bad.ini",
        "[server\n"
        "port = 8081\n");

    Config cfg;
    EXPECT_FALSE(cfg.load(base));

    std::filesystem::remove_all(base);
}

TEST(ConfigLoad, RejectsUnreadableFileInsteadOfSilentlySkipping) {
    // chmod 无法阻止 root 读取任何文件：以 root 运行时该用例无法构造
    // "不可读" 文件，只能跳过（文件权限由操作系统强制，无法在进程内模拟）。
    if (geteuid() == 0) {
        GTEST_SKIP() << "running as root: cannot create an unreadable file";
    }

    auto base = make_temp_config_dir();
    write_file(base / "config.d" / "00-server.ini",
        "[server]\n"
        "port = 8081\n");
    auto unreadable = base / "config.d" / "01-secret.ini";
    write_file(unreadable, "[server]\nport = 9090\n");
    // 清空全部权限（而不是只移除 owner 位）：默认 umask 常为 group/other
    // 保留读权限，仅移除 owner 位不足以让非特权用户也读不到。
    std::filesystem::permissions(unreadable, std::filesystem::perms::none,
        std::filesystem::perm_options::replace);

    Config cfg;
    EXPECT_FALSE(cfg.load(base));

    std::filesystem::permissions(unreadable, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace);
    std::filesystem::remove_all(base);
}

TEST(ConfigLoad, RejectsMalformedSingleFile) {
    auto path = std::filesystem::temp_directory_path() /
        ("asio_owen_config_bad_file_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ini");
    write_file(path, "no_equals_here\n");

    Config cfg;
    EXPECT_FALSE(cfg.load_file(path));

    std::filesystem::remove(path);
}

TEST(ConfigLoad, RejectsInvalidTypedValuesInsteadOfUsingDefaults) {
    auto base = make_temp_config_dir();
    write_file(base / "config.d" / "00-invalid.ini",
        "[values]\n"
        "bad_int = 12x\n"
        "empty_int =\n"
        "overflow_int = 999999999999999999999\n"
        "bad_double = 1.5ms\n"
        "non_finite_double = inf\n"
        "bad_bool = enabled\n");

    Config cfg;
    ASSERT_TRUE(cfg.load(base));
    EXPECT_THROW(cfg.get_int("values", "bad_int", 7), std::invalid_argument);
    EXPECT_THROW(cfg.get_int("values", "empty_int", 7), std::invalid_argument);
    EXPECT_THROW(cfg.get_int("values", "overflow_int", 7), std::invalid_argument);
    EXPECT_THROW(cfg.get_double("values", "bad_double", 2.0), std::invalid_argument);
    EXPECT_THROW(cfg.get_double("values", "non_finite_double", 2.0), std::invalid_argument);
    EXPECT_THROW(cfg.get_bool("values", "bad_bool", false), std::invalid_argument);

    std::filesystem::remove_all(base);
}

TEST(ConfigLoad, MissingTypedValuesStillUseDefaults) {
    Config cfg;
    EXPECT_EQ(cfg.get_int("missing", "int", 7), 7);
    EXPECT_DOUBLE_EQ(cfg.get_double("missing", "double", 2.5), 2.5);
    EXPECT_TRUE(cfg.get_bool("missing", "bool", true));
}
