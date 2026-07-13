#include <gtest/gtest.h>

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
