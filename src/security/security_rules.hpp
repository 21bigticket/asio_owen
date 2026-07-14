#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <functional>
#include <fstream>
#include <sstream>
#include <asio.hpp>

#include "../common/config.hpp"
#include "../common/logger.hpp"
#include "path_normalize.hpp"
#include "real_ip.hpp"
#include "ip_blacklist.hpp"
#include "auth_whitelist.hpp"
#include "path_blacklist.hpp"
#include "jwt_auth.hpp"
#include "rate_limiter.hpp"

// Security rules: holds all security module instances, exposes a unified check() interface
class SecurityRules {
public:
    // Load all rules from Config
    void load_from_config(const Config& cfg) {
        // Build + validate the JWT config FIRST, into a local. This is the only
        // step that can throw (fail-closed on missing secret/key). Doing it
        // before mutating any member state keeps load_from_config atomic on
        // hot-reload: a bad new config throws here and leaves the existing
        // blacklists / whitelists / rate limits untouched, rather than applying
        // half of them and then aborting.
        bool jwt_disabled = false;
        auto new_jwt = build_jwt_auth(cfg, jwt_disabled);  // throws on invalid config

        // 0. Path case-sensitivity config
        {
            case_sensitive_paths_ = cfg.get("security", "case_sensitive_paths", "false") == "true";
        }

        // 1. IP blacklist
        {
            auto items = cfg.get_list("ip_blacklist");
            ip_blacklist_.reload(items);
        }

        // 2. Trusted proxies (pre-normalized at load time to avoid per-request normalize_ip_str calls)
        {
            auto items = cfg.get_list("trusted_proxies");
            std::vector<std::string> normalized;
            normalized.reserve(items.size());
            for (auto& ip : items) {
                normalized.push_back(normalize_ip_str(ip));
            }
            trusted_proxies_ = std::move(normalized);
        }

        // 3. Auth whitelist
        {
            auto items = cfg.get_list("auth_whitelist");
            normalize_path_items(items);
            auth_whitelist_.reload(items);
        }

        // 4. Path blacklist
        {
            auto items = cfg.get_section("path_blacklist");
            normalize_path_keys(items);
            path_blacklist_.reload(items);
        }

        // 5. JWT config (already validated/built above; just publish it)
        jwt_auth_ = std::move(new_jwt);
        if (jwt_disabled) {
            LOG_WARN("JWT verification explicitly disabled via security.jwt_disabled=true");
        }

        // 6. Rate limit config
        {
            RateLimiter::Config rate_cfg;
            rate_cfg.ip_rps = cfg.get_double("rate_limit", "ip_rps", 100.0);
            rate_cfg.ip_burst = cfg.get_double("rate_limit", "ip_burst", rate_cfg.ip_rps * 2);
            rate_cfg.global_rps = cfg.get_double("rate_limit", "global_rps", 50000.0);
            rate_cfg.max_buckets = static_cast<size_t>(cfg.get_int("rate_limit", "max_buckets", 100000));
            rate_cfg.snapshot_interval_sec = cfg.get_int("rate_limit", "snapshot_interval_sec", 30);
            rate_cfg.snapshot_path = cfg.get("rate_limit", "snapshot_path",
                "/var/lib/asio_owen/rate_limit.bin");

            // path rate limits
            auto path_items = cfg.get_section("rate_limit_paths");
            for (auto& [key, val] : path_items) {
                if (!normalize_path_key(key)) continue;
                auto rule = parse_rate_limit_value(val);
                if (key.back() == '/') {
                    rate_cfg.path_prefix_limits.emplace_back(key, rule);
                } else {
                    rate_cfg.path_limits[key] = rule;
                }
            }

            // service rate limits
            auto svc_items = cfg.get_section("rate_limit_services");
            for (auto& [key, val] : svc_items) {
                rate_cfg.service_limits[key] = parse_rate_limit_value(val);
            }

            if (rate_limiter_) {
                rate_limiter_->update_config(std::move(rate_cfg));
            } else {
                rate_limiter_ = std::make_shared<RateLimiter>(std::move(rate_cfg));
            }
        }

        LOG_INFO("Security rules loaded");
    }

