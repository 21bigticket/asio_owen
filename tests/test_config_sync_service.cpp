#include <gtest/gtest.h>

#include <asio.hpp>
#include <asio/co_spawn.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "app/app_config.hpp"
#include "app/config_sync_service.hpp"

namespace {

using Reply = RedisPool::Reply;

Reply string_reply(std::string value) {
    Reply reply;
    reply.ok = true;
    reply.type = "string";
    reply.str = std::move(value);
    return reply;
}

Reply nil_reply() {
    Reply reply;
    reply.ok = true;
    reply.type = "nil";
    return reply;
}

Reply integer_reply(int64_t value) {
    Reply reply;
    reply.ok = true;
    reply.type = "integer";
    reply.integer = value;
    return reply;
}

Reply array_reply(std::vector<std::string> values) {
    Reply reply;
    reply.ok = true;
    reply.type = "array";
    reply.elements = std::move(values);
    return reply;
}

Reply error_reply(std::string message) {
    Reply reply;
    reply.ok = false;
    reply.type = "error";
    reply.error = std::move(message);
    return reply;
}

struct FakeRedis {
    std::vector<std::vector<std::string>> calls;
    std::function<Reply(const std::vector<std::string>&)> handler;

    Reply handle(std::vector<std::string> args) {
        calls.push_back(std::move(args));
        if (!handler) return error_reply("no fake handler");
        return handler(calls.back());
    }

    size_t count(std::string_view command) const {
        size_t found = 0;
        for (const auto& call : calls) {
            if (!call.empty() && call.front() == command) ++found;
        }
        return found;
    }
};

std::filesystem::path make_temp_base(const std::string& tag) {
    static std::atomic<int> counter{0};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto base = std::filesystem::temp_directory_path() /
        ("asio_owen_config_sync_" + tag + "_" + std::to_string(getpid()) +
         "_" + std::to_string(now) + "_" + std::to_string(counter.fetch_add(1)));
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base / "config.d");
    return base;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_never_sync_files(const std::filesystem::path& base) {
    write_file(base / "config.d" / "11-redis.ini",
        "[redis]\n"
        "mode = direct\n");
    write_file(base / "config.d" / "12-config-sync.ini",
        "[config_sync]\n"
        "enabled = true\n"
        "machine_name = test-machine\n"
        "[auth_whitelist]\n"
        "path = /admin\n"
        "path = /admin/\n"
        "path = /api/admin/\n");
}

ConfigSyncConfig sync_config() {
    ConfigSyncConfig cfg;
    cfg.enabled = true;
    cfg.sync_interval_sec = 5;
    cfg.machine_name = "test-machine";
    cfg.first_pull = "async";
    cfg.first_pull_timeout_ms = 3000;
    cfg.history.read_mode = "compat";
    return cfg;
}

std::string snapshot_meta(
    const std::map<std::string, std::string>& files) {
    auto hash = config_history::content_sha256(files);
    if (!hash) throw std::runtime_error("failed to hash test snapshot");
    return "{\"content_sha256\":\"" + *hash + "\"}";
}

Reply snapshot_meta_reply(
    std::initializer_list<std::pair<const std::string, std::string>> files) {
    return string_reply(snapshot_meta(
        std::map<std::string, std::string>(files)));
}

AppConfig app_config() {
    AppConfig cfg;
    cfg.redis.cmd_timeout_ms = 50;
    return cfg;
}

bool run_sync_once(const std::filesystem::path& base, FakeRedis& redis,
                   ConfigSyncConfig cfg = sync_config()) {
    asio::io_context ioc;
    ConfigSyncService::Command command =
        [&ioc, &redis](std::vector<std::string> args,
                 ConfigSyncService::CommandCompletion completion) {
            asio::post(ioc,
                [&redis, args = std::move(args),
                 completion = std::move(completion)]() mutable {
                    completion(redis.handle(std::move(args)));
                });
        };
    auto service = std::make_shared<ConfigSyncService>(
        ioc, std::move(command), base, std::move(cfg), app_config());

    bool result = false;
    bool completed = false;
    service->sync_once_for_test([&](bool ok) {
        result = ok;
        completed = true;
    });
    ioc.run();
    if (!completed) throw std::runtime_error("ConfigSync completion was not called");
    return result;
}

}  // namespace

