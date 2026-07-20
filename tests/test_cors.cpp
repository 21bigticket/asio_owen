#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "http/cors.hpp"

namespace {

// Build a request context carrying the given Origin (and optional preflight
// headers) so the policy helpers can be exercised without a live socket.
HttpContext make_ctx(const std::string& origin,
                     const std::string& request_method = "",
                     const std::string& request_headers = "") {
    HttpContext ctx;
    if (!origin.empty()) ctx.headers.emplace_back("Origin", origin);
    if (!request_method.empty())
        ctx.headers.emplace_back("Access-Control-Request-Method", request_method);
    if (!request_headers.empty())
        ctx.headers.emplace_back("Access-Control-Request-Headers", request_headers);
    return ctx;
}

bool has_header(const HttpContext& ctx, const std::string& name) {
    return !get_header_value(ctx.response_headers, name).empty();
}

}  // namespace

// ============== resolve_origin ==============

TEST(CorsPolicy, DisabledResolvesNullopt) {
    CorsPolicy policy;  // enabled defaults to false
    policy.allow_all_origins = true;
    EXPECT_FALSE(policy.resolve_origin("https://app.example.com").has_value());
}

TEST(CorsPolicy, EmptyOriginResolvesNullopt) {
    CorsPolicy policy;
    policy.enabled = true;
    policy.allow_all_origins = true;
    EXPECT_FALSE(policy.resolve_origin("").has_value());
}

TEST(CorsPolicy, ExactWhitelistMatch) {
    CorsPolicy policy;
    policy.enabled = true;
    policy.allowed_origins.insert("https://app.example.com");
    auto r = policy.resolve_origin("https://app.example.com");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "https://app.example.com");
}

TEST(CorsPolicy, RejectUnlistedOrigin) {
    CorsPolicy policy;
    policy.enabled = true;
    policy.allowed_origins.insert("https://app.example.com");
    EXPECT_FALSE(policy.resolve_origin("https://evil.example.com").has_value());
}

TEST(CorsPolicy, WildcardWithoutCredentialsReturnsStar) {
    CorsPolicy policy;
    policy.enabled = true;
    policy.allow_all_origins = true;
    policy.allow_credentials = false;
    auto r = policy.resolve_origin("https://anything.example.com");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "*");
}

TEST(CorsPolicy, WildcardWithCredentialsEchoesOrigin) {
    CorsPolicy policy;
    policy.enabled = true;
    policy.allow_all_origins = true;
    policy.allow_credentials = true;
    auto r = policy.resolve_origin("https://anything.example.com");
    ASSERT_TRUE(r.has_value());
    // resolve_origin is the pure primitive: if both flags are somehow set it
    // echoes the concrete Origin (echoing is safer than sending "*" with
    // credentials). At config load time this dangerous combo never arises
    // because load_cors_policy downgrades it — see
    // LoadCorsPolicy.WildcardWithCredentialsIsDowngraded below.
    EXPECT_EQ(*r, "https://anything.example.com");
}

// ============== apply_cors_headers ==============

TEST(ApplyHeaders, NoopWhenDisabled) {
    CorsPolicy policy;  // disabled
    auto ctx = make_ctx("https://app.example.com");
    apply_cors_headers(ctx, policy);
    EXPECT_TRUE(ctx.response_headers.empty());
}

TEST(ApplyHeaders, NoopForUnlistedOrigin) {
    CorsPolicy policy;
    policy.enabled = true;
    policy.allowed_origins.insert("https://app.example.com");
    auto ctx = make_ctx("https://evil.example.com");
    apply_cors_headers(ctx, policy);
    EXPECT_FALSE(has_header(ctx, "Access-Control-Allow-Origin"));
}

TEST(ApplyHeaders, ExactOriginAddsVary) {
    CorsPolicy policy;
    policy.enabled = true;
    policy.allowed_origins.insert("https://app.example.com");
    auto ctx = make_ctx("https://app.example.com");
    apply_cors_headers(ctx, policy);
    EXPECT_EQ(get_header_value(ctx.response_headers, "Access-Control-Allow-Origin"),
              "https://app.example.com");
    EXPECT_EQ(get_header_value(ctx.response_headers, "Vary"), "Origin");
}

