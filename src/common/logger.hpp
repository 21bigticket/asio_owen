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

// 先用 ostringstream 拼接，再调用 spdlog，兼容原有的 << 风格
#define LOG_DEBUG(...)   do { std::ostringstream _log_oss; _log_oss << __VA_ARGS__; Logger::instance().get()->debug(_log_oss.str()); } while(0)
#define LOG_INFO(...)    do { std::ostringstream _log_oss; _log_oss << __VA_ARGS__; Logger::instance().get()->info(_log_oss.str()); } while(0)
#define LOG_WARN(...)    do { std::ostringstream _log_oss; _log_oss << __VA_ARGS__; Logger::instance().get()->warn(_log_oss.str()); } while(0)
#define LOG_ERROR(...)   do { std::ostringstream _log_oss; _log_oss << __VA_ARGS__; Logger::instance().get()->error(_log_oss.str()); } while(0)
