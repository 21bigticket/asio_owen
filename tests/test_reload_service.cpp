#include <gtest/gtest.h>

#include <asio.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "app/app_config.hpp"
#include "app/reload_service.hpp"
#include "common/config.hpp"
#include "http/upstream_manager.hpp"
#include "security/security_rules.hpp"

namespace {

std::filesystem::path make_temp_config_dir() {
    auto base = std::filesystem::temp_directory_path() /
        ("asio_owen_reload_service_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(base / "config.d");
    return base;
}

void write_config(const std::filesystem::path& base, const std::string& contents) {
    std::ofstream out(base / "config.d" / "00-test.ini", std::ios::trunc);
    out << contents;
}

std::string make_config(bool valid_security, bool cors_enabled,
                        size_t pool_max_size, int upstream_port,
                        const std::filesystem::path& snapshot_path) {
    std::ostringstream out;
    out << "[security]\n";
    if (valid_security) {
        out << "jwt_disabled = true\n";
    } else {
        out << "jwt_algorithm = HS256\n";
    }
    out << "config_reload_interval_sec = 1\n"
        << "[cors]\n"
        << "enabled = " << (cors_enabled ? "true" : "false") << "\n";
    if (cors_enabled) {
        out << "allowed_origins = https://example.test\n";
    }
    out << "[rate_limit]\n"
        << "snapshot_path = " << snapshot_path.string() << "\n"
        << "[http_pool]\n"
        << "max_size = " << pool_max_size << "\n"
        << "[upstream]\n"
        << "zebra-config = 127.0.0.1:" << upstream_port << "\n";
    return out.str();
}

void drain_cancelled_timer(asio::io_context& ioc, ReloadService& service) {
    service.stop();
    if (ioc.stopped()) ioc.restart();
    ioc.run();
}

}  // namespace

TEST(ReloadService, FailedPreparePublishesNothingAndLaterRetrySucceeds) {
    auto base = make_temp_config_dir();
    auto snapshot_path = base / "rate_limit.bin";
    {
        write_config(base, make_config(true, false, 10, 30001, snapshot_path));

        Config initial_cfg;
        ASSERT_TRUE(initial_cfg.load(base));
        SecurityRules security_rules;
        security_rules.load_from_config(initial_cfg);
        asio::io_context ioc;
        UpstreamManager upstreams(ioc);
        upstreams.reload(initial_cfg, http_pool_config_from(initial_cfg));

        ReloadService service(ioc, base, security_rules, upstreams);
        service.start(1);

        write_config(base, make_config(false, true, 20, 30002, snapshot_path));

        ioc.run_for(std::chrono::milliseconds(1200));
        auto after_failed_reload = upstreams.route("/zebra-config/path");
        ASSERT_TRUE(after_failed_reload.has_value());
        EXPECT_EQ(after_failed_reload->config.port, 30001);
        EXPECT_EQ(after_failed_reload->pool->cfg().max_size, 10u);
        EXPECT_FALSE(security_rules.cors_enabled_fast());

        write_config(base, make_config(true, true, 20, 30002, snapshot_path));

        // tick2 将变化后的合法指纹直接作为新 pending，tick3 稳定后发布。
        ioc.run_for(std::chrono::milliseconds(2400));
        auto after_successful_retry = upstreams.route("/zebra-config/path");
        ASSERT_TRUE(after_successful_retry.has_value());
        EXPECT_EQ(after_successful_retry->config.port, 30002);
        EXPECT_EQ(after_successful_retry->pool->cfg().max_size, 20u);
        EXPECT_TRUE(security_rules.cors_enabled_fast());

        drain_cancelled_timer(ioc, service);
    }
    std::filesystem::remove_all(base);
}

// 请求级代际一致性的机制基础：security 先 publish、upstream 后 publish，
// 且两者在各自 publish 时递增代际。client_session 据此在安全检查与路由
// 之间检测“热更新已落地”，用最新一代规则重检后再路由。
TEST(ReloadService, PublishOrderKeepsRulesGenerationAtLeastAsNewAsRoutes) {
    auto base = make_temp_config_dir();
    auto snapshot_path = base / "rate_limit.bin";
    write_config(base, make_config(true, false, 10, 30001, snapshot_path));

    Config cfg;
    ASSERT_TRUE(cfg.load(base));
    SecurityRules security_rules;
    security_rules.load_from_config(cfg);
    auto sec_g0 = security_rules.generation();
    ASSERT_GT(sec_g0, 0u);

    asio::io_context ioc;
    UpstreamManager upstreams(ioc);
    upstreams.reload(cfg, http_pool_config_from(cfg));
    auto up_g0 = upstreams.generation();

    // 模拟一次 reload 的发布顺序：security 先，upstream 后。
    auto prepared_sec = security_rules.prepare_reload(cfg);
    auto prepared_up = upstreams.prepare_reload(cfg, http_pool_config_from(cfg));
    EXPECT_EQ(security_rules.generation(), sec_g0);
    EXPECT_EQ(upstreams.generation(), up_g0);

    security_rules.publish_reload(std::move(prepared_sec));
    EXPECT_GT(security_rules.generation(), sec_g0);
    // 中间态：规则已新、路由未变 —— 对请求是安全方向。
    EXPECT_EQ(upstreams.generation(), up_g0);

    upstreams.publish_reload(std::move(prepared_up));
    EXPECT_GT(upstreams.generation(), up_g0);

    std::filesystem::remove_all(base);
}

// 写了一半的中间文件（语法合法但内容未完成）不得被发布：reload 必须先观察
// 到 fingerprint 连续两个 tick 稳定才加载。时间轴（interval=1s）：
//   tick1 观察到中间态 M  -> pending=M；写入最终态 F 后
//   tick2 发现 F != M     -> pending=F，不发布；
//   tick3 F 连续稳定      -> 才发布。
TEST(ReloadService, DebouncesIntermediateFileUntilFingerprintIsStable) {
    auto base = make_temp_config_dir();
    auto snapshot_path = base / "rate_limit.bin";
    {
        write_config(base, make_config(true, false, 10, 30001, snapshot_path));

        Config initial_cfg;
        ASSERT_TRUE(initial_cfg.load(base));
        SecurityRules security_rules;
        security_rules.load_from_config(initial_cfg);
        asio::io_context ioc;
        UpstreamManager upstreams(ioc);
        upstreams.reload(initial_cfg, http_pool_config_from(initial_cfg));

        ReloadService service(ioc, base, security_rules, upstreams);
        service.start(1);

        // 中间态：语法合法但端口仍会变（编辑器先截断再写入的典型中间文件）。
        // 与最终态大小不同，确保 fingerprint 在 mtime 之外再有一个差异维度。
        write_config(base, make_config(true, false, 100, 30002, snapshot_path));

        ioc.run_for(std::chrono::milliseconds(1100));  // tick1: pending=中间态
        write_config(base, make_config(true, false, 10, 30003, snapshot_path));
        ioc.run_for(std::chrono::milliseconds(1100));  // tick2: pending 更新为最终态

        // 中间态（30002）不得发布；旧路由仍存活。
        auto mid = upstreams.route("/zebra-config/path");
        ASSERT_TRUE(mid.has_value());
        EXPECT_EQ(mid->config.port, 30001);

        ioc.run_for(std::chrono::milliseconds(1200));  // tick3: 稳定后发布
        auto after_stable = upstreams.route("/zebra-config/path");
        ASSERT_TRUE(after_stable.has_value());
        EXPECT_EQ(after_stable->config.port, 30003);

        drain_cancelled_timer(ioc, service);
    }
    std::filesystem::remove_all(base);
}

TEST(ReloadService, InvalidTypedValuePublishesNeitherRulesNorRoutes) {
    auto base = make_temp_config_dir();
    auto snapshot_path = base / "rate_limit.bin";
    {
        write_config(base, make_config(true, false, 10, 30001, snapshot_path));

        Config initial_cfg;
        ASSERT_TRUE(initial_cfg.load(base));
        SecurityRules security_rules;
        security_rules.load_from_config(initial_cfg);
        asio::io_context ioc;
        UpstreamManager upstreams(ioc);
        upstreams.reload(initial_cfg, http_pool_config_from(initial_cfg));
        const auto security_generation = security_rules.generation();
        const auto upstream_generation = upstreams.generation();

        ReloadService service(ioc, base, security_rules, upstreams);
        service.start(1);

        auto invalid = make_config(true, true, 20, 30002, snapshot_path);
        auto value_pos = invalid.find("max_size = 20");
        ASSERT_NE(value_pos, std::string::npos);
        invalid.replace(value_pos, std::string("max_size = 20").size(),
                        "max_size = 12x");
        write_config(base, invalid);

        // tick1 observes; tick2 parses and rejects the typed value.
        ioc.run_for(std::chrono::milliseconds(2200));

        EXPECT_EQ(security_rules.generation(), security_generation);
        EXPECT_EQ(upstreams.generation(), upstream_generation);
        auto route = upstreams.route("/zebra-config/path");
        ASSERT_TRUE(route.has_value());
        EXPECT_EQ(route->config.port, 30001);
        EXPECT_EQ(route->pool->cfg().max_size, 10u);
        EXPECT_FALSE(security_rules.cors_enabled_fast());

        drain_cancelled_timer(ioc, service);
    }
    std::filesystem::remove_all(base);
}

TEST(ReloadService, InvalidReloadIntervalPublishesNeitherRulesNorRoutes) {
    auto base = make_temp_config_dir();
    auto snapshot_path = base / "rate_limit.bin";
    {
        write_config(base, make_config(true, false, 10, 30001, snapshot_path));

        Config initial_cfg;
        ASSERT_TRUE(initial_cfg.load(base));
        SecurityRules security_rules;
        security_rules.load_from_config(initial_cfg);
        asio::io_context ioc;
        UpstreamManager upstreams(ioc);
        upstreams.reload(initial_cfg, http_pool_config_from(initial_cfg));
        const auto security_generation = security_rules.generation();
        const auto upstream_generation = upstreams.generation();

        ReloadService service(ioc, base, security_rules, upstreams);
        service.start(1);

        auto invalid = make_config(true, true, 20, 30002, snapshot_path);
        const std::string valid_interval = "config_reload_interval_sec = 1";
        auto value_pos = invalid.find(valid_interval);
        ASSERT_NE(value_pos, std::string::npos);
        invalid.replace(value_pos, valid_interval.size(),
                        "config_reload_interval_sec = invalid");
        write_config(base, invalid);

        // The interval is parsed with the rest of the candidate before either
        // live subsystem is published.
        ioc.run_for(std::chrono::milliseconds(2200));

        EXPECT_EQ(security_rules.generation(), security_generation);
        EXPECT_EQ(upstreams.generation(), upstream_generation);
        auto route = upstreams.route("/zebra-config/path");
        ASSERT_TRUE(route.has_value());
        EXPECT_EQ(route->config.port, 30001);
        EXPECT_EQ(route->pool->cfg().max_size, 10u);
        EXPECT_FALSE(security_rules.cors_enabled_fast());

        drain_cancelled_timer(ioc, service);
    }
    std::filesystem::remove_all(base);
}

TEST(ReloadService, FdBudgetFailurePublishesNeitherRulesNorRoutes) {
    auto base = make_temp_config_dir();
    auto snapshot_path = base / "rate_limit.bin";
    write_config(base, make_config(true, false, 10, 30001, snapshot_path));

    Config initial_cfg;
    ASSERT_TRUE(initial_cfg.load(base));
    SecurityRules security_rules;
    security_rules.load_from_config(initial_cfg);
    asio::io_context ioc;
    UpstreamManager upstreams(ioc);
    upstreams.reload(initial_cfg, http_pool_config_from(initial_cfg));
    const auto security_generation = security_rules.generation();
    const auto upstream_generation = upstreams.generation();

    ReloadService service(ioc, base, security_rules, upstreams,
        [](const HttpPool::Config& cfg) {
            if (cfg.max_total_connections > 15) {
                throw std::invalid_argument("test FD budget exceeded");
            }
        });
    service.start(1);

    auto over_budget = make_config(true, true, 20, 30002, snapshot_path);
    const std::string marker = "max_size = 20\n";
    const auto marker_pos = over_budget.find(marker);
    ASSERT_NE(marker_pos, std::string::npos);
    over_budget.insert(marker_pos + marker.size(),
                       "max_total_connections = 16\n");
    write_config(base, over_budget);
    ioc.run_for(std::chrono::milliseconds(2200));

    EXPECT_EQ(security_rules.generation(), security_generation);
    EXPECT_EQ(upstreams.generation(), upstream_generation);
    auto route = upstreams.route("/zebra-config/path");
    ASSERT_TRUE(route.has_value());
    EXPECT_EQ(route->config.port, 30001);
    EXPECT_FALSE(security_rules.cors_enabled_fast());

    drain_cancelled_timer(ioc, service);
    std::filesystem::remove_all(base);
}
