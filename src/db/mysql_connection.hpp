#pragma once

#include <mysql/mysql.h>
#include <string>

struct MysqlConnectionConfig {
    std::string host;
    int port = 3306;
    std::string user;
    std::string pass;
    std::string db;
    int connect_timeout_ms = 1000;
    int read_timeout_ms = 500;
};

MYSQL* create_mysql_connection_with_timeout(const MysqlConnectionConfig& cfg);
int mysql_ping_with_timeout(MYSQL* conn, int read_timeout_ms);
