#include "mysql_connection.hpp"

#include "../common/logger.hpp"

MYSQL* create_mysql_connection_with_timeout(const MysqlConnectionConfig& cfg) {
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) {
        LOG_ERROR("MySQL init failed");
        return nullptr;
    }

    unsigned int timeout_sec = cfg.connect_timeout_ms / 1000;
    if (timeout_sec < 1) timeout_sec = 1;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_sec);

    if (!mysql_real_connect(conn, cfg.host.c_str(), cfg.user.c_str(),
            cfg.pass.c_str(), cfg.db.c_str(), cfg.port, nullptr, 0)) {
        LOG_ERROR("MySQL connect failed: ", mysql_error(conn));
        mysql_close(conn);
        return nullptr;
    }
    return conn;
}

int mysql_ping_with_timeout(MYSQL* conn, int read_timeout_ms) {
    unsigned int rt = (read_timeout_ms + 999) / 1000;
    if (rt < 1) rt = 1;
    mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &rt);

    int ret = mysql_ping(conn);

    unsigned int restore_rt = 0;
    mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &restore_rt);

    return ret;
}
