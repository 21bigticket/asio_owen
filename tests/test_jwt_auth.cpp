#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <jwt-cpp/jwt.h>

#include "common/config.hpp"
#include "security/jwt_auth.hpp"
#include "security/security_rules.hpp"

namespace {

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

using Clock = std::chrono::system_clock;

std::string make_rs256_token(
    std::optional<Clock::time_point> exp,
    std::optional<Clock::time_point> nbf = std::nullopt,
    std::string issuer = "pixiu-gateway")
{
    auto builder = jwt::create();
    builder.set_issuer(std::move(issuer));
    builder.set_subject("1");
    builder.set_payload_claim("name", jwt::claim(std::string("tester")));

    if (exp) {
        builder.set_expires_at(*exp);
    }
    if (nbf) {
        builder.set_not_before(*nbf);
    }

    return builder.sign(jwt::algorithm::rs256{kPublicKey, kPrivateKey});
}

std::string make_none_token() {
    return jwt::create()
        .set_issuer("pixiu-gateway")
        .set_subject("1")
        .set_expires_at(Clock::now() + std::chrono::hours(1))
        .sign(jwt::algorithm::none{});
}

std::string make_hs256_token() {
    return jwt::create()
        .set_issuer("pixiu-gateway")
        .set_subject("1")
        .set_expires_at(Clock::now() + std::chrono::hours(1))
        .sign(jwt::algorithm::hs256{"test-secret-for-wrong-algorithm-token"});
}

JWTAuth make_auth() {
    return JWTAuth("test-secret-that-is-not-used-for-rs256",
        "pixiu-gateway", "RS256", kPublicKey);
}

std::filesystem::path make_temp_config_dir() {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto base = std::filesystem::temp_directory_path() /
        ("asio_owen_jwt_test_" + std::to_string(now));
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base / "config.d");
    return base;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
}

}  // namespace

TEST(JWTAuth, RS256AcceptsValidToken) {
    auto auth = make_auth();
    auto token = make_rs256_token(Clock::now() + std::chrono::hours(1));

    auto claims = auth.verify("Bearer " + token);

    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->subject, "1");
    EXPECT_EQ(claims->username, "tester");
}

TEST(JWTAuth, RS256RejectsMissingExpiration) {
    auto auth = make_auth();
    auto token = make_rs256_token(std::nullopt);

    EXPECT_FALSE(auth.verify("Bearer " + token).has_value());
}

TEST(JWTAuth, RS256RejectsExpiredTokenBeyondLeeway) {
    auto auth = make_auth();
    auto token = make_rs256_token(Clock::now() - std::chrono::seconds(120));

    EXPECT_FALSE(auth.verify("Bearer " + token).has_value());
}

TEST(JWTAuth, RS256AcceptsNotBeforeWithinLeeway) {
    auto auth = make_auth();
    auto token = make_rs256_token(
        Clock::now() + std::chrono::hours(1),
        Clock::now() + std::chrono::seconds(30));

    EXPECT_TRUE(auth.verify("Bearer " + token).has_value());
}

TEST(JWTAuth, RS256RejectsNotBeforeBeyondLeeway) {
    auto auth = make_auth();
    auto token = make_rs256_token(
        Clock::now() + std::chrono::hours(1),
        Clock::now() + std::chrono::seconds(120));

    EXPECT_FALSE(auth.verify("Bearer " + token).has_value());
}

TEST(JWTAuth, RS256RejectsNoneAlgorithmToken) {
    auto auth = make_auth();
    auto token = make_none_token();

    EXPECT_FALSE(auth.verify("Bearer " + token).has_value());
}

TEST(JWTAuth, RS256RejectsHS256AlgorithmToken) {
    auto auth = make_auth();
    auto token = make_hs256_token();

    EXPECT_FALSE(auth.verify("Bearer " + token).has_value());
}

TEST(SecurityRules, RS256DoesNotRequireJwtSecretToEnableVerification) {
    auto base = make_temp_config_dir();
    auto key_path = base / "public-key.pem";
    write_file(key_path, kPublicKey);
    write_file(base / "config.d" / "30-security.ini",
        "[security]\n"
        "jwt_algorithm = RS256\n"
        "jwt_public_key = " + key_path.string() + "\n"
        "jwt_issuer = pixiu-gateway\n");
    write_file(base / "config.d" / "40-rate_limit.ini",
        "[rate_limit]\n"
        "ip_rps = 0\n"
        "global_rps = 0\n");

    Config cfg;
    ASSERT_TRUE(cfg.load(base));

    SecurityRules rules;
    rules.load_from_config(cfg);

    asio::io_context ioc;
    asio::ip::tcp::socket socket(ioc);
    auto result = rules.check(socket, "GET", "/zebra-config/config.ConfigService/Get",
        "", "");

    EXPECT_EQ(result.status_code, 401);

    std::filesystem::remove_all(base);
}
