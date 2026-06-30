#pragma once
#include <asio.hpp>
#include <functional>
#include "logger.hpp"

class SignalExit {
public:
    SignalExit(asio::io_context& ioc) : signals_(ioc, SIGINT, SIGTERM) {}

    void on_exit(std::function<void()> cb) {
        exit_cb_ = std::move(cb);
        signals_.async_wait([this](std::error_code, int sig) {
            LOG_INFO("Received signal ", sig, ", exiting gracefully...");
            if (exit_cb_) exit_cb_();
        });
    }

private:
    asio::signal_set signals_;
    std::function<void()> exit_cb_;
};
