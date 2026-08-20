#include <gtest/gtest.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

#include "app/routes.hpp"

namespace {

std::filesystem::path make_base(const char* tag) {
    auto base = std::filesystem::temp_directory_path() /
        (std::string("asio_owen_ready_") + tag + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(base);
    return base;
}

void run_ready(asio::io_context& ioc, HttpContext& ctx, AppServices services) {
    asio::co_spawn(ioc, api_ready(ctx, std::move(services)), asio::detached);
    ioc.run();
}

}  // namespace

TEST(ReadyRoute, DisabledConfigSyncIsReady) {
    asio::io_context ioc;
    HttpContext ctx;
    run_ready(ioc, ctx, {});

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("\"ready\":true"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("\"status\":\"disabled\""), std::string::npos);
}

TEST(ReadyRoute, MissingRequiredSyncStateIsNotReady) {
    const auto base = make_base("missing");
    HttpContext ctx;
    AppServices services;
    services.config_base = base;
    services.config_sync.enabled = true;

    asio::io_context ioc;
    run_ready(ioc, ctx, std::move(services));

    EXPECT_EQ(ctx.status_code, 503);
    EXPECT_NE(ctx.response_body.find("\"ready\":false"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("\"healthy\":false"), std::string::npos);
    std::filesystem::remove_all(base);
}

TEST(ReadyRoute, HealthySyncStateBecomesNotReadyDuringDrain) {
    const auto base = make_base("drain");
    {
        std::ofstream out(base / ".config-sync-state");
        out << "synced_version=7\nstatus=ok\n";
    }
    auto draining = std::make_shared<std::atomic<bool>>(false);
    HttpContext ctx;
    AppServices services;
    services.config_base = base;
    services.config_sync.enabled = true;
    services.draining_state = draining;

    asio::io_context ioc;
    run_ready(ioc, ctx, services);
    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("\"synced_version\":7"), std::string::npos);

    draining->store(true, std::memory_order_release);
    ctx = {};
    asio::io_context draining_ioc;
    run_ready(draining_ioc, ctx, std::move(services));
    EXPECT_EQ(ctx.status_code, 503);
    EXPECT_NE(ctx.response_body.find("\"draining\":true"), std::string::npos);
    std::filesystem::remove_all(base);
}

TEST(ReadyRoute, MetricsExposeSyncAndShutdownState) {
    const auto base = make_base("metrics");
    {
        std::ofstream out(base / ".config-sync-state");
        out << "synced_version=11\nstatus=ok\n";
    }
    HttpContext ctx;
    AppServices services;
    services.config_base = base;
    services.config_sync.enabled = true;

    asio::io_context ioc;
    asio::co_spawn(ioc, api_metrics(ctx, services), asio::detached);
    ioc.run();

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("\"version\":11"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("\"history\""), std::string::npos);
    EXPECT_NE(ctx.response_body.find("\"admin_auth\""), std::string::npos);
    EXPECT_NE(ctx.response_body.find("\"http_pool\""), std::string::npos);
    std::filesystem::remove_all(base);
}