TEST(ApplyHeaders, WildcardOmitsVary) {
    CorsPolicy policy;
    policy.enabled = true;
    policy.allow_all_origins = true;
    auto ctx = make_ctx("https://app.example.com");
    apply_cors_headers(ctx, policy);
    EXPECT_EQ(get_header_value(ctx.response_headers, "Access-Control-Allow-Origin"), "*");
    EXPECT_FALSE(has_header(ctx, "Vary"));
}

TEST(ApplyHeaders, CredentialsAndExposeHeaders) {
    CorsPolicy policy;
    policy.enabled = true;
    policy.allowed_origins.insert("https://app.example.com");
    policy.allow_credentials = true;
    policy.expose_headers = "X-Total-Count";
    auto ctx = make_ctx("https://app.example.com");
    apply_cors_headers(ctx, policy);
    EXPECT_EQ(get_header_value(ctx.response_headers, "Access-Control-Allow-Credentials"), "true");
    EXPECT_EQ(get_header_value(ctx.response_headers, "Access-Control-Expose-Headers"),
              "X-Total-Count");
}

TEST(ApplyHeaders, StripsUpstreamCorsBeforeInjecting) {
    CorsPolicy policy;
    policy.enabled = true;
    policy.allowed_origins.insert("https://app.example.com");
    auto ctx = make_ctx("https://app.example.com");
    // Simulate an upstream that already emitted its own (untrusted) CORS header.
    ctx.response_headers.emplace_back("Access-Control-Allow-Origin", "https://evil.example.com");
    apply_cors_headers(ctx, policy);
    // Exactly one Allow-Origin remains, and it is the gateway's.
    int count = 0;
    std::string value;
    for (auto& [k, v] : ctx.response_headers) {
        if (header_iequals(k, "access-control-allow-origin")) {
            ++count;
            value = v;
        }
    }
    EXPECT_EQ(count, 1);
    EXPECT_EQ(value, "https://app.example.com");
}

// ============== build_preflight_response ==============

TEST(Preflight, DisabledReturnsFalse) {
    CorsPolicy policy;  // disabled
    auto ctx = make_ctx("https://app.example.com", "POST");
    EXPECT_FALSE(build_preflight_response(ctx, policy));
    EXPECT_EQ(ctx.status_code, 0);
}

TEST(Preflight, IllegalOriginReturnsFalse) {
    CorsPolicy policy;
    policy.enabled = true;
    policy.allowed_origins.insert("https://app.example.com");
    auto ctx = make_ctx("https://evil.example.com", "POST");
    EXPECT_FALSE(build_preflight_response(ctx, policy));
    // ctx untouched so the caller can fall through to the normal path.
    EXPECT_EQ(ctx.status_code, 0);
    EXPECT_TRUE(ctx.response_headers.empty());
}

TEST(Preflight, BuildsWithVaryAndMaxAge) {
    CorsPolicy policy;
    policy.enabled = true;
    policy.allowed_origins.insert("https://app.example.com");
    policy.max_age = 600;
    auto ctx = make_ctx("https://app.example.com", "POST");
    ASSERT_TRUE(build_preflight_response(ctx, policy));
    EXPECT_EQ(ctx.status_code, 204);
    EXPECT_EQ(ctx.response_status_text, "No Content");
    EXPECT_TRUE(ctx.response_body.empty());
    EXPECT_EQ(get_header_value(ctx.response_headers, "Access-Control-Allow-Origin"),
              "https://app.example.com");
    EXPECT_EQ(get_header_value(ctx.response_headers, "Vary"), "Origin");
    EXPECT_EQ(get_header_value(ctx.response_headers, "Access-Control-Allow-Methods"),
              policy.allowed_methods);
    EXPECT_EQ(get_header_value(ctx.response_headers, "Access-Control-Max-Age"), "600");
}

TEST(Preflight, EchoesRequestedHeaders) {
    CorsPolicy policy;
    policy.enabled = true;
    policy.allowed_origins.insert("https://app.example.com");
    auto ctx = make_ctx("https://app.example.com", "POST", "X-Custom, Authorization");
    ASSERT_TRUE(build_preflight_response(ctx, policy));
    EXPECT_EQ(get_header_value(ctx.response_headers, "Access-Control-Allow-Headers"),
              "X-Custom, Authorization");
}

