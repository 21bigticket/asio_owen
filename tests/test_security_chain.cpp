#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <asio.hpp>

#include "security/security_rules.hpp"

namespace {

using tcp = asio::ip::tcp;

std::filesystem::path make_temp_config_dir(const std::string& tag) {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto base = std::filesystem::temp_directory_path() /
        ("asio_owen_security_test_" + tag + "_" + std::to_string(now));
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base / "config.d");
    std::ofstream snapshot_config(base / "config.d" / "99-test-rate-limit.ini");
    snapshot_config << "[rate_limit]\n"
                    << "snapshot_path = " << (base / "rate_limit.bin").string() << '\n';
    return base;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
}

// Connect a client to the acceptor, accept on the server side, and return the
// accepted socket. The accepted socket's remote_endpoint will be 127.0.0.1.
std::unique_ptr<tcp::socket> make_connected_pair(
    asio::io_context& ioc, tcp::acceptor& acceptor) {
    // Start an async accept.
    std::unique_ptr<tcp::socket> server_side = std::make_unique<tcp::socket>(ioc);
    asio::error_code accept_ec;
    bool accept_done = false;

    acceptor.async_accept(*server_side, [&](const asio::error_code& ec) {
        accept_ec = ec;
        accept_done = true;
    });

    // Connect from client side.
    tcp::socket client(ioc);
    asio::error_code connect_ec;
    client.connect(acceptor.local_endpoint(), connect_ec);
    EXPECT_FALSE(connect_ec) << "client connect failed: " << connect_ec.message();

    // Pump the io_context until the accept completes.
    ioc.restart();
    ioc.run();
    EXPECT_FALSE(accept_ec) << "accept failed: " << accept_ec.message();
    EXPECT_TRUE(accept_done);

    // Keep client alive for the duration of the test by storing it — leak
    // intentionally to avoid premature close; process exit will clean up.
    (void)client.release();

    return server_side;
}

// Build SecurityRules from an inline config body (the [security], [ip_blacklist],
// etc. sections). Returns the configured SecurityRules instance.
std::unique_ptr<SecurityRules> build_rules(const std::string& tag, const std::string& config_body) {
    auto base = make_temp_config_dir(tag);
    write_file(base / "config.d" / "00-test.ini", config_body);
    write_file(base / "config.d" / "99-test-snapshot.ini", "[rate_limit]\nsnapshot_path = " + (base / "rate_limit.bin").string() + "\n");

    auto cfg = std::make_unique<Config>();
    if (!cfg->load(base)) {
        ADD_FAILURE() << "config load failed";
        return nullptr;
    }
    auto rules = std::make_unique<SecurityRules>();
    rules->load_from_config(*cfg);
    std::filesystem::remove_all(base);
    return rules;
}

}  // namespace

class SecurityChainTest : public ::testing::Test {
protected:
    asio::io_context ioc;
    tcp::acceptor acceptor{ioc, {tcp::v4(), 0}};
    std::unique_ptr<tcp::socket> server_side;
    std::unique_ptr<SecurityRules> rules;

    void SetUp() override {
        // Default rules: permissive (no blacklist, no JWT required, generous rate).
        rules = build_rules("setup",
            "[security]\n"
            "case_sensitive_paths = false\n"
            "jwt_disabled = true\n"
            "[rate_limit]\n"
            "ip_rps = 10000\n"
            "ip_burst = 10000\n"
            "global_rps = 100000\n"
            "max_buckets = 1024\n");
        ASSERT_NE(rules, nullptr);

        server_side = make_connected_pair(ioc, acceptor);
        ASSERT_NE(server_side, nullptr);
    }
};

// ============== Steps 0-1: prechain gating ==============

TEST_F(SecurityChainTest, OptionsPreflightAlwaysAllowed) {
    auto r = rules->check(*server_side, "OPTIONS", "/api/anything", "", "");
    EXPECT_EQ(r.status_code, 0);
}

TEST_F(SecurityChainTest, CorsFastPathReflectsCurrentSnapshot) {
    EXPECT_FALSE(rules->cors_enabled_fast());

    rules = build_rules("cors_enabled",
        "[security]\n"
        "jwt_disabled = true\n"
        "[cors]\n"
        "enabled = true\n"
        "allowed_origins = https://example.test\n");

    ASSERT_TRUE(rules->cors_enabled_fast());
    auto policy = rules->cors_policy();
    ASSERT_NE(policy, nullptr);
    EXPECT_TRUE(policy->enabled);
    EXPECT_TRUE(policy->allowed_origins.contains("https://example.test"));
}

