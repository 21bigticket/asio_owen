#include <gtest/gtest.h>
#include <asio.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "common/config.hpp"
#include "http/upstream_manager.hpp"

namespace {

Config make_upstream_config(const std::string& name, const std::string& host, int port) {
    auto path = std::filesystem::temp_directory_path() /
        ("asio_owen_upstream_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ini");
    {
        std::ofstream out(path);
        out << "[upstream]\n";
        out << name << " = " << host << ":" << port << "\n";
    }
    Config cfg;
    cfg.load_file(path);
    std::filesystem::remove(path);
    return cfg;
}

void load_upstream(UpstreamManager& manager, const std::string& name,
                   const std::string& host, int port) {
    auto cfg = make_upstream_config(name, host, port);
    manager.reload(cfg, HttpPool::Config{});
}

}  // namespace

TEST(UpstreamManager, RoutesServicePrefixAndStripsItForUpstream) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    load_upstream(manager, "zebra-config", "127.0.0.1", 30001);

    auto route = manager.route("/zebra-config/config.ConfigService/GetByAppAndKey");

    ASSERT_TRUE(route.has_value());
    EXPECT_EQ(route->config.host, "127.0.0.1");
    EXPECT_EQ(route->config.port, 30001);
    EXPECT_NE(route->pool, nullptr);
    EXPECT_EQ(route->upstream_path, "/config.ConfigService/GetByAppAndKey");
}

TEST(UpstreamManager, RoutesBareServiceToRootPath) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    load_upstream(manager, "zebra-config", "127.0.0.1", 30001);

    auto route = manager.route("/zebra-config");

    ASSERT_TRUE(route.has_value());
    EXPECT_EQ(route->upstream_path, "/");
}

TEST(UpstreamManager, IgnoresUnknownService) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    load_upstream(manager, "zebra-config", "127.0.0.1", 30001);

    EXPECT_FALSE(manager.route("/config.ConfigService/GetByAppAndKey").has_value());
}

TEST(UpstreamManager, PoolStatsIncludesServiceNameAndCounters) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    load_upstream(manager, "zebra-config", "127.0.0.1", 30001);

    auto stats = manager.pool_stats();

    EXPECT_NE(stats.find("zebra-config={"), std::string::npos);
    EXPECT_NE(stats.find("total=0"), std::string::npos);
    EXPECT_NE(stats.find("reused=0"), std::string::npos);
    EXPECT_NE(stats.find("created=0"), std::string::npos);
}

TEST(UpstreamManager, ReloadReusesUnchangedPoolAndReplacesChangedPool) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    auto initial = make_upstream_config("zebra-config", "127.0.0.1", 30001);
    manager.reload(initial, HttpPool::Config{});
    auto before = manager.route("/zebra-config/path");
    ASSERT_TRUE(before.has_value());

    manager.reload(initial, HttpPool::Config{});
    auto unchanged = manager.route("/zebra-config/path");
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(unchanged->pool, before->pool);

    auto changed = make_upstream_config("zebra-config", "127.0.0.1", 30002);
    manager.reload(changed, HttpPool::Config{});
    auto after = manager.route("/zebra-config/path");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->config.port, 30002);
    EXPECT_NE(after->pool, before->pool);
}

TEST(UpstreamManager, ReloadPublishesRemovalAsPartOfWholeMapReplacement) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    load_upstream(manager, "zebra-config", "127.0.0.1", 30001);

    Config empty;
    manager.reload(empty, HttpPool::Config{});

    EXPECT_FALSE(manager.route("/zebra-config/path").has_value());
}
