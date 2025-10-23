//
// Created by lg on 10/23/25.
//
#include "log.h"

namespace avs_analyzer {

LoggerSingleton& LoggerSingleton::instance() {
  static LoggerSingleton instance;
  return instance;
}

// --- Private Constructor: Logger Setup ---

LoggerSingleton::LoggerSingleton() {
  // 1. Create the custom formatter
  auto formatter = std::make_unique<LevelBasedFormatter>();

  // 2. Create a Console Sink (supports color output)
  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

  // 3. Apply the custom formatter to the sink
  console_sink->set_formatter(std::move(formatter));

  // 4. Create the logger with the sink
  const std::string logger_name = "van";
  logger_ = std::make_shared<spdlog::logger>(logger_name, console_sink);

  // 5. Configure the logger
  logger_->set_level(spdlog::level::debug); // Set default level to DEBUG to see all custom levels
  logger_->flush_on(spdlog::level::err);   // Ensure ERROR messages are immediately flushed

  // 6. Optionally set this as spdlog's default logger
  // spdlog::set_default_logger(logger_);
  // Note: It's often safer to use the dedicated get_logger() method than setting the global default.
}

// --- Accessor Method ---

std::shared_ptr<spdlog::logger> LoggerSingleton::get_logger() const {
  return logger_;
}
void LoggerSingleton::set_log_level(std::string_view level_str) {
  using spdlog::level::level_enum;
  // to lower case
  std::string level_lower {level_str};
  std::transform(level_lower.begin(), level_lower.end(), level_lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });

  level_enum level;
  if (level_lower == "info") {
    level = level_enum::info;
  } else if (level_lower == "warn") {
    level = level_enum::warn;
  } else if (level_lower == "debug") {
    level = level_enum::debug;
  } else if (level_lower == "error") {
    level = level_enum::err;
  } else {
    logger_->error("unknow level {}, back to default.", level_str);
    return;
  }

  if (logger_) {
    logger_->set_level(level);
    // Inform the user of the change (optional)
    // logger_->info("Log level set to: {}", spdlog::level::to_string_view(level));
  }
}

} // namespace avs_analyzer