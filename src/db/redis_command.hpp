#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct RedisCommandArgv {
    // Owns the argument strings so argv pointers stay valid for the object's
    // lifetime. Storing only const char* into the caller's vector was a
    // use-after-free when the caller passed a temporary (get_sync did): the
    // temporary died at the end of the statement, leaving argv dangling, which
    // surfaced under load as intermittent "ERR unknown command ``" (~0.5%).
    std::vector<std::string> args;
    std::vector<const char*> argv;
    std::vector<size_t> argv_len;

    explicit RedisCommandArgv(const std::vector<std::string>& in);

    // Copy/move would leave argv pointing into the wrong args vector.
    RedisCommandArgv(const RedisCommandArgv&) = delete;
    RedisCommandArgv& operator=(const RedisCommandArgv&) = delete;
    RedisCommandArgv(RedisCommandArgv&&) = delete;
    RedisCommandArgv& operator=(RedisCommandArgv&&) = delete;

    int argc() const;
};