TEST_F(SecurityChainTest, RequestCheckKeepsSecurityAndCorsOnTheSameGeneration) {
    auto base = make_temp_config_dir("request_snapshot");
    write_file(base / "config.d" / "00-test.ini",
        "[security]\n"
        "jwt_disabled = true\n"
        "[cors]\n"
        "enabled = true\n"
        "allowed_origins = https://old.example.test\n");

    Config old_cfg;
    ASSERT_TRUE(old_cfg.load(base));
    SecurityRules local_rules;
    local_rules.load_from_config(old_cfg);

    auto old_request = local_rules.check_request(
        *server_side, "GET", "/api/anything", "", "");
    ASSERT_EQ(old_request.security.status_code, 0);
    ASSERT_NE(old_request.cors_policy, nullptr);
    ASSERT_TRUE(old_request.cors_policy->enabled);
    EXPECT_TRUE(old_request.cors_policy->allowed_origins.contains(
        "https://old.example.test"));

    write_file(base / "config.d" / "00-test.ini",
        "[security]\n"
        "jwt_disabled = true\n"
        "[cors]\n"
        "enabled = false\n");
    Config new_cfg;
    ASSERT_TRUE(new_cfg.load(base));
    local_rules.publish_reload(local_rules.prepare_reload(new_cfg));

    EXPECT_NE(old_request.generation, local_rules.generation());
    // The in-flight request retains its original immutable policy.
    EXPECT_TRUE(old_request.cors_policy->enabled);

    auto new_request = local_rules.check_request(
        *server_side, "GET", "/api/anything", "", "");
    EXPECT_EQ(new_request.generation, local_rules.generation());
    // Disabled CORS stays on the no-refcount fast path.
    EXPECT_EQ(new_request.cors_policy, nullptr);

    std::filesystem::remove_all(base);
}

TEST_F(SecurityChainTest, RootPathReturns404) {
    auto r = rules->check(*server_side, "GET", "/", "", "");
    EXPECT_EQ(r.status_code, 404);
}

// ============== Step 4: IP blacklist ==============

TEST_F(SecurityChainTest, IpBlacklistBlocksLoopback) {
    rules = build_rules("ip_block",
        "[security]\n"
        "case_sensitive_paths = false\n"
        "jwt_disabled = true\n"
        "[ip_blacklist]\n"
        "loopback = 127.0.0.0/8\n"
        "[rate_limit]\n"
        "ip_rps = 10000\n"
        "ip_burst = 10000\n"
        "global_rps = 100000\n"
        "max_buckets = 1024\n");

    auto r = rules->check(*server_side, "GET", "/api/x", "", "");
    EXPECT_EQ(r.status_code, 403);
    EXPECT_EQ(r.reason, "ip blocked");
}

TEST_F(SecurityChainTest, IpBlacklistExtractsRealIpFromXff) {
    // Trust loopback so XFF is parsed; the XFF claims to be from a blacklisted IP.
    rules = build_rules("xff_block",
        "[security]\n"
        "case_sensitive_paths = false\n"
        "jwt_disabled = true\n"
        "[trusted_proxies]\n"
        "trusted = 127.0.0.1\n"
        "[ip_blacklist]\n"
        "bad = 10.99.99.99/32\n"
        "[rate_limit]\n"
        "ip_rps = 10000\n"
        "ip_burst = 10000\n"
        "global_rps = 100000\n"
        "max_buckets = 1024\n");

    auto r = rules->check(*server_side, "GET", "/api/x",
        "10.99.99.99", "");
    EXPECT_EQ(r.status_code, 403);
    EXPECT_EQ(r.reason, "ip blocked");
}

// ============== Step 6: Auth whitelist ==============

TEST_F(SecurityChainTest, AuthWhitelistSkipsJwt) {
    // Configure JWT but whitelist /api/health — request should pass without auth.
    rules = build_rules("whitelist",
        "[security]\n"
        "case_sensitive_paths = false\n"
        "jwt_secret = super-secret-key\n"
        "jwt_issuer = asio_owen\n"
        "jwt_algorithm = HS256\n"
        "[auth_whitelist]\n"
        "h1 = /api/health\n"
        "[rate_limit]\n"
        "ip_rps = 10000\n"
        "ip_burst = 10000\n"
        "global_rps = 100000\n"
        "max_buckets = 1024\n");

    auto r = rules->check(*server_side, "GET", "/api/health", "",
        "Bearer invalid.token.here");
    EXPECT_EQ(r.status_code, 0);
}

// ============== Step 7: JWT ==============

