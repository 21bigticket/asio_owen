#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <functional>
#include <fstream>
#include <mutex>
#include <optional>
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
#include "principal.hpp"
#include "../http/cors.hpp"

// Security rules: holds all security module instances, exposes a unified check() interface
class SecurityRules {
private:
    struct SecuritySnapshot;

public:
    class PreparedReload {
    public:
        PreparedReload(PreparedReload&&) = default;
        PreparedReload& operator=(PreparedReload&&) = default;
        PreparedReload(const PreparedReload&) = delete;
        PreparedReload& operator=(const PreparedReload&) = delete;

    private:
        friend class SecurityRules;
        PreparedReload() = default;
        std::shared_ptr<const SecuritySnapshot> snapshot_;
        std::optional<RateLimiter::Config> rate_limiter_config_;
    };

    SecurityRules() = default;

    static void validate_config_for_staging(const Config& cfg) {
        SecurityRules staged(true);
        staged.load_from_config(cfg);
    }

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

        auto next = std::make_shared<SecuritySnapshot>();
        next->case_sensitive_paths = cfg.get("security", "case_sensitive_paths", "false") == "true";

        // 1. IP blacklist
        {
            auto items = cfg.get_list("ip_blacklist");
            next->ip_blacklist = std::make_shared<IpBlacklist>();
            next->ip_blacklist->reload(items);
        }

        // 2. Trusted proxies (pre-normalized at load time to avoid per-request normalize_ip_str calls)
        {
            auto items = cfg.get_list("trusted_proxies");
            std::vector<std::string> normalized;
            normalized.reserve(items.size());
            for (auto& ip : items) {
                normalized.push_back(normalize_ip_str(ip));
            }
            next->trusted_proxies = std::move(normalized);
        }

        // 3. Auth whitelist
        {
            auto items = cfg.get_list("auth_whitelist");
            normalize_path_items(items, next->case_sensitive_paths);
            next->auth_whitelist = std::make_shared<AuthWhitelist>();
            next->auth_whitelist->reload(items);
        }

        // 4. Path blacklist
        {
            auto items = cfg.get_section("path_blacklist");
            normalize_path_keys(items, next->case_sensitive_paths);
            next->path_blacklist = std::make_shared<PathBlacklist>();
            next->path_blacklist->reload(items);
        }

        // 5. JWT config (already validated/built above; just publish it)
        next->jwt_auth = std::move(new_jwt);
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
                if (!normalize_path_key(key, next->case_sensitive_paths)) continue;
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

            next->rate_limiter = std::make_shared<RateLimiter>(std::move(rate_cfg),
                !staging_, !staging_);
            next->rate_policy = std::make_shared<RateLimiter::Config>(
                next->rate_limiter->config_snapshot());
        }

        // 7. CORS policy (absent/empty [cors] section -> disabled default)
        next->cors_policy = std::make_shared<const CorsPolicy>(load_cors_policy(cfg));

        {
            std::lock_guard<std::mutex> lock(snapshot_mu_);
            snapshot_ = std::shared_ptr<const SecuritySnapshot>(std::move(next));
            generation_.store(next_generation(), std::memory_order_release);
        }

        LOG_INFO("Security rules loaded");
    }

    // Generation of the currently published rules, incremented on every
    // load_from_config/publish_reload. Client sessions record it at check time
    // and re-check when it moved before routing, so one request never mixes
    // an old security generation with newly published routes.
    uint64_t generation() const {
        return generation_.load(std::memory_order_acquire);
    }

    // Full-chain security check
    // Return value: status_code=0 means allow; non-zero means reject (with HTTP status code)
    struct CheckResult {
        int status_code = 0;
        std::string reason;
        int retry_after_ms = 0;  // only meaningful for 429 (rate limited)
        std::optional<Principal> principal;
        std::string client_ip;
        bool jwt_disabled = false;
    };

    struct RequestCheckResult {
        CheckResult security;
        std::shared_ptr<const CorsPolicy> cors_policy;
        uint64_t generation = 0;
    };

    CheckResult check(
        asio::ip::tcp::socket& socket,
        const std::string& method,
        const std::string& raw_path, // NOLINT(bugprone-easily-swappable-parameters)
        const std::string& xff_header,
        const std::string& auth_header) const
    {
        auto view = snapshot_view_fast();
        return check_snapshot(
            socket, method, raw_path, xff_header, auth_header, view.snapshot);
    }

    // The security decision, CORS policy, and generation must come from the
    // same immutable snapshot. ClientSession retains the policy across awaits
    // and checks the generation again before accepting a dynamic route.
    RequestCheckResult check_request(
        asio::ip::tcp::socket& socket,
        const std::string& method,
        const std::string& raw_path,
        const std::string& xff_header,
        const std::string& auth_header) const
    {
        auto view = snapshot_view_fast();
        RequestCheckResult result;
        result.generation = view.generation;
        if (view.snapshot && view.snapshot->cors_policy &&
            view.snapshot->cors_policy->enabled) {
            result.cors_policy = view.snapshot->cors_policy;
        }
        result.security = check_snapshot(
            socket, method, raw_path, xff_header, auth_header, view.snapshot);
        return result;
    }