    // Full-chain security check
    // Return value: status_code=0 means allow; non-zero means reject (with HTTP status code)
    struct CheckResult {
        int status_code = 0;
        std::string reason;
        int retry_after_ms = 0;  // only meaningful for 429 (rate limited)
    };

    CheckResult check(
        asio::ip::tcp::socket& socket,
        const std::string& method,
        const std::string& raw_path,
        const std::string& xff_header,
        const std::string& auth_header) const
    {
        // 0. OPTIONS always allowed (CORS preflight)
        if (method == "OPTIONS") {
            return {0, ""};
        }

        // Root path returns 404 directly, skip auth chain
        if (raw_path.empty() || raw_path == "/") {
            return {404, "not found"};
        }

        // Snapshot trusted_proxies_ and jwt_auth_ (fine-grained lock, does not block CPU-heavy operations)
        std::vector<std::string> proxies_copy;
        std::shared_ptr<const JWTAuth> jwt_copy;
        std::shared_ptr<RateLimiter> rate_limiter_copy;
        bool case_sensitive_copy = false;
        {
            std::lock_guard<std::mutex> lock(rules_mu_);
            proxies_copy = trusted_proxies_;
            jwt_copy = jwt_auth_;
            rate_limiter_copy = rate_limiter_;
            case_sensitive_copy = case_sensitive_paths_;
        }

        // 1. Extract real IP (uses proxies_copy, not holding rules_mu_)
        auto client_ip = get_client_ip(socket, xff_header, proxies_copy);
        auto normalized_ip_result = normalize_ip(client_ip);
        if (!normalized_ip_result.parse_ok) {
            return {400, "invalid client ip"};
        }
        auto& normalized_ip = normalized_ip_result.str;

        // 2. Path normalization (case_sensitive controls whether paths are lowercased)
        auto norm = normalize_path(raw_path, case_sensitive_copy);
        if (!norm.valid) {
            return {400, "invalid path"};
        }
        auto& path = norm.path;

        // 3. Extract service name
        auto service = extract_service(path);

        // 4. IP blacklist check
        if (ip_blacklist_.is_blocked(normalized_ip)) {
            return {403, "ip blocked"};
        }

        // 5. Rate limit check
        if (rate_limiter_copy) {
            auto decision = rate_limiter_copy->check_all(normalized_ip, path, service);
            if (!decision.allowed) {
                return {429, "too many requests", decision.retry_after_ms};
            }
        }

        // 6. Auth whitelist
        bool is_whitelisted = auth_whitelist_.is_whitelisted(path, service);

        // 7. JWT verification (non-whitelisted paths, using jwt_copy without lock)
        // If jwt_copy == nullptr, JWT is disabled — skip verification.
        std::optional<JWTClaims> claims;
        if (!is_whitelisted && jwt_copy) {
            claims = jwt_copy->verify(auth_header);
            if (!claims) {
                return {401, "invalid jwt"};
            }
        }

        // 8. Path blacklist (single lock for both blocked + role check)
        auto path_result = path_blacklist_.check(path);
        if (path_result.blocked) {
            return {403, "path blocked"};
        }
        // Role check: compare JWT claims roles with path requirements
        if (!path_result.required_role.empty()) {
            // Path requires a role but request is whitelisted (no JWT), reject
            if (is_whitelisted) {
                return {403, "role required"};
            }
            // Check if JWT claims contain the required role
            if (!claims || !has_role(*claims, path_result.required_role)) {
                return {403, "insufficient role"};
            }
        }

        return {0, ""};
    }

    // Get rate limiter reference (for snapshot timer)
    std::shared_ptr<RateLimiter> rate_limiter_snapshot() const {
        std::lock_guard<std::mutex> lock(rules_mu_);
        return rate_limiter_;
    }

    bool has_rate_limiter() const {
        return rate_limiter_snapshot() != nullptr;
    }