TEST_F(SecurityChainTest, InvalidJwtReturns401) {
    rules = build_rules("jwt_invalid",
        "[security]\n"
        "case_sensitive_paths = false\n"
        "jwt_secret = super-secret-key\n"
        "jwt_issuer = asio_owen\n"
        "jwt_algorithm = HS256\n"
        "[rate_limit]\n"
        "ip_rps = 10000\n"
        "ip_burst = 10000\n"
        "global_rps = 100000\n"
        "max_buckets = 1024\n");

    auto r = rules->check(*server_side, "GET", "/api/protected", "",
        "Bearer not-a-valid-jwt");
    EXPECT_EQ(r.status_code, 401);
    EXPECT_EQ(r.reason, "invalid jwt");
}

// ============== Step 8: Path blacklist ==============

TEST_F(SecurityChainTest, PathBlacklistBlocksExactMatch) {
    rules = build_rules("path_block",
        "[security]\n"
        "case_sensitive_paths = false\n"
        "jwt_disabled = true\n"
        "[path_blacklist]\n"
        "/api/internal =\n"
        "[rate_limit]\n"
        "ip_rps = 10000\n"
        "ip_burst = 10000\n"
        "global_rps = 100000\n"
        "max_buckets = 1024\n");

    auto r = rules->check(*server_side, "GET", "/api/internal", "", "");
    EXPECT_EQ(r.status_code, 403);
    EXPECT_EQ(r.reason, "path blocked");
}

TEST_F(SecurityChainTest, PathBlacklistNormalizesConfiguredCase) {
    rules = build_rules("path_block_case",
        "[security]\n"
        "case_sensitive_paths = false\n"
        "jwt_disabled = true\n"
        "[path_blacklist]\n"
        "/Admin =\n"
        "[rate_limit]\n"
        "ip_rps = 10000\n"
        "ip_burst = 10000\n"
        "global_rps = 100000\n"
        "max_buckets = 1024\n");

    auto r = rules->check(*server_side, "GET", "/ADMIN", "", "");
    EXPECT_EQ(r.status_code, 403);
    EXPECT_EQ(r.reason, "path blocked");
}

TEST_F(SecurityChainTest, RejectsUnsafeEncodedPath) {
    auto r = rules->check(*server_side, "GET", "/api/foo%2F..%2Fadmin", "", "");
    EXPECT_EQ(r.status_code, 400);
    EXPECT_EQ(r.reason, "invalid path");
}

TEST_F(SecurityChainTest, PathBlacklistPrefixMatchesAtSegmentBoundary) {
    rules = build_rules("path_prefix",
        "[security]\n"
        "case_sensitive_paths = false\n"
        "jwt_disabled = true\n"
        "[path_blacklist]\n"
        "/api/internal/ =\n"
        "[rate_limit]\n"
        "ip_rps = 10000\n"
        "ip_burst = 10000\n"
        "global_rps = 100000\n"
        "max_buckets = 1024\n");

    EXPECT_EQ(rules->check(*server_side, "GET", "/api/internal/secret", "", "").status_code, 403);
    // boundary: /api/internalXXX should NOT match /api/internal/
    EXPECT_EQ(rules->check(*server_side, "GET", "/api/internalxxx", "", "").status_code, 0);
}

// ============== Step 5: Rate limit ==============

TEST_F(SecurityChainTest, RateLimitReturns429WhenBurstExhausted) {
    rules = build_rules("rl_tight",
        "[security]\n"
        "case_sensitive_paths = false\n"
        "jwt_disabled = true\n"
        "[rate_limit]\n"
        "ip_rps = 1\n"
        "ip_burst = 1\n"
        "global_rps = 100000\n"
        "max_buckets = 1024\n");

    // First request consumes the burst (1 token).
    auto first = rules->check(*server_side, "GET", "/api/x", "", "");
    ASSERT_EQ(first.status_code, 0);

    // Second request should hit the IP limit.
    auto second = rules->check(*server_side, "GET", "/api/x", "", "");
    EXPECT_EQ(second.status_code, 429);
    EXPECT_GT(second.retry_after_ms, 0);
}

TEST_F(SecurityChainTest, RateLimitRunsBeforeJwtVerification) {
    // Tight IP rate + required JWT. The first request drains burst and would
    // also fail JWT (no auth header), but rate limit wins because it runs first.
    rules = build_rules("rl_before_jwt",
        "[security]\n"
        "case_sensitive_paths = false\n"
        "jwt_secret = super-secret-key\n"
        "jwt_issuer = asio_owen\n"
        "jwt_algorithm = HS256\n"
        "[rate_limit]\n"
        "ip_rps = 1\n"
        "ip_burst = 1\n"
        "global_rps = 100000\n"
        "max_buckets = 1024\n");

    // First request: passes rate limit, fails JWT (no auth).
    auto first = rules->check(*server_side, "GET", "/api/x", "", "");
    EXPECT_EQ(first.status_code, 401);

    // Second request: rate limit rejects (returns 429, not 401).
    auto second = rules->check(*server_side, "GET", "/api/x", "", "");
    EXPECT_EQ(second.status_code, 429);
}

