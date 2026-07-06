#include <gtest/gtest.h>

#include "security/ip_blacklist.hpp"

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
