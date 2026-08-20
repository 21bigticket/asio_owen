#include <gtest/gtest.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>

#include <chrono>

#include "app/routes.hpp"

namespace {

class FakeComboBackend final : public ComboBackend {
public:
    std::string cache;
    MysqlPool::Result query_result{true, {}, "[{\"name\":\"from_mysql\"}]"};
    std::chrono::milliseconds query_delay{0};

    asio::awaitable<std::string> get_cache() override {
        co_return cache;
    }

    asio::awaitable<MysqlPool::Result> query() override {
        if (query_delay.count() > 0) {
            auto ex = co_await asio::this_coro::executor;
            asio::steady_timer timer(ex);
            timer.expires_after(query_delay);
            co_await timer.async_wait(asio::use_awaitable);
        }
        co_return query_result;
    }

};

void run_combo(asio::io_context& ioc, HttpContext& ctx, AppServices services) {
    asio::co_spawn(ioc, handle_api_combo(ctx, std::move(services)), asio::detached);
    ioc.run();
}

AppServices combo_services(std::shared_ptr<ComboQueryLimiter> limiter,
                           std::shared_ptr<ComboBackend> backend,
                           int deadline_ms = 500) {
    AppServices services{};
    services.combo_query_limiter = std::move(limiter);
    services.combo_backend = std::move(backend);
    services.combo_deadline_ms = deadline_ms;
    return services;
}

}  // namespace

TEST(ComboRoute, Returns503WhenFallbackPermitIsExhausted) {
    asio::io_context ioc;
    auto limiter = std::make_shared<ComboQueryLimiter>(1);
    auto held = limiter->try_acquire();
    ASSERT_TRUE(held.has_value());
    auto backend = std::make_shared<FakeComboBackend>();
    HttpContext ctx;
    run_combo(ioc, ctx, combo_services(limiter, backend));

    EXPECT_EQ(ctx.status_code, 503);
    EXPECT_NE(ctx.response_body.find("too many in-flight"), std::string::npos);
}

TEST(ComboRoute, Returns504AndReleasesPermitAfterLateQueryCompletion) {
    asio::io_context ioc;
    auto limiter = std::make_shared<ComboQueryLimiter>(1);
    auto backend = std::make_shared<FakeComboBackend>();
    backend->query_delay = std::chrono::milliseconds(30);
    HttpContext ctx;
    run_combo(ioc, ctx, combo_services(limiter, backend, 1));

    EXPECT_EQ(ctx.status_code, 504);
    EXPECT_TRUE(limiter->try_acquire().has_value());
}

TEST(ComboRoute, ReturnsMysqlValueWhenCacheWriteIsUnavailableInUnitTest) {
    asio::io_context ioc;
    auto limiter = std::make_shared<ComboQueryLimiter>(1);
    auto backend = std::make_shared<FakeComboBackend>();
    HttpContext ctx;
    run_combo(ioc, ctx, combo_services(limiter, backend));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("from_mysql"), std::string::npos);
    EXPECT_TRUE(backend->cache.empty());
}
