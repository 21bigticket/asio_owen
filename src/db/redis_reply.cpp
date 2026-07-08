#include "redis_reply.hpp"

void parse_redis_reply(redisReply* reply, RedisReplyData& r) {
    switch (reply->type) {
        case REDIS_REPLY_STRING:
            r.type = "string";
            r.str.assign(reply->str, reply->len);
            r.ok = true;
            break;
        case REDIS_REPLY_INTEGER:
            r.type = "integer";
            r.integer = reply->integer;
            r.ok = true;
            break;
        case REDIS_REPLY_ARRAY:
            r.type = "array";
            r.ok = true;
            for (size_t i = 0; i < reply->elements; ++i) {
                if (reply->element[i]->type == REDIS_REPLY_STRING ||
                    reply->element[i]->type == REDIS_REPLY_STATUS) {
                    r.elements.emplace_back(reply->element[i]->str, reply->element[i]->len);
                } else if (reply->element[i]->type == REDIS_REPLY_INTEGER) {
                    r.elements.push_back(std::to_string(reply->element[i]->integer));
                } else if (reply->element[i]->type == REDIS_REPLY_NIL) {
                    r.elements.push_back("(nil)");
                } else {
                    r.elements.push_back("(unknown)");
                }
            }
            break;
        case REDIS_REPLY_STATUS:
            r.type = "string";
            r.str.assign(reply->str, reply->len);
            r.ok = true;
            break;
        case REDIS_REPLY_ERROR:
            r.type = "error";
            r.error.assign(reply->str, reply->len);
            break;
        case REDIS_REPLY_NIL:
            r.type = "nil";
            r.ok = true;
            break;
        default:
            r.type = "unknown";
            r.ok = true;
            break;
    }
}