// ============== Reload ==============

TEST_F(SecurityChainTest, ReloadUpdatesRulesHot) {
    // Start permissive, then reload with blacklist.
    auto base = make_temp_config_dir("reload_v1");
    write_file(base / "config.d" / "00-test.ini",
        "[security]\n"
        "case_sensitive_paths = false\n"
        "jwt_disabled = true\n"
        "[rate_limit]\n"
        "ip_rps = 10000\n"
        "ip_burst = 10000\n"
        "global_rps = 100000\n"
        "max_buckets = 1024\n");

    Config cfg;
    ASSERT_TRUE(cfg.load(base));
    SecurityRules rules_local;
    rules_local.load_from_config(cfg);

    // Initially allowed.
    EXPECT_EQ(rules_local.check(*server_side, "GET", "/api/x", "", "").status_code, 0);

    // Update config file with blacklist.
    write_file(base / "config.d" / "00-test.ini",
        "[security]\n"
        "case_sensitive_paths = false\n"
        "jwt_disabled = true\n"
        "[ip_blacklist]\n"
        "lb = 127.0.0.0/8\n"
        "[rate_limit]\n"
        "ip_rps = 10000\n"
        "ip_burst = 10000\n"
        "global_rps = 100000\n"
        "max_buckets = 1024\n");

    Config cfg2;
    ASSERT_TRUE(cfg2.load(base));
    rules_local.reload(cfg2);

    // Now blocked.
    auto r = rules_local.check(*server_side, "GET", "/api/x", "", "");
    EXPECT_EQ(r.status_code, 403);

    std::filesystem::remove_all(base);
}

// Consecutive reloads on the same thread must invalidate TLS cache
TEST_F(SecurityChainTest, ConsecutiveReloadsInvalidateTLSCache) {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto base = std::filesystem::temp_directory_path() /
        ("asio_owen_test_consecutive_reload_" + std::to_string(now));
    std::filesystem::create_directories(base / "config.d");

    // Initial config: allow all
    write_file(base / "config.d" / "00-test.ini",
        "[security]\n"
        "case_sensitive_paths = false\n"
        "jwt_disabled = true\n"
        "[rate_limit]\n"
        "ip_rps = 10000\n"
        "ip_burst = 10000\n"
        "global_rps = 100000\n"
        "max_buckets = 1024\n");
    write_file(base / "config.d" / "99-test-snapshot.ini",
        "[rate_limit]\nsnapshot_path = " + (base / "rate_limit.bin").string() + "\n");

    Config cfg1;
    ASSERT_TRUE(cfg1.load(base));
    SecurityRules rules_local;
    rules_local.load_from_config(cfg1);

    // Initially allowed.
    EXPECT_EQ(rules_local.check(*server_side, "GET", "/api/test", "", "").status_code, 0);

    // First reload: add IP blacklist
    write_file(base / "config.d" / "01-ip.ini",
        "[ip_blacklist]\n"
        "lb = 127.0.0.0/8\n");

    Config cfg2;
    ASSERT_TRUE(cfg2.load(base));
    rules_local.reload(cfg2);

    // Should be blocked by IP blacklist
    auto r2 = rules_local.check(*server_side, "GET", "/api/test", "", "");
    EXPECT_EQ(r2.status_code, 403);
    EXPECT_EQ(r2.reason, "ip blocked");

    // Second reload: remove IP blacklist, add path blacklist
    std::filesystem::remove(base / "config.d" / "01-ip.ini");
    write_file(base / "config.d" / "02-path.ini",
        "[path_blacklist]\n"
        "/api/test =\n");  // empty value means blocked

    Config cfg3;
    ASSERT_TRUE(cfg3.load(base));
    rules_local.reload(cfg3);

    // Should be blocked by path blacklist (not IP)
    auto r3 = rules_local.check(*server_side, "GET", "/api/test", "", "");
    EXPECT_EQ(r3.status_code, 403);
    EXPECT_EQ(r3.reason, "path blocked");

    // Different path should be allowed
    EXPECT_EQ(rules_local.check(*server_side, "GET", "/api/other", "", "").status_code, 0);

    std::filesystem::remove_all(base);
}