TEST(ConfigSyncService, ParsesConfigSection) {
    auto base = make_temp_base("config_parse");
    write_file(base / "config.d" / "12-config-sync.ini",
        "[config_sync]\n"
        "enabled = on\n"
        "sync_interval_sec = 7\n"
        "machine_name = node-a\n"
        "first_pull = blocking\n"
        "first_pull_timeout_ms = 1234\n"
        "[config_history]\n"
        "read_mode = COMPAT\n"
        "retention_versions = 12\n"
        "max_snapshot_bytes = 9999999\n"
        "max_file_bytes = 9999999\n"
        "history_page_size = 49\n"
        "history_page_size_max = 7\n"
        "gc_interval_sec = 1\n"
        "[admin]\n"
        "admin = pbkdf2_sha256$100000$YXNpb19vd2VuX2FkbWlu$FQidl7NTJAglmXo5_SZL7LC2T_ikCZhweAE8Wg6FIPI\n"
        "jwt_private_key = jwt_keys/admin-private-key.pem\n"
        "jwt_public_key = jwt_keys/admin-public-key.pem\n"
        "token_ttl_min = 30\n"
        "insecure_no_auth = yes\n");

    Config cfg;
    ASSERT_TRUE(cfg.load(base));
    auto app = app_config_from(cfg);
    EXPECT_TRUE(app.config_sync.enabled);
    EXPECT_EQ(app.config_sync.sync_interval_sec, 7);
    EXPECT_EQ(app.config_sync.machine_name, "node-a");
    EXPECT_EQ(app.config_sync.first_pull, "blocking");
    EXPECT_EQ(app.config_sync.first_pull_timeout_ms, 1234);
    EXPECT_EQ(app.config_sync.history.read_mode, "compat");
    EXPECT_EQ(app.config_sync.history.retention_versions, 12);
    EXPECT_EQ(app.config_sync.history.max_snapshot_bytes, 512u * 1024u);
    EXPECT_EQ(app.config_sync.history.max_file_bytes, 128u * 1024u);
    EXPECT_EQ(app.config_sync.history.history_page_size, 7u);
    EXPECT_EQ(app.config_sync.history.history_page_size_max, 7u);
    EXPECT_EQ(app.config_sync.history.gc_interval_sec, 10);
    ASSERT_EQ(app.config_sync.admin.accounts.size(), 1u);
    EXPECT_EQ(app.config_sync.admin.accounts[0].username, "admin");
    EXPECT_EQ(app.config_sync.admin.jwt_private_key, "jwt_keys/admin-private-key.pem");
    EXPECT_EQ(app.config_sync.admin.jwt_public_key, "jwt_keys/admin-public-key.pem");
    EXPECT_EQ(app.config_sync.admin.token_ttl_min, 30);
    EXPECT_TRUE(app.config_sync.admin.insecure_no_auth);

    std::filesystem::remove_all(base);
}

