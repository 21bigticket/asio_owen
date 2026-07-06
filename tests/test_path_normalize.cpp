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

TEST(PathNormalize, PreservesEncodedSlash) {
    auto normalized = normalize_path("/api/foo%2Fbar");

    EXPECT_EQ(normalized.path, "/api/foo%2fbar");
}

TEST(PathNormalize, CaseSensitiveModePreservesCase) {
    auto normalized = normalize_path("/API/Health", true);

    EXPECT_EQ(normalized.path, "/API/Health");
}
