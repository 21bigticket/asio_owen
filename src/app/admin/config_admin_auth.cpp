#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include "../../common/config.hpp"
#include "../../db/redis_pool.hpp"
#include "../../http/response.hpp"
#include "../../http/upstream_manager.hpp"
#include "../../security/jwt_auth.hpp"
#include "../../security/principal.hpp"
#include "../../security/security_rules.hpp"
#include "../app_config.hpp"
#include "../config_sync_service.hpp"
#include "config_history.hpp"
#include "generated/admin_login_html.hpp"
#include "generated/admin_settings_html.hpp"
#include "config_admin.hpp"

namespace config_admin {

std::string base64url_encode(const unsigned char* data, size_t len) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len * 4 + 2) / 3);
    uint32_t bits = 0;
    int bit_count = 0;
    for (size_t i = 0; i < len; ++i) {
        bits = (bits << 8) | data[i];
        bit_count += 8;
        while (bit_count >= 6) {
            bit_count -= 6;
            out.push_back(alphabet[(bits >> bit_count) & 0x3f]);
        }
    }
    if (bit_count > 0) {
        out.push_back(alphabet[(bits << (6 - bit_count)) & 0x3f]);
    }
    return out;
}

std::string base64url_encode(std::string_view data) {
    return base64url_encode(
        reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

std::optional<std::vector<unsigned char>> base64url_decode(std::string_view input) {
    std::vector<unsigned char> out;
    out.reserve(input.size() * 3 / 4);
    uint32_t bits = 0;
    int bit_count = 0;
    for (char c : input) {
        if (c == '=') break;
        int value = -1;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '-') value = 62;
        else if (c == '_') value = 63;
        else return std::nullopt;
        bits = (bits << 6) | static_cast<uint32_t>(value);
        bit_count += 6;
        if (bit_count >= 8) {
            bit_count -= 8;
            out.push_back(static_cast<unsigned char>((bits >> bit_count) & 0xff));
        }
    }
    return out;
}

std::optional<std::vector<unsigned char>> pbkdf2_sha256(
    std::string_view password,
    const std::vector<unsigned char>& salt,
    int iterations,
    size_t output_len) {
    if (iterations <= 0 || iterations > 10000000 || output_len == 0) {
        return std::nullopt;
    }
    std::vector<unsigned char> out(output_len);
    if (PKCS5_PBKDF2_HMAC(
            password.data(), static_cast<int>(password.size()),
            salt.data(), static_cast<int>(salt.size()),
            iterations, EVP_sha256(), static_cast<int>(out.size()),
            out.data()) != 1) {
        return std::nullopt;
    }
    return out;
}

std::string pbkdf2_sha256_hash_string(
    std::string_view password,
    const std::vector<unsigned char>& salt,
    int iterations,
    size_t output_len) {
    auto hash = pbkdf2_sha256(password, salt, iterations, output_len);
    if (!hash) return {};
    return "pbkdf2_sha256$" + std::to_string(iterations) + "$" +
        base64url_encode(salt.data(), salt.size()) + "$" +
        base64url_encode(hash->data(), hash->size());
}

// The output buffers are distinct semantic fields despite sharing a type.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool split_password_hash(
    std::string_view encoded,
    int& iterations,
    std::vector<unsigned char>& salt,
    std::vector<unsigned char>& expected) {
    std::array<std::string_view, 4> parts{};
    size_t pos = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        auto next = encoded.find('$', pos);
        if (i + 1 == parts.size()) {
            if (next != std::string_view::npos) return false;
            parts[i] = encoded.substr(pos);
        } else {
            if (next == std::string_view::npos) return false;
            parts[i] = encoded.substr(pos, next - pos);
            pos = next + 1;
        }
    }
    if (parts[0] != "pbkdf2_sha256") return false;
    auto parsed_iter = parse_int64(parts[1]);
    if (!parsed_iter || *parsed_iter <= 0 || *parsed_iter > 10000000) {
        return false;
    }
    auto parsed_salt = base64url_decode(parts[2]);
    auto parsed_hash = base64url_decode(parts[3]);
    if (!parsed_salt || !parsed_hash || parsed_salt->empty() ||
        parsed_hash->empty()) {
        return false;
    }
    iterations = static_cast<int>(*parsed_iter);
    salt = std::move(*parsed_salt);
    expected = std::move(*parsed_hash);
    return true;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

