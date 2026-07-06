#pragma once

#include <filesystem>

#include <asio.hpp>

#include "../common/config.hpp"
#include "../http/http_pool.hpp"
#include "../http/upstream_manager.hpp"
#include "../security/security_rules.hpp"

class ReloadService {
public:
    ReloadService(asio::io_context& ioc,
                  std::filesystem::path config_base,
                  SecurityRules& security_rules,
                  UpstreamManager& upstreams,
                  HttpPool::Config http_pool_cfg)
        : timer_(ioc)
        , config_base_(std::move(config_base))
        , security_rules_(security_rules)
        , upstreams_(upstreams)
        , http_pool_cfg_(std::move(http_pool_cfg)) {}

    void start(int interval_sec) {
        if (interval_sec <= 0) return;
        running_ = true;
        schedule(interval_sec);
    }

    void stop() {
        running_ = false;
        timer_.cancel();
    }

private:
    void schedule(int interval_sec) {
        timer_.expires_after(std::chrono::seconds(interval_sec));
        timer_.async_wait([this](std::error_code ec) {
            if (ec || !running_) return;

            Config new_cfg;
            int next_sec = 30;
            if (new_cfg.load(config_base_)) {
                security_rules_.reload(new_cfg);
                upstreams_.reload(new_cfg, http_pool_cfg_);
                next_sec = new_cfg.get_int("security", "config_reload_interval_sec", 30);
            }

            if (running_ && next_sec > 0) {
                schedule(next_sec);
            }
        });
    }

    asio::steady_timer timer_;
    std::filesystem::path config_base_;
    SecurityRules& security_rules_;
    UpstreamManager& upstreams_;
    HttpPool::Config http_pool_cfg_;
    bool running_ = false;
};
