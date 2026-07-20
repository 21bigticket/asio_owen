#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../common/config.hpp"
#include "http_context.hpp"
#include "http_protocol.hpp"  // header_iequals, trim_copy

// CORS (cross-origin) policy. Disabled by default: when enabled == false the
// gateway adds no Access-Control-* headers and does not answer preflight
// requests locally, so the browser same-origin policy applies (industry default).
struct CorsPolicy {
    bool enabled = false;                             // master switch; false = default mode
    bool allow_all_origins = false;                   // configured as "*"
    std::unordered_set<std::string> allowed_origins;  // exact-match whitelist
    std::string allowed_methods = "GET, POST, PUT, DELETE, OPTIONS";
    std::string allowed_headers = "Content-Type, Authorization";
    std::string expose_headers;                       // may be empty
    bool allow_credentials = false;
    int max_age = 600;

    // Returns the value to write into Access-Control-Allow-Origin, or nullopt
    // when the request is not an allowed cross-origin request.
    std::optional<std::string> resolve_origin(const std::string& origin) const {
        if (!enabled || origin.empty()) return std::nullopt;
        if (allow_all_origins) {
            // With credentials the wildcard is illegal; echo the concrete Origin.
            return allow_credentials ? origin : std::string("*");
        }
        if (allowed_origins.count(origin)) return origin;
        return std::nullopt;
    }
};

// Parse the [cors] section. An absent section (or enabled=false) yields a
// disabled policy, so "no config" == default mode.
inline CorsPolicy load_cors_policy(const Config& cfg) {
    CorsPolicy policy;
    policy.enabled = cfg.get_bool("cors", "enabled", false);
    if (!policy.enabled) return policy;

    auto origins = cfg.get("cors", "allowed_origins", "");
    std::string_view view(origins);
    size_t start = 0;
    while (start <= view.size()) {
        auto comma = view.find(',', start);
        auto end = comma == std::string_view::npos ? view.size() : comma;
        auto token = trim_copy(view.substr(start, end - start));
        if (!token.empty()) {
            if (token == "*") policy.allow_all_origins = true;
            else policy.allowed_origins.insert(std::move(token));
        }
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }

    policy.allowed_methods = cfg.get("cors", "allowed_methods", policy.allowed_methods);
    policy.allowed_headers = cfg.get("cors", "allowed_headers", policy.allowed_headers);
    policy.expose_headers = cfg.get("cors", "expose_headers", "");
    policy.allow_credentials = cfg.get_bool("cors", "allow_credentials", false);
    policy.max_age = cfg.get_int("cors", "max_age", 600);

    // Guardrail against a well-known dangerous misconfiguration: reflecting any
    // origin (allowed_origins=*) together with credentials lets ANY site call
    // the API with the user's cookies/Authorization. Force credentials off for
    // the wildcard case rather than silently honoring it. Exact-origin
    // whitelists with credentials remain allowed (that is the intended use).
    if (policy.allow_all_origins && policy.allow_credentials) {
        LOG_WARN("cors: allowed_origins=* with allow_credentials=true is unsafe; "
                 "disabling credentials for wildcard origin");
        policy.allow_credentials = false;
    }

    return policy;
}

// Remove any Access-Control-* response headers so the gateway is the single
// source of truth when it manages CORS. Vary is intentionally left alone
// (upstream may vary on Accept-Encoding etc.; multiple Vary headers combine).
inline void strip_cors_headers(
    std::vector<std::pair<std::string, std::string>>& headers) {
    headers.erase(
        std::remove_if(headers.begin(), headers.end(),
            [](const std::pair<std::string, std::string>& kv) {
                const std::string& n = kv.first;
                return header_iequals(n, "access-control-allow-origin") ||
                    header_iequals(n, "access-control-allow-credentials") ||
                    header_iequals(n, "access-control-allow-methods") ||
                    header_iequals(n, "access-control-allow-headers") ||
                    header_iequals(n, "access-control-expose-headers") ||
                    header_iequals(n, "access-control-max-age");
            }),
        headers.end());
}

// Inject CORS headers onto an actual (non-preflight) response. No-op when the
// policy is disabled, so the default mode leaves the response untouched.
inline void apply_cors_headers(HttpContext& ctx, const CorsPolicy& policy) {
    if (!policy.enabled) return;
    // When the gateway owns CORS, strip upstream CORS headers first to avoid
    // duplicates and to prevent leaking an origin we did not whitelist.
    strip_cors_headers(ctx.response_headers);
    auto allow = policy.resolve_origin(ctx.get_header("Origin"));
    if (!allow) return;
    ctx.response_headers.emplace_back("Access-Control-Allow-Origin", *allow);
    // Echoing a concrete origin requires Vary: Origin so shared caches don't
    // serve one site's response to another.
    if (*allow != "*")
        ctx.response_headers.emplace_back("Vary", "Origin");
    if (policy.allow_credentials)
        ctx.response_headers.emplace_back("Access-Control-Allow-Credentials", "true");
    if (!policy.expose_headers.empty())
        ctx.response_headers.emplace_back("Access-Control-Expose-Headers", policy.expose_headers);
}

// Build a 204 preflight response in-place. Returns false (without touching ctx)
// when the policy is disabled or the Origin is not allowed, so the caller can
// fall through to the normal request path instead of faking a success.
inline bool build_preflight_response(HttpContext& ctx, const CorsPolicy& policy) {
    if (!policy.enabled) return false;
    auto allow = policy.resolve_origin(ctx.get_header("Origin"));
    if (!allow) return false;
    ctx.status_code = 204;
    ctx.response_status_text = "No Content";
    ctx.response_body.clear();
    ctx.response_headers.clear();
    ctx.response_headers.emplace_back("Access-Control-Allow-Origin", *allow);
    if (*allow != "*")
        ctx.response_headers.emplace_back("Vary", "Origin");
    ctx.response_headers.emplace_back("Access-Control-Allow-Methods", policy.allowed_methods);
    // Echoing the browser's requested headers is more permissive than a fixed
    // list; fall back to the configured default when none were requested.
    auto req_h = ctx.get_header("Access-Control-Request-Headers");
    ctx.response_headers.emplace_back(
        "Access-Control-Allow-Headers",
        req_h.empty() ? policy.allowed_headers : req_h);
    if (policy.allow_credentials)
        ctx.response_headers.emplace_back("Access-Control-Allow-Credentials", "true");
    ctx.response_headers.emplace_back("Access-Control-Max-Age", std::to_string(policy.max_age));
    return true;
}