// Username and password are intentionally adjacent authentication inputs.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool verify_admin_password(
    const AdminConfig& admin,
    const std::string& username,
    const std::string& password) {
    const AdminAccountConfig* account = nullptr;
    for (const auto& item : admin.accounts) {
        if (item.username == username) {
            account = &item;
            break;
        }
    }

    int iterations = 100000;
    std::vector<unsigned char> salt{
        'a', 's', 'i', 'o', '_', 'o', 'w', 'e', 'n', '_',
        'a', 'd', 'm', 'i', 'n', '_', 'd', 'u', 'm', 'm', 'y'
    };
    std::vector<unsigned char> expected(32, 0);
    bool valid_account_hash = false;
    if (account) {
        valid_account_hash = split_password_hash(
            account->password_hash, iterations, salt, expected);
    }
    auto actual = pbkdf2_sha256(password, salt, iterations, expected.size());
    if (!actual || actual->size() != expected.size()) return false;
    bool matched =
        CRYPTO_memcmp(actual->data(), expected.data(), expected.size()) == 0;
    return valid_account_hash && matched;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

std::optional<std::string> load_pem_or_file(
    const std::string& value,
    const std::filesystem::path& base) {
    if (value.empty()) return std::nullopt;
    if (value.find("-----BEGIN") != std::string::npos) return value;
    std::filesystem::path path(value);
    if (!base.empty() && path.is_relative()) {
        path = base / path;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    auto loaded = ss.str();
    if (loaded.find("-----BEGIN") == std::string::npos) return std::nullopt;
    return loaded;
}

std::optional<AdminConfig> load_local_admin_config(
    const std::filesystem::path& config_base,
    std::string* error) {
    if (config_base.empty()) {
        if (error) *error = "config base is unavailable";
        return std::nullopt;
    }

    Config cfg;
    constexpr std::array<std::string_view, 2> local_files = {
        "12-config-sync.ini", "99-local.ini"
    };
    bool loaded_any = false;
    for (auto name : local_files) {
        auto path = config_base / "config.d" / std::string(name);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            continue;
        }
        if (!cfg.load_file(path)) {
            if (error) *error = "failed to parse " + std::string(name);
            return std::nullopt;
        }
        loaded_any = true;
    }
    if (!loaded_any) {
        if (error) *error = "local admin config not found";
        return std::nullopt;
    }

    try {
        return admin_config_from(cfg);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

bool admin_configured(
    const AdminConfig& admin,
    const std::filesystem::path& base) {
    return !admin.accounts.empty() &&
           load_pem_or_file(admin.jwt_private_key, base).has_value() &&
           load_pem_or_file(admin.jwt_public_key, base).has_value();
}

struct PkeyFree {
    void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};

std::optional<std::string> sign_rs256(
    std::string_view data,
    const std::string& private_key_pem) {
    std::unique_ptr<BIO, detail::BioFree> bio(
        BIO_new_mem_buf(private_key_pem.data(),
            static_cast<int>(private_key_pem.size())));
    if (!bio) return std::nullopt;
    std::unique_ptr<EVP_PKEY, PkeyFree> pkey(
        PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
    if (!pkey) return std::nullopt;

    std::unique_ptr<EVP_MD_CTX, detail::MdCtxFree> ctx(EVP_MD_CTX_new());
    if (!ctx) return std::nullopt;
    if (EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr,
            pkey.get()) != 1 ||
        EVP_DigestSignUpdate(ctx.get(), data.data(), data.size()) != 1) {
        return std::nullopt;
    }
    size_t sig_len = 0;
    if (EVP_DigestSignFinal(ctx.get(), nullptr, &sig_len) != 1 ||
        sig_len == 0) {
        return std::nullopt;
    }
    std::string signature(sig_len, '\0');
    if (EVP_DigestSignFinal(ctx.get(),
            reinterpret_cast<unsigned char*>(signature.data()),
            &sig_len) != 1) {
        return std::nullopt;
    }
    signature.resize(sig_len);
    return signature;
}

std::optional<std::string> admin_auth_version(
    const AdminConfig& admin,
    std::string_view username) {
    const AdminAccountConfig* account = nullptr;
    for (const auto& item : admin.accounts) {
        if (item.username == username) {
            account = &item;
            break;
        }
    }
    if (!account || account->password_hash.empty()) return std::nullopt;
    const std::string material =
        std::string(username) + "\n" + account->password_hash;
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;
    if (EVP_Digest(material.data(), material.size(), digest.data(),
            &digest_len, EVP_sha256(), nullptr) != 1 || digest_len == 0) {
        return std::nullopt;
    }
    return base64url_encode(digest.data(), digest_len);
}

std::optional<IssuedToken> issue_admin_token_with_pem(
    const AdminConfig& admin,
    const std::string& username,
    std::string_view private_key_pem) {
    auto auth_version = admin_auth_version(admin, username);
    if (!auth_version) return std::nullopt;
    const int ttl_sec = std::max(1, admin.token_ttl_min) * 60;
    const auto now = static_cast<int64_t>(std::time(nullptr));
    const auto exp = now + ttl_sec;
    const std::string header = R"({"alg":"RS256","typ":"JWT"})";
    std::ostringstream payload;
    payload << "{\"iss\":\"" << kAdminIssuer
            << "\",\"sub\":\"" << json_escape(username)
            << "\",\"name\":\"" << json_escape(username)
            << "\",\"roles\":[\"admin\"],\"av\":\""
            << *auth_version << "\",\"iat\":" << now
            << ",\"exp\":" << exp << "}";
    auto signing_input = base64url_encode(header) + "." +
        base64url_encode(payload.str());
    auto signature = sign_rs256(signing_input, std::string(private_key_pem));
    if (!signature) return std::nullopt;
    return IssuedToken{
        .token = signing_input + "." + base64url_encode(*signature),
        .expires_in = ttl_sec
    };
}

std::optional<IssuedToken> issue_admin_token(
    const AdminConfig& admin,
    const std::string& username,
    const std::filesystem::path& base) {
    auto private_key = load_pem_or_file(admin.jwt_private_key, base);
    if (!private_key) return std::nullopt;
    return issue_admin_token_with_pem(admin, username, *private_key);
}

std::optional<Principal> verify_admin_token_with_auth(
    const AdminConfig& admin,
    const std::string& auth_header,
    const JWTAuth& auth) {
    try {
        auto claims = auth.verify(auth_header);
        if (!claims) return std::nullopt;
        const std::string username = claims->username.empty() ?
            claims->subject : claims->username;
        auto expected_auth_version = admin_auth_version(admin, username);
        if (!expected_auth_version) return std::nullopt;
        static constexpr std::string_view bearer = "Bearer ";
        if (auth_header.size() <= bearer.size() ||
            !std::equal(bearer.begin(), bearer.end(), auth_header.begin(),
                [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                        std::tolower(static_cast<unsigned char>(b));
                })) {
            return std::nullopt;
        }
        auto decoded = jwt::decode(auth_header.substr(bearer.size()));
        if (!decoded.has_payload_claim("av") ||
            decoded.get_payload_claim("av").as_string() !=
                *expected_auth_version) {
            return std::nullopt;
        }
        Principal principal{
            .subject = claims->subject,
            .username = claims->username,
            .roles = claims->roles
        };
        return principal;
    } catch (const std::exception& e) {
        LOG_WARN("admin JWT verifier unavailable: ", e.what());
        return std::nullopt;
    }
}

std::optional<Principal> verify_admin_token_with_pem(
    const AdminConfig& admin,
    const std::string& auth_header,
    std::string_view public_key_pem) {
    try {
        JWTAuth auth("admin-rs256-unused-secret",
            std::string(kAdminIssuer), "RS256", std::string(public_key_pem));
        return verify_admin_token_with_auth(admin, auth_header, auth);
    } catch (const std::exception& e) {
        LOG_WARN("admin JWT verifier unavailable: ", e.what());
        return std::nullopt;
    }
}

std::optional<Principal> verify_admin_token(
    const AdminConfig& admin,
    const std::string& auth_header,
    const std::filesystem::path& base) {
    auto public_key = load_pem_or_file(admin.jwt_public_key, base);
    if (!public_key) return std::nullopt;
    return verify_admin_token_with_pem(admin, auth_header, *public_key);
}

}  // namespace config_admin
