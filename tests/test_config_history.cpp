#include <gtest/gtest.h>

#include <asio.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/admin/config_history.hpp"
#include "app/config_history_service.hpp"

namespace {

using Reply = RedisPool::Reply;

Reply array_reply(std::vector<std::string> values) {
    Reply reply;
    reply.ok = true;
    reply.type = "array";
    reply.elements = std::move(values);
    return reply;
}

Reply integer_reply(int64_t value) {
    Reply reply;
    reply.ok = true;
    reply.type = "integer";
    reply.integer = value;
    return reply;
}

struct FakeRedis {
    std::vector<std::vector<std::string>> calls;
    std::function<Reply(const std::vector<std::string>&)> handler;

    void command(std::vector<std::string> args,
                 ConfigHistoryService::CommandCompletion completion) {
        calls.push_back(std::move(args));
        completion(handler(calls.back()));
    }
};

ConfigHistoryService::Stats run_once(
    FakeRedis& redis, ConfigHistoryConfig cfg = ConfigHistoryConfig{}) {
    asio::io_context ioc;
    auto service = std::make_shared<ConfigHistoryService>(
        ioc,
        [&redis](std::vector<std::string> args,
                 ConfigHistoryService::CommandCompletion completion) {
            redis.command(std::move(args), std::move(completion));
        },
        std::move(cfg), 50);
    bool completed = false;
    service->run_once_for_test([&completed]() { completed = true; });
    EXPECT_TRUE(completed);
    return service->stats();
}

}  // namespace

TEST(ConfigHistory, SnapshotHashIsDeterministicAndContentSensitive) {
    const std::map<std::string, std::string> first{
        {"20-upstream.ini", "[upstream]\na = 1\n"},
        {"30-security.ini", "[security]\njwt_disabled = true\n"}
    };
    std::map<std::string, std::string> second;
    second.emplace("30-security.ini", "[security]\njwt_disabled = true\n");
    second.emplace("20-upstream.ini", "[upstream]\na = 1\n");

    auto first_hash = config_history::content_sha256(first);
    auto second_hash = config_history::content_sha256(second);
    ASSERT_TRUE(first_hash);
    ASSERT_TRUE(second_hash);
    EXPECT_EQ(*first_hash, *second_hash);
    EXPECT_EQ(first_hash->size(), 64u);

    second["20-upstream.ini"] = "[upstream]\na = 2\n";
    EXPECT_NE(*first_hash, *config_history::content_sha256(second));
}

TEST(ConfigHistory, SnapshotCapacityRejectsFileCountFileSizeAndTotalSize) {
    ConfigHistoryConfig cfg;
    cfg.max_files = 2;
    cfg.max_file_bytes = 8;
    cfg.max_snapshot_bytes = 12;
    config_history::SnapshotInfo info;

    EXPECT_TRUE(config_history::validate_snapshot({}, cfg, info).has_value());
    EXPECT_TRUE(config_history::validate_snapshot({
        {"20-a.ini", "1"}, {"21-b.ini", "2"}, {"22-c.ini", "3"}
    }, cfg, info).has_value());
    EXPECT_TRUE(config_history::validate_snapshot({
        {"20-a.ini", "123456789"}
    }, cfg, info).has_value());
    EXPECT_TRUE(config_history::validate_snapshot({
        {"20-a.ini", "1234567"}, {"21-b.ini", "1234567"}
    }, cfg, info).has_value());

    EXPECT_FALSE(config_history::validate_snapshot({
        {"20-a.ini", "123456"}, {"21-b.ini", "123456"}
    }, cfg, info).has_value());
    EXPECT_EQ(info.file_count, 2u);
    EXPECT_EQ(info.total_bytes, 12u);
    EXPECT_EQ(info.content_sha256.size(), 64u);
}

TEST(ConfigHistory, DiffDirectionAndSensitiveFlagAreExplicit) {
    config_history::SnapshotRecord from;
    from.files = {
        {"20-upstream.ini", "[upstream]\na = 1\n"},
        {"30-old.ini", "[server]\nport = 8080\n"}
    };
    config_history::SnapshotRecord to;
    to.files = {
        {"20-upstream.ini", "[upstream]\na = 2\n"},
        {"40-mysql.ini", "[mysql]\npass = rotated\n"}
    };

    auto diff = config_history::diff_json(7, from, 8, to, 64 * 1024);
    ASSERT_TRUE(diff);
    EXPECT_NE(diff->find("\"from\":7,\"to\":8"), std::string::npos);
    EXPECT_NE(diff->find("\"type\":\"deleted\""), std::string::npos);
    EXPECT_NE(diff->find("\"type\":\"added\""), std::string::npos);
    EXPECT_NE(diff->find("\"type\":\"modified\""), std::string::npos);
    EXPECT_NE(diff->find("\"sensitive\":true"), std::string::npos);
    EXPECT_FALSE(config_history::diff_json(7, from, 8, to, 16).has_value());
}

