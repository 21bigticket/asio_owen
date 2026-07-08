#include <gtest/gtest.h>

#include <string>

#include "db/mysql_result_json.hpp"

TEST(MysqlResultJson, EscapesJsonStringContent) {
    std::string out;
    const char value[] = {'a', '"', 'b', '\\', 'c', '\n', '\r', '\t', '\x01'};

    append_json_string(out, value, sizeof(value));

    EXPECT_EQ(out, R"("a\"b\\c\n\r\t\u0001")");
}

TEST(MysqlResultJson, EscapesStringWithEmbeddedNul) {
    std::string out;
    const char value[] = {'a', '\0', 'b'};

    append_json_string(out, value, sizeof(value));

    EXPECT_EQ(out, "\"a\\u0000b\"");
}
