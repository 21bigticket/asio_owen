#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <chrono>

#include "common/config.hpp"

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
