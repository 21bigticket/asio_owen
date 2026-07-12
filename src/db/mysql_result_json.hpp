#pragma once

#include <mysql/mysql.h>
#include <optional>
#include <string>
#include <string_view>

void append_json_string(std::string& out, const char* data, unsigned long len);
std::string mysql_result_to_json(MYSQL_RES* mr);

// Locate the first string value in a JSON document and return its (unescaped) content.
// State-machine parser; does not allocate on hot path unless the value is found.
// Returns nullopt on: no colon found, non-string first value, unterminated string.
std::optional<std::string> extract_first_string_value(std::string_view json);
