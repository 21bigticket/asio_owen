#include <gtest/gtest.h>

#include <asio/co_spawn.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <jwt-cpp/jwt.h>
#include <unistd.h>

#include "app/admin/config_admin.hpp"
#include "app/routes.hpp"

namespace {

using Reply = RedisPool::Reply;

constexpr const char* kPrivateKey = R"(-----BEGIN PRIVATE KEY-----
MIICdgIBADANBgkqhkiG9w0BAQEFAASCAmAwggJcAgEAAoGBAMPOpziUO0j9bkom
nBvWqiZScbYSW4KD8Aee66aFCkjALMqbBMA1x6zIJw20fKsca0Ai5IlxYE2u1f8l
xfBCs3cQQkNWZW3W2K0MdsZEPZl/J/9/9RF5hVyD3d6iIwYwUj9Ke5pVIDbHSJwS
m8tFtxFNNbFBxWptapluYXZyZy7lAgMBAAECgYAlBuoWR+miFtKJUR3KIeDRGFwK
axRE7QAx4Lp9JcFZGoYd1gyi8EiPAtZnwA6nKNubKD4BQ6BLcFllQ1ZX5bUb5Jvw
b89uxVFiwmoLzxvEGuZxgp+bNFvoR6zNaW++EbXCmEPLqKoVXahRbbyLp8C40wYL
7kUdmxyKt/zEp/MdgQJBAPlHRKP0unebf6GFadjLx3MGC5v6W8Dqy2PrTg5xdk3G
BCZRgKYoCEKcW1lu3szwmmed7MEvvCfL/Seu28SPaGECQQDJFkgYjLMGqMb2yyQr
AQztHYtYUO9at1lsBl9zN8eWSUvvS6tTQix/zevyVVyXQcYUZ2PniY7JSHmaY5ln
eEUFAkEA5ajGUXOYE7f8d3gt02GzCILqUTLwM6Vd61mPmXjpMLAdhJwUNYCuU4gw
FQ4zUIbfClWSGU38QIIMYvITYV1qIQJAHxQrhmfQj5nsTl5tM5xQ9CDZ1YeExt+J
mZGOlQ8s8MRZUR2/1/llSUd5TRX2XoZS5/pmzXVMNT3XVY5JOl2zRQJAXklBNtc+
ngEj7lFhKWW1+XQSxTnKjyHICySAQuefeFp1RFuUVP1bIr4x6QlP7JAW38zlf4Y6
rZGOhGnCbTecMA==
-----END PRIVATE KEY-----)";

constexpr const char* kPublicKey = R"(-----BEGIN PUBLIC KEY-----
MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDDzqc4lDtI/W5KJpwb1qomUnG2
EluCg/AHnuumhQpIwCzKmwTANcesyCcNtHyrHGtAIuSJcWBNrtX/JcXwQrN3EEJD
VmVt1titDHbGRD2Zfyf/f/UReYVcg93eoiMGMFI/SnuaVSA2x0icEpvLRbcRTTWx
QcVqbWqZbmF2cmcu5QIDAQAB
-----END PUBLIC KEY-----)";

Reply string_reply(std::string value) {
    Reply reply;
    reply.ok = true;
    reply.type = "string";
    reply.str = std::move(value);
    return reply;
}

Reply integer_reply(int64_t value) {
    Reply reply;
    reply.ok = true;
    reply.type = "integer";
    reply.integer = value;
    return reply;
}

Reply nil_reply() {
    Reply reply;
    reply.ok = true;
    reply.type = "nil";
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
        ("asio_owen_admin_routes_" + tag + "_" + std::to_string(getpid()) +
         "_" + std::to_string(now) + "_" + std::to_string(counter.fetch_add(1)));
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base / "config.d");
    return base;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

void write_never_sync(const std::filesystem::path& base) {
    std::filesystem::create_directories(base / "jwt_keys");
    auto private_key = base / "jwt_keys" / "admin-private-key.pem";
    auto public_key = base / "jwt_keys" / "admin-public-key.pem";
    write_file(private_key, kPrivateKey);
    write_file(public_key, kPublicKey);
    write_file(base / "config.d" / "11-redis.ini",
        "[redis]\n"
        "mode = direct\n");
    write_file(base / "config.d" / "12-config-sync.ini",
        "[config_sync]\n"
        "enabled = true\n"
        "[admin]\n"
        "admin = " + config_admin::pbkdf2_sha256_hash_string(
            "admin", std::vector<unsigned char>{'s', 'a', 'l', 't'}, 1000) + "\n"
        "jwt_private_key = jwt_keys/admin-private-key.pem\n"
        "jwt_public_key = jwt_keys/admin-public-key.pem\n"
        "token_ttl_min = 120\n"
        "[auth_whitelist]\n"
        "path = /admin\n"
        "path = /admin/\n"
        "path = /api/admin/\n");
}

