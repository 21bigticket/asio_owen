#include "mysql_connection.hpp"

#include <mutex>

#include "../common/logger.hpp"

void ensure_mysql_thread_initialized() {
    static std::once_flag library_once;
    std::call_once(library_once, [] {
        mysql_library_init(0, nullptr, nullptr);
    });

    struct MysqlThreadState {
        MysqlThreadState() { mysql_thread_init(); }
        ~MysqlThreadState() { mysql_thread_end(); }
    };

    thread_local MysqlThreadState state;
    (void)state;
}

MYSQL* create_mysql_connection_with_timeout(const MysqlConnectionConfig& cfg) {
    ensure_mysql_thread_initialized();

    MYSQL* conn = mysql_init(nullptr);
    if (!conn) {
        LOG_ERROR("MySQL init failed");
        return nullptr;
    }

    unsigned int timeout_sec = cfg.connect_timeout_ms / 1000;
    if (timeout_sec < 1) timeout_sec = 1;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_sec);

    unsigned int read_timeout_sec = (cfg.query_timeout_ms + 999) / 1000;
    if (read_timeout_sec < 1) read_timeout_sec = 1;
    mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &read_timeout_sec);

    if (!mysql_real_connect(conn, cfg.host.c_str(), cfg.user.c_str(),
            cfg.pass.c_str(), cfg.db.c_str(), cfg.port, nullptr, 0)) {
        LOG_ERROR("MySQL connect failed: ", mysql_error(conn));
        mysql_close(conn);
        return nullptr;
    }
    return conn;
}

int mysql_ping_with_timeout(MYSQL* conn, int read_timeout_ms) {
    ensure_mysql_thread_initialized();
    unsigned int ping_timeout_sec = static_cast<unsigned int>((read_timeout_ms + 999) / 1000);
    if (ping_timeout_sec < 1) ping_timeout_sec = 1;

    unsigned int old_timeout_sec = 0;
    bool restore_timeout = mysql_get_option(conn, MYSQL_OPT_READ_TIMEOUT, &old_timeout_sec) == 0;

    if (mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &ping_timeout_sec) != 0) {
        LOG_WARN("MySQL ping timeout option failed: ", mysql_error(conn));
    }
    int rc = mysql_ping(conn);

    if (restore_timeout &&
        mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &old_timeout_sec) != 0) {
        LOG_WARN("MySQL ping timeout restore failed: ", mysql_error(conn));
    }
    return rc;
}
