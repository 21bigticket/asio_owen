#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <chrono>

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
