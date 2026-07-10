#pragma once

#include <asio.hpp>

#include "../common/logger.hpp"
#include "../http/upstream_manager.hpp"

class PoolStatsService {
public:
    PoolStatsService(asio::io_context& ioc, UpstreamManager& upstreams)
        : timer_(ioc), upstreams_(upstreams) {}

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
        timer_.async_wait([this, interval_sec](std::error_code ec) {
            if (ec || !running_) return;
            LOG_INFO("HttpPool stats: ", upstreams_.pool_stats());
            if (running_) {
                schedule(interval_sec);
            }
        });
    }

    asio::steady_timer timer_;
    UpstreamManager& upstreams_;
    bool running_ = false;
};
