#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "db/mysql_result_json.hpp"

TEST(MysqlResultJson, EscapesJsonStringContent) {
    std::string out;
    const char value[] = {'a', '"', 'b', '\\', 'c', '\n', '\r', '\t', '\x01'};

    append_json_string(out, value, sizeof(value));

    EXPECT_EQ(out, "\"a\\\"b\\\\c\\n\\r\\t\\u0001\"");
}

TEST(MysqlResultJson, EscapesStringWithEmbeddedNul) {
    std::string out;
    const char value[] = {'a', '\0', 'b'};

    append_json_string(out, value, sizeof(value));

    EXPECT_EQ(out, "\"a\\u0000b\"");
}

TEST(ExtractFirstStringValue, ReturnsSimpleStringValue) {
    auto v = extract_first_string_value("[{\"name\":\"from_mysql\"}]");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "from_mysql");
}

TEST(ExtractFirstStringValue, SkipsLeadingWhitespace) {
    auto v = extract_first_string_value("{\"name\":   \"hello\"}");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "hello");
}

TEST(ExtractFirstStringValue, HandlesEscapedQuotes) {
    auto v = extract_first_string_value("{\"k\":\"a\\\"b\"}");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "a\"b");
}

TEST(ExtractFirstStringValue, HandlesBackslash) {
    auto v = extract_first_string_value("{\"k\":\"a\\\\b\"}");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "a\\b");
}

TEST(ExtractFirstStringValue, ReturnsNulloptForNonStringValue) {
    EXPECT_FALSE(extract_first_string_value("{\"k\":123}"));
    EXPECT_FALSE(extract_first_string_value("{\"k\":true}"));
    EXPECT_FALSE(extract_first_string_value("{\"k\":null}"));
    EXPECT_FALSE(extract_first_string_value("{\"k\":[1,2]}"));
}

TEST(ExtractFirstStringValue, ReturnsNulloptForEmptyArray) {
    EXPECT_FALSE(extract_first_string_value("[]"));
    EXPECT_FALSE(extract_first_string_value(""));
    EXPECT_FALSE(extract_first_string_value("garbage"));
}

TEST(ExtractFirstStringValue, HandlesUnicodeEscape) {
    auto v = extract_first_string_value("{\"k\":\"a\\u0041b\"}");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "aAb");
}

TEST(ExtractFirstStringValue, ReturnsNulloptForUnterminatedString) {
    EXPECT_FALSE(extract_first_string_value("{\"k\":\"abc"));
    EXPECT_FALSE(extract_first_string_value("{\"k\":\"abc\\"));
}

TEST(ExtractFirstStringValue, FindsFirstStringValueAcrossMultipleFields) {
    auto v = extract_first_string_value("{\"id\":42,\"name\":\"alpha\",\"tag\":\"beta\"}");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "alpha");
}