TEST(ConfigSyncService, RejectsNeverSyncRedisAndReservedAdminRules) {
    EXPECT_FALSE(ConfigSyncService::validate_managed_file(
        "11-redis.ini", "[server]\nport = 8080\n").ok);
    EXPECT_FALSE(ConfigSyncService::validate_managed_file(
        "20-upstream.ini", "[redis]\nhost = 127.0.0.1\n").ok);
    EXPECT_FALSE(ConfigSyncService::validate_managed_file(
        "20-admin.ini", "[ADMIN]\ninsecure_no_auth = true\n").ok);
    EXPECT_FALSE(ConfigSyncService::validate_managed_file(
        "20-sync.ini", "[config_sync]\nenabled = true\n").ok);
    EXPECT_FALSE(ConfigSyncService::validate_managed_file(
        "20-history.ini", "[config_history]\nread_mode = compat\n").ok);
    EXPECT_FALSE(ConfigSyncService::validate_managed_file(
        "31-auth.ini", "[auth_whitelist]\npath = /admin\n").ok);
    EXPECT_FALSE(ConfigSyncService::validate_managed_file(
        "32-path_blacklist.ini", "[path_blacklist]\n/api/admin/ =\n").ok);
    EXPECT_FALSE(ConfigSyncService::validate_managed_file(
        "33-auth.ini", "[auth_whitelist]\npath = /admin/metrics\n").ok);
    EXPECT_TRUE(ConfigSyncService::validate_managed_file(
        "20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n").ok);
}

TEST(ConfigSyncService, EmptyManagedSetCanSeedNewMachine) {
    auto base = make_temp_base("empty_seed");
    write_never_sync_files(base);

    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "GET") return nil_reply();
        if (args.front() == "EVAL") {
            EXPECT_EQ(args.size(), 14u);
            EXPECT_EQ(args[2], "7");
            EXPECT_NE(args[1].find("if file_count == 0"), std::string::npos);
            return integer_reply(1);
        }
        if (args.front() == "HSET") return integer_reply(1);
        return error_reply("unexpected command " + args.front());
    };

    EXPECT_TRUE(run_sync_once(base, redis));
    EXPECT_EQ(redis.count("EVAL"), 1u);
    EXPECT_EQ(redis.count("HSET"), 1u);

    auto state = ConfigSyncService::load_state(base);
    EXPECT_TRUE(state.exists);
    EXPECT_EQ(state.synced_version, 1);
    EXPECT_EQ(state.status, "ok");
    EXPECT_TRUE(state.managed_files.empty());
    EXPECT_TRUE(state.last_ok.empty());

    std::filesystem::remove_all(base);
}

TEST(ConfigSyncService, RequiredModeRejectsEmptyInitialSnapshot) {
    auto base = make_temp_base("required_empty_seed");
    write_never_sync_files(base);

    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "GET") return nil_reply();
        return error_reply("unexpected command " + args.front());
    };
    auto cfg = sync_config();
    cfg.history.read_mode = "required";

    EXPECT_FALSE(run_sync_once(base, redis, std::move(cfg)));
    EXPECT_EQ(redis.count("EVAL"), 0u);
    EXPECT_FALSE(ConfigSyncService::load_state(base).exists);

    std::filesystem::remove_all(base);
}

TEST(ConfigSyncService, RequiredModeSeedsLocalDefaultsAsVersionOneHistory) {
    auto base = make_temp_base("required_local_seed");
    write_never_sync_files(base);
    const std::string content =
        "[upstream]\nzebra-config = 127.0.0.1:30001\n";
    write_file(base / "config.d" / "20-upstream.ini", content);

    FakeRedis redis;
    redis.handler = [&content](const std::vector<std::string>& args) {
        if (args.front() == "GET") return nil_reply();
        if (args.front() == "EVAL") {
            EXPECT_EQ(args[2], "7");
            EXPECT_EQ(args[3], "asio_owen:config:version");
            EXPECT_EQ(args[4], "asio_owen:config:files");
            EXPECT_EQ(args[8], "asio_owen:config:history:1");
            EXPECT_NE(args[10].find("\"action\":\"seed\""),
                std::string::npos);
            EXPECT_EQ(args[14], "20-upstream.ini");
            EXPECT_EQ(args[15], content);
            return integer_reply(1);
        }
        if (args.front() == "HSET") return integer_reply(1);
        return error_reply("unexpected command " + args.front());
    };
    auto cfg = sync_config();
    cfg.history.read_mode = "required";

    EXPECT_TRUE(run_sync_once(base, redis, std::move(cfg)));
    auto state = ConfigSyncService::load_state(base);
    EXPECT_TRUE(state.exists);
    EXPECT_EQ(state.synced_version, 1);
    EXPECT_EQ(state.status, "ok");
    ASSERT_EQ(state.managed_files.size(), 1u);
    EXPECT_EQ(state.managed_files.count("20-upstream.ini"), 1u);
    EXPECT_EQ(redis.count("EVAL"), 1u);
    EXPECT_EQ(redis.count("HSET"), 1u);

    std::filesystem::remove_all(base);
}

