#include "redis_command.hpp"

// Copy the caller's strings into our own storage, then point argv at them.
// The caller may pass a temporary; copying here keeps the data alive for as
// long as this object lives.
RedisCommandArgv::RedisCommandArgv(const std::vector<std::string>& in) : args(in) {
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
