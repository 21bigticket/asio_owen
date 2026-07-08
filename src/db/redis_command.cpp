#include "redis_command.hpp"

RedisCommandArgv::RedisCommandArgv(const std::vector<std::string>& args) {
    argv.reserve(args.size());
    argv_len.reserve(args.size());
    for (const auto& arg : args) {
        argv.push_back(arg.data());
        argv_len.push_back(arg.size());
    }
}

int RedisCommandArgv::argc() const {
    return static_cast<int>(argv.size());
}
