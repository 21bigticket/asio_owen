#include <gtest/gtest.h>

#include <string>

#include "http/json_transform.hpp"

namespace {

std::string convert(std::string json) {
    json_keys_snake_to_camel(json);
    return json;
}

}  // namespace

TEST(JsonTransform, ConvertsObjectKeysOnly) {
    EXPECT_EQ(convert(R"({"a_b":1})"), R"({"aB":1})");
    EXPECT_EQ(convert(R"({"outer_obj":{"inner_key":"value_text"}})"),
        R"({"outerObj":{"innerKey":"value_text"}})");
}

TEST(JsonTransform, PreservesArrayStringValues) {
    EXPECT_EQ(convert(R"({"list":["hello_world","foo_bar"]})"),
        R"({"list":["hello_world","foo_bar"]})");
}

TEST(JsonTransform, ConvertsObjectKeysInsideArrays) {
    EXPECT_EQ(convert(R"({"list":[{"item_key":"foo_bar"}]})"),
        R"({"list":[{"itemKey":"foo_bar"}]})");
}

TEST(JsonTransform, PreservesEscapedStringContent) {
    EXPECT_EQ(convert(R"({"a\"b_key":1})"), R"({"a\"bKey":1})");
    EXPECT_EQ(convert(R"({"key":"a\"b_c"})"), R"({"key":"a\"b_c"})");
    EXPECT_EQ(convert(R"({"arr":["a\"b_c",{"inner_key":"x_y"}]})"),
        R"({"arr":["a\"b_c",{"innerKey":"x_y"}]})");
}