    // Hot reload: reload from Config
    void reload(const Config& cfg) {
        // Write lock: prevents check() from reading incomplete state
        std::lock_guard<std::mutex> lock(rules_mu_);
        try {
            load_from_config(cfg);
            LOG_INFO("Security rules hot-reloaded");
        } catch (const std::exception& e) {
            // load_from_config throws when the new config would disable auth
            // implicitly (missing jwt_secret / jwt_public_key). At startup that
            // aborts boot by design; on hot-reload we must NOT terminate a
            // running server — keep the previously-loaded rules and warn.
            LOG_ERROR("Security rules hot-reload rejected, keeping previous rules: ", e.what());
        }
    }

private:
    // Parse + validate the JWT section into a JWTAuth (or nullptr when auth is
    // explicitly disabled). Throws std::invalid_argument on a fail-open config
    // (missing secret/key without jwt_disabled). Pure w.r.t. member state, so
    // callers can build it before mutating anything.
    static std::shared_ptr<JWTAuth> build_jwt_auth(const Config& cfg, bool& jwt_disabled_out) {
        // Check jwt_disabled FIRST before any validation, so disabled mode can skip
        // secret/key requirements entirely (fixes test failures where jwt_disabled=true
        // but algorithm defaults to HS256 without secret).
        bool jwt_disabled = cfg.get_bool("security", "jwt_disabled", false);
        jwt_disabled_out = jwt_disabled;
        if (jwt_disabled) {
            return nullptr;
        }

        auto secret = cfg.get("security", "jwt_secret", "");
        auto issuer = cfg.get("security", "jwt_issuer", "asio_owen");
        auto configured_algorithm = cfg.get("security", "jwt_algorithm", "");
        auto algorithm = configured_algorithm.empty() ? "HS256" : configured_algorithm;
        // Disabling authentication must be an explicit, auditable decision.
        // Previously a missing jwt_secret / jwt_public_key silently disabled JWT
        // for the whole server (fail-OPEN). Now the only way to run without JWT
        // is security.jwt_disabled=true.
        auto pub_key = cfg.get("security", "jwt_public_key", "");
        // Try to load public key from file if it's not already a PEM string
        if (!pub_key.empty() && pub_key.find("-----BEGIN") == std::string::npos) {
            std::ifstream key_file(pub_key);
            if (key_file.is_open()) {
                std::stringstream buf;
                buf << key_file.rdbuf();
                auto loaded = buf.str();
                if (loaded.find("-----BEGIN") != std::string::npos) {
                    pub_key = loaded;
                }
            } else {
                LOG_WARN("JWT public key file not found: ", pub_key);
                pub_key.clear();
            }
        }
        if (pub_key.empty() && algorithm == "RS256") {
            // RS256 without explicit pub_key: build from JWKS n/e params
            auto n = cfg.get("security", "jwt_rsa_n", "");
            auto e = cfg.get("security", "jwt_rsa_e", "");
            if (!n.empty() && !e.empty()) {
                pub_key = detail::build_rsa_pubkey_from_jwks(n, e);
                if (pub_key.empty()) {
                    LOG_WARN("JWT: failed to build RSA public key from jwks params");
                } else {
                    LOG_INFO("JWT: built RSA public key from jwks params, len=", pub_key.size());
                    LOG_DEBUG("JWT PEM:\n", pub_key);
                }
            }
        }
        if (algorithm == "HS256" && secret.empty()) {
            // fail-closed: refuse to start rather than silently allow all traffic
            throw std::invalid_argument(
                "JWT HS256 requires jwt_secret; set security.jwt_disabled=true to run without auth");
        }
        if (algorithm == "RS256" && pub_key.empty()) {
            throw std::invalid_argument(
                "JWT RS256 requires jwt_public_key or jwks params; "
                "set security.jwt_disabled=true to run without auth");
        }
        return std::make_shared<JWTAuth>(secret, issuer, algorithm, pub_key);
    }

    mutable std::mutex rules_mu_;  // protects shared rule pointers and snapshot fields
    IpBlacklist ip_blacklist_;
    AuthWhitelist auth_whitelist_;
    PathBlacklist path_blacklist_;
    std::shared_ptr<JWTAuth> jwt_auth_;
    std::shared_ptr<RateLimiter> rate_limiter_;
    std::vector<std::string> trusted_proxies_;
    bool case_sensitive_paths_ = false;

    // Check if JWT claims contain the specified role
    static bool has_role(const JWTClaims& claims, const std::string& role) {
        for (auto& r : claims.roles) {
            if (r == role) return true;
        }
        return false;
    }

