#pragma once

#include <algorithm>
#include <filesystem>
#include <optional>
#include <vector>

#include <asio.hpp>

#include "../common/config.hpp"
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
            auto current_fingerprint = read_config_fingerprint();
            const bool config_changed =
                !current_fingerprint ||
                !last_config_fingerprint_ ||
                *current_fingerprint != *last_config_fingerprint_;

            if (config_changed) {
                Config new_cfg;
                if (new_cfg.load(config_base_)) {
                    security_rules_.reload(new_cfg);
                    // Re-read [http_pool] each reload instead of using the value
                    // captured at construction, so pool tuning changes take effect.
                    upstreams_.reload(new_cfg, http_pool_config_from(new_cfg));
                    next_sec = new_cfg.get_int("security", "config_reload_interval_sec", interval_sec);
                    last_config_fingerprint_ = std::move(current_fingerprint);
                }
            }

            if (running_ && next_sec > 0) {
                schedule(next_sec);
            }
        });
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
    bool running_ = false;
};
