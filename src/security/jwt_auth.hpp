#pragma once
#include <string>
#include <string_view>
#include <optional>
#include <memory>
#include <vector>
#include <cstdint>
#include <cstring>
#include <chrono>

#include <jwt-cpp/jwt.h>
#include <openssl/bio.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/params.h>
#include <openssl/pem.h>
#include <openssl/bn.h>
#include "../common/logger.hpp"

// JWT verification result
struct JWTClaims {
    std::string subject;
    std::string username;
    std::vector<std::string> roles;
    int64_t exp = 0;
    int64_t iat = 0;
    std::string raw_token;
};

namespace detail {

// Build PEM-encoded RSA public key from JWKS base64url n (modulus) and e (exponent)
// Returns empty string on failure.
inline std::string build_rsa_pubkey_from_jwks(const std::string& n_b64url, const std::string& e_b64url) {
    // 1. Base64url decode
    auto b64url_decode = [](const std::string& in) -> std::vector<unsigned char> {
        std::string b64 = in;
        // Replace URL-safe chars
        for (auto& c : b64) {
            if (c == '-') c = '+';
            else if (c == '_') c = '/';
        }
        // Add padding
        while (b64.size() % 4) b64 += '=';
        
        auto len = b64.size();
        auto bio = BIO_new_mem_buf(b64.data(), static_cast<int>(len));
        if (!bio) return {};
        auto b64_bio = BIO_new(BIO_f_base64());
        BIO_push(b64_bio, bio);
        BIO_set_flags(b64_bio, BIO_FLAGS_BASE64_NO_NL);
        
        std::vector<unsigned char> out(len);
        int dec_len = BIO_read(b64_bio, out.data(), static_cast<int>(len));
        BIO_free_all(b64_bio);
        if (dec_len <= 0) return {};
        out.resize(dec_len);
        return out;
    };
    
    auto n_bytes = b64url_decode(n_b64url);
    auto e_bytes = b64url_decode(e_b64url);
    if (n_bytes.empty() || e_bytes.empty()) return {};
    
    // 2. Build RSA key from n and e via EVP_PKEY_fromdata (OpenSSL 3 compatible).
    struct BignumDeleter { void operator()(BIGNUM* p) const { BN_free(p); } };
    struct ParamBldDeleter { void operator()(OSSL_PARAM_BLD* p) const { OSSL_PARAM_BLD_free(p); } };
    struct ParamDeleter { void operator()(OSSL_PARAM* p) const { OSSL_PARAM_free(p); } };
    struct PkeyCtxDeleter { void operator()(EVP_PKEY_CTX* p) const { EVP_PKEY_CTX_free(p); } };

    std::unique_ptr<BIGNUM, BignumDeleter> n_bn(
        BN_bin2bn(n_bytes.data(), static_cast<int>(n_bytes.size()), nullptr));
    std::unique_ptr<BIGNUM, BignumDeleter> e_bn(
        BN_bin2bn(e_bytes.data(), static_cast<int>(e_bytes.size()), nullptr));
    if (!n_bn || !e_bn) {
        return {};
    }

    std::unique_ptr<OSSL_PARAM_BLD, ParamBldDeleter> bld(OSSL_PARAM_BLD_new());
    if (!bld) return {};
    if (OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_N, n_bn.get()) != 1 ||
        OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_E, e_bn.get()) != 1) {
        return {};
    }

    std::unique_ptr<OSSL_PARAM, ParamDeleter> params(OSSL_PARAM_BLD_to_param(bld.get()));
    if (!params) return {};

    std::unique_ptr<EVP_PKEY_CTX, PkeyCtxDeleter> ctx(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr));
    if (!ctx) return {};

    EVP_PKEY* raw_pkey = nullptr;
    bool ok = EVP_PKEY_fromdata_init(ctx.get()) == 1 &&
        EVP_PKEY_fromdata(ctx.get(), &raw_pkey, EVP_PKEY_PUBLIC_KEY, params.get()) == 1;
    if (!ok || !raw_pkey) {
        EVP_PKEY_free(raw_pkey);
        return {};
    }

    struct EVP_PKEY_Deleter { void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); } };
    std::unique_ptr<EVP_PKEY, EVP_PKEY_Deleter> pkey(raw_pkey);
    
    auto mem_bio = BIO_new(BIO_s_mem());
    if (!mem_bio) return {};
    
    if (PEM_write_bio_PUBKEY(mem_bio, pkey.get()) != 1) {
        BIO_free(mem_bio);
        return {};
    }
    
    char* pem_data = nullptr;
    long pem_len = BIO_get_mem_data(mem_bio, &pem_data);
    std::string pem(pem_data, static_cast<size_t>(pem_len));
    
    BIO_free(mem_bio);
    return pem;
}

} // namespace detail