    // Extract service name from path: /{service}/... -> service
    // Validation: [a-z][a-z0-9-]*, returns empty on mismatch (caller should return 400)
    static std::string extract_service(const std::string& path) {
        if (path.empty() || path[0] != '/') return {};
        auto second = path.find('/', 1);
        if (second == std::string::npos) {
            auto svc = path.substr(1);
            if (!is_valid_service_name(svc)) return {};
            return svc;
        }
        auto svc = path.substr(1, second - 1);
        if (!is_valid_service_name(svc)) return {};
        return svc;
    }

    // Service name validation: [a-z][a-z0-9-]*
    static bool is_valid_service_name(const std::string& name) {
        if (name.empty()) return false;
        if (!(name[0] >= 'a' && name[0] <= 'z')) return false;
        for (size_t i = 1; i < name.size(); ++i) {
            auto c = name[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
                return false;
            }
        }
        return true;
    }

    // Parse rate limit value: supports "limit=10;window=60s;burst=10" or bare "10"
    // window parameter: value is per-window, auto-convert to per-second rate
    // example: limit=10;window=60s -> rate = 10/60 = 0.167 req/s
    static RateLimitRule parse_rate_limit_value(const std::string& val) {
        RateLimitRule rule{100.0, 100.0};  // default

        if (val.find('=') == std::string::npos) {
            // bare number: rate = val, burst = val
            try {
                rule.rate = std::stod(val);
                rule.burst = rule.rate;
            } catch (...) {}
            return rule;
        }

        // key=value;key=value format
        double window_sec = 1.0;  // default 1s window
        double limit = 0.0;
        bool has_limit = false;

        size_t pos = 0;
        while (pos < val.size()) {
            auto semi = val.find(';', pos);
            auto part = (semi == std::string::npos) ? val.substr(pos) : val.substr(pos, semi - pos);
            auto eq = part.find('=');
            if (eq != std::string::npos) {
                auto k = part.substr(0, eq);
                auto v = part.substr(eq + 1);
                // trim
                while (!k.empty() && std::isspace(k.front())) k.erase(k.begin());
                while (!k.empty() && std::isspace(k.back())) k.pop_back();
                while (!v.empty() && std::isspace(v.front())) v.erase(v.begin());
                while (!v.empty() && std::isspace(v.back())) v.pop_back();

                try {
                    if (k == "limit") {
                        limit = std::stod(v);
                        has_limit = true;
                    } else if (k == "burst") {
                        rule.burst = std::stod(v);
                    } else if (k == "window") {
                        // support "60s", "1m", "1h" suffixes
                        auto val_str = v;
                        double multiplier = 1.0;
                        if (!val_str.empty()) {
                            char suffix = val_str.back();
                            if (suffix == 's') {
                                multiplier = 1.0;
                                val_str.pop_back();
                            } else if (suffix == 'm') {
                                multiplier = 60.0;
                                val_str.pop_back();
                            } else if (suffix == 'h') {
                                multiplier = 3600.0;
                                val_str.pop_back();
                            }
                        }
                        window_sec = std::stod(val_str) * multiplier;
                    }
                } catch (...) {}
            }
            pos = (semi == std::string::npos) ? val.size() : semi + 1;
        }

        // convert per-window limit to per-second rate
        if (has_limit) {
            rule.rate = limit / window_sec;
            if (rule.burst == 100.0) rule.burst = rule.rate;  // default burst = rate
        }
        return rule;
    }

    void normalize_path_items(std::vector<std::string>& items) const {
        for (auto& item : items) {
            if (!item.empty() && item.front() == '/') {
                normalize_path_key(item);
            } else if (!case_sensitive_paths_) {
                std::transform(item.begin(), item.end(), item.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            }
        }
    }

    void normalize_path_keys(std::vector<std::pair<std::string, std::string>>& items) const {
        for (auto& [key, _] : items) {
            normalize_path_key(key);
        }
    }

    bool normalize_path_key(std::string& key) const {
        if (key.empty() || key.front() != '/') {
            return true;
        }
        bool prefix = key.size() > 1 && key.back() == '/';
        auto norm = normalize_path(key, case_sensitive_paths_);
        if (!norm.valid) {
            LOG_WARN("Ignoring invalid security path rule: ", key, ", reason=", norm.error);
            key.clear();
            return false;
        }
        key = std::move(norm.path);
        if (prefix && key.back() != '/') key.push_back('/');
        return true;
    }
};