TEST(Preflight, FallsBackToConfiguredHeadersWhenNoneRequested) {
    CorsPolicy policy;
    policy.enabled = true;
    policy.allowed_origins.insert("https://app.example.com");
    policy.allowed_headers = "Content-Type, Authorization";
    auto ctx = make_ctx("https://app.example.com", "POST");
    ASSERT_TRUE(build_preflight_response(ctx, policy));
    EXPECT_EQ(get_header_value(ctx.response_headers, "Access-Control-Allow-Headers"),
              "Content-Type, Authorization");
}

// ============== load_cors_policy ==============

namespace {

std::filesystem::path make_temp_config_dir(const std::string& tag) {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto base = std::filesystem::temp_directory_path() /
        ("asio_owen_cors_test_" + tag + "_" + std::to_string(now));
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base / "config.d");
    return base;
}

Config load_config(const std::filesystem::path& base, const std::string& body) {
    std::ofstream out(base / "config.d" / "00-test.ini");
    out << body;
    out.close();
    Config cfg;
    cfg.load(base);
    return cfg;
}

}  // namespace

TEST(LoadCorsPolicy, AbsentSectionIsDisabled) {
    auto base = make_temp_config_dir("absent");
    auto cfg = load_config(base, "[security]\njwt_disabled = true\n");
    auto policy = load_cors_policy(cfg);
    EXPECT_FALSE(policy.enabled);
    std::filesystem::remove_all(base);
}

TEST(LoadCorsPolicy, ParsesEnabledSection) {
    auto base = make_temp_config_dir("enabled");
    auto cfg = load_config(base,
        "[cors]\n"
        "enabled = true\n"
        "allowed_origins = https://app.example.com, https://admin.example.com\n"
        "allowed_methods = GET, POST\n"
        "allowed_headers = Content-Type\n"
        "expose_headers = X-Total-Count\n"
        "allow_credentials = true\n"
        "max_age = 120\n");
    auto policy = load_cors_policy(cfg);
    EXPECT_TRUE(policy.enabled);
    EXPECT_FALSE(policy.allow_all_origins);
    EXPECT_EQ(policy.allowed_origins.count("https://app.example.com"), 1u);
    EXPECT_EQ(policy.allowed_origins.count("https://admin.example.com"), 1u);
    EXPECT_EQ(policy.allowed_methods, "GET, POST");
    EXPECT_EQ(policy.allowed_headers, "Content-Type");
    EXPECT_EQ(policy.expose_headers, "X-Total-Count");
    EXPECT_TRUE(policy.allow_credentials);
    EXPECT_EQ(policy.max_age, 120);
    std::filesystem::remove_all(base);
}

TEST(LoadCorsPolicy, WildcardOriginSetsAllowAll) {
    auto base = make_temp_config_dir("wildcard");
    auto cfg = load_config(base,
        "[cors]\n"
        "enabled = true\n"
        "allowed_origins = *\n");
    auto policy = load_cors_policy(cfg);
    EXPECT_TRUE(policy.enabled);
    EXPECT_TRUE(policy.allow_all_origins);
    std::filesystem::remove_all(base);
}

TEST(LoadCorsPolicy, DisabledSectionKeepsDefaults) {
    auto base = make_temp_config_dir("disabled");
    auto cfg = load_config(base,
        "[cors]\n"
        "enabled = false\n"
        "allowed_origins = https://app.example.com\n");
    auto policy = load_cors_policy(cfg);
    EXPECT_FALSE(policy.enabled);
    // Parsing short-circuits on disabled, so the whitelist is not populated.
    EXPECT_TRUE(policy.allowed_origins.empty());
    std::filesystem::remove_all(base);
}

TEST(LoadCorsPolicy, WildcardWithCredentialsIsDowngraded) {
    auto base = make_temp_config_dir("wildcard_creds");
    auto cfg = load_config(base,
        "[cors]\n"
        "enabled = true\n"
        "allowed_origins = *\n"
        "allow_credentials = true\n");
    auto policy = load_cors_policy(cfg);
    EXPECT_TRUE(policy.enabled);
    EXPECT_TRUE(policy.allow_all_origins);
    // Guardrail: reflecting any origin with credentials is unsafe, so the
    // loader forces credentials off for the wildcard case.
    EXPECT_FALSE(policy.allow_credentials);
    // With credentials disabled, the wildcard now resolves to "*".
    auto r = policy.resolve_origin("https://anything.example.com");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "*");
    std::filesystem::remove_all(base);
}
