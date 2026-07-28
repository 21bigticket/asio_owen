#include <gtest/gtest.h>

#include <random>

#include "security/path_normalize.hpp"

TEST(PathNormalize, StripsQueryStringBeforeMatching) {
    auto normalized = normalize_path("/api/health?token=secret");

    EXPECT_EQ(normalized.path, "/api/health");
}

TEST(PathNormalize, ResolvesDotSegmentsAndPercentDecode) {
    auto normalized = normalize_path("/API/%70ublic/../Health");

    EXPECT_EQ(normalized.path, "/api/health");
}

TEST(PathNormalize, RejectsEncodedSlash) {
    auto normalized = normalize_path("/api/foo%2Fbar");

    EXPECT_FALSE(normalized.valid);
}

TEST(PathNormalize, RejectsEncodedNul) {
    auto normalized = normalize_path("/api/foo%00bar");

    EXPECT_FALSE(normalized.valid);
}

TEST(PathNormalize, RejectsDoubleEncodingResidualPercent) {
    auto normalized = normalize_path("/api/%252e%252e/admin");

    EXPECT_FALSE(normalized.valid);
}

TEST(PathNormalize, CaseSensitiveModePreservesCase) {
    auto normalized = normalize_path("/API/Health", true);

    EXPECT_EQ(normalized.path, "/API/Health");
}

TEST(PathNormalize, ValidOutputIsIdempotentAcrossDeterministicFuzzInputs) {
    std::minstd_rand rng(0x5EED);
    constexpr std::string_view alphabet = "/.%?ABCxyz012";
    for (int i = 0; i < 2000; ++i) {
        std::string raw = "/";
        for (int j = 0; j < 32; ++j) raw += alphabet[rng() % alphabet.size()];

        auto once = normalize_path(raw);
        if (!once.valid) continue;
        auto twice = normalize_path(once.path);
        ASSERT_TRUE(twice.valid) << raw;
        EXPECT_EQ(twice.path, once.path) << raw;
    }
}
