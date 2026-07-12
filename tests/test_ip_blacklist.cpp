#include <gtest/gtest.h>

#include "security/ip_blacklist.hpp"
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