private:
    // Path and forwarding/auth headers are distinct request fields.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    CheckResult check_snapshot(
        asio::ip::tcp::socket& socket,
        const std::string& method,
        const std::string& raw_path,
        const std::string& xff_header,
        const std::string& auth_header,
        const SecuritySnapshot* snapshot) const
    {
        // Root path returns 404 directly, skip auth chain
        if (raw_path.empty() || raw_path == "/") {
            return {404, "not found", 0, std::nullopt, "", false};
        }

        if (!snapshot) return {500, "security rules unavailable", 0, std::nullopt, "", false};

        // 1. Extract real IP from the immutable snapshot.
        auto client_ip = get_client_ip(socket, xff_header, snapshot->trusted_proxies);
        auto normalized_ip_result = normalize_ip(client_ip);
        if (!normalized_ip_result.parse_ok) {
            return {400, "invalid client ip", 0, std::nullopt, "", false};
        }
        auto& normalized_ip = normalized_ip_result.str;
        CheckResult result;
        result.client_ip = normalized_ip;
        result.jwt_disabled = !snapshot->jwt_auth;

        // 2. Path normalization (case_sensitive controls whether paths are lowercased)
        auto norm = normalize_path(raw_path, snapshot->case_sensitive_paths);
        if (!norm.valid) {
            return {400, "invalid path", 0, std::nullopt, "", false};
        }
        auto& path = norm.path;

        // 3. Extract service name
        auto service = extract_service(path);

        // 4. IP blacklist check
        if (snapshot->ip_blacklist->is_blocked(normalized_ip)) {
            result.status_code = 403;
            result.reason = "ip blocked";
            return result;
        }

        // 5. Rate limit check
        if (snapshot->rate_limiter && snapshot->rate_policy) {
            auto decision = snapshot->rate_limiter->check_all(normalized_ip, path, service, *snapshot->rate_policy);
            if (!decision.allowed) {
                result.status_code = 429;
                result.reason = "too many requests";
                result.retry_after_ms = decision.retry_after_ms;
                return result;
            }
        }

        // 5.5 CORS preflight bypasses authentication (browsers send preflight
        // without Authorization), but only AFTER the IP blacklist + rate limit
        // above, so OPTIONS cannot be used to slip past those controls.
        if (method == "OPTIONS") {
            return {0, "", 0, std::nullopt, "", false};
        }

        // 6. Auth whitelist
        bool is_whitelisted = snapshot->auth_whitelist->is_whitelisted(path, service);

        // 7. JWT verification (non-whitelisted paths, using jwt_copy without lock)
        // If jwt_copy == nullptr, JWT is disabled — skip verification.
        std::optional<JWTClaims> claims;
        if (!is_whitelisted && snapshot->jwt_auth) {
            claims = snapshot->jwt_auth->verify(auth_header);
            if (!claims) {
                result.status_code = 401;
                result.reason = "invalid jwt";
                return result;
            }
            result.principal = Principal{
                .subject = claims->subject,
                .username = claims->username,
                .roles = claims->roles
            };
        }

        // 8. Path blacklist (single lock for both blocked + role check)
        auto path_result = snapshot->path_blacklist->check(path);
        if (path_result.blocked) {
            result.status_code = 403;
            result.reason = "path blocked";
            return result;
        }
        // Role check: compare JWT claims roles with path requirements
        if (!path_result.required_role.empty()) {
            // Path requires a role but request is whitelisted (no JWT), reject
            if (is_whitelisted) {
                result.status_code = 403;
                result.reason = "role required";
                return result;
            }
            // Check if JWT claims contain the required role
            if (!claims || !has_role(*claims, path_result.required_role)) {
                result.status_code = 403;
                result.reason = "insufficient role";
                return result;
            }
        }

        return result;
    }
    // NOLINTEND(bugprone-easily-swappable-parameters)