ConfigSyncConfig admin_sync_config() {
    ConfigSyncConfig cfg;
    cfg.admin.accounts.push_back({
        "admin",
        config_admin::pbkdf2_sha256_hash_string(
            "admin", std::vector<unsigned char>{'s', 'a', 'l', 't'}, 1000)
    });
    cfg.admin.jwt_private_key = kPrivateKey;
    cfg.admin.jwt_public_key = kPublicKey;
    cfg.admin.token_ttl_min = 120;
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

std::string snapshot_meta_json(
    int64_t version,
    const std::map<std::string, std::string>& files,
    std::string_view action = "save") {
    ConfigHistoryConfig cfg;
    config_history::SnapshotInfo info;
    auto error = config_history::validate_snapshot(files, cfg, info);
    if (error) throw std::runtime_error(*error);
    return config_history::metadata_json(
        version, version > 0 ? version - 1 : 0, 1786880000,
        "admin", action, "test reason", info);
}

Reply detail_reply(
    int64_t current_version,
    int64_t version,
    const std::map<std::string, std::string>& files) {
    std::vector<std::string> elements{
        std::to_string(current_version), snapshot_meta_json(version, files)
    };
    for (const auto& [name, content] : files) {
        elements.push_back(name);
        elements.push_back(content);
    }
    return array_reply(std::move(elements));
}

std::string admin_token(const ConfigSyncConfig& cfg) {
    auto issued = config_admin::issue_admin_token(cfg.admin, "admin");
    if (!issued) throw std::runtime_error("failed to issue test admin token");
    return issued->token;
}

void set_admin_bearer(HttpContext& ctx, const ConfigSyncConfig& cfg) {
    ctx.headers.emplace_back("Authorization", "Bearer " + admin_token(cfg));
}

std::string business_admin_token() {
    return jwt::create()
        .set_issuer("pixiu-gateway")
        .set_subject("42")
        .set_payload_claim("name", jwt::claim(std::string("business-admin")))
        .set_payload_claim("role", jwt::claim(std::string("admin")))
        .set_expires_at(std::chrono::system_clock::now() + std::chrono::hours(1))
        .sign(jwt::algorithm::rs256{kPublicKey, kPrivateKey});
}

std::string expired_admin_token() {
    return jwt::create()
        .set_issuer(std::string(config_admin::kAdminIssuer))
        .set_subject("admin")
        .set_payload_claim("role", jwt::claim(std::string("admin")))
        .set_expires_at(std::chrono::system_clock::now() - std::chrono::seconds(120))
        .sign(jwt::algorithm::rs256{kPublicKey, kPrivateKey});
}

AppServices services_for(const std::filesystem::path& base, FakeRedis& redis,
                         ConfigSyncConfig cfg) {
    return AppServices{
        .config_base = base,
        .config_sync = cfg,
        .redis_command = [&redis](
                              std::vector<std::string> args) -> asio::awaitable<Reply> {
            co_return redis.handle(std::move(args));
        }
    };
}

AppServices services_for(const std::filesystem::path& base, FakeRedis& redis) {
    return services_for(base, redis, admin_sync_config());
}

void run_awaitable(asio::io_context& ioc, asio::awaitable<void> awaitable) {
    std::exception_ptr error;
    asio::co_spawn(ioc, std::move(awaitable),
        [&](std::exception_ptr ep) {
            error = ep;
        });
    ioc.run();
    if (error) std::rethrow_exception(error);
}

void run_admin_config(HttpContext& ctx, AppServices services) {
    asio::io_context ioc;
    run_awaitable(ioc, handle_api_admin_config(ctx, std::move(services)));
}

void run_admin_machines(HttpContext& ctx, AppServices services) {
    asio::io_context ioc;
    run_awaitable(ioc, handle_api_admin_machines(ctx, std::move(services)));
}

void run_admin_history(HttpContext& ctx, AppServices services) {
    asio::io_context ioc;
    run_awaitable(ioc, handle_api_admin_history(ctx, std::move(services)));
}

void run_admin_history_path(HttpContext& ctx, AppServices services) {
    asio::io_context ioc;
    run_awaitable(ioc, handle_api_admin_history_path(ctx, std::move(services)));
}

void run_admin_rollback(HttpContext& ctx, AppServices services) {
    asio::io_context ioc;
    run_awaitable(ioc, handle_api_admin_rollback(ctx, std::move(services)));
}

void run_admin_snapshot_repair(HttpContext& ctx, AppServices services) {
    asio::io_context ioc;
    run_awaitable(
        ioc, handle_api_admin_snapshot_repair(ctx, std::move(services)));
}

void run_admin_mirror_rebuild(HttpContext& ctx, AppServices services) {
    asio::io_context ioc;
    run_awaitable(
        ioc, handle_api_admin_mirror_rebuild(ctx, std::move(services)));
}

void run_admin_history_migration(HttpContext& ctx, AppServices services) {
    asio::io_context ioc;
    run_awaitable(
        ioc, handle_api_admin_history_migration(ctx, std::move(services)));
}

void run_admin_orphan_resolution(HttpContext& ctx, AppServices services) {
    asio::io_context ioc;
    run_awaitable(
        ioc, handle_api_admin_orphan_resolution(ctx, std::move(services)));
}

void run_admin_page(HttpContext& ctx) {
    asio::io_context ioc;
    run_awaitable(ioc, handle_admin_page(ctx));
}

void run_admin_settings_page(HttpContext& ctx) {
    asio::io_context ioc;
    run_awaitable(ioc, handle_admin_settings_page(ctx));
}

void run_admin_login(HttpContext& ctx, AppServices services) {
    asio::io_context ioc;
    run_awaitable(ioc, handle_api_admin_login(ctx, std::move(services)));
}

std::string save_body(const std::string& upstream_content) {
    std::ostringstream out;
    out << "{\"base_version\":7,\"files\":["
        << "{\"name\":\"20-upstream.ini\",\"content\":\""
        << json_escape(upstream_content) << "\"},"
        << "{\"name\":\"30-security.ini\",\"content\":\""
        << json_escape("[security]\njwt_disabled = true\n") << "\"}"
        << "]}";
    return out.str();
}

}  // namespace