TEST(ConfigSyncService, LostSeedRaceDoesNotWriteLocalState) {
    auto base = make_temp_base("seed_race");
    write_never_sync_files(base);

    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "GET") return nil_reply();
        if (args.front() == "EVAL") return integer_reply(0);
        return error_reply("unexpected command " + args.front());
    };

    EXPECT_FALSE(run_sync_once(base, redis));
    EXPECT_EQ(redis.count("EVAL"), 1u);
    EXPECT_FALSE(ConfigSyncService::load_state(base).exists);

    std::filesystem::remove_all(base);
}

TEST(ConfigSyncService, PartialStateRefusesReseed) {
    auto base = make_temp_base("partial_reseed");
    write_never_sync_files(base);
    write_file(base / "config.d" / "20-upstream.ini",
        "[upstream]\nzebra = 127.0.0.1:30001\n");

    ConfigSyncService::State state;
    state.exists = true;
    state.synced_version = 2;
    state.status = "partial";
    state.managed_files["20-upstream.ini"] = "stale";
    state.last_ok["20-upstream.ini"] = "stale";
    ASSERT_TRUE(ConfigSyncService::write_state(base, state));

    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "GET") return nil_reply();
        return error_reply("unexpected command " + args.front());
    };

    EXPECT_FALSE(run_sync_once(base, redis));
    EXPECT_EQ(redis.count("EVAL"), 0u);

    auto after = ConfigSyncService::load_state(base);
    EXPECT_EQ(after.synced_version, 2);
    EXPECT_EQ(after.status, "partial");

    std::filesystem::remove_all(base);
}

TEST(ConfigSyncService, NonNumericVersionDoesNotSeedOrSync) {
    auto base = make_temp_base("bad_version");
    write_never_sync_files(base);

    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "GET") return string_reply("abc");
        return error_reply("unexpected command " + args.front());
    };

    EXPECT_FALSE(run_sync_once(base, redis));
    EXPECT_EQ(redis.count("GET"), 1u);
    EXPECT_EQ(redis.count("EVAL"), 0u);
    EXPECT_EQ(redis.count("HGETALL"), 0u);
    EXPECT_FALSE(ConfigSyncService::load_state(base).exists);

    std::filesystem::remove_all(base);
}

