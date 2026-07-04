#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <list>
#include <mutex>
#include <atomic>
#include <memory>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <cstring>

#include "../common/logger.hpp"

// ========== Rate limit decision ==========

struct Decision {
    bool allowed;
    int retry_after_ms;  // only meaningful when !allowed; 0 when allowed
};

// ========== Current time in milliseconds ==========

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

// ========== Token bucket ==========

struct TokenBucket {
    int64_t last_refill_ms = 0;
    double tokens = 0.0;  // current tokens, double for ms-level refill precision
};

inline Decision allow(TokenBucket& b, double rate, double burst) {
    if (rate <= 0.0) return {true, 0};  // unlimited / misconfigured
    auto now = now_ms();
    auto elapsed = now - b.last_refill_ms;
    b.last_refill_ms = now;
    // refill tokens, capped at burst
    b.tokens = std::min(burst, b.tokens + elapsed * rate / 1000.0);
    if (b.tokens >= 1.0) {
        b.tokens -= 1.0;
        return {true, 0};
    }
    int retry_after = static_cast<int>(std::ceil((1.0 - b.tokens) / rate * 1000));
    return {false, retry_after};
}

// ========== Global rate limit: single atomic CAS token bucket ==========

struct GlobalBucket {
    std::atomic<int64_t> tokens_milli{0};    // millitokens, 1 request = 1000 millitokens
    std::atomic<int64_t> last_refill_ms{0};
};

inline Decision check_global(GlobalBucket& global, double rate, double burst) {
    if (rate <= 0.0) return {true, 0};  // unlimited
    int64_t now = now_ms();
    int64_t last = global.last_refill_ms.load(std::memory_order_relaxed);
    int64_t elapsed = now - last;

    // CAS refill: only one thread advances time and refills tokens
    if (elapsed > 0 &&
        global.last_refill_ms.compare_exchange_strong(last, now)) {
        int64_t add_milli = static_cast<int64_t>(elapsed * rate);
        int64_t old_t = global.tokens_milli.load(std::memory_order_relaxed);
        int64_t burst_milli = static_cast<int64_t>(burst * 1000);
        do {
            int64_t new_t = std::min(burst_milli, old_t + add_milli);
            if (global.tokens_milli.compare_exchange_weak(
                    old_t, new_t, std::memory_order_relaxed))
                break;
        } while (true);
    }

    int64_t before = global.tokens_milli.fetch_sub(1000, std::memory_order_relaxed);
    if (before >= 1000) return {true, 0};
    // Refund: rejected requests should not consume tokens.
    // Unlike per-shard buckets (which are mutex-protected and refilled immediately),
    // the global bucket is atomic and refills slowly at `rate` per second.
    // Without refund, 1000 concurrent rejections would drain tokens_milli to -1,000,000,
    // taking ~20 seconds to recover at 50k RPS.
    global.tokens_milli.fetch_add(1000, std::memory_order_relaxed);
    int retry_ms = static_cast<int>(std::ceil(1000.0 / rate));
    return {false, retry_ms};
}

// ========== Rate limit rule ==========

struct RateLimitRule {
    double rate;
    double burst;
};

// ========== Snapshot file format ==========

struct SnapshotHeader {
    char magic[10];         // "RATE_LIMIT"
    uint32_t version;       // 1
    uint32_t checksum;      // content checksum
    int64_t written_at_ms;  // snapshot write time
    int64_t total_buckets;  // total entry count
};

// ========== RateLimiter: 32-shard token bucket ==========

class RateLimiter {
public:
    static constexpr size_t kShards = 32;

    struct Config {
        double ip_rps = 100.0;
        double ip_burst = 200.0;
        double global_rps = 50000.0;
        size_t max_buckets = 100000;
        int snapshot_interval_sec = 30;
        std::string snapshot_path = "/var/lib/asio_owen/rate_limit.bin";
        std::unordered_map<std::string, RateLimitRule> path_limits;
        std::vector<std::pair<std::string, RateLimitRule>> path_prefix_limits;
        std::unordered_map<std::string, RateLimitRule> service_limits;
    };

