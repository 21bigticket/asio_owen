#include <gtest/gtest.h>
#include <asio.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

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

// Build a config whose [upstream] section body is given verbatim (for
// malformed-entry tests).
Config make_raw_upstream_config(const std::string& section_body) {
    auto path = std::filesystem::temp_directory_path() /
        ("asio_owen_upstream_bad_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ini");
    {
        std::ofstream out(path);
        out << "[upstream]\n" << section_body;
    }
    Config cfg;
    cfg.load_file(path);
    std::filesystem::remove(path);
    return cfg;
}

// Build a config from verbatim ini text (for [gateway] switch tests).
Config make_raw_config(const std::string& ini_text) {
    auto path = std::filesystem::temp_directory_path() /
        ("asio_owen_gateway_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ini");
    {
        std::ofstream out(path);
        out << ini_text;
    }
    Config cfg;
    cfg.load_file(path);
    std::filesystem::remove(path);
    return cfg;
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

TEST(UpstreamManager, ReloadReplacesPoolWhenPoolConfigChanges) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    auto upstream = make_upstream_config("zebra-config", "127.0.0.1", 30001);
    HttpPool::Config initial_pool_cfg;
    initial_pool_cfg.max_size = 10;
    manager.reload(upstream, initial_pool_cfg);
    auto before = manager.route("/zebra-config/path");
    ASSERT_TRUE(before.has_value());

    auto changed_pool_cfg = initial_pool_cfg;
    changed_pool_cfg.max_size = 20;
    manager.reload(upstream, changed_pool_cfg);
    auto after = manager.route("/zebra-config/path");
    ASSERT_TRUE(after.has_value());

    EXPECT_NE(after->pool, before->pool);
    EXPECT_EQ(after->pool->cfg().max_size, 20u);
}

TEST(UpstreamManager, PreparedReloadDoesNotPublishEarly) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    auto initial = make_upstream_config("zebra-config", "127.0.0.1", 30001);
    manager.reload(initial, HttpPool::Config{});

    auto changed = make_upstream_config("zebra-config", "127.0.0.1", 30002);
    auto prepared = manager.prepare_reload(changed, HttpPool::Config{});
    auto before_publish = manager.route("/zebra-config/path");
    ASSERT_TRUE(before_publish.has_value());
    EXPECT_EQ(before_publish->config.port, 30001);

    manager.publish_reload(std::move(prepared));
    auto after_publish = manager.route("/zebra-config/path");
    ASSERT_TRUE(after_publish.has_value());
    EXPECT_EQ(after_publish->config.port, 30002);
}

TEST(UpstreamManager, ReloadPublishesRemovalAsPartOfWholeMapReplacement) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    load_upstream(manager, "zebra-config", "127.0.0.1", 30001);

    Config empty;
    manager.reload(empty, HttpPool::Config{});

    EXPECT_FALSE(manager.route("/zebra-config/path").has_value());
}

TEST(UpstreamManager, MalformedEntryRejectsWholeReloadAndKeepsExistingRoutes) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    load_upstream(manager, "zebra-config", "127.0.0.1", 30001);

    EXPECT_THROW(manager.prepare_reload(
        make_raw_upstream_config("zebra-config = 127.0.0.1\n"), HttpPool::Config{}),
        std::invalid_argument);
    EXPECT_THROW(manager.prepare_reload(
        make_raw_upstream_config("zebra-config = 127.0.0.1:\n"), HttpPool::Config{}),
        std::invalid_argument);
    EXPECT_THROW(manager.prepare_reload(
        make_raw_upstream_config("zebra-config = 127.0.0.1:abc\n"), HttpPool::Config{}),
        std::invalid_argument);
    EXPECT_THROW(manager.prepare_reload(
        make_raw_upstream_config("zebra-config = 127.0.0.1:30001x\n"), HttpPool::Config{}),
        std::invalid_argument);
    EXPECT_THROW(manager.prepare_reload(
        make_raw_upstream_config("zebra-config = 127.0.0.1:70000\n"), HttpPool::Config{}),
        std::invalid_argument);
    EXPECT_THROW(manager.prepare_reload(
        make_raw_upstream_config("zebra-config = 127.0.0.1:0\n"), HttpPool::Config{}),
        std::invalid_argument);
    EXPECT_THROW(manager.prepare_reload(
        make_raw_upstream_config("zebra-config = :30001\n"), HttpPool::Config{}),
        std::invalid_argument);
    // IPv6 literals are explicitly rejected rather than mis-parsed.
    EXPECT_THROW(manager.prepare_reload(
        make_raw_upstream_config("zebra-config = ::1:30001\n"), HttpPool::Config{}),
        std::invalid_argument);

    // 即使一批里只有一条非法，既有路由也必须原样保留。
    auto route = manager.route("/zebra-config/path");
    ASSERT_TRUE(route.has_value());
    EXPECT_EQ(route->config.port, 30001);
}

TEST(UpstreamManager, GenerationAdvancesOnlyOnPublish) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    auto g0 = manager.generation();

    auto cfg = make_upstream_config("zebra-config", "127.0.0.1", 30001);
    manager.reload(cfg, HttpPool::Config{});
    EXPECT_EQ(manager.generation(), g0 + 1);

    // prepare 不改变代际；只有 publish 递增。
    auto changed = make_upstream_config("zebra-config", "127.0.0.1", 30002);
    auto prepared = manager.prepare_reload(changed, HttpPool::Config{});
    EXPECT_EQ(manager.generation(), g0 + 1);
    manager.publish_reload(std::move(prepared));
    EXPECT_EQ(manager.generation(), g0 + 2);
}