TEST(ConfigSyncService, VersionDoubleReadMismatchDoesNotWriteFiles) {
    auto base = make_temp_base("double_read");
    write_never_sync_files(base);

    int get_count = 0;
    FakeRedis redis;
    redis.handler = [&get_count](const std::vector<std::string>& args) {
        if (args.front() == "GET") {
            ++get_count;
            return string_reply(get_count == 1 ? "2" : "3");
        }
        if (args.front() == "HGETALL") {
            return array_reply({
                "20-upstream.ini",
                "[upstream]\nzebra = 127.0.0.1:30001\n"
            });
        }
        if (args.front() == "HGET") {
            return snapshot_meta_reply({
                {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"}
            });
        }
        return error_reply("unexpected command " + args.front());
    };

    EXPECT_FALSE(run_sync_once(base, redis));
    EXPECT_EQ(redis.count("HGETALL"), 1u);
    EXPECT_EQ(redis.count("HSET"), 0u);
    EXPECT_FALSE(std::filesystem::exists(base / "config.d" / "20-upstream.ini"));
    EXPECT_FALSE(ConfigSyncService::load_state(base).exists);

    std::filesystem::remove_all(base);
}

TEST(ConfigSyncService, BadManagedFileWritesPartialWithoutVersionAdvance) {
    auto base = make_temp_base("partial");
    write_never_sync_files(base);

    ConfigSyncService::State previous;
    previous.exists = true;
    previous.synced_version = 1;
    previous.status = "ok";
    ASSERT_TRUE(ConfigSyncService::write_state(base, previous));

    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "GET") return string_reply("2");
        if (args.front() == "HGETALL") {
            return array_reply({
                "20-upstream.ini",
                "[upstream]\nzebra = 127.0.0.1:30001\n",
                "32-path_blacklist.ini",
                "[path_blacklist]\n/api/admin/ =\n"
            });
        }
        if (args.front() == "HGET") {
            return snapshot_meta_reply({
                {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"},
                {"32-path_blacklist.ini", "[path_blacklist]\n/api/admin/ =\n"}
            });
        }
        if (args.front() == "HSET") {
            EXPECT_NE(args[3].find("|partial|"), std::string::npos);
            EXPECT_NE(args[3].find("32-path_blacklist.ini"), std::string::npos);
            return integer_reply(1);
        }
        return error_reply("unexpected command " + args.front());
    };

    EXPECT_FALSE(run_sync_once(base, redis));
    EXPECT_EQ(read_file(base / "config.d" / "20-upstream.ini"),
        "[upstream]\nzebra = 127.0.0.1:30001\n");
    EXPECT_FALSE(std::filesystem::exists(base / "config.d" / "32-path_blacklist.ini"));

    auto state = ConfigSyncService::load_state(base);
    EXPECT_EQ(state.synced_version, 1);
    EXPECT_EQ(state.status, "partial");
    ASSERT_TRUE(state.failures.count("32-path_blacklist.ini"));
    EXPECT_TRUE(state.managed_files.empty());
    EXPECT_TRUE(state.last_ok.empty());

    std::filesystem::remove_all(base);
}

TEST(ConfigSyncService, AdminAndConfigSyncSectionsWritePartialWithoutFiles) {
    auto base = make_temp_base("partial_admin_sections");
    write_never_sync_files(base);

    ConfigSyncService::State previous;
    previous.exists = true;
    previous.synced_version = 1;
    previous.status = "ok";
    ASSERT_TRUE(ConfigSyncService::write_state(base, previous));

    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "GET") return string_reply("2");
        if (args.front() == "HGETALL") {
            return array_reply({
                "20-admin.ini",
                "[admin]\ninsecure_no_auth = true\n",
                "21-config-sync.ini",
                "[config_sync]\nenabled = true\n"
            });
        }
        if (args.front() == "HGET") {
            return snapshot_meta_reply({
                {"20-admin.ini", "[admin]\ninsecure_no_auth = true\n"},
                {"21-config-sync.ini", "[config_sync]\nenabled = true\n"}
            });
        }
        if (args.front() == "HSET") {
            EXPECT_NE(args[3].find("|partial|"), std::string::npos);
            EXPECT_NE(args[3].find("20-admin.ini"), std::string::npos);
            EXPECT_NE(args[3].find("21-config-sync.ini"), std::string::npos);
            return integer_reply(1);
        }
        return error_reply("unexpected command " + args.front());
    };

    EXPECT_FALSE(run_sync_once(base, redis));
    EXPECT_FALSE(std::filesystem::exists(base / "config.d" / "20-admin.ini"));
    EXPECT_FALSE(std::filesystem::exists(base / "config.d" / "21-config-sync.ini"));

    auto state = ConfigSyncService::load_state(base);
    EXPECT_EQ(state.synced_version, 1);
    EXPECT_EQ(state.status, "partial");
    EXPECT_TRUE(state.failures.count("20-admin.ini"));
    EXPECT_TRUE(state.failures.count("21-config-sync.ini"));

    std::filesystem::remove_all(base);
}

