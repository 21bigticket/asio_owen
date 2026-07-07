#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "http/http_protocol.hpp"

TEST(HttpProtocol, ParsesHeaderFieldsWithoutMutatingInput) {
    std::string headers =
        "Content-Length: 5\r\n"
        "Connection: keep-alive\r\n"
        "Transfer-Encoding: gzip, chunked";
    std::vector<std::pair<std::string, std::string>> out;
    HeaderParseState state;

    parse_header_fields(headers, out, state);

    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].first, "Content-Length");
    EXPECT_EQ(out[0].second, "5");
    ASSERT_TRUE(state.content_length.has_value());
    EXPECT_EQ(*state.content_length, 5u);
    EXPECT_TRUE(state.connection_keep_alive);
    EXPECT_TRUE(state.has_transfer_encoding);
    EXPECT_TRUE(state.is_chunked);
    EXPECT_EQ(headers.find("Content-Length"), 0u);
}

TEST(HttpProtocol, DetectsDuplicateConflictingContentLength) {
    std::vector<std::pair<std::string, std::string>> out;
    HeaderParseState state;

    parse_header_fields("Content-Length: 5\r\nContent-Length: 6", out, state);

    EXPECT_TRUE(state.duplicate_content_length);
}

TEST(HttpProtocol, ContainsHeaderNameIsCaseInsensitive) {
    std::vector<std::string> filtered = {"connection", "X-Hop-Token"};

    EXPECT_TRUE(contains_header_name(filtered, "Connection"));
    EXPECT_TRUE(contains_header_name(filtered, "x-hop-token"));
    EXPECT_FALSE(contains_header_name(filtered, "content-type"));
}
