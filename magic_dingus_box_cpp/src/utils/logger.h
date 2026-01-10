#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <string>
#include <memory>
#include <vector>

namespace logging {

/**
 * Initialize the logging system.
 *
 * @param log_file_path Optional path to a log file. If empty, only console logging is enabled.
 * @param console_level Minimum log level for console output (default: info)
 * @param file_level Minimum log level for file output (default: debug)
 */
inline void init(
    const std::string& log_file_path = "",
    spdlog::level::level_enum console_level = spdlog::level::info,
    spdlog::level::level_enum file_level = spdlog::level::debug
) {
    try {
        std::vector<spdlog::sink_ptr> sinks;

        // Console sink with colors
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(console_level);
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        sinks.push_back(console_sink);

        // Optional file sink with rotation
        if (!log_file_path.empty()) {
            // 5MB max file size, 3 rotated files
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file_path, 1024 * 1024 * 5, 3
            );
            file_sink->set_level(file_level);
            file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%s:%#] %v");
            sinks.push_back(file_sink);
        }

        // Create and register the logger
        auto logger = std::make_shared<spdlog::logger>("magic", sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::trace); // Allow all levels, sinks filter
        logger->flush_on(spdlog::level::warn);   // Auto-flush on warnings and errors

        spdlog::set_default_logger(logger);
        spdlog::info("Logging initialized");
    } catch (const spdlog::spdlog_ex& ex) {
        // Fallback to basic console logging if initialization fails
        spdlog::error("Logger initialization failed: {}", ex.what());
    }
}

/**
 * Shutdown the logging system and flush all pending messages.
 */
inline void shutdown() {
    spdlog::shutdown();
}

/**
 * Set the log level for the console sink.
 * @param level The minimum log level to display on console
 */
inline void set_console_level(spdlog::level::level_enum level) {
    auto logger = spdlog::default_logger();
    if (logger && !logger->sinks().empty()) {
        logger->sinks()[0]->set_level(level);
    }
}

/**
 * Enable or disable debug-level logging on console.
 * @param enable If true, shows debug messages; if false, only info and above
 */
inline void set_debug_mode(bool enable) {
    set_console_level(enable ? spdlog::level::debug : spdlog::level::info);
}

} // namespace logging

// Convenience macros for logging with source location
// These use SPDLOG_* macros which include file:line information

#define LOG_TRACE(...)    SPDLOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...)    SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...)     SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...)     SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...)    SPDLOG_ERROR(__VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)

// Conditional logging (useful for expensive operations)
#define LOG_DEBUG_IF(condition, ...) \
    do { if (condition) SPDLOG_DEBUG(__VA_ARGS__); } while(0)

#define LOG_INFO_IF(condition, ...) \
    do { if (condition) SPDLOG_INFO(__VA_ARGS__); } while(0)