TEST(ConfigSyncService, MissingHistoryAndMirrorDoNotDeleteExistingManagedFiles) {
    auto base = make_temp_base("empty_remote_guard");
    write_never_sync_files(base);
    const std::string upstream = "[upstream]\nzebra = 127.0.0.1:30001\n";
    const std::string pool = "[http_pool]\nmax_size = 9\n";
    write_file(base / "config.d" / "20-upstream.ini", upstream);
    write_file(base / "config.d" / "21-http_pool.ini", pool);

    ConfigSyncService::State previous;
    previous.exists = true;
    previous.synced_version = 1;
    previous.status = "ok";
    previous.managed_files["20-upstream.ini"] =
        ConfigSyncService::content_hash(upstream);
    previous.managed_files["21-http_pool.ini"] =
        ConfigSyncService::content_hash(pool);
    previous.last_ok = previous.managed_files;
    ASSERT_TRUE(ConfigSyncService::write_state(base, previous));

    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "GET") return string_reply("2");
        if (args.front() == "HGETALL") return array_reply({});
        if (args.front() == "HSET") {
            EXPECT_NE(args[3].find("|partial|"), std::string::npos);
            EXPECT_NE(args[3].find("history_snapshot"), std::string::npos);
            return integer_reply(1);
        }
        return error_reply("unexpected command " + args.front());
    };

    EXPECT_FALSE(run_sync_once(base, redis));
    EXPECT_EQ(read_file(base / "config.d" / "20-upstream.ini"), upstream);
    EXPECT_EQ(read_file(base / "config.d" / "21-http_pool.ini"), pool);

    auto state = ConfigSyncService::load_state(base);
    EXPECT_EQ(state.synced_version, 1);
    EXPECT_EQ(state.status, "partial");
    EXPECT_TRUE(state.failures.count("history_snapshot"));
    EXPECT_EQ(state.last_ok, previous.last_ok);

    std::filesystem::remove_all(base);
}

TEST(ConfigSyncService, CleanSyncAdvancesVersionAndDeletesStaleFiles) {
    auto base = make_temp_base("clean");
    write_never_sync_files(base);
    write_file(base / "config.d" / "20-old.ini", "[server]\nport = 8081\n");
    write_file(base / "config.d" / "21-http_pool.ini", "[http_pool]\nmax_size = 9\n");

    ConfigSyncService::State previous;
    previous.exists = true;
    previous.synced_version = 1;
    previous.status = "ok";
    previous.managed_files["20-old.ini"] =
        ConfigSyncService::content_hash("[server]\nport = 8081\n");
    previous.last_ok = previous.managed_files;
    ASSERT_TRUE(ConfigSyncService::write_state(base, previous));

    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "GET") return string_reply("2");
        if (args.front() == "HGETALL") {
            return array_reply({
                "20-upstream.ini",
                "[upstream]\nzebra = 127.0.0.1:30002\n"
            });
        }
        if (args.front() == "HGET") {
            return snapshot_meta_reply({
                {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30002\n"}
            });
        }
        if (args.front() == "HSET") {
            EXPECT_NE(args[3].find("|ok"), std::string::npos);
            return integer_reply(1);
        }
        return error_reply("unexpected command " + args.front());
    };

    EXPECT_TRUE(run_sync_once(base, redis));
    EXPECT_FALSE(std::filesystem::exists(base / "config.d" / "20-old.ini"));
    EXPECT_FALSE(std::filesystem::exists(base / "config.d" / "21-http_pool.ini"));
    EXPECT_TRUE(std::filesystem::exists(base / "config.d" / "12-config-sync.ini"));
    EXPECT_EQ(read_file(base / "config.d" / "20-upstream.ini"),
        "[upstream]\nzebra = 127.0.0.1:30002\n");

    auto state = ConfigSyncService::load_state(base);
    EXPECT_EQ(state.synced_version, 2);
    EXPECT_EQ(state.status, "ok");
    EXPECT_FALSE(state.managed_files.count("20-old.ini"));
    EXPECT_TRUE(state.managed_files.count("20-upstream.ini"));
    EXPECT_EQ(state.last_ok, state.managed_files);

    std::filesystem::remove_all(base);
}

