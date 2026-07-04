#pragma once
#include <string>
#include <string_view>
#include <optional>
#include <memory>
#include <vector>
#include <cstdint>

#include <jwt-cpp/jwt.h>
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

// JWT verifier (using jwt-cpp header-only library)
// Requires: OpenSSL::SSL + OpenSSL::Crypto
class JWTAuth {
public:
    // Currently only supports HS256, validated at construction
    JWTAuth(std::string secret, std::string issuer, std::string algorithm)
        : secret_(std::move(secret))
        , issuer_(std::move(issuer))
    {
        if (algorithm != "HS256") {
            throw std::runtime_error("JWT: unsupported algorithm " + algorithm
                + ", only HS256 is supported");
        }
        if (secret_.size() < 32) {
            LOG_WARN("JWT secret is less than 32 bytes, HS256 recommended minimum");
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

    // Actual JWT verification via jwt-cpp
    std::optional<JWTClaims> do_verify(const std::string& token) const {
        auto decoded = jwt::decode(token);

        // Explicit algorithm verification (only HS256, rejecting alg: none attacks)
        auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::hs256{secret_})
            .with_issuer(issuer_)
            .leeway(60);  // +/-60s clock skew tolerance

        verifier.verify(decoded);

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
