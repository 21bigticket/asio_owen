#pragma once
#include <string>
#include <sstream>

enum HttpCode {
    OK = 0,
    PARAM_ERROR = 400,
    UNAUTHORIZED = 401,
    NOT_FOUND = 404,
    SERVER_ERROR = 500,
    DB_ERROR = 501,
    TIMEOUT = 504,
};

inline std::string json_resp(int code, const std::string& msg, const std::string& data = "null") {
    std::ostringstream oss;
    oss << R"({"code":)" << code 
        << R"(,"msg":")" << msg << R"(")"
        << R"(,"data":)" << data 
        << "}";
    return oss.str();
}

inline std::string resp_ok(const std::string& data = "null") {
    return json_resp(OK, "ok", data);
}

inline std::string resp_ok_str(const std::string& data) {
    std::ostringstream oss;
    oss << "\"" << data << "\"";
    return json_resp(OK, "ok", oss.str());
}

inline std::string resp_err(int code, const std::string& msg) {
    return json_resp(code, msg);
}
