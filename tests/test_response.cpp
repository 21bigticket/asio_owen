#include <gtest/gtest.h>

#include <string>

#include "http/response.hpp"
#include "http/response_builder.hpp"

TEST(Response, JsonEscapeHandlesBasicSpecialChars) {
    auto s = resp_err(500, "syntax error near \"WHERE\"");
    // The embedded quotes must appear as \" in the output.
    EXPECT_NE(s.find("\\\"WHERE\\\""), std::string::npos) << s;
    // The envelope must remain balanced.
    EXPECT_NE(s.find("{\"code\":500,\"msg\":"), std::string::npos) << s;
    EXPECT_NE(s.rfind(",\"data\":null}"), std::string::npos) << s;
}

TEST(Response, JsonEscapeHandlesControlChars) {
    auto s = resp_err(500, std::string("line1\nline2\ttab\rcr"));
    EXPECT_NE(s.find("\\n"), std::string::npos) << s;
    EXPECT_NE(s.find("\\t"), std::string::npos) << s;
    EXPECT_NE(s.find("\\r"), std::string::npos) << s;
    // No raw control bytes between the msg quotes.
    auto start = s.find("\"msg\":\"") + 6;
    auto end = s.find("\",", start);
    ASSERT_NE(end, std::string::npos);
    for (size_t i = start; i < end; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20) FAIL() << "raw control char at offset " << i;
    }
}

TEST(Response, JsonEscapeHandlesBackslash) {
    auto s = resp_err(500, std::string("path\\to\\thing"));
    // Input has 2 backslashes; output must have 4 (each escaped).
    EXPECT_NE(s.find("\\\\to\\\\t"), std::string::npos) << s;
}

TEST(Response, JsonEscapeHandlesLowControlBytes) {
    auto s = resp_err(500, std::string(1, '\x01') + std::string(1, '\x1f'));
    EXPECT_NE(s.find("\\u0001"), std::string::npos) << s;
    EXPECT_NE(s.find("\\u001f"), std::string::npos) << s;
}

TEST(Response, RespOkStrEscapesData) {
    auto s = resp_ok_str("value\"with\"quotes");
    EXPECT_NE(s.find("\\\"with\\\""), std::string::npos) << s;
}

TEST(Response, RespOkPassesRawData) {
    // resp_ok is for callers that have already-serialized JSON; verbatim splice.
    auto s = resp_ok("[1,2,3]");
    EXPECT_NE(s.find("\"data\":[1,2,3]"), std::string::npos) << s;
}

TEST(Response, DownstreamResponseDropsUnsafeHeaderValue) {
    HttpContext ctx;
    ctx.status_code = 200;
    ctx.response_body = "{}";
    ctx.response_headers.emplace_back("X-Good", "ok");
    ctx.response_headers.emplace_back("X-Bad", "ok\r\nInjected: yes");

    auto s = build_downstream_response(ctx, "GET", true);

    EXPECT_NE(s.find("X-Good: ok\r\n"), std::string::npos) << s;
    EXPECT_EQ(s.find("Injected: yes"), std::string::npos) << s;
}
