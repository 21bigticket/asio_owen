#include <gtest/gtest.h>

#include <random>

#include "security/auth_whitelist.hpp"
#include "security/ip_blacklist.hpp"
#include "security/path_blacklist.hpp"
#include "security/real_ip.hpp"

TEST(IpBlacklist, IPv4Cidr8MatchesOnlyExpectedRange) {
    IpBlacklist blacklist;
    blacklist.reload({"10.0.0.0/8"});

    EXPECT_TRUE(blacklist.is_blocked("10.1.2.3"));
    EXPECT_TRUE(blacklist.is_blocked("10.255.255.255"));
    EXPECT_FALSE(blacklist.is_blocked("11.0.0.1"));
    EXPECT_FALSE(blacklist.is_blocked("192.168.1.1"));
}

TEST(IpBlacklist, IPv4Cidr32MatchesSingleAddress) {
    IpBlacklist blacklist;
    blacklist.reload({"10.0.0.1/32"});

    EXPECT_TRUE(blacklist.is_blocked("10.0.0.1"));
    EXPECT_FALSE(blacklist.is_blocked("10.0.0.2"));
}

TEST(IpBlacklist, IPv6MappedIPv4MatchesIPv4Rule) {
    IpBlacklist blacklist;
    blacklist.reload({"10.0.0.1/32"});

    EXPECT_TRUE(blacklist.is_blocked("::ffff:10.0.0.1"));
    EXPECT_FALSE(blacklist.is_blocked("::ffff:10.0.0.2"));
}

TEST(IpBlacklist, IPv6Cidr128MatchesSingleAddress) {
    IpBlacklist blacklist;
    blacklist.reload({"::1/128"});

    EXPECT_TRUE(blacklist.is_blocked("::1"));
    EXPECT_FALSE(blacklist.is_blocked("::2"));
}

TEST(NormalizeIp, ParsesIPv4AndSetsFields) {
    auto n = normalize_ip("10.0.0.1");
    EXPECT_TRUE(n.parse_ok);
    EXPECT_EQ(n.str, "10.0.0.1");
    EXPECT_FALSE(n.addr.is_v6());
}

TEST(NormalizeIp, UnmapsIPv6MappedIPv4) {
    auto n = normalize_ip("::ffff:10.0.0.1");
    EXPECT_TRUE(n.parse_ok);
    EXPECT_EQ(n.str, "10.0.0.1");
    EXPECT_FALSE(n.addr.is_v6());
}

TEST(NormalizeIp, KeepsPlainIPv6) {
    auto n = normalize_ip("::1");
    EXPECT_TRUE(n.parse_ok);
    EXPECT_EQ(n.str, "::1");
    EXPECT_TRUE(n.addr.is_v6());
}

TEST(NormalizeIp, InvalidStringIsNotParseOk) {
    auto n = normalize_ip("not-an-ip");
    EXPECT_FALSE(n.parse_ok);
    EXPECT_EQ(n.str, "not-an-ip");
}

TEST(NormalizeIp, WrapperPreservesStringBehavior) {
    EXPECT_EQ(normalize_ip_str("::ffff:10.0.0.1"), "10.0.0.1");
    EXPECT_EQ(normalize_ip_str("not-an-ip"), "not-an-ip");
}

TEST(MatchCidr, RegressionMatchesAfterNormalize) {
    // Regression: match_cidr must accept both v4 and v6-mapped-v4 forms
    // without second make_address() failure.
    auto rule = parse_cidr_rule("10.0.0.0/8");
    ASSERT_TRUE(rule.has_value());
    EXPECT_TRUE(match_cidr("10.1.2.3", *rule));
    EXPECT_TRUE(match_cidr("::ffff:10.1.2.3", *rule));
    EXPECT_FALSE(match_cidr("11.0.0.1", *rule));
    EXPECT_FALSE(match_cidr("not-an-ip", *rule));
}

TEST(MatchCidr, HandlesIPv6Rule) {
    auto rule = parse_cidr_rule("2001:db8::/32");
    ASSERT_TRUE(rule.has_value());
    EXPECT_TRUE(match_cidr("2001:db8::1", *rule));
    EXPECT_FALSE(match_cidr("2001:db9::1", *rule));
}

TEST(SecurityRuleValues, HandleDeterministicFuzzInputsWithoutInvalidAccess) {
    std::minstd_rand rng(0xC1D2);
    constexpr std::string_view alphabet = "/:.%ABCxyz012";
    std::vector<std::string> items;
    std::vector<std::pair<std::string, std::string>> path_items;
    for (int i = 0; i < 500; ++i) {
        std::string value;
        for (int j = 0; j < 24; ++j) value += alphabet[rng() % alphabet.size()];
        items.push_back(value);
        path_items.emplace_back("/" + value, i % 2 == 0 ? "" : "role:admin");
    }

    IpBlacklist blacklist;
    AuthWhitelist whitelist;
    PathBlacklist paths;
    blacklist.reload(items);
    whitelist.reload(items);
    paths.reload(path_items);
    for (const auto& value : items) {
        EXPECT_NO_THROW(blacklist.is_blocked(value));
        EXPECT_NO_THROW(whitelist.is_whitelisted("/" + value, value));
        EXPECT_NO_THROW(paths.check("/" + value));
    }
}
