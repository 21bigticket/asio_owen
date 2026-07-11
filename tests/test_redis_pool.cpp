#include <gtest/gtest.h>

#include <hiredis/hiredis.h>
#include <cstdint>
#include <string>
#include <vector>

#include "db/redis_command.hpp"
#include "db/redis_connection.hpp"
#include "db/redis_pool_stats.hpp"
#include "db/redis_reply.hpp"

TEST(RedisReply, ParsesStringStatusIntegerNilAndError) {
    redisReply string_reply{};
    string_reply.type = REDIS_REPLY_STRING;
    string_reply.str = const_cast<char*>("hello");
    string_reply.len = 5;

    RedisReplyData out;
    parse_redis_reply(&string_reply, out);
    EXPECT_TRUE(out.ok);
    EXPECT_EQ(out.type, "string");
    EXPECT_EQ(out.str, "hello");

    redisReply integer_reply{};
    integer_reply.type = REDIS_REPLY_INTEGER;
    integer_reply.integer = 42;

    out = RedisReplyData{};
    parse_redis_reply(&integer_reply, out);
    EXPECT_TRUE(out.ok);
    EXPECT_EQ(out.type, "integer");
    EXPECT_EQ(out.integer, 42);

    redisReply nil_reply{};
    nil_reply.type = REDIS_REPLY_NIL;

    out = RedisReplyData{};
    parse_redis_reply(&nil_reply, out);
    EXPECT_TRUE(out.ok);
    EXPECT_EQ(out.type, "nil");

    redisReply error_reply{};
    error_reply.type = REDIS_REPLY_ERROR;
    error_reply.str = const_cast<char*>("ERR bad command");
    error_reply.len = 15;

    out = RedisReplyData{};
    parse_redis_reply(&error_reply, out);
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.type, "error");
    EXPECT_EQ(out.error, "ERR bad command");
}

TEST(RedisReply, ParsesArrayElements) {
    redisReply first{};
    first.type = REDIS_REPLY_STRING;
    first.str = const_cast<char*>("alpha");
    first.len = 5;

    redisReply second{};
    second.type = REDIS_REPLY_INTEGER;
    second.integer = 7;

    redisReply third{};
    third.type = REDIS_REPLY_NIL;

    redisReply* elements[] = {&first, &second, &third};

    redisReply array{};
    array.type = REDIS_REPLY_ARRAY;
    array.elements = 3;
    array.element = elements;

    RedisReplyData out;
    parse_redis_reply(&array, out);

    ASSERT_TRUE(out.ok);
    EXPECT_EQ(out.type, "array");
    ASSERT_EQ(out.elements.size(), 3u);
    EXPECT_EQ(out.elements[0], "alpha");
    EXPECT_EQ(out.elements[1], "7");
    EXPECT_EQ(out.elements[2], "(nil)");
}

TEST(RedisCommandArgv, PreservesArgumentBoundariesAndLengths) {
    std::vector<std::string> args = {"SET", "key with spaces", std::string("a\0b", 3)};

    RedisCommandArgv command(args);

    ASSERT_EQ(command.argc(), 3);
    EXPECT_EQ(std::string(command.argv[0], command.argv_len[0]), "SET");
    EXPECT_EQ(std::string(command.argv[1], command.argv_len[1]), "key with spaces");
    EXPECT_EQ(std::string(command.argv[2], command.argv_len[2]), std::string("a\0b", 3));
}

TEST(RedisTlsOwner, RequiresMatchingOwnerAndGeneration) {
    auto* owner_a = reinterpret_cast<const RedisPool*>(static_cast<uintptr_t>(0x1000));
    auto* owner_b = reinterpret_cast<const RedisPool*>(static_cast<uintptr_t>(0x2000));

    TlsRedisConn tls;
    reset_redis_tls_owner(tls, owner_a, 7);

    EXPECT_TRUE(redis_tls_owner_matches(tls, owner_a, 7));
    EXPECT_FALSE(redis_tls_owner_matches(tls, owner_a, 8));
    EXPECT_FALSE(redis_tls_owner_matches(tls, owner_b, 7));

    reset_redis_tls_owner(tls, owner_b, 1);
    EXPECT_TRUE(redis_tls_owner_matches(tls, owner_b, 1));
    EXPECT_FALSE(redis_tls_owner_matches(tls, owner_a, 7));
}

TEST(RedisPoolStats, FormatsSnapshotCounters) {
    RedisPoolStats stats;
    stats.inc_reconnect();
    stats.inc_cmd_ok();
    stats.inc_cmd_fail();
    stats.inc_nil();
    stats.inc_timeout();

    auto snapshot = stats.snapshot(3);

    EXPECT_EQ(snapshot.created_total, 3u);
    EXPECT_EQ(snapshot.reconnect_total, 1u);
    EXPECT_EQ(snapshot.cmd_ok_total, 1u);
    EXPECT_EQ(snapshot.cmd_fail_total, 1u);
    EXPECT_EQ(snapshot.nil_total, 1u);
    EXPECT_EQ(snapshot.timeout_total, 1u);
    EXPECT_EQ(format_redis_pool_stats(snapshot),
              "created_total=3, reconnect_total=1, cmd_ok_total=1, cmd_fail_total=1, nil_total=1, timeout_total=1"
              ", connect_ok_total=0, connect_fail_total=0, acquire_wait_total=0, acquire_timeout_total=0"
              ", acquire_retry_exhausted_total=0, idle_recycled_total=0, ping_fail_total=0"
              ", total_conn=0, idle_conn=0, creating_conn=0, max_creating=0");
}
