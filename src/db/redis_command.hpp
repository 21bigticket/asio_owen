#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct RedisCommandArgv {
    std::vector<const char*> argv;
    std::vector<size_t> argv_len;

    explicit RedisCommandArgv(const std::vector<std::string>& args);

    int argc() const;
};
