#pragma once

#include <mysql/mysql.h>
#include <string>

void append_json_string(std::string& out, const char* data, unsigned long len);
std::string mysql_result_to_json(MYSQL_RES* mr);
