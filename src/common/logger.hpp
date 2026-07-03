#pragma once
#include <memory>
#include <string>
#include <sstream>

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

enum LogLevel { DEBUG = SPDLOG_LEVEL_DEBUG, INFO = SPDLOG_LEVEL_INFO,
                WARN = SPDLOG_LEVEL_WARN, ERROR = SPDLOG_LEVEL_ERROR };

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void init(const std::string& file_path, LogLevel level = INFO) {
        try {
            spdlog::init_thread_pool(262144, 1);

            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                file_path, 50 * 1024 * 1024, 3);

            std::vector<spdlog::sink_ptr> sinks = {console_sink, file_sink};
            logger_ = std::make_shared<spdlog::async_logger>(
                "server", sinks.begin(), sinks.end(),
                spdlog::thread_pool(), spdlog::async_overflow_policy::block);

            logger_->set_level(static_cast<spdlog::level::level_enum>(level));
            logger_->flush_on(spdlog::level::warn);

            logger_->info("Logger initialized, level={}, file={}",
                          spdlog::level::to_string_view(logger_->level()), file_path);

        } catch (const std::exception& e) {
            std::cerr << "Logger init failed: " << e.what() << std::endl;
            logger_ = spdlog::stdout_color_mt("default");
            logger_->set_level(static_cast<spdlog::level::level_enum>(level));
            logger_->warn("Fallback to default console logger");
        }
    }

    std::shared_ptr<spdlog::logger>& get() { return logger_; }

private:
    Logger() = default;
    std::shared_ptr<spdlog::logger> logger_;
};

namespace detail {

template <typename T>
const T& stream_arg(const T& arg) { return arg; }

inline void log_to_stream(std::ostringstream&) {}

template <typename T, typename... Args>
void log_to_stream(std::ostringstream& oss, const T& first, const Args&... rest) {
    oss << first;
    log_to_stream(oss, rest...);
}

template <typename... Args>
void log_info_impl(const Args&... args) {
    if (Logger::instance().get()->should_log(spdlog::level::info)) {
        std::ostringstream oss;
        log_to_stream(oss, args...);
        Logger::instance().get()->info(oss.str());
    }
}

template <typename... Args>
void log_warn_impl(const Args&... args) {
    std::ostringstream oss;
    log_to_stream(oss, args...);
    Logger::instance().get()->warn(oss.str());
}

template <typename... Args>
void log_error_impl(const Args&... args) {
    std::ostringstream oss;
    log_to_stream(oss, args...);
    Logger::instance().get()->error(oss.str());
}

template <typename... Args>
void log_debug_impl(const Args&... args) {
    if (Logger::instance().get()->should_log(spdlog::level::debug)) {
        std::ostringstream oss;
        log_to_stream(oss, args...);
        Logger::instance().get()->debug(oss.str());
    }
}

} // namespace detail

#define LOG_DEBUG(...)   detail::log_debug_impl(__VA_ARGS__)
#define LOG_INFO(...)    detail::log_info_impl(__VA_ARGS__)
#define LOG_WARN(...)    detail::log_warn_impl(__VA_ARGS__)
#define LOG_ERROR(...)   detail::log_error_impl(__VA_ARGS__)
