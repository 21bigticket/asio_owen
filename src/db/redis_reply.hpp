#pragma once

#include <hiredis/hiredis.h>
#include <cstdint>
#include <string>
#include <vector>

struct RedisReplyData {
    bool ok = false;
    std::string error;
    std::string str;
    int64_t integer = 0;
    std::vector<std::string> elements;
    std::string type;
};

void parse_redis_reply(redisReply* reply, RedisReplyData& r);