// 多个 config.d 文件按文件名升序加载，后加载的同一 service 条目必须覆盖
// 先加载的（与 Config::get() 的 "later files override earlier files" 一致）。
// 回归：曾用 emplace 导致第一条生效、99-local.ini 的覆盖值被忽略。
TEST(UpstreamManager, LaterConfigFileOverridesEarlierForSameService) {
    auto base = std::filesystem::temp_directory_path() /
        ("asio_owen_upstream_override_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(base / "config.d");
    {
        std::ofstream out(base / "config.d" / "00-first.ini");
        out << "[upstream]\n"
            << "zebra-config = 127.0.0.1:30001\n";
    }
    {
        std::ofstream out(base / "config.d" / "01-second.ini");
        out << "[upstream]\n"
            << "zebra-config = 127.0.0.2:30002\n";
    }

    Config cfg;
    ASSERT_TRUE(cfg.load(base));
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    manager.reload(cfg, HttpPool::Config{});

    auto route = manager.route("/zebra-config/path");
    ASSERT_TRUE(route.has_value());
    EXPECT_EQ(route->config.host, "127.0.0.2");
    EXPECT_EQ(route->config.port, 30002);

    std::filesystem::remove_all(base);
}

// [gateway] json_keys_snake_to_camel 缺省必须打开（老配置行为不变）。
TEST(UpstreamManager, GatewaySnakeToCamelDefaultsToEnabled) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    load_upstream(manager, "zebra-config", "127.0.0.1", 30001);

    auto route = manager.route("/zebra-config/path");
    ASSERT_TRUE(route.has_value());
    EXPECT_TRUE(route->json_keys_snake_to_camel);
}

TEST(UpstreamManager, GatewaySnakeToCamelSwitchDisablesTransform) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    auto cfg = make_raw_config(
        "[upstream]\nzebra-config = 127.0.0.1:30001\n"
        "[gateway]\njson_keys_snake_to_camel = false\n");
    manager.reload(cfg, HttpPool::Config{});

    auto route = manager.route("/zebra-config/path");
    ASSERT_TRUE(route.has_value());
    EXPECT_FALSE(route->json_keys_snake_to_camel);

    // get_bool 接受的其它拼写同样生效，热加载翻回 true。
    auto re_enabled = make_raw_config(
        "[upstream]\nzebra-config = 127.0.0.1:30001\n"
        "[gateway]\njson_keys_snake_to_camel = on\n");
    manager.reload(re_enabled, HttpPool::Config{});
    auto after = manager.route("/zebra-config/path");
    ASSERT_TRUE(after.has_value());
    EXPECT_TRUE(after->json_keys_snake_to_camel);
}

// 非法取值必须拒绝整次热加载，且已发布的状态（路由 + 当前开关值）原样保留。
TEST(UpstreamManager, InvalidGatewaySnakeToCamelValueRejectsWholeReload) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    auto initial = make_raw_config(
        "[upstream]\nzebra-config = 127.0.0.1:30001\n"
        "[gateway]\njson_keys_snake_to_camel = false\n");
    manager.reload(initial, HttpPool::Config{});

    auto bad = make_raw_config(
        "[upstream]\nzebra-config = 127.0.0.1:30001\n"
        "[gateway]\njson_keys_snake_to_camel = maybe\n");
    EXPECT_THROW(manager.prepare_reload(bad, HttpPool::Config{}),
        std::invalid_argument);

    auto route = manager.route("/zebra-config/path");
    ASSERT_TRUE(route.has_value());
    EXPECT_EQ(route->config.port, 30001);
    EXPECT_FALSE(route->json_keys_snake_to_camel);
}

// 只翻开关、上游表不变时，连接池必须原样复用（开关不参与池重建比较）。
// 回归保护：避免把开关放进 HttpPool::Config 导致翻一次开关就重建全部上游池。
TEST(UpstreamManager, TogglingGatewaySnakeToCamelKeepsPoolsAlive) {
    asio::io_context ioc;
    UpstreamManager manager(ioc);
    auto enabled = make_raw_config(
        "[upstream]\nzebra-config = 127.0.0.1:30001\n");
    manager.reload(enabled, HttpPool::Config{});
    auto before = manager.route("/zebra-config/path");
    ASSERT_TRUE(before.has_value());
    EXPECT_TRUE(before->json_keys_snake_to_camel);

    auto disabled = make_raw_config(
        "[upstream]\nzebra-config = 127.0.0.1:30001\n"
        "[gateway]\njson_keys_snake_to_camel = false\n");
    manager.reload(disabled, HttpPool::Config{});
    auto after_off = manager.route("/zebra-config/path");
    ASSERT_TRUE(after_off.has_value());
    EXPECT_FALSE(after_off->json_keys_snake_to_camel);
    EXPECT_EQ(after_off->pool, before->pool);

    manager.reload(enabled, HttpPool::Config{});
    auto after_on = manager.route("/zebra-config/path");
    ASSERT_TRUE(after_on.has_value());
    EXPECT_TRUE(after_on->json_keys_snake_to_camel);
    EXPECT_EQ(after_on->pool, before->pool);
}