TEST(AdminConfigRoutes, MissingAdminTokenIsUnauthorized) {
    auto base = make_temp_base("missing_token");
    write_never_sync(base);
    FakeRedis redis;
    HttpContext ctx;
    ctx.method = "GET";

    run_admin_config(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 401);
    EXPECT_EQ(redis.calls.size(), 0u);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, BusinessAdminTokenIsUnauthorized) {
    auto base = make_temp_base("business_token");
    write_never_sync(base);
    FakeRedis redis;
    HttpContext ctx;
    ctx.method = "GET";
    ctx.headers.emplace_back("Authorization", "Bearer " + business_admin_token());

    run_admin_config(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 401);
    EXPECT_EQ(redis.calls.size(), 0u);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, ExpiredAdminTokenIsUnauthorized) {
    auto base = make_temp_base("expired_admin_token");
    write_never_sync(base);
    FakeRedis redis;
    HttpContext ctx;
    ctx.method = "GET";
    ctx.headers.emplace_back("Authorization", "Bearer " + expired_admin_token());

    run_admin_config(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 401);
    EXPECT_EQ(redis.calls.size(), 0u);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, UnconfiguredAdminFailsClosed) {
    auto base = make_temp_base("unconfigured");
    write_file(base / "config.d" / "12-config-sync.ini",
        "[config_sync]\n"
        "enabled = true\n");
    FakeRedis redis;
    HttpContext ctx;
    ctx.method = "GET";

    run_admin_config(ctx, services_for(base, redis, ConfigSyncConfig{}));

    EXPECT_EQ(ctx.status_code, 503);
    EXPECT_EQ(redis.calls.size(), 0u);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, InsecureAdminPermitsLabMode) {
    auto base = make_temp_base("insecure");
    write_never_sync(base);
    write_file(base / "config.d" / "99-local.ini",
        "[admin]\n"
        "insecure_no_auth = true\n");
    ConfigSyncConfig sync_cfg;
    sync_cfg.admin.insecure_no_auth = true;
    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "GET") return string_reply("3");
        if (args.front() == "HGETALL") return array_reply({
            "20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"
        });
        if (args.front() == "HGET") return snapshot_meta_reply({
            {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"}
        });
        return error_reply("unexpected command " + args.front());
    };
    HttpContext ctx;
    ctx.method = "GET";

    run_admin_config(ctx, services_for(base, redis, sync_cfg));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_EQ(redis.count("GET"), 2u);
    EXPECT_EQ(redis.count("HGETALL"), 1u);
    EXPECT_EQ(redis.count("HGET"), 1u);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, AdminAuthRereadsLocalConfigInsteadOfStartupSnapshot) {
    auto base = make_temp_base("auth_reread");
    write_never_sync(base);
    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "GET") return string_reply("7");
        if (args.front() == "HGETALL") return array_reply({
            "20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"
        });
        if (args.front() == "HGET") return snapshot_meta_reply({
            {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"}
        });
        return error_reply("unexpected command " + args.front());
    };
    HttpContext ctx;
    ctx.method = "GET";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_config(ctx, services_for(base, redis, ConfigSyncConfig{}));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_EQ(redis.count("GET"), 2u);
    EXPECT_EQ(redis.count("HGETALL"), 1u);
    EXPECT_EQ(redis.count("HGET"), 1u);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, LoginIssuesAdminTokenForAdminPassword) {
    auto base = make_temp_base("login_success");
    write_never_sync(base);
    FakeRedis redis;
    HttpContext ctx;
    ctx.method = "POST";
    ctx.client_ip = "10.0.0.10";
    ctx.body = R"({"username":"admin","password":"admin"})";

    run_admin_login(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("\"token\":\""), std::string::npos);
    EXPECT_NE(ctx.response_body.find("\"expires_in\":7200"), std::string::npos);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, PasswordRotationRevokesPreviouslyIssuedToken) {
    auto cfg = admin_sync_config();
    auto issued = config_admin::issue_admin_token(cfg.admin, "admin");
    ASSERT_TRUE(issued);
    cfg.admin.accounts[0].password_hash =
        config_admin::pbkdf2_sha256_hash_string(
            "rotated", std::vector<unsigned char>{'n', 'e', 'w'}, 1000);

    EXPECT_FALSE(config_admin::verify_admin_token(
        cfg.admin, "Bearer " + issued->token));
}

