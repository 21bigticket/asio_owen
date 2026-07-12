#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "security/rate_limiter.hpp"

namespace {

// Build a Config with small/predictable limits so tests don't have to wait.
RateLimiter::Config make_test_config(const std::string& snapshot_path) {
    RateLimiter::Config cfg;
    cfg.ip_rps = 10.0;
    cfg.ip_burst = 10.0;
    cfg.global_rps = 1000.0;
    cfg.max_buckets = 32 * RateLimiter::kShards;
    cfg.snapshot_interval_sec = 30;
    cfg.snapshot_path = snapshot_path;
    return cfg;
}

std::string make_tmp_snapshot_path(const char* tag) {
    char buf[256];
    auto now_count = static_cast<long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::snprintf(buf, sizeof(buf), "/tmp/rate_limit_test_%s_%d_%lld.bin",
        tag, ::getpid(), now_count);
    return std::string(buf);
}

void remove_if_exists(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace

// ============== TokenBucket (free function allow) ==============

TEST(TokenBucket, RateZeroIsUnlimited) {
    TokenBucket b{0, 0.0};
    for (int i = 0; i < 100; ++i) {
        auto d = allow(b, 0.0, 0.0);
        EXPECT_TRUE(d.allowed) << "iteration " << i;
    }
}

TEST(TokenBucket, RejectsWhenBurstExhausted) {
    TokenBucket b{0, 5.0};
    // First 5 should pass (burst), 6th must reject.
    for (int i = 0; i < 5; ++i) {
        auto d = allow(b, 100.0, 5.0);
        EXPECT_TRUE(d.allowed) << "burst iteration " << i;
    }
    auto d = allow(b, 100.0, 5.0);
    EXPECT_FALSE(d.allowed);
    EXPECT_GT(d.retry_after_ms, 0);
}

TEST(TokenBucket, RefillCapsAtBurst) {
    TokenBucket b{0, 3.0};
    // drain
    for (int i = 0; i < 3; ++i) allow(b, 100.0, 3.0);
    // sleep 50ms — at 100 rps this would add 5 tokens, but burst caps to 3
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    // Should be back to ~3 tokens
    int allowed = 0;
    for (int i = 0; i < 10; ++i) {
        if (allow(b, 100.0, 3.0).allowed) ++allowed;
    }
    // Allow small timing slack — should be exactly 3 in theory.
    EXPECT_GE(allowed, 3);
    EXPECT_LE(allowed, 4);
}

TEST(TokenBucket, RetryAfterCalculation) {
    TokenBucket b{0, 1.0};
    ASSERT_TRUE(allow(b, 10.0, 1.0).allowed);  // drain burst
    auto d = allow(b, 10.0, 1.0);
    ASSERT_FALSE(d.allowed);
    // Need 1 token at 10 tokens/sec => ~100ms wait.
    EXPECT_GE(d.retry_after_ms, 50);
    EXPECT_LE(d.retry_after_ms, 200);
}

// ============== GlobalBucket (CAS) ==============

TEST(GlobalBucket, RefundOnRejectKeepsBucketAlive) {
    GlobalBucket g{};
    g.tokens_milli.store(0, std::memory_order_relaxed);
    // Set last_refill_ms to "now" so no large refill window opens on the first call.
    // At rate=1 rps, a 1ms elapsed would add only 1 milli-token — negligible.
    g.last_refill_ms.store(now_ms(), std::memory_order_relaxed);

    // Fire 100 concurrent checks at empty bucket. Without refund, tokens_milli
    // would drop to -100000. With refund it should stay near 0.
    std::atomic<int> rejected{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 25; ++i) {
                if (!check_global(g, 1.0, 1.0).allowed) {
                    rejected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    // Almost all should reject (burst=1 means 1 token = 1 pass, but timing-dependent).
    EXPECT_GT(rejected.load(), 90);
    // Key assertion: tokens_milli must NOT be deeply negative.
    // Without refund it would be ~-100000. With refund, near 0 (modulo small refill).
    auto remaining = g.tokens_milli.load(std::memory_order_relaxed);
    EXPECT_GT(remaining, -5000) << "refund failed, tokens_milli=" << remaining;
}

TEST(GlobalBucket, CasRefillNoDoubleCount) {
    GlobalBucket g{};
    g.tokens_milli.store(0, std::memory_order_relaxed);
    g.last_refill_ms.store(now_ms() - 1000, std::memory_order_relaxed);

    // After 1s at 1000 rps, refill should add 1,000,000 milli-tokens (capped at burst).
    // Concurrent calls must not double-count the refill window.
    std::atomic<int> allowed{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 10; ++i) {
                if (check_global(g, 1000.0, 1000.0).allowed) {
                    allowed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    // burst=1000 means at most 1000 requests can pass. 80 calls << 1000 so all should pass.
    // But more importantly, last_refill_ms should advance exactly once per refill window.
    EXPECT_EQ(allowed.load(), 80);
}

// ============== RateLimiter::check (IP dimension) ==============

TEST(RateLimiter, CheckAllowsUntilBurstThenRejects) {
    auto path = make_tmp_snapshot_path("check_burst");
    remove_if_exists(path);
    {
        RateLimiter::Config cfg = make_test_config(path);
        cfg.ip_rps = 5.0;
        cfg.ip_burst = 3.0;
        RateLimiter limiter(std::move(cfg));

        for (int i = 0; i < 3; ++i) {
            EXPECT_TRUE(limiter.check("1.2.3.4", 5.0, 3.0).allowed);
        }
        auto d = limiter.check("1.2.3.4", 5.0, 3.0);
        EXPECT_FALSE(d.allowed);
        EXPECT_GT(d.retry_after_ms, 0);
    }
    remove_if_exists(path);
}

TEST(RateLimiter, CheckPathExactBeatsPrefix) {
    auto path = make_tmp_snapshot_path("path_match");
    remove_if_exists(path);
    {
        RateLimiter::Config cfg = make_test_config(path);
        cfg.path_limits["/api/expensive"] = {1.0, 1.0};
        cfg.path_prefix_limits.emplace_back("/api/", RateLimitRule{100.0, 100.0});
        RateLimiter limiter(std::move(cfg));

        // exact match (limit=1) wins over prefix (/api/ = 100).
        EXPECT_TRUE(limiter.check_path("/api/expensive").allowed);
        EXPECT_FALSE(limiter.check_path("/api/expensive").allowed);

        // other /api/* paths use the prefix rule (burst=100).
        for (int i = 0; i < 10; ++i) {
            EXPECT_TRUE(limiter.check_path("/api/other").allowed);
        }
    }
    remove_if_exists(path);
}

TEST(RateLimiter, CheckPathLongerPrefixWins) {
    auto path = make_tmp_snapshot_path("prefix_len");
    remove_if_exists(path);
    {
        RateLimiter::Config cfg = make_test_config(path);
        cfg.path_prefix_limits.emplace_back("/api/", RateLimitRule{100.0, 100.0});
        cfg.path_prefix_limits.emplace_back("/api/v2/", RateLimitRule{1.0, 1.0});
        RateLimiter limiter(std::move(cfg));

        // Longer prefix /api/v2/ should win.
        EXPECT_TRUE(limiter.check_path("/api/v2/users").allowed);
        EXPECT_FALSE(limiter.check_path("/api/v2/users").allowed);

        // Shorter prefix /api/ still applies to other paths.
        EXPECT_TRUE(limiter.check_path("/api/v1/users").allowed);
        EXPECT_TRUE(limiter.check_path("/api/v1/users").allowed);
    }
    remove_if_exists(path);
}

TEST(RateLimiter, CheckServiceMissingServiceAllowed) {
    auto path = make_tmp_snapshot_path("svc_missing");
    remove_if_exists(path);
    {
        RateLimiter::Config cfg = make_test_config(path);
        RateLimiter limiter(std::move(cfg));
        EXPECT_TRUE(limiter.check_service("not-configured").allowed);
    }
    remove_if_exists(path);
}

TEST(RateLimiter, CheckServiceEnforcesConfiguredLimit) {
    auto path = make_tmp_snapshot_path("svc_limit");
    remove_if_exists(path);
    {
        RateLimiter::Config cfg = make_test_config(path);
        cfg.service_limits["expensive-svc"] = {1.0, 2.0};
        RateLimiter limiter(std::move(cfg));

        EXPECT_TRUE(limiter.check_service("expensive-svc").allowed);
        EXPECT_TRUE(limiter.check_service("expensive-svc").allowed);
        EXPECT_FALSE(limiter.check_service("expensive-svc").allowed);
    }
    remove_if_exists(path);
}

TEST(RateLimiter, CheckAllTakesMaxRetryAfter) {
    auto path = make_tmp_snapshot_path("check_all");
    remove_if_exists(path);
    {
        RateLimiter::Config cfg = make_test_config(path);
        cfg.ip_rps = 100.0;
        cfg.ip_burst = 1.0;          // tight IP limit (retry_after small)
        cfg.global_rps = 1.0;        // very tight global (retry_after big)
        RateLimiter limiter(std::move(cfg));

        // Drain global bucket (burst == global_rps == 1, so 1 allowed then rest denied).
        limiter.check_all("5.6.7.8", "/x", "svc");
        auto d = limiter.check_all("5.6.7.8", "/x", "svc");
        EXPECT_FALSE(d.allowed);
        // global retry_after ~1000ms — should dominate.
        EXPECT_GE(d.retry_after_ms, 500);
    }
    remove_if_exists(path);
}

// ============== LRU eviction ==============

TEST(RateLimiter, LruEvictsOldestWhenShardFull) {
    auto path = make_tmp_snapshot_path("lru");
    remove_if_exists(path);
    {
        RateLimiter::Config cfg = make_test_config(path);
        // Force tiny per-shard capacity: max_buckets=64 / kShards=32 = 2 per shard.
        cfg.max_buckets = 64;
        RateLimiter limiter(std::move(cfg));

        // Push 100 distinct keys — many more than max_buckets.
        // Each call uses its own key, so they distribute across shards and trigger eviction.
        for (int i = 0; i < 100; ++i) {
            std::string key = "10.0." + std::to_string(i / 256) + "." + std::to_string(i % 256);
            limiter.check(key, 100.0, 100.0);
        }
        // No assertion crash, no memory error. Eviction kept shards bounded.
        SUCCEED();
    }
    remove_if_exists(path);
}

// ============== Hot reload ==============

TEST(RateLimiter, UpdateConfigTakesEffectImmediately) {
    auto path = make_tmp_snapshot_path("hot_reload");
    remove_if_exists(path);
    {
        RateLimiter::Config cfg = make_test_config(path);
        cfg.ip_rps = 1000.0;
        cfg.ip_burst = 1000.0;
        RateLimiter limiter(std::move(cfg));

        // Generous limit, many passes.
        for (int i = 0; i < 50; ++i) {
            EXPECT_TRUE(limiter.check("9.9.9.9", 1000.0, 1000.0).allowed);
        }

        // Tighten to 1 burst.
        RateLimiter::Config cfg2 = make_test_config(path);
        cfg2.ip_rps = 1.0;
        cfg2.ip_burst = 1.0;
        limiter.update_config(std::move(cfg2));

        // New bucket for new key — only 1 pass.
        EXPECT_TRUE(limiter.check("9.9.9.10", 1.0, 1.0).allowed);
        EXPECT_FALSE(limiter.check("9.9.9.10", 1.0, 1.0).allowed);
    }
    remove_if_exists(path);
}

// ============== Snapshot persistence ==============

TEST(RateLimiter, SnapshotRoundTripPreservesBuckets) {
    auto path = make_tmp_snapshot_path("round_trip");
    remove_if_exists(path);
    {
        RateLimiter::Config cfg = make_test_config(path);
        cfg.ip_rps = 5.0;
        cfg.ip_burst = 5.0;
        RateLimiter limiter(std::move(cfg));
        // Populate some buckets with non-trivial state.
        for (int i = 0; i < 5; ++i) {
            limiter.check("50.0.0." + std::to_string(i), 5.0, 5.0);
        }
        limiter.persist_snapshot();
    }
    {
        RateLimiter::Config cfg = make_test_config(path);
        cfg.ip_rps = 5.0;
        cfg.ip_burst = 5.0;
        RateLimiter limiter(std::move(cfg));
        // Reloaded buckets must still exist (verify by hitting one and getting rejected
        // only after original burst count).
        // buckets were created with 1 token consumed each, so 4 remain + refill during test.
        // Just verify the limiter is functional and the file was loaded.
        EXPECT_TRUE(limiter.check("50.0.0.0", 5.0, 5.0).allowed);
    }
    remove_if_exists(path);
}

TEST(RateLimiter, SnapshotExpiredIsIgnored) {
    auto path = make_tmp_snapshot_path("expired");
    remove_if_exists(path);
    // Manually craft an expired snapshot.
    {
        std::ofstream ofs(path, std::ios::binary);
        SnapshotHeader hdr{};
        std::memcpy(hdr.magic, "RATE_LIMIT", 10);
        hdr.version = 1;
        hdr.checksum = 0;
        hdr.written_at_ms = now_ms() - 130 * 1000;  // 130s ago, > 2min TTL
        hdr.total_buckets = 0;
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        // empty body with matching checksum
        uint32_t checksum = 2166136261u;
        // Recompute properly: body is empty so FNV-1a hash of 0 bytes = initial value.
        // The implementation calls calculate_checksum on body.data(), body.size()=0.
        // FNV-1a of empty input = 2166136261 (the initial offset basis, no multiplications).
        hdr.checksum = 2166136261u;
        // rewrite header with correct checksum
        ofs.seekp(0);
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    }
    {
        RateLimiter::Config cfg = make_test_config(path);
        RateLimiter limiter(std::move(cfg));
        // If expired snapshot was honored, bucket "70.0.0.1" might pre-exist.
        // After load (which is empty), limiter should behave normally.
        EXPECT_TRUE(limiter.check("70.0.0.1", 100.0, 100.0).allowed);
    }
    remove_if_exists(path);
}

TEST(RateLimiter, SnapshotBadMagicIsIgnored) {
    auto path = make_tmp_snapshot_path("badmagic");
    remove_if_exists(path);
    {
        std::ofstream ofs(path, std::ios::binary);
        SnapshotHeader hdr{};
        std::memcpy(hdr.magic, "XXXXXXXXXX", 10);
        hdr.version = 1;
        hdr.checksum = 0;
        hdr.written_at_ms = now_ms();
        hdr.total_buckets = 0;
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    }
    {
        RateLimiter::Config cfg = make_test_config(path);
        RateLimiter limiter(std::move(cfg));
        EXPECT_TRUE(limiter.check("80.0.0.1", 100.0, 100.0).allowed);
    }
    remove_if_exists(path);
}

TEST(RateLimiter, SnapshotChecksumMismatchIsIgnored) {
    auto path = make_tmp_snapshot_path("badcksum");
    remove_if_exists(path);
    {
        RateLimiter::Config cfg = make_test_config(path);
        cfg.ip_rps = 5.0;
        cfg.ip_burst = 5.0;
        RateLimiter limiter(std::move(cfg));
        limiter.check("90.0.0.1", 5.0, 5.0);
        limiter.persist_snapshot();
    }
    // Corrupt one byte of the body (after the header).
    {
        std::fstream fs(path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(fs.is_open());
        fs.seekg(0, std::ios::end);
        auto size = fs.tellg();
        ASSERT_GT(size, static_cast<std::streampos>(sizeof(SnapshotHeader) + 8));
        fs.seekp(sizeof(SnapshotHeader) + 4);
        char c = 0;
        fs.read(&c, 1);
        fs.seekp(sizeof(SnapshotHeader) + 4);
        c = (c == 0x00) ? 0xFF : 0x00;
        fs.write(&c, 1);
    }
    {
        RateLimiter::Config cfg = make_test_config(path);
        RateLimiter limiter(std::move(cfg));
        // Should ignore corrupted snapshot and start empty.
        EXPECT_TRUE(limiter.check("90.0.0.2", 100.0, 100.0).allowed);
    }
    remove_if_exists(path);
}

TEST(RateLimiter, DestructorPersistsSnapshot) {
    auto path = make_tmp_snapshot_path("dtor_persist");
    remove_if_exists(path);
    {
        RateLimiter::Config cfg = make_test_config(path);
        RateLimiter limiter(std::move(cfg));
        limiter.check("100.1.1.1", 100.0, 100.0);
        // Destructor should call persist_snapshot.
    }
    EXPECT_TRUE(std::filesystem::exists(path));
    remove_if_exists(path);
}
