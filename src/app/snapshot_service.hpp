#pragma once

#include <asio.hpp>

#include "../security/security_rules.hpp"

class SnapshotService {
public:
    SnapshotService(asio::io_context& ioc, SecurityRules& security_rules)
        : timer_(ioc), security_rules_(security_rules) {}

    void start(int interval_sec) {
        if (interval_sec <= 0 || !security_rules_.has_rate_limiter()) return;
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
            if (auto limiter = security_rules_.rate_limiter_snapshot()) {
                limiter->persist_snapshot();
            }
            if (running_) {
                schedule(interval_sec);
            }
        });
    }

    asio::steady_timer timer_;
    SecurityRules& security_rules_;
    bool running_ = false;
};