TEST(ConfigHistory, PublishScriptsKeepVersionAsLastCoreWrite) {
    const auto save = config_history::save_script();
    const auto rename = save.rfind("redis.call('RENAME', KEYS[4], KEYS[2])");
    const auto publish = save.rfind("redis.call('INCR', KEYS[1])");
    const auto audit = save.rfind("redis.call('LPUSH', KEYS[3], ARGV[2])");
    ASSERT_NE(rename, std::string::npos);
    ASSERT_NE(publish, std::string::npos);
    ASSERT_NE(audit, std::string::npos);
    EXPECT_LT(rename, publish);
    EXPECT_LT(publish, audit);
    EXPECT_NE(save.find("high > cur"), std::string::npos);
    EXPECT_NE(config_history::migration_script().find(
        "redis.call('ZCARD', KEYS[4]) ~= 0"), std::string::npos);
    EXPECT_NE(config_history::migration_script().find(
        "redis.call('HLEN', KEYS[3]) ~= 0"), std::string::npos);
    EXPECT_NE(config_history::rollback_script().find("return -9"),
        std::string::npos);
}

TEST(ConfigHistoryService, HealthyStateRunsBoundedCandidateQuery) {
    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.size() > 1 && args[1].find("machine_high") != std::string::npos) {
            EXPECT_EQ(args[2], "5");
            EXPECT_EQ(args.back(), config_history::kSnapshotPrefix);
            return array_reply({"12", "12", "12", "ok"});
        }
        EXPECT_NE(args[1].find("batch * 4"), std::string::npos);
        return array_reply({});
    };

    auto stats = run_once(redis);
    EXPECT_FALSE(stats.inconsistent);
    EXPECT_EQ(stats.checks, 1u);
    EXPECT_EQ(stats.max_observed_version, 12);
    ASSERT_EQ(redis.calls.size(), 2u);
    EXPECT_EQ(redis.calls[1][8], "20");
}

TEST(ConfigHistoryService, VersionRollbackFreezesBeforeGc) {
    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>&) {
        return array_reply({"11", "12", "12", "ok"});
    };

    auto stats = run_once(redis);
    EXPECT_TRUE(stats.inconsistent);
    EXPECT_EQ(stats.inconsistent_checks, 1u);
    EXPECT_EQ(redis.calls.size(), 1u);
}

TEST(ConfigHistoryService, GcDeletesEachTripleWithShortAtomicLua) {
    FakeRedis redis;
    int phase = 0;
    redis.handler = [&phase](const std::vector<std::string>& args) {
        ++phase;
        if (phase == 1) return array_reply({"150", "150", "150", "ok"});
        if (phase == 2) return array_reply({"1", "2"});
        EXPECT_EQ(args[2], "4");
        EXPECT_NE(args[1].find("redis.call('UNLINK', KEYS[4])"),
            std::string::npos);
        EXPECT_NE(args[1].find("redis.call('HDEL', KEYS[3], member)"),
            std::string::npos);
        EXPECT_NE(args[1].find("redis.call('ZREM', KEYS[2], member)"),
            std::string::npos);
        return integer_reply(1);
    };

    auto stats = run_once(redis);
    EXPECT_FALSE(stats.inconsistent);
    EXPECT_EQ(stats.gc_deleted, 2u);
    EXPECT_EQ(redis.calls.size(), 4u);
}

TEST(ConfigHistoryService, MissingCurrentTripleFreezesBeforeGc) {
    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>&) {
        return array_reply({"12", "12", "12", "incomplete"});
    };

    auto stats = run_once(redis);
    EXPECT_TRUE(stats.inconsistent);
    EXPECT_EQ(stats.inconsistent_checks, 1u);
    EXPECT_EQ(redis.calls.size(), 1u);
}

TEST(ConfigHistoryService, AutoMigratesPureLegacyState) {
    FakeRedis redis;
    int calls = 0;
    redis.handler = [&calls](const std::vector<std::string>& args) {
        ++calls;
        if (calls == 1) {
            EXPECT_NE(args[1].find("integrity = 'legacy'"), std::string::npos);
            return array_reply({"12", "0", "12", "legacy"});
        }
        if (calls == 2) {
            EXPECT_EQ(args.front(), "HGETALL");
            return array_reply({
                "20-upstream.ini", "[upstream]\na = 1\n",
                "30-security.ini", "[security]\njwt_disabled = true\n"
            });
        }
        if (calls == 3) {
            EXPECT_EQ(args.front(), "EVAL");
            EXPECT_EQ(args[2], "7");
            EXPECT_NE(args[1].find("redis.call('RENAME', KEYS[6], KEYS[5])"),
                std::string::npos);
            EXPECT_NE(args[11].find("\"action\":\"migration\""),
                std::string::npos);
            return integer_reply(12);
        }
        if (calls == 4) return array_reply({"12", "12", "12", "ok"});
        return array_reply({});
    };

    auto stats = run_once(redis);
    EXPECT_FALSE(stats.inconsistent);
    EXPECT_EQ(stats.checks, 2u);
    EXPECT_EQ(stats.max_observed_version, 12);
    EXPECT_EQ(redis.calls.size(), 5u);
}

TEST(ConfigHistoryService, CompatCanLeavePureLegacyStateUnmodified) {
    FakeRedis redis;
    int calls = 0;
    redis.handler = [&calls](const std::vector<std::string>& args) {
        ++calls;
        if (calls == 1) return array_reply({"1", "0", "1", "legacy"});
        EXPECT_NE(args[1].find("batch * 4"), std::string::npos);
        return array_reply({});
    };
    ConfigHistoryConfig cfg;
    cfg.read_mode = "compat";
    cfg.auto_migrate_legacy = false;

    auto stats = run_once(redis, cfg);
    EXPECT_FALSE(stats.inconsistent);
    EXPECT_EQ(redis.calls.size(), 2u);
}
