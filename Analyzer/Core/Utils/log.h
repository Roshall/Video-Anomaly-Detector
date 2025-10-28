#ifndef ANALYZER_LOG_H
#define ANALYZER_LOG_H
#include "log_formatter.h"

#include <memory>
#include <source_location>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace avs_analyzer {
  
/**
 * @brief Singleton class to manage the single instance of the spdlog logger.
 * * Provides thread-safe, lazy initialization of the logger with the custom formatter.
 */
class LoggerSingleton final {
 public:
  /**
     * @brief Gets the single instance of the LoggerSingleton.
     * @return LoggerSingleton& Reference to the Singleton instance.
   */
  static LoggerSingleton& instance();

  /**
     * @brief Gets the shared pointer to the configured spdlog logger.
     * @return std::shared_ptr<spdlog::logger> The logger instance.
   */
  std::shared_ptr<spdlog::logger> get_logger() const;

  /**
     * @brief Sets the logging level for the internal spdlog logger.
     * @param level_str The new logging level (e.g., spdlog::level::info).
   */
  void set_log_level(std::string_view level_str);

  // Prevent copy, move, and assignment operations to enforce Singleton property
  LoggerSingleton(const LoggerSingleton&) = delete;
  LoggerSingleton& operator=(const LoggerSingleton&) = delete;
  LoggerSingleton(LoggerSingleton&&) = delete;
  LoggerSingleton& operator=(LoggerSingleton&&) = delete;

 private:
  /**
     * @brief Private constructor to prevent direct instantiation.
     * * Initializes the spdlog logger with the custom LevelBasedFormatter.
   */
  LoggerSingleton();

  // The single logger instance managed by the Singleton
  std::shared_ptr<spdlog::logger> logger_;
};

[[nodiscard]] constexpr spdlog::source_loc to_spdlog_loc(
    const std::source_location& location)
{
  return spdlog::source_loc{
    location.file_name(),
    static_cast<int>(location.line()),
    location.function_name()
};
}

// Convenient macro to access the logger throughout the application
#define LOGGER ::avs_analyzer::LoggerSingleton::instance().get_logger()

// Convenience macros
#define LOGI(...) SPDLOG_INFO(__VA_ARGS__)
#define LOGW(...) SPDLOG_WARN(__VA_ARGS__)
#define LOGD(...) SPDLOG_DEBUG(__VA_ARGS__)
#define LOGE(...) SPDLOG_ERROR(__VA_ARGS__)

}  // namespace avs_analyzer
#endif  // ANALYZER_LOG_H