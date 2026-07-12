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

TEST(HttpProtocol, ParseHexSizeLineHandlesUpperLowerAndExtension) {
    EXPECT_EQ(parse_hex_size_line("1a"), std::optional<size_t>(26u));
    EXPECT_EQ(parse_hex_size_line("1A"), std::optional<size_t>(26u));
    EXPECT_EQ(parse_hex_size_line("1A;foo=bar"), std::optional<size_t>(26u));
    EXPECT_EQ(parse_hex_size_line("  1f ;x=y "), std::optional<size_t>(31u));
    EXPECT_EQ(parse_hex_size_line("0"), std::optional<size_t>(0u));
}

TEST(HttpProtocol, ParseHexSizeLineRejectsInvalidHex) {
    EXPECT_FALSE(parse_hex_size_line("xyz"));
    EXPECT_FALSE(parse_hex_size_line(""));
    EXPECT_FALSE(parse_hex_size_line(";ext"));
    EXPECT_FALSE(parse_hex_size_line("0x10"));
}

TEST(HttpProtocol, ParseDecimalSizeAcceptsAndRejects) {
    EXPECT_EQ(parse_decimal_size("0"), std::optional<size_t>(0u));
    EXPECT_EQ(parse_decimal_size(" 42 "), std::optional<size_t>(42u));
    EXPECT_FALSE(parse_decimal_size(""));
    EXPECT_FALSE(parse_decimal_size("abc"));
    EXPECT_FALSE(parse_decimal_size("-1"));
    EXPECT_FALSE(parse_decimal_size("12.5"));
}

TEST(HttpProtocol, UpdateHeaderStateConnectionClose) {
    HeaderParseState state;
    update_header_state("Connection", "close", state);
    EXPECT_TRUE(state.connection_close);
    EXPECT_FALSE(state.connection_keep_alive);
}

TEST(HttpProtocol, UpdateHeaderStateConnectionKeepAlive) {
    HeaderParseState state;
    update_header_state("connection", "Keep-Alive", state);
    EXPECT_TRUE(state.connection_keep_alive);
    EXPECT_FALSE(state.connection_close);
}

TEST(HttpProtocol, UpdateHeaderStateTransferEncodingChunked) {
    HeaderParseState state;
    update_header_state("Transfer-Encoding", "chunked", state);
    EXPECT_TRUE(state.has_transfer_encoding);
    EXPECT_TRUE(state.is_chunked);
}

TEST(HttpProtocol, UpdateHeaderStateTransferEncodingGzipOnly) {
    HeaderParseState state;
    update_header_state("Transfer-Encoding", "gzip", state);
    EXPECT_TRUE(state.has_transfer_encoding);
    EXPECT_FALSE(state.is_chunked);
}

TEST(HttpProtocol, UpdateHeaderStateTransferEncodingGzipChunked) {
    HeaderParseState state;
    update_header_state("Transfer-Encoding", "gzip, chunked", state);
    EXPECT_TRUE(state.has_transfer_encoding);
    EXPECT_TRUE(state.is_chunked);
}

TEST(HttpProtocol, UpdateHeaderStateTransferEncodingChunkedGzipIsNotChunked) {
    HeaderParseState state;
    update_header_state("Transfer-Encoding", "chunked, gzip", state);
    EXPECT_TRUE(state.has_transfer_encoding);
    EXPECT_FALSE(state.is_chunked);
}

TEST(HttpProtocol, UpdateHeaderStateDuplicateContentLength) {
    HeaderParseState state;
    update_header_state("Content-Length", "5", state);
    update_header_state("Content-Length", "6", state);
    EXPECT_TRUE(state.duplicate_content_length);
    ASSERT_TRUE(state.content_length.has_value());
    EXPECT_EQ(*state.content_length, 5u);
}

TEST(HttpProtocol, UpdateHeaderStateInvalidContentLength) {
    HeaderParseState state;
    update_header_state("Content-Length", "abc", state);
    EXPECT_TRUE(state.invalid_content_length);
    EXPECT_FALSE(state.content_length.has_value());
}

TEST(HttpProtocol, ParseHeaderFieldsHandlesMultipleLines) {
    std::string headers =
        "Content-Length: 5\r\n"
        "X-Empty:\r\n"
        "X-Trim-Me:    value    \r\n"
        "Content-Type: application/json";
    std::vector<std::pair<std::string, std::string>> out;
    HeaderParseState state;
    parse_header_fields(headers, out, state);

    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0].first, "Content-Length");
    EXPECT_EQ(out[0].second, "5");
    EXPECT_EQ(out[1].first, "X-Empty");
    EXPECT_EQ(out[1].second, "");
    EXPECT_EQ(out[2].first, "X-Trim-Me");
    EXPECT_EQ(out[2].second, "value");
    EXPECT_EQ(out[3].first, "Content-Type");
    ASSERT_TRUE(state.content_length.has_value());
    EXPECT_EQ(*state.content_length, 5u);
}

TEST(HttpProtocol, IsHopByHopHeaderMatchesAllRfcHeaders) {
    EXPECT_TRUE(is_hop_by_hop_header("Connection"));
    EXPECT_TRUE(is_hop_by_hop_header("Keep-Alive"));
    EXPECT_TRUE(is_hop_by_hop_header("Proxy-Authenticate"));
    EXPECT_TRUE(is_hop_by_hop_header("Proxy-Authorization"));
    EXPECT_TRUE(is_hop_by_hop_header("TE"));
    EXPECT_TRUE(is_hop_by_hop_header("Trailer"));
    EXPECT_TRUE(is_hop_by_hop_header("Transfer-Encoding"));
    EXPECT_TRUE(is_hop_by_hop_header("Upgrade"));

    EXPECT_FALSE(is_hop_by_hop_header("Content-Type"));
    EXPECT_FALSE(is_hop_by_hop_header("Authorization"));
}

TEST(HttpProtocol, HeaderIequalsHandlesCase) {
    EXPECT_TRUE(header_iequals("connection", "Connection"));
    EXPECT_TRUE(header_iequals("CONTENT-TYPE", "content-type"));
    EXPECT_FALSE(header_iequals("connection", "content-type"));
    EXPECT_FALSE(header_iequals("abc", "abcd"));
    EXPECT_TRUE(header_iequals("", ""));
}

TEST(HttpProtocol, SplitConnectionTokensExtractsMultiple) {
    auto tokens = split_connection_tokens("close, keep-alive, foo");
    EXPECT_TRUE(tokens.close);
    EXPECT_TRUE(tokens.keep_alive);
}

TEST(HttpProtocol, SanitizeHeaderValueRedactsAuthorization) {
    EXPECT_EQ(sanitize_header_value("Authorization", "Bearer xyz"), "<redacted len=10>");
    EXPECT_EQ(sanitize_header_value("Content-Type", "application/json"), "application/json");

    std::string long_val(200, 'x');
    auto truncated = sanitize_header_value("X-Custom", long_val);
    EXPECT_NE(truncated.find("truncated"), std::string::npos);
}