public:
    // Get rate limiter reference (for snapshot timer)
    std::shared_ptr<RateLimiter> rate_limiter_snapshot() const {
        auto snapshot = snapshot_copy();
        return snapshot ? snapshot->rate_limiter : nullptr;
    }

    bool has_rate_limiter() const {
        return rate_limiter_snapshot() != nullptr;
    }

    // Most requests run with CORS disabled. Keep that check on the TLS-backed
    // snapshot path so it does not reintroduce per-request locking.
    bool cors_enabled_fast() const {
        auto snapshot = snapshot_fast();
        return snapshot && snapshot->cors_policy && snapshot->cors_policy->enabled;
    }

    // Retain the policy across coroutine suspension when CORS is enabled.
    // snapshot_fast() keeps the source snapshot alive while this shared_ptr is copied.
    std::shared_ptr<const CorsPolicy> cors_policy() const {
        auto snapshot = snapshot_fast();
        return snapshot ? snapshot->cors_policy : nullptr;
    }

    // Hot reload: reload from Config
    PreparedReload prepare_reload(const Config& cfg) {
        SecurityRules next(true);
        next.load_from_config(cfg);

        PreparedReload prepared;
        prepared.snapshot_ = next.snapshot_copy();
        auto current_snapshot = snapshot_copy();
        if (current_snapshot && current_snapshot->rate_limiter &&
            prepared.snapshot_->rate_policy) {
            auto mutable_next = std::make_shared<SecuritySnapshot>(*prepared.snapshot_);
            mutable_next->rate_limiter = current_snapshot->rate_limiter;
            prepared.rate_limiter_config_.emplace(*prepared.snapshot_->rate_policy);
            prepared.snapshot_ = std::move(mutable_next);
        }
        return prepared;
    }

    void publish_reload(PreparedReload prepared) {
        {
            std::lock_guard<std::mutex> lock(snapshot_mu_);
            if (prepared.rate_limiter_config_ && prepared.snapshot_->rate_limiter) {
                prepared.snapshot_->rate_limiter->publish_config(
                    std::move(*prepared.rate_limiter_config_));
            }
            snapshot_ = std::move(prepared.snapshot_);
            generation_.store(next_generation(), std::memory_order_release);
        }
        try {
            LOG_INFO("Security rules hot-reloaded");
        } catch (...) {
        }
    }

    bool reload(const Config& cfg) {
        try {
            publish_reload(prepare_reload(cfg));
            return true;
        } catch (const std::exception& e) {
            // load_from_config throws when the new config would disable auth
            // implicitly (missing jwt_secret / jwt_public_key). At startup that
            // aborts boot by design; on hot-reload we must NOT terminate a
            // running server — keep the previously-loaded rules and warn.
            try {
                LOG_ERROR("Security rules hot-reload rejected, keeping previous rules: ", e.what());
            } catch (...) {
            }
            return false;
        } catch (...) {
            try {
                LOG_ERROR("Security rules hot-reload rejected by an unknown exception, keeping previous rules");
            } catch (...) {
            }
            return false;
        }
    }

