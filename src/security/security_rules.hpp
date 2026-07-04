#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <functional>
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
            auth_whitelist_.reload(items);
        }

        // 4. Path blacklist
        {
            auto items = cfg.get_section("path_blacklist");
            path_blacklist_.reload(items);
        }

        // 5. JWT config
        {
            auto secret = cfg.get("security", "jwt_secret", "");
            auto issuer = cfg.get("security", "jwt_issuer", "asio_owen");
            auto algorithm = cfg.get("security", "jwt_algorithm", "HS256");
            if (!secret.empty()) {
                jwt_auth_ = std::make_shared<JWTAuth>(secret, issuer, algorithm);
            } else {
                jwt_auth_.reset();
                LOG_WARN("JWT secret not configured, JWT verification disabled");
            }
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
                rate_limiter_ = std::make_unique<RateLimiter>(std::move(rate_cfg));
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
        return check(socket, method, raw_path, xff_header, auth_header, case_sensitive_paths_);
    }

    // Full-chain security check with case_sensitive flag
    CheckResult check(
        asio::ip::tcp::socket& socket,
        const std::string& method,
        const std::string& raw_path,
        const std::string& xff_header,
        const std::string& auth_header,
        bool case_sensitive) const
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
        {
            std::lock_guard<std::mutex> lock(rules_mu_);
            proxies_copy = trusted_proxies_;
            jwt_copy = jwt_auth_;
        }

        // 1. Extract real IP (uses proxies_copy, not holding rules_mu_)
        auto client_ip = get_client_ip(socket, xff_header, proxies_copy);
        auto normalized_ip = normalize_ip_str(client_ip);

        // 2. Path normalization (case_sensitive controls whether paths are lowercased)
        auto norm = normalize_path(raw_path, case_sensitive);
        auto& path = norm.path;

        // 3. Extract service name
        auto service = extract_service(path);

        // 4. IP blacklist check
        if (ip_blacklist_.is_blocked(normalized_ip)) {
            return {403, "ip blocked"};
        }

        // 5. Rate limit check
        if (rate_limiter_) {
            auto decision = rate_limiter_->check_all(normalized_ip, path, service);
            if (!decision.allowed) {
                return {429, "too many requests", decision.retry_after_ms};
            }
        }

        // 6. Auth whitelist
        bool is_whitelisted = auth_whitelist_.is_whitelisted(path, service);

        // 7. JWT verification (non-whitelisted paths, using jwt_copy without lock)
        std::optional<JWTClaims> claims;
        if (!is_whitelisted) {
            if (!jwt_copy) {
                return {401, "jwt not configured"};
            }
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
    RateLimiter& rate_limiter() { return *rate_limiter_; }
    bool has_rate_limiter() const { return rate_limiter_ != nullptr; }

    // Hot reload: reload from Config
    void reload(const Config& cfg) {
        // Write lock: prevents check() from reading incomplete state
        std::lock_guard<std::mutex> lock(rules_mu_);
        load_from_config(cfg);
        LOG_INFO("Security rules hot-reloaded");
    }

private:
    mutable std::mutex rules_mu_;  // protects trusted_proxies_ and jwt_auth_ concurrent read/write
    IpBlacklist ip_blacklist_;
    AuthWhitelist auth_whitelist_;
    PathBlacklist path_blacklist_;
    std::shared_ptr<JWTAuth> jwt_auth_;
    std::unique_ptr<RateLimiter> rate_limiter_;
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
};
