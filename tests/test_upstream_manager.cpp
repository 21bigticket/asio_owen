#include <gtest/gtest.h>
#include <asio.hpp>

#include "http/upstream_manager.hpp"

TEST(UpstreamManager, RoutesServicePrefixAndStripsItForUpstream) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    manager.add_upstream("zebra-config", "127.0.0.1", 30001);

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
    manager.add_upstream("zebra-config", "127.0.0.1", 30001);

    auto route = manager.route("/zebra-config");

    ASSERT_TRUE(route.has_value());
    EXPECT_EQ(route->upstream_path, "/");
}

TEST(UpstreamManager, IgnoresUnknownService) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    manager.add_upstream("zebra-config", "127.0.0.1", 30001);

    EXPECT_FALSE(manager.route("/config.ConfigService/GetByAppAndKey").has_value());
}

TEST(UpstreamManager, PoolStatsIncludesServiceNameAndCounters) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    manager.add_upstream("zebra-config", "127.0.0.1", 30001);

    auto stats = manager.pool_stats();

    EXPECT_NE(stats.find("zebra-config={"), std::string::npos);
    EXPECT_NE(stats.find("total=0"), std::string::npos);
    EXPECT_NE(stats.find("reused=0"), std::string::npos);
    EXPECT_NE(stats.find("created=0"), std::string::npos);
}