    explicit RateLimiter(Config cfg)
        : shards_(kShards)
    {
        if (cfg.max_buckets < kShards) {
            LOG_WARN("rate_limit: max_buckets=", cfg.max_buckets,
                     " < kShards=", kShards, ", clamping to ", kShards);
            cfg.max_buckets = kShards;
        }
        // Sort path prefixes by length descending for deterministic match order
        std::sort(cfg.path_prefix_limits.begin(), cfg.path_prefix_limits.end(),
            [](const auto& a, const auto& b) {
                return a.first.size() > b.first.size();
            });
        cfg_ = std::move(cfg);
        max_buckets_per_shard_ = cfg_.max_buckets / kShards;
        load_snapshot();
    }

    ~RateLimiter() {
        persist_snapshot();
    }

    // Single-key rate limit (IP dimension)
    // rate=0 means unlimited, burst=0 means misconfigured (skip)
    Decision check(const std::string& key, double rate, double burst) {
        if (rate <= 0.0) return {true, 0};  // unlimited
        auto& s = shard(key);
        std::lock_guard<std::mutex> lock(s.mu);
        auto& bucket = s.buckets[key];
        lru_touch(s, key);
        evict_if_needed(s);
        return allow(bucket, rate, burst > 0.0 ? burst : rate);
    }

    // Path-dimension rate limit
    Decision check_path(const std::string& path) {
        std::lock_guard<std::mutex> lock(cfg_mu_);
        // exact match first
        auto it = cfg_.path_limits.find(path);
        if (it != cfg_.path_limits.end()) {
            return check("path:" + path, it->second.rate, it->second.burst);
        }
        // prefix match: path starts with configured prefix
        for (auto& [prefix, rule] : cfg_.path_prefix_limits) {
            if (path.starts_with(prefix)) {
                return check("path:" + prefix, rule.rate, rule.burst);
            }
        }
        return {true, 0};
    }

    // Service-dimension rate limit
    Decision check_service(const std::string& service) {
        std::lock_guard<std::mutex> lock(cfg_mu_);
        auto it = cfg_.service_limits.find(service);
        if (it == cfg_.service_limits.end()) return {true, 0};
        return check("svc:" + service, it->second.rate, it->second.burst);
    }

    // Multi-dimension check: any dimension fails -> 429, Retry-After = max
    Decision check_all(
        const std::string& ip,
        const std::string& path,
        const std::string& service)
    {
        // snapshot rate config under lock to avoid race with update_config
        double ip_rps, ip_burst, global_rps;
        {
            std::lock_guard<std::mutex> lock(cfg_mu_);
            ip_rps = cfg_.ip_rps;
            ip_burst = cfg_.ip_burst;
            global_rps = cfg_.global_rps;
        }
        auto ip_d = check(ip, ip_rps, ip_burst);
        auto path_d = check_path(path);
        auto svc_d = check_service(service);
        auto global_d = check_global(global_, global_rps, global_rps);

        if (ip_d.allowed && path_d.allowed && svc_d.allowed && global_d.allowed) {
            return {true, 0};
        }
        int max_retry = std::max({ip_d.retry_after_ms,
            path_d.retry_after_ms, svc_d.retry_after_ms,
            global_d.retry_after_ms});
        return {false, max_retry};
    }

    // Update rate limit config (hot reload)
    void update_config(Config cfg) {
        std::lock_guard<std::mutex> lock(cfg_mu_);
        // Sort path prefixes by length descending (longest match first)
        // The input order from Config::get_section is non-deterministic (unordered_map),
        // so explicit sorting is required for deterministic first-match-wins behavior.
        std::sort(cfg.path_prefix_limits.begin(), cfg.path_prefix_limits.end(),
            [](const auto& a, const auto& b) {
                return a.first.size() > b.first.size();
            });
        cfg_ = std::move(cfg);
        max_buckets_per_shard_ = cfg_.max_buckets / kShards;
    }