TEST(ConfigSyncService, StaleFileDeleteFailureDoesNotAdvanceVersion) {
    auto base = make_temp_base("delete_failure");
    write_never_sync_files(base);
    auto stale_path = base / "config.d" / "20-old.ini";
    std::filesystem::create_directories(stale_path);
    write_file(stale_path / "keep", "not empty");

    ConfigSyncService::State previous;
    previous.exists = true;
    previous.synced_version = 1;
    previous.status = "ok";
    previous.managed_files["20-old.ini"] = "old-hash";
    previous.last_ok = previous.managed_files;
    ASSERT_TRUE(ConfigSyncService::write_state(base, previous));

    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "GET") return string_reply("2");
        if (args.front() == "HGETALL") {
            return array_reply({
                "20-upstream.ini",
                "[upstream]\nzebra = 127.0.0.1:30002\n"
            });
        }
        if (args.front() == "HGET") {
            return snapshot_meta_reply({
                {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30002\n"}
            });
        }
        if (args.front() == "HSET") {
            EXPECT_NE(args[3].find("|partial|"), std::string::npos);
            EXPECT_NE(args[3].find("20-old.ini"), std::string::npos);
            return integer_reply(1);
        }
        return error_reply("unexpected command " + args.front());
    };

    EXPECT_FALSE(run_sync_once(base, redis));
    EXPECT_TRUE(std::filesystem::exists(stale_path));

    auto state = ConfigSyncService::load_state(base);
    EXPECT_EQ(state.synced_version, 1);
    EXPECT_EQ(state.status, "partial");
    EXPECT_TRUE(state.failures.count("20-old.ini"));

    std::filesystem::remove_all(base);
}

TEST(ConfigSyncService, EqualVersionLocalDriftReappliesSnapshot) {
    auto base = make_temp_base("equal_version_drift");
    write_never_sync_files(base);
    const std::string expected =
        "[upstream]\nzebra = 127.0.0.1:30002\n";
    write_file(base / "config.d" / "20-upstream.ini",
        "[upstream]\nzebra = 127.0.0.1:39999\n");

    ConfigSyncService::State previous;
    previous.exists = true;
    previous.synced_version = 2;
    previous.status = "ok";
    previous.managed_files["20-upstream.ini"] =
        ConfigSyncService::content_hash(expected);
    previous.last_ok = previous.managed_files;
    ASSERT_TRUE(ConfigSyncService::write_state(base, previous));

    FakeRedis redis;
    redis.handler = [&expected](const std::vector<std::string>& args) {
        if (args.front() == "GET") return string_reply("2");
        if (args.front() == "HGETALL") {
            return array_reply({"20-upstream.ini", expected});
        }
        if (args.front() == "HGET") {
            return string_reply(snapshot_meta({{"20-upstream.ini", expected}}));
        }
        if (args.front() == "HSET") {
            EXPECT_NE(args[3].find("|ok"), std::string::npos);
            return integer_reply(1);
        }
        return error_reply("unexpected command " + args.front());
    };

    EXPECT_TRUE(run_sync_once(base, redis));
    EXPECT_EQ(read_file(base / "config.d" / "20-upstream.ini"), expected);
    EXPECT_EQ(redis.count("HGETALL"), 1u);

    auto state = ConfigSyncService::load_state(base);
    EXPECT_EQ(state.synced_version, 2);
    EXPECT_EQ(state.status, "ok");

    std::filesystem::remove_all(base);
}