private:
    struct SecuritySnapshot {
        bool case_sensitive_paths = false;
        std::vector<std::string> trusted_proxies;
        std::shared_ptr<IpBlacklist> ip_blacklist = std::make_shared<IpBlacklist>();
        std::shared_ptr<AuthWhitelist> auth_whitelist = std::make_shared<AuthWhitelist>();
        std::shared_ptr<PathBlacklist> path_blacklist = std::make_shared<PathBlacklist>();
        std::shared_ptr<JWTAuth> jwt_auth;
        std::shared_ptr<RateLimiter> rate_limiter;
        std::shared_ptr<const RateLimiter::Config> rate_policy;
        std::shared_ptr<const CorsPolicy> cors_policy = std::make_shared<const CorsPolicy>();
    };

    explicit SecurityRules(bool staging) : staging_(staging) {}

    // Global monotonic generation counter for snapshot versioning.
    // Both load_from_config() and reload() call this to ensure unique generations.
    static uint64_t next_generation() {
        static std::atomic<uint64_t> counter{1};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

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

    struct SnapshotView {
        const SecuritySnapshot* snapshot = nullptr;
        uint64_t generation = 0;
    };

    SnapshotView snapshot_view_fast() const {
        // TLS cache eliminates per-request refcount contention.
        // Key: (owner pointer, generation). Each snapshot publish gets unique generation.
        struct TLSCache {
            const SecurityRules* owner = nullptr;
            uint64_t generation = 0;
            std::shared_ptr<const SecuritySnapshot> holder;
        };
        thread_local TLSCache tls_cache;

        auto current_gen = generation_.load(std::memory_order_acquire);
        if (tls_cache.owner != this || tls_cache.generation != current_gen) {
            std::lock_guard<std::mutex> lock(snapshot_mu_);
            tls_cache.holder = snapshot_;
            tls_cache.owner = this;
            // Re-read while holding the publication lock. A publisher may have
            // advanced between the optimistic load above and this lock.
            tls_cache.generation = generation_.load(std::memory_order_relaxed);
        }
        return {tls_cache.holder.get(), tls_cache.generation};
    }

    const SecuritySnapshot* snapshot_fast() const {
        return snapshot_view_fast().snapshot;
    }

    std::shared_ptr<const SecuritySnapshot> snapshot_copy() const {
        std::lock_guard<std::mutex> lock(snapshot_mu_);
        return snapshot_;
    }

    mutable std::mutex snapshot_mu_;
    std::shared_ptr<const SecuritySnapshot> snapshot_;
    std::atomic<uint64_t> generation_{0};
    bool staging_ = false;

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

    static void normalize_path_items(std::vector<std::string>& items, bool case_sensitive_paths) {
        for (auto& item : items) {
            if (!item.empty() && item.front() == '/') {
                normalize_path_key(item, case_sensitive_paths);
            } else if (!case_sensitive_paths) {
                std::transform(item.begin(), item.end(), item.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            }
        }
    }

    static void normalize_path_keys(std::vector<std::pair<std::string, std::string>>& items,
                                    bool case_sensitive_paths) {
        for (auto& [key, _] : items) {
            normalize_path_key(key, case_sensitive_paths);
        }
    }

    static bool normalize_path_key(std::string& key, bool case_sensitive_paths) {
        if (key.empty() || key.front() != '/') {
            return true;
        }
        bool prefix = key.size() > 1 && key.back() == '/';
        auto norm = normalize_path(key, case_sensitive_paths);
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