    // Persist snapshot (manual trigger)
    void persist_snapshot() {
        // prevent concurrent persist from destructor and timer
        if (snapshot_busy_.exchange(true)) return;

        Snapshot snap;
        snap.header.written_at_ms = now_ms();

        for (size_t i = 0; i < kShards; i++) {
            std::lock_guard<std::mutex> lock(shards_[i].mu);
            snap.shards.push_back(shards_[i].buckets);
        }

        auto tmp = cfg_.snapshot_path + ".tmp";
        if (write_snapshot(tmp, snap)) {
            if (std::rename(tmp.c_str(), cfg_.snapshot_path.c_str()) != 0) {
                LOG_WARN("rate_limit: snapshot rename failed");
            }
        }

        snapshot_busy_.store(false);
    }

    // persist_worker is driven by steady_timer in main.cpp

private:
    struct Shard {
        std::mutex mu;
        std::unordered_map<std::string, TokenBucket> buckets;
        std::list<std::string> lru_list;
        std::unordered_map<std::string, std::list<std::string>::iterator> lru_index;
    };

    struct Snapshot {
        SnapshotHeader header;
        std::vector<std::unordered_map<std::string, TokenBucket>> shards;
    };

    mutable std::mutex cfg_mu_;  // protects cfg_ and max_buckets_per_shard_
    Config cfg_;
    size_t max_buckets_per_shard_;
    std::vector<Shard> shards_;
    GlobalBucket global_;
    std::atomic<bool> snapshot_busy_{false};  // prevents persist_snapshot re-entry

    Shard& shard(const std::string& key) {
        return shards_[std::hash<std::string>{}(key) % kShards];
    }

    const Shard& shard(const std::string& key) const {
        return shards_[std::hash<std::string>{}(key) % kShards];
    }

    void lru_touch(Shard& s, const std::string& key) {
        auto it = s.lru_index.find(key);
        if (it != s.lru_index.end()) {
            s.lru_list.erase(it->second);
        }
        s.lru_list.push_front(key);
        s.lru_index[key] = s.lru_list.begin();
    }

    void evict_if_needed(Shard& s) {
        while (s.buckets.size() > max_buckets_per_shard_) {
            auto lru_key = s.lru_list.back();
            s.buckets.erase(lru_key);
            s.lru_index.erase(lru_key);
            s.lru_list.pop_back();
        }
    }

