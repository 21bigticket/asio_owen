#include <gtest/gtest.h>

#include <string>

#include "http/proxy_forwarder.hpp"

TEST(ProxyForwarder, DoesNotSendKeepAliveHeaderByDefault) {
    UpstreamManager::UpstreamConfig cfg{"127.0.0.1", 30001};
    HttpContext ctx;
    ctx.headers = {
        {"Connection", "close"},
        {"Content-Type", "application/json"}
    };
    HeaderParseState request_header_state;

    auto request = build_proxy_request(
        "POST", "/config.ConfigService/GetByAppAndKey", cfg, ctx, request_header_state);

    EXPECT_EQ(request.find("Connection:"), std::string::npos) << request;
    EXPECT_EQ(request.find("Connection: close"), std::string::npos) << request;
}

TEST(ProxyForwarder, SendsKeepAliveHeaderWhenEnabled) {
    UpstreamManager::UpstreamConfig cfg{"127.0.0.1", 30001};
    HttpContext ctx;
    ctx.headers = {
        {"Connection", "close"},
        {"Content-Type", "application/json"}
    };
    HeaderParseState request_header_state;

    auto request = build_proxy_request(
        "POST", "/config.ConfigService/GetByAppAndKey", cfg, ctx, request_header_state, true);

    EXPECT_NE(request.find("Connection: keep-alive\r\n"), std::string::npos) << request;
    EXPECT_EQ(request.find("Connection: close"), std::string::npos) << request;
}
