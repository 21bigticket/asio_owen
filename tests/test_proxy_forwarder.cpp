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

TEST(ProxyForwarder, RejectsControlCharInFirstHeader) {
    UpstreamManager::UpstreamConfig cfg{"127.0.0.1", 30001};
    HttpContext ctx;
    ctx.headers = {
        {"X-Evil", "value\r\nInjected: yes"},
        {"Content-Type", "application/json"}
    };
    HeaderParseState request_header_state;

    auto request = build_proxy_request(
        "POST", "/config.ConfigService/Get", cfg, ctx, request_header_state);
    EXPECT_TRUE(request.empty());
}

TEST(ProxyForwarder, RejectsControlCharInHeaderAfterTransferEncoding) {
    // Regression for P0 split-loop fix: previously, the loop broke early when
    // Transfer-Encoding was seen, so subsequent headers' CR/LF were not scanned.
    UpstreamManager::UpstreamConfig cfg{"127.0.0.1", 30001};
    HttpContext ctx;
    ctx.headers = {
        {"Transfer-Encoding", "chunked"},
        {"X-Sneak", "clean\r\nInjected: POST /evil"}
    };
    HeaderParseState request_header_state;

    auto request = build_proxy_request(
        "POST", "/config.ConfigService/Get", cfg, ctx, request_header_state);
    EXPECT_TRUE(request.empty());
}

TEST(ProxyForwarder, RejectsNulInHeaderValue) {
    UpstreamManager::UpstreamConfig cfg{"127.0.0.1", 30001};
    HttpContext ctx;
    ctx.headers = {
        {"X-Bad", std::string("a\0b", 3)},
        {"Content-Type", "application/json"}
    };
    HeaderParseState request_header_state;

    auto request = build_proxy_request(
        "POST", "/config.ConfigService/Get", cfg, ctx, request_header_state);
    EXPECT_TRUE(request.empty());
}