// JWT verifier (using jwt-cpp header-only library)
// Requires: OpenSSL::SSL + OpenSSL::Crypto
// Supports: HS256, RS256
class JWTAuth {
public:
    JWTAuth(std::string secret, std::string issuer, std::string algorithm,
            std::string public_key = "")
        : secret_(std::move(secret))
        , issuer_(std::move(issuer))
        , algorithm_(std::move(algorithm))
        , public_key_(std::move(public_key))
    {
        if (algorithm_ == "HS256") {
            if (secret_.size() < 32) {
                LOG_WARN("JWT secret is less than 32 bytes, HS256 recommended minimum");
            }
        } else if (algorithm_ == "RS256") {
            if (public_key_.empty()) {
                throw std::runtime_error("JWT: RS256 requires jwt_public_key");
            }
            // Pre-load EVP_PKEY to catch bad PEM at construction time
            load_rsa_key();
        } else {
            throw std::runtime_error("JWT: unsupported algorithm " + algorithm_
                + ", supported: HS256, RS256");
        }
    }

    // Verify Authorization header
    // Returns JWTClaims (on success) or nullopt (on failure)
    std::optional<JWTClaims> verify(const std::string& auth_header) const {
        // 1. extract Bearer token
        // RFC 6750: Bearer prefix is case-insensitive
        auto token = extract_token(auth_header);
        if (!token) {
            LOG_WARN("JWT: invalid Authorization header format");
            return std::nullopt;
        }

        // 2. decode and verify with jwt-cpp
        try {
            return do_verify(*token);
        } catch (const std::exception& e) {
            LOG_WARN("JWT: verification failed: ", e.what());
            return std::nullopt;
        }
    }

private:
    std::string secret_;
    std::string issuer_;
    std::string algorithm_;
    std::string public_key_;
    struct EVP_PKEY_Deleter { void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); } };
    std::unique_ptr<EVP_PKEY, EVP_PKEY_Deleter> pkey_;

    // Load RSA public key from PEM string at construction time
    void load_rsa_key() {
        auto bio = BIO_new_mem_buf(public_key_.data(), static_cast<int>(public_key_.size()));
        if (!bio) {
            throw std::runtime_error("JWT: failed to create BIO for RSA public key");
        }
        EVP_PKEY* raw = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!raw) {
            throw std::runtime_error("JWT: failed to parse RSA public key PEM");
        }
        pkey_.reset(raw);
    }

    // Extract token from "Authorization: Bearer <token>"
    static std::optional<std::string> extract_token(const std::string& auth_header) {
        if (auth_header.size() < 7) return std::nullopt;

        // case-insensitive match "Bearer "
        auto prefix = "Bearer ";
        auto header_trimmed = auth_header;
        // trim leading whitespace
        while (!header_trimmed.empty() && std::isspace(header_trimmed.front()))
            header_trimmed.erase(header_trimmed.begin());

        if (header_trimmed.size() < 7) return std::nullopt;

        bool match = true;
        for (size_t i = 0; i < 7; ++i) {
            if (std::tolower(static_cast<unsigned char>(header_trimmed[i])) !=
                std::tolower(static_cast<unsigned char>(prefix[i]))) {
                match = false;
                break;
            }
        }
        if (!match) return std::nullopt;

        auto token = header_trimmed.substr(7);
        // trim whitespace
        while (!token.empty() && std::isspace(token.front())) token.erase(token.begin());
        while (!token.empty() && std::isspace(token.back())) token.pop_back();
        if (token.empty()) return std::nullopt;

        return token;
    }

    // Verify RS256 signature using pre-loaded pkey_ directly via the EVP API.
    //
    // Why not jwt-cpp's built-in RS256 verifier?
    // jwt-cpp wraps the PEM string in a jwt-cpp::openssl::pem_reader that calls
    // PEM_read_bio_PUBKEY on every verify() call. On macOS Homebrew OpenSSL 3.x
    // we observed intermittent BIO_read failures ("no start line") inside that
    // helper, even with the same PEM that PEM_read_bio_PUBKEY accepts at process
    // startup. By loading the key once at construction (load_rsa_key) and using
    // the low-level EVP_Verify* API here, we:
    //   1. sidestep the broken jwt-cpp PEM hot-path entirely;
    //   2. catch malformed PEM at startup, not on the first request;
    //   3. avoid paying PEM parsing cost per request.
    //
    // Trade-off: we lose jwt-cpp's algorithm/claims registry, so do_verify
    // manually checks `alg` header, issuer, exp, and nbf.
    static bool verify_rs256(const std::string& data, const std::string& signature, EVP_PKEY* pkey) {
        auto ctx = EVP_MD_CTX_new();
        if (!ctx) return false;
        bool ok = false;
        if (EVP_VerifyInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
            EVP_VerifyUpdate(ctx, data.data(), data.size()) == 1 &&
            EVP_VerifyFinal(ctx, reinterpret_cast<const unsigned char*>(signature.data()),
                           static_cast<unsigned int>(signature.size()), pkey) == 1) {
            ok = true;
        }
        EVP_MD_CTX_free(ctx);
        return ok;
    }

    template <typename DecodedJwt>
    void verify_registered_claims(const DecodedJwt& decoded) const {
        constexpr auto leeway = std::chrono::seconds(60);
        auto now = std::chrono::system_clock::now();

        auto iss = decoded.get_issuer();
        if (iss != issuer_) {
            throw std::runtime_error("wrong issuer: expected " + issuer_ + ", got " + iss);
        }

        if (!decoded.has_expires_at()) {
            throw std::runtime_error("missing exp");
        }
        if (now > decoded.get_expires_at() + leeway) {
            throw std::runtime_error("token expired");
        }

        if (decoded.has_not_before() && now + leeway < decoded.get_not_before()) {
            throw std::runtime_error("token not active yet");
        }
    }

    // Actual JWT verification via jwt-cpp
    std::optional<JWTClaims> do_verify(const std::string& token) const {
        auto decoded = jwt::decode(token);

        if (algorithm_ == "HS256") {
            auto verifier = jwt::verify()
                .allow_algorithm(jwt::algorithm::hs256{secret_})
                .with_issuer(issuer_)
                .leeway(60);
            verifier.verify(decoded);
            // jwt-cpp treats exp as optional, so a token with no exp would be
            // accepted as permanently valid. Enforce exp explicitly, matching
            // the RS256 path (verify_registered_claims throws on missing exp).
            verify_registered_claims(decoded);
        } else if (algorithm_ == "RS256") {
            // Manual RS256 path — see verify_rs256() comment for the macOS
            // OpenSSL 3.x issue that forces us to bypass jwt-cpp here.
            auto header_b64 = decoded.get_header_base64();
            auto payload_b64 = decoded.get_payload_base64();
            auto sig = decoded.get_signature();
            auto msg = header_b64 + "." + payload_b64;
            if (!verify_rs256(msg, sig, pkey_.get())) {
                throw std::runtime_error("failed to verify signature");
            }
            // Check issuer and expiration manually (bypass jwt-cpp's algorithm registry)
            auto algo = decoded.get_algorithm();
            if (algo != "RS256") {
                throw std::runtime_error("wrong algorithm: expected RS256, got " + algo);
            }
            verify_registered_claims(decoded);
        }

        // extract claims
        JWTClaims claims;
        claims.raw_token = token;

        claims.subject = decoded.get_subject();

        // try extracting username or name claim
        if (decoded.has_payload_claim("username")) {
            claims.username = decoded.get_payload_claim("username").as_string();
        } else if (decoded.has_payload_claim("name")) {
            claims.username = decoded.get_payload_claim("name").as_string();
        }

        // extract roles
        if (decoded.has_payload_claim("roles")) {
            auto roles_array = decoded.get_payload_claim("roles").as_array();
            for (auto& r : roles_array) {
                if (r.is<std::string>()) {
                    claims.roles.push_back(r.get<std::string>());
                }
            }
        }
        if (decoded.has_payload_claim("role")) {
            claims.roles.push_back(
                decoded.get_payload_claim("role").as_string());
        }

        // extract exp / iat
        if (decoded.has_payload_claim("exp")) {
            claims.exp = decoded.get_payload_claim("exp").as_integer();
        }
        if (decoded.has_payload_claim("iat")) {
            claims.iat = decoded.get_payload_claim("iat").as_integer();
        }

        return claims;
    }
};
