#pragma once

#include <algorithm>
#include <filesystem>
#include <optional>
#include <vector>

#include <asio.hpp>

#include "../common/config.hpp"
#include "../common/logger.hpp"
#include "../http/http_pool.hpp"
#include "../http/upstream_manager.hpp"
#include "../security/security_rules.hpp"
#include "app_config.hpp"

class ReloadService {
public:
    ReloadService(asio::io_context& ioc,
                  std::filesystem::path config_base,
                  SecurityRules& security_rules,
                  UpstreamManager& upstreams)
        : timer_(ioc)
        , config_base_(std::move(config_base))
        , security_rules_(security_rules)
        , upstreams_(upstreams) {}

    void start(int interval_sec) {
        if (interval_sec <= 0) return;
        running_ = true;
        last_config_fingerprint_ = read_config_fingerprint();
        schedule(interval_sec);
    }

    void stop() {
        running_ = false;
        timer_.cancel();
    }

private:
    void schedule(int interval_sec) {
        timer_.expires_after(std::chrono::seconds(interval_sec));
        timer_.async_wait([this, interval_sec](std::error_code ec) {
            if (ec || !running_) return;

            int next_sec = interval_sec;
            try {
                auto current_fingerprint = read_config_fingerprint();

                // Debounce + mid-load consistency check. A changed fingerprint
                // is first OBSERVED (pending), and only loaded when it stays
                // stable for two consecutive ticks. This prevents publishing a
                // half-written intermediate file (editors often truncate a
                // file before rewriting it). The fingerprint is re-read after
                // parsing and compared again, so a file that is still being
                // written while we load it also aborts the publish and keeps
                // the previously published config.
                if (pending_fingerprint_ && current_fingerprint &&
                    *pending_fingerprint_ == *current_fingerprint) {
                    const bool config_changed =
                        !last_config_fingerprint_ ||
                        *current_fingerprint != *last_config_fingerprint_;
                    if (config_changed) {
                        Config new_cfg;
                        if (new_cfg.load(config_base_)) {
                            auto after_load_fingerprint = read_config_fingerprint();
                            if (!after_load_fingerprint ||
                                *after_load_fingerprint != *current_fingerprint) {
                                // Files changed while we were parsing; publish
                                // nothing and re-observe on the next tick.
                                LOG_ERROR("Config changed during load; reload deferred");
                            } else {
                                const int prepared_next_sec = new_cfg.get_int(
                                    "security", "config_reload_interval_sec", interval_sec);
                                auto prepared_security = security_rules_.prepare_reload(new_cfg);
                                // Re-read [http_pool] each reload instead of using
                                // the value captured at construction, so pool
                                // tuning changes take effect.
                                auto prepared_upstreams = upstreams_.prepare_reload(
                                    new_cfg, http_pool_config_from(new_cfg));

                                // All allocation and parsing completes before
                                // either live subsystem is changed. Publish
                                // security first so a newly added upstream is
                                // never exposed under stale rules.
                                security_rules_.publish_reload(std::move(prepared_security));
                                upstreams_.publish_reload(std::move(prepared_upstreams));
                                next_sec = prepared_next_sec;
                                // Only acknowledge the fingerprint after every
                                // reload step succeeds. Failures are retried on
                                // the next timer tick.
                                last_config_fingerprint_ = std::move(current_fingerprint);
                            }
                        }
                    }
                    pending_fingerprint_.reset();
                } else if (current_fingerprint &&
                           (!last_config_fingerprint_ ||
                            *current_fingerprint != *last_config_fingerprint_)) {
                    // First observation, or a pending fingerprint that changed
                    // again: start the stability window from the newest value.
                    // Keeping the newest observation avoids an unnecessary
                    // empty tick before the next stability check.
                    pending_fingerprint_ = std::move(current_fingerprint);
                } else {
                    // No unapplied change remains.
                    pending_fingerprint_.reset();
                }
            } catch (const std::exception& e) {
                log_reload_failure(e.what());
            } catch (...) {
                log_reload_failure("unknown exception");
            }

            if (running_ && next_sec > 0) {
                try {
                    schedule(next_sec);
                } catch (const std::exception& e) {
                    log_reload_failure(e.what());
                } catch (...) {
                    log_reload_failure("unknown exception while scheduling next reload");
                }
            }
        });
    }

    static void log_reload_failure(const char* message) noexcept {
        try {
            LOG_ERROR("Config hot-reload failed; the change will be retried: ", message);
        } catch (...) {
            // Logging must not let a timer completion handler terminate an
            // io_context worker thread.
        }
    }

    struct ConfigFileFingerprint {
        std::string name;
        uintmax_t size = 0;
        std::filesystem::file_time_type mtime;

        bool operator==(const ConfigFileFingerprint& other) const {
            return name == other.name && size == other.size && mtime == other.mtime;
        }
    };

    using ConfigFingerprint = std::vector<ConfigFileFingerprint>;

    std::optional<ConfigFingerprint> read_config_fingerprint() const {
        auto dir_path = config_base_ / "config.d";
        std::error_code ec;
        if (!std::filesystem::is_directory(dir_path, ec)) {
            return std::nullopt;
        }

        ConfigFingerprint fingerprint;
        for (const auto& entry : std::filesystem::directory_iterator(dir_path, ec)) {
            if (ec) return std::nullopt;
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".ini") {
                ec.clear();
                continue;
            }

            auto size = entry.file_size(ec);
            if (ec) return std::nullopt;
            auto mtime = entry.last_write_time(ec);
            if (ec) return std::nullopt;

            fingerprint.push_back({
                entry.path().filename().string(),
                size,
                mtime
            });
        }

        std::sort(fingerprint.begin(), fingerprint.end(),
            [](const auto& a, const auto& b) {
                return a.name < b.name;
            });
        return fingerprint;
    }

    asio::steady_timer timer_;
    std::filesystem::path config_base_;
    SecurityRules& security_rules_;
    UpstreamManager& upstreams_;
    std::optional<ConfigFingerprint> last_config_fingerprint_;
    // Fingerprint observed for the first time; published only after it is
    // confirmed stable on the next tick (debounce against half-written files).
    std::optional<ConfigFingerprint> pending_fingerprint_;
    bool running_ = false;
};