    // Compute checksum (FNV-1a hash)
    static uint32_t calculate_checksum(const void* data, size_t len) {
        uint32_t hash = 2166136261u;
        auto bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) {
            hash ^= bytes[i];
            hash *= 16777619u;
        }
        return hash;
    }

    bool write_snapshot(const std::string& path, const Snapshot& snap) {
        std::ofstream ofs(path, std::ios::binary);
        if (!ofs) {
            LOG_WARN("rate_limit: cannot open snapshot file for writing: ", path);
            return false;
        }

        // serialize buckets to buffer, compute checksum
        std::ostringstream oss(std::ios::binary);
        for (auto& shard_buckets : snap.shards) {
            uint32_t count = static_cast<uint32_t>(shard_buckets.size());
            oss.write(reinterpret_cast<const char*>(&count), sizeof(count));
            for (auto& [key, bucket] : shard_buckets) {
                uint32_t key_len = static_cast<uint32_t>(key.size());
                oss.write(reinterpret_cast<const char*>(&key_len), sizeof(key_len));
                oss.write(key.data(), key_len);
                oss.write(reinterpret_cast<const char*>(&bucket.last_refill_ms), sizeof(bucket.last_refill_ms));
                oss.write(reinterpret_cast<const char*>(&bucket.tokens), sizeof(bucket.tokens));
            }
        }

        auto body = oss.str();
        uint32_t checksum = calculate_checksum(body.data(), body.size());

        // write header
        SnapshotHeader hdr;
        std::memcpy(hdr.magic, "RATE_LIMIT", 10);
        hdr.version = 1;
        hdr.checksum = checksum;
        hdr.written_at_ms = snap.header.written_at_ms;
        // count total entries for logging
        size_t total = 0;
        for (auto& shard_buckets : snap.shards) {
            total += shard_buckets.size();
        }
        hdr.total_buckets = static_cast<int64_t>(total);
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

        // write body
        ofs.write(body.data(), body.size());

        ofs.close();
        return true;
    }

    void load_snapshot() {
        std::ifstream ifs(cfg_.snapshot_path, std::ios::binary);
        if (!ifs) {
            // first start, no snapshot file
            return;
        }

        // read header
        SnapshotHeader hdr;
        if (!ifs.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
            LOG_WARN("rate_limit: snapshot header read failed, starting empty");
            return;
        }

        // validate magic
        if (std::memcmp(hdr.magic, "RATE_LIMIT", 10) != 0) {
            LOG_WARN("rate_limit: snapshot corrupted (bad magic), starting empty");
            return;
        }

        // validate version
        if (hdr.version != 1) {
            LOG_WARN("rate_limit: snapshot version mismatch (got=", hdr.version,
                     ", expected=1), starting empty");
            return;
        }

        // check expiry (older than 2 minutes is stale)
        if (hdr.written_at_ms + 120 * 1000 < now_ms()) {
            LOG_WARN("rate_limit: snapshot expired (age > 2min), starting empty");
            return;
        }

        // read body
        std::string body((std::istreambuf_iterator<char>(ifs)),
                          std::istreambuf_iterator<char>());

        // validate checksum
        uint32_t computed = calculate_checksum(body.data(), body.size());
        if (computed != hdr.checksum) {
            LOG_WARN("rate_limit: snapshot checksum mismatch, starting empty");
            return;
        }

        // deserialize
        const char* ptr = body.data();
        const char* end = ptr + body.size();

        std::vector<std::unordered_map<std::string, TokenBucket>> loaded_shards(kShards);

        for (size_t i = 0; i < kShards && ptr < end; ++i) {
            if (static_cast<size_t>(end - ptr) < sizeof(uint32_t)) break;
            uint32_t count;
            std::memcpy(&count, ptr, sizeof(count));
            ptr += sizeof(uint32_t);

            for (uint32_t j = 0; j < count && ptr < end; ++j) {
                if (static_cast<size_t>(end - ptr) < sizeof(uint32_t)) break;
                uint32_t key_len;
                std::memcpy(&key_len, ptr, sizeof(key_len));
                ptr += sizeof(uint32_t);

                if (static_cast<size_t>(end - ptr) < key_len) break;
                std::string key(ptr, key_len);
                ptr += key_len;

                if (static_cast<size_t>(end - ptr) <
                    sizeof(int64_t) + sizeof(double)) break;
                int64_t last_refill_ms;
                double tokens;
                std::memcpy(&last_refill_ms, ptr, sizeof(last_refill_ms));
                ptr += sizeof(int64_t);
                std::memcpy(&tokens, ptr, sizeof(tokens));
                ptr += sizeof(double);

                loaded_shards[i].emplace(std::move(key),
                    TokenBucket{last_refill_ms, tokens});
            }
        }

        // Rebuild shards, synchronously rebuild LRU, reset all last_refill_ms
        // Reason: steady_clock is not continuous across processes. The old process's
        // last_refill_ms is meaningless in the new process (would be a large negative
        // number -> elapsed negative -> tokens negative -> permanent 429).
        // After reset, tokens start from full burst capacity.
        auto load_now = now_ms();
        for (size_t i = 0; i < kShards; i++) {
            std::lock_guard<std::mutex> lock(shards_[i].mu);
            shards_[i].buckets = std::move(loaded_shards[i]);
            shards_[i].lru_list.clear();
            shards_[i].lru_index.clear();
            for (auto& [k, v] : shards_[i].buckets) {
                v.last_refill_ms = load_now;
                shards_[i].lru_list.push_front(k);
                shards_[i].lru_index[k] = shards_[i].lru_list.begin();
            }
        }

        LOG_INFO("rate_limit: snapshot loaded (buckets=", loaded_shards_total(), ")");
    }

    // Count total loaded entries across all shards (for logging, lock held by caller)
    size_t loaded_shards_total() const {
        size_t total = 0;
        for (auto& s : shards_) {
            total += s.buckets.size();
        }
        return total;
    }
};