TEST(AdminConfigRoutes, LoginRereadsLocalAdminConfigEachAttempt) {
    auto base = make_temp_base("login_reread");
    write_never_sync(base);
    FakeRedis redis;

    HttpContext before;
    before.method = "POST";
    before.client_ip = "10.0.0.13";
    before.body = R"({"username":"admin","password":"admin"})";
    run_admin_login(before, services_for(base, redis));
    EXPECT_EQ(before.status_code, 200);

    write_file(base / "config.d" / "99-local.ini",
        "[admin]\n"
        "admin = " + config_admin::pbkdf2_sha256_hash_string(
            "rotated", std::vector<unsigned char>{'s', 'a', 'l', 't', '2'}, 1000) + "\n");

    HttpContext old_password;
    old_password.method = "POST";
    old_password.client_ip = "10.0.0.14";
    old_password.body = R"({"username":"admin","password":"admin"})";
    run_admin_login(old_password, services_for(base, redis));
    EXPECT_EQ(old_password.status_code, 401);

    HttpContext new_password;
    new_password.method = "POST";
    new_password.client_ip = "10.0.0.15";
    new_password.body = R"({"username":"admin","password":"rotated"})";
    run_admin_login(new_password, services_for(base, redis));
    EXPECT_EQ(new_password.status_code, 200);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, LoginRejectsWrongPasswordUniformly) {
    auto base = make_temp_base("login_wrong");
    write_never_sync(base);
    FakeRedis redis;
    HttpContext ctx;
    ctx.method = "POST";
    ctx.client_ip = "10.0.0.11";
    ctx.body = R"({"username":"admin","password":"wrong"})";

    run_admin_login(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 401);
    EXPECT_NE(ctx.response_body.find("invalid credentials"), std::string::npos);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, LoginRejectsOversizedFields) {
    auto base = make_temp_base("login_oversized");
    write_never_sync(base);
    FakeRedis redis;
    HttpContext ctx;
    ctx.method = "POST";
    ctx.client_ip = "10.0.0.16";
    ctx.body = "{\"username\":\"" + std::string(257, 'a') +
        "\",\"password\":\"admin\"}";

    run_admin_login(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 400);
    EXPECT_NE(ctx.response_body.find("invalid username"), std::string::npos);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, LoginLocksAfterRepeatedFailures) {
    auto base = make_temp_base("login_lock");
    write_never_sync(base);
    FakeRedis redis;

    for (int i = 0; i < 5; ++i) {
        HttpContext ctx;
        ctx.method = "POST";
        ctx.client_ip = "10.0.0.12";
        ctx.body = R"({"username":"admin","password":"wrong"})";
        run_admin_login(ctx, services_for(base, redis));
        EXPECT_EQ(ctx.status_code, 401);
    }

    HttpContext locked;
    locked.method = "POST";
    locked.client_ip = "10.0.0.12";
    locked.body = R"({"username":"admin","password":"admin"})";
    run_admin_login(locked, services_for(base, redis));

    EXPECT_EQ(locked.status_code, 401);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, LoginLockCannotBeBypassedByRotatingUsernames) {
    auto base = make_temp_base("login_lock_ip");
    write_never_sync(base);
    FakeRedis redis;

    for (int i = 0; i < 5; ++i) {
        HttpContext ctx;
        ctx.method = "POST";
        ctx.client_ip = "10.0.0.17";
        ctx.body = "{\"username\":\"unknown" + std::to_string(i) +
            "\",\"password\":\"wrong\"}";
        run_admin_login(ctx, services_for(base, redis));
        EXPECT_EQ(ctx.status_code, 401);
    }

    HttpContext locked;
    locked.method = "POST";
    locked.client_ip = "10.0.0.17";
    locked.body = R"({"username":"admin","password":"admin"})";
    run_admin_login(locked, services_for(base, redis));

    EXPECT_EQ(locked.status_code, 401);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, LoginRunsWithDedicatedAuthWorker) {
    auto base = make_temp_base("login_worker");
    write_never_sync(base);
    FakeRedis redis;
    asio::thread_pool workers(1);
    auto services = services_for(base, redis);
    services.admin_auth_workers = &workers;
    HttpContext ctx;
    ctx.method = "POST";
    ctx.client_ip = "10.0.0.18";
    ctx.body = R"({"username":"admin","password":"admin"})";

    run_admin_login(ctx, std::move(services));
    workers.join();

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("\"token\":\""), std::string::npos);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, GetConfigReturnsVersionFilesAndRestartFlags) {
    auto base = make_temp_base("get");
    write_never_sync(base);
    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "GET") return string_reply("7");
        if (args.front() == "HGETALL") {
            return array_reply({
                "20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n",
                "10-mysql.ini", "[mysql]\nhost = 127.0.0.1\n"
            });
        }
        if (args.front() == "HGET") {
            return snapshot_meta_reply({
                {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"},
                {"10-mysql.ini", "[mysql]\nhost = 127.0.0.1\n"}
            });
        }
        return error_reply("unexpected command " + args.front());
    };
    HttpContext ctx;
    ctx.method = "GET";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_config(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("\"version\":7"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("20-upstream.ini"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("\"restart_required\":true"), std::string::npos);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, ConfigRequestRunsWithDedicatedAuthWorker) {
    auto base = make_temp_base("config_worker");
    write_never_sync(base);
    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "GET") return string_reply("7");
        if (args.front() == "HGETALL") return array_reply({
            "20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"
        });
        if (args.front() == "HGET") return snapshot_meta_reply({
            {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"}
        });
        return error_reply("unexpected command " + args.front());
    };
    asio::thread_pool workers(1);
    auto services = services_for(base, redis);
    services.admin_auth_workers = &workers;
    HttpContext ctx;
    ctx.method = "GET";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_config(ctx, std::move(services));
    workers.join();

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_EQ(redis.count("GET"), 2u);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, CompatModeUsesVerifiedMirrorFallback) {
    auto base = make_temp_base("compat_mirror");
    write_never_sync(base);
    FakeRedis redis;
    int hgetall_calls = 0;
    redis.handler = [&hgetall_calls](const std::vector<std::string>& args) {
        if (args.front() == "GET") return string_reply("7");
        if (args.front() == "HGETALL") {
            ++hgetall_calls;
            if (hgetall_calls == 1) return array_reply({});
            return array_reply({
                "20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"
            });
        }
        return error_reply("unexpected command " + args.front());
    };
    auto cfg = admin_sync_config();
    cfg.history.read_mode = "compat";
    HttpContext ctx;
    ctx.method = "GET";
    set_admin_bearer(ctx, cfg);

    run_admin_config(ctx, services_for(base, redis, cfg));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("\"degraded\":true"), std::string::npos);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, RequiredModeVerifiesMirrorHashBeforeFallback) {
    auto base = make_temp_base("required_mirror");
    write_never_sync(base);
    const std::map<std::string, std::string> files{
        {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"}
    };
    FakeRedis redis;
    int hgetall_calls = 0;
    redis.handler = [&hgetall_calls, &files](const std::vector<std::string>& args) {
        if (args.front() == "GET") return string_reply("7");
        if (args.front() == "HGETALL") {
            ++hgetall_calls;
            if (hgetall_calls == 1) return array_reply({});
            return array_reply({files.begin()->first, files.begin()->second});
        }
        if (args.front() == "HGET") return string_reply(snapshot_meta(files));
        return error_reply("unexpected command " + args.front());
    };
    auto cfg = admin_sync_config();
    cfg.history.read_mode = "required";
    HttpContext ctx;
    ctx.method = "GET";
    set_admin_bearer(ctx, cfg);

    run_admin_config(ctx, services_for(base, redis, cfg));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("\"degraded\":true"), std::string::npos);
    EXPECT_EQ(redis.count("HGET"), 1u);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, GetConfigRejectsNonNumericVersion) {
    auto base = make_temp_base("bad_version");
    write_never_sync(base);
    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "GET") return string_reply("abc");
        return error_reply("unexpected command " + args.front());
    };
    HttpContext ctx;
    ctx.method = "GET";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_config(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 500);
    EXPECT_EQ(redis.count("HGETALL"), 0u);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, GetConfigReturns409WhenVersionChangesDuringRead) {
    auto base = make_temp_base("get_version_moved");
    write_never_sync(base);
    int get_count = 0;
    FakeRedis redis;
    redis.handler = [&get_count](const std::vector<std::string>& args) {
        if (args.front() == "GET") {
            ++get_count;
            return string_reply(std::to_string(6 + get_count));
        }
        if (args.front() == "HGETALL") return array_reply({
            "20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"
        });
        if (args.front() == "HGET") return snapshot_meta_reply({
            {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"}
        });
        return error_reply("unexpected command " + args.front());
    };
    HttpContext ctx;
    ctx.method = "GET";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_config(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 409);
    EXPECT_EQ(redis.count("GET"), 4u);
    EXPECT_EQ(redis.count("HGETALL"), 2u);
    EXPECT_EQ(redis.count("HGET"), 2u);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, PostRejectsEmptyFileSetBeforeRedis) {
    auto base = make_temp_base("empty");
    write_never_sync(base);
    FakeRedis redis;
    HttpContext ctx;
    ctx.method = "POST";
    set_admin_bearer(ctx, admin_sync_config());
    ctx.body = "{\"base_version\":7,\"files\":[]}";

    run_admin_config(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 400);
    EXPECT_EQ(redis.calls.size(), 0u);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, PostRejectsReservedAdminBlacklistBeforeRedis) {
    auto base = make_temp_base("reserved");
    write_never_sync(base);
    FakeRedis redis;
    HttpContext ctx;
    ctx.method = "POST";
    set_admin_bearer(ctx, admin_sync_config());
    ctx.body = "{\"base_version\":7,\"files\":[{\"name\":\"34-path_blacklist.ini\","
               "\"content\":\"[path_blacklist]\\n/api/admin/ =\\n\"}]}";

    run_admin_config(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 400);
    EXPECT_EQ(redis.calls.size(), 0u);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, PostRejectsManagedAdminSectionBeforeRedis) {
    auto base = make_temp_base("managed_admin_section");
    write_never_sync(base);
    FakeRedis redis;
    HttpContext ctx;
    ctx.method = "POST";
    set_admin_bearer(ctx, admin_sync_config());
    ctx.body = "{\"base_version\":7,\"files\":[{\"name\":\"20-admin.ini\","
               "\"content\":\"[admin]\\ninsecure_no_auth = true\\n\"}]}";

    run_admin_config(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 400);
    EXPECT_NE(ctx.response_body.find("[admin]"), std::string::npos);
    EXPECT_EQ(redis.calls.size(), 0u);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, PostRejectsManagedConfigSyncSectionBeforeRedis) {
    auto base = make_temp_base("managed_config_sync_section");
    write_never_sync(base);
    FakeRedis redis;
    HttpContext ctx;
    ctx.method = "POST";
    set_admin_bearer(ctx, admin_sync_config());
    ctx.body = "{\"base_version\":7,\"files\":[{\"name\":\"20-config-sync.ini\","
               "\"content\":\"[config_sync]\\nenabled = true\\n\"}]}";

    run_admin_config(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 400);
    EXPECT_NE(ctx.response_body.find("[config_sync]"), std::string::npos);
    EXPECT_EQ(redis.calls.size(), 0u);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, PostDryRunRejectsInvalidWholeDirectory) {
    auto base = make_temp_base("dryrun");
    write_never_sync(base);
    FakeRedis redis;
    HttpContext ctx;
    ctx.method = "POST";
    set_admin_bearer(ctx, admin_sync_config());
    ctx.body = "{\"base_version\":7,\"files\":[{\"name\":\"30-security.ini\","
               "\"content\":\"[security]\\njwt_algorithm = HS256\\n\"}]}";

    run_admin_config(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 400);
    EXPECT_NE(ctx.response_body.find("dry-run rejected"), std::string::npos);
    EXPECT_EQ(redis.calls.size(), 0u);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, PostSaveSuccessRunsLuaAndReturnsNewVersion) {
    auto base = make_temp_base("save");
    write_never_sync(base);
    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "EVAL") {
            EXPECT_NE(args[1].find("return -4"), std::string::npos);
            EXPECT_NE(args[1].find("if #ARGV < 8"), std::string::npos);
            EXPECT_NE(args[1].find("local published = redis.call('INCR'"),
                std::string::npos);
            EXPECT_EQ(args[2], "9");
            EXPECT_EQ(args[11], config_history::snapshot_key(7));
            EXPECT_EQ(args[12], "7");
            EXPECT_NE(args[13].find("admin"), std::string::npos);
            EXPECT_NE(args[14].find("\"version\":8"), std::string::npos);
            return integer_reply(8);
        }
        return error_reply("unexpected command " + args.front());
    };
    HttpContext ctx;
    ctx.method = "POST";
    set_admin_bearer(ctx, admin_sync_config());
    ctx.body = save_body("[upstream]\nzebra = 127.0.0.1:30001\n");

    run_admin_config(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("\"version\":8"), std::string::npos);
    EXPECT_EQ(redis.count("EVAL"), 1u);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, PostSaveCasConflictReturns409AndCurrentVersion) {
    auto base = make_temp_base("cas");
    write_never_sync(base);
    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "EVAL") return integer_reply(-1);
        if (args.front() == "GET") return string_reply("9");
        return error_reply("unexpected command " + args.front());
    };
    HttpContext ctx;
    ctx.method = "POST";
    set_admin_bearer(ctx, admin_sync_config());
    ctx.body = save_body("[upstream]\nzebra = 127.0.0.1:30001\n");

    run_admin_config(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 409);
    EXPECT_NE(ctx.response_body.find("\"current_version\":9"), std::string::npos);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, PostSaveInvalidVersionCodeReturns500Not409) {
    auto base = make_temp_base("invalid_version");
    write_never_sync(base);
    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "EVAL") return integer_reply(-4);
        return error_reply("unexpected command " + args.front());
    };
    HttpContext ctx;
    ctx.method = "POST";
    set_admin_bearer(ctx, admin_sync_config());
    ctx.body = save_body("[upstream]\nzebra = 127.0.0.1:30001\n");

    run_admin_config(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 500);
    EXPECT_NE(ctx.response_body.find("not numeric"), std::string::npos);
    EXPECT_EQ(redis.count("GET"), 0u);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, MachinesReturnsHeartbeatRows) {
    auto base = make_temp_base("machines");
    write_never_sync(base);
    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "HGETALL") {
            return array_reply({
                "node-a", "7|1786880000|123|ok",
                "node-b", "6|1786880001|124|partial|34-path_blacklist.ini:bad"
            });
        }
        return error_reply("unexpected command " + args.front());
    };
    HttpContext ctx;
    ctx.method = "GET";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_machines(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("node-a"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("\"status\":\"partial\""), std::string::npos);
    EXPECT_NE(ctx.response_body.find("34-path_blacklist.ini"), std::string::npos);

    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, HistoryListUsesOneBoundedRedisCall) {
    auto base = make_temp_base("history_list");
    write_never_sync(base);
    const std::map<std::string, std::string> files{
        {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"},
        {"30-security.ini", "[security]\njwt_disabled = true\n"}
    };
    FakeRedis redis;
    redis.handler = [&files](const std::vector<std::string>& args) {
        EXPECT_EQ(args.front(), "EVAL");
        EXPECT_EQ(args[2], "3");
        EXPECT_EQ(args[6], "9");
        EXPECT_EQ(args[7], "2");
        EXPECT_EQ(args[8], config_history::kSnapshotPrefix);
        return array_reply({
            "10", "10", snapshot_meta_json(10, files),
            "9", snapshot_meta_json(9, files)
        });
    };
    HttpContext ctx;
    ctx.method = "GET";
    ctx.path = "/api/admin/config/history?before=9&limit=2";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_history(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("\"current_version\":10"),
        std::string::npos);
    EXPECT_NE(ctx.response_body.find("\"next_before\":9"),
        std::string::npos);
    EXPECT_EQ(redis.count("EVAL"), 1u);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, HistoryDetailValidatesSnapshotHash) {
    auto base = make_temp_base("history_detail");
    write_never_sync(base);
    const std::map<std::string, std::string> files{
        {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"}
    };
    FakeRedis redis;
    redis.handler = [&files](const std::vector<std::string>& args) {
        EXPECT_EQ(args[2], "4");
        EXPECT_EQ(args.back(), "7");
        return detail_reply(8, 7, files);
    };
    HttpContext ctx;
    ctx.method = "GET";
    ctx.path = "/api/admin/config/history/7";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_history_path(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("\"version\":7"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("20-upstream.ini"), std::string::npos);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, HistoryDetailRejectsSnapshotHashMismatch) {
    auto base = make_temp_base("history_hash_mismatch");
    write_never_sync(base);
    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>&) {
        return array_reply({
            "8", "{\"content_sha256\":\"bad\"}",
            "20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"
        });
    };
    HttpContext ctx;
    ctx.method = "GET";
    ctx.path = "/api/admin/config/history/7";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_history_path(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 409);
    EXPECT_NE(ctx.response_body.find("hash mismatch"), std::string::npos);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, MirrorRedisFailureReturns500InsteadOfMissingSnapshot409) {
    auto base = make_temp_base("mirror_redis_failure");
    write_never_sync(base);
    FakeRedis redis;
    int hgetall_calls = 0;
    redis.handler = [&hgetall_calls](const std::vector<std::string>& args) {
        if (args.front() == "GET") return string_reply("7");
        if (args.front() == "HGETALL" && ++hgetall_calls == 1) {
            return array_reply({});
        }
        return error_reply("redis unavailable");
    };
    HttpContext ctx;
    ctx.method = "GET";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_config(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 500);
    EXPECT_NE(ctx.response_body.find("redis unavailable"), std::string::npos);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, HistoryDiffUsesPathVersionAsFrom) {
    auto base = make_temp_base("history_diff");
    write_never_sync(base);
    const std::map<std::string, std::string> from_files{
        {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"}
    };
    const std::map<std::string, std::string> to_files{
        {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30002\n"}
    };
    FakeRedis redis;
    redis.handler = [&from_files, &to_files](const std::vector<std::string>& args) {
        if (args.back() == "7") return detail_reply(8, 7, from_files);
        if (args.back() == "8") return detail_reply(8, 8, to_files);
        return error_reply("unexpected history version");
    };
    HttpContext ctx;
    ctx.method = "GET";
    ctx.path = "/api/admin/config/history/7/diff?to=8";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_history_path(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("\"from\":7,\"to\":8"),
        std::string::npos);
    EXPECT_NE(ctx.response_body.find("\"type\":\"modified\""),
        std::string::npos);
    EXPECT_EQ(redis.count("EVAL"), 2u);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, RollbackPublishesNewVersionWithoutLoweringVersion) {
    auto base = make_temp_base("history_rollback");
    write_never_sync(base);
    const std::map<std::string, std::string> files{
        {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"},
        {"30-security.ini", "[security]\njwt_disabled = true\n"}
    };
    FakeRedis redis;
    redis.handler = [&files](const std::vector<std::string>& args) {
        if (args[2] == "4") return detail_reply(8, 3, files);
        EXPECT_EQ(args[2], "10");
        EXPECT_NE(args[1].find("local source_version"), std::string::npos);
        EXPECT_EQ(args[12], config_history::snapshot_key(8));
        EXPECT_EQ(args[13], "8");
        EXPECT_NE(args[14].find("\"new_version\":9"), std::string::npos);
        EXPECT_NE(args[15].find("\"rollback_from\":3"), std::string::npos);
        EXPECT_EQ(args[20], "3");
        return integer_reply(9);
    };
    HttpContext ctx;
    ctx.method = "POST";
    ctx.path = "/api/admin/config/rollback";
    ctx.body = R"({"base_version":8,"target_version":3,"reason":"restore known good"})";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_rollback(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("\"version\":9"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("\"rollback_from\":3"),
        std::string::npos);
    EXPECT_EQ(redis.count("EVAL"), 2u);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, RollbackRejectsCasConflictBeforePublish) {
    auto base = make_temp_base("rollback_cas");
    write_never_sync(base);
    const std::map<std::string, std::string> files{
        {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"}
    };
    FakeRedis redis;
    redis.handler = [&files](const std::vector<std::string>&) {
        return detail_reply(9, 3, files);
    };
    HttpContext ctx;
    ctx.method = "POST";
    ctx.body = R"({"base_version":8,"target_version":3,"reason":"stale base"})";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_rollback(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 409);
    EXPECT_NE(ctx.response_body.find("\"current_version\":9"), std::string::npos);
    EXPECT_EQ(redis.count("EVAL"), 1u);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, RollbackRejectsNeverSyncFileFromHistory) {
    auto base = make_temp_base("rollback_never_sync");
    write_never_sync(base);
    const std::map<std::string, std::string> files{
        {"11-redis.ini", "[redis]\nmode = direct\n"}
    };
    FakeRedis redis;
    redis.handler = [&files](const std::vector<std::string>&) {
        return detail_reply(8, 3, files);
    };
    HttpContext ctx;
    ctx.method = "POST";
    ctx.body = R"({"base_version":8,"target_version":3,"reason":"invalid old snapshot"})";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_rollback(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 409);
    EXPECT_NE(ctx.response_body.find("never-sync"), std::string::npos);
    EXPECT_EQ(redis.count("EVAL"), 1u);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, RollbackReturns404WhenGcDeletedSource) {
    auto base = make_temp_base("rollback_gc");
    write_never_sync(base);
    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>&) {
        return array_reply({});
    };
    HttpContext ctx;
    ctx.method = "POST";
    ctx.body = R"({"base_version":8,"target_version":3,"reason":"deleted source"})";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_rollback(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 404);
    EXPECT_EQ(redis.count("EVAL"), 1u);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, SnapshotRepairRequiresMatchingMirrorMetadata) {
    auto base = make_temp_base("snapshot_repair");
    write_never_sync(base);
    const std::map<std::string, std::string> files{
        {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"}
    };
    FakeRedis redis;
    redis.handler = [&files](const std::vector<std::string>& args) {
        if (args.front() == "HGETALL") {
            return array_reply({
                "20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"
            });
        }
        if (args.front() == "HGET") {
            return string_reply(snapshot_meta_json(8, files));
        }
        EXPECT_EQ(args.front(), "EVAL");
        EXPECT_EQ(args[2], "7");
        EXPECT_NE(args[1].find("redis.call('RENAME', KEYS[6], KEYS[5])"),
            std::string::npos);
        return integer_reply(8);
    };
    HttpContext ctx;
    ctx.method = "POST";
    ctx.path = "/api/admin/config/history/repair-snapshot";
    ctx.body = R"({"version":8,"reason":"repair deleted current snapshot"})";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_snapshot_repair(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("\"action\":\"snapshot-repair\""),
        std::string::npos);
    EXPECT_EQ(redis.calls.size(), 3u);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, MirrorRebuildUsesValidatedCurrentSnapshot) {
    auto base = make_temp_base("mirror_rebuild");
    write_never_sync(base);
    const std::map<std::string, std::string> files{
        {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"}
    };
    FakeRedis redis;
    redis.handler = [&files](const std::vector<std::string>& args) {
        if (args[2] == "4") return detail_reply(8, 8, files);
        EXPECT_EQ(args[2], "7");
        EXPECT_NE(args[1].find("redis.call('RENAME', KEYS[6], KEYS[2])"),
            std::string::npos);
        return integer_reply(8);
    };
    HttpContext ctx;
    ctx.method = "POST";
    ctx.body = R"({"version":8,"reason":"rebuild mirror"})";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_mirror_rebuild(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("mirror-rebuild"), std::string::npos);
    EXPECT_EQ(redis.count("EVAL"), 2u);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, HistoryMigrationPublishesLegacyMirrorInCompatMode) {
    auto base = make_temp_base("history_migration");
    write_never_sync(base);
    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args.front() == "HGETALL") {
            return array_reply({
                "20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"
            });
        }
        EXPECT_EQ(args.front(), "EVAL");
        EXPECT_NE(args[1].find("redis.call('ZCARD', KEYS[4]) ~= 0"),
            std::string::npos);
        return integer_reply(8);
    };
    auto cfg = admin_sync_config();
    cfg.history.read_mode = "compat";
    HttpContext ctx;
    ctx.method = "POST";
    ctx.body = R"({"version":8,"reason":"migrate legacy history"})";
    set_admin_bearer(ctx, cfg);

    run_admin_history_migration(ctx, services_for(base, redis, cfg));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("migration"), std::string::npos);
    EXPECT_EQ(redis.count("HGETALL"), 1u);
    EXPECT_EQ(redis.count("EVAL"), 1u);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, OrphanRestoreRebuildsMirrorThenRestoresVersionPointer) {
    auto base = make_temp_base("orphan_restore");
    write_never_sync(base);
    const std::map<std::string, std::string> files{
        {"20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"}
    };
    const std::string meta = snapshot_meta_json(9, files);
    FakeRedis redis;
    redis.handler = [&files, &meta](const std::vector<std::string>& args) {
        if (args[2] == "5") {
            return array_reply({
                "8", "9", "8", "1", meta, "1",
                files.begin()->first, files.begin()->second
            });
        }
        EXPECT_EQ(args[2], "8");
        EXPECT_NE(args[1].find("redis.call('SET', KEYS[1], target)"),
            std::string::npos);
        EXPECT_EQ(args[11], "8");
        EXPECT_EQ(args[12], "9");
        return integer_reply(9);
    };
    HttpContext ctx;
    ctx.method = "POST";
    ctx.path = "/api/admin/config/history/resolve-orphan";
    ctx.body = R"({"current_version":8,"target_version":9,"action":"restore-version","reason":"confirmed published from AOF","confirmation":"RESTORE_VERSION_POINTER"})";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_orphan_resolution(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("\"version\":9"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("\"action\":\"restore-version\""),
        std::string::npos);
    EXPECT_EQ(redis.count("EVAL"), 2u);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, OrphanDeleteAtomicallyRemovesConfirmedPartialTriple) {
    auto base = make_temp_base("orphan_delete");
    write_never_sync(base);
    FakeRedis redis;
    redis.handler = [](const std::vector<std::string>& args) {
        if (args[2] == "5") {
            return array_reply({
                "8", "8", "8", "0", "__missing__", "1",
                "20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"
            });
        }
        EXPECT_EQ(args[2], "6");
        EXPECT_NE(args[1].find("redis.call('UNLINK', KEYS[4])"),
            std::string::npos);
        EXPECT_NE(args[1].find("target ~= current + 1"), std::string::npos);
        return integer_reply(1);
    };
    HttpContext ctx;
    ctx.method = "POST";
    ctx.path = "/api/admin/config/history/resolve-orphan";
    ctx.body = R"({"current_version":8,"target_version":9,"action":"delete-orphan","reason":"confirmed unpublished from logs","confirmation":"DELETE_UNPUBLISHED_ORPHAN"})";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_orphan_resolution(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 200);
    EXPECT_NE(ctx.response_body.find("\"version\":8"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("\"action\":\"delete-orphan\""),
        std::string::npos);
    EXPECT_EQ(redis.count("EVAL"), 2u);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, OrphanNilScriptReplyReturns500WithoutDereference) {
    auto base = make_temp_base("orphan_nil");
    write_never_sync(base);
    FakeRedis redis;
    int calls = 0;
    redis.handler = [&calls](const std::vector<std::string>& args) {
        ++calls;
        if (calls == 1) {
            return array_reply({
                "8", "8", "8", "0", "__missing__", "1",
                "20-upstream.ini", "[upstream]\nzebra = 127.0.0.1:30001\n"
            });
        }
        EXPECT_EQ(args[2], "6");
        return nil_reply();
    };
    HttpContext ctx;
    ctx.method = "POST";
    ctx.body = R"({"current_version":8,"target_version":9,"action":"delete-orphan","reason":"nil regression","confirmation":"DELETE_UNPUBLISHED_ORPHAN"})";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_orphan_resolution(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 500);
    EXPECT_NE(ctx.response_body.find("non-integer"), std::string::npos);
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, OrphanResolutionRejectsWrongConfirmationBeforeRedis) {
    auto base = make_temp_base("orphan_confirmation");
    write_never_sync(base);
    FakeRedis redis;
    HttpContext ctx;
    ctx.method = "POST";
    ctx.body = R"({"current_version":8,"target_version":9,"action":"delete-orphan","reason":"test","confirmation":"yes"})";
    set_admin_bearer(ctx, admin_sync_config());

    run_admin_orphan_resolution(ctx, services_for(base, redis));

    EXPECT_EQ(ctx.status_code, 400);
    EXPECT_TRUE(redis.calls.empty());
    std::filesystem::remove_all(base);
}

TEST(AdminConfigRoutes, AdminPageReturnsLoginPage) {
    HttpContext ctx;
    ctx.method = "GET";

    run_admin_page(ctx);

    EXPECT_EQ(ctx.status_code, 200);
    ASSERT_FALSE(ctx.response_headers.empty());
    EXPECT_EQ(get_header_value(ctx.response_headers, "Cache-Control"), "no-store");
    EXPECT_FALSE(get_header_value(
        ctx.response_headers, "Content-Security-Policy").empty());
    EXPECT_NE(ctx.response_body.find("asio_owen Config Center"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("/api/admin/login"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("adminToken"), std::string::npos);
}

TEST(AdminConfigRoutes, AdminSettingsPageReturnsEmbeddedSinglePageApp) {
    HttpContext ctx;
    ctx.method = "GET";

    run_admin_settings_page(ctx);

    EXPECT_EQ(ctx.status_code, 200);
    ASSERT_FALSE(ctx.response_headers.empty());
    EXPECT_EQ(get_header_value(ctx.response_headers, "Cache-Control"), "no-store");
    EXPECT_NE(ctx.response_body.find("/api/admin/config"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("function render"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("/admin/login"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("from the pending changes?"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("prompt(\"Change reason\", \"\")"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("if (!reason) return"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("from the saved configuration:"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("Save these configuration changes as a new version?"),
        std::string::npos);
    EXPECT_NE(ctx.response_body.find("const selectedFile = files[selected]"),
        std::string::npos);
    EXPECT_NE(ctx.response_body.find("await load();"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("while (names.has(name))"), std::string::npos);
    EXPECT_NE(ctx.response_body.find("if (append && !historyBefore) return"),
        std::string::npos);
}
