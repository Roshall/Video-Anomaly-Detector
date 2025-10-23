#include "log_formatter.h"
#include <spdlog/spdlog.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/details/os.h>
#include <spdlog/details/fmt_helper.h>
#include <memory>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>

namespace avs_analyzer {

// Pattern flags that are common across most levels
// %^%L%$ : Colorized log level
// %Y/%m/%d %H:%M:%S : Date and Time
// +0800 : Hardcoded offset for Beijing Time Zone (since set_tz_offset is removed)
// %v : The message payload

LevelBasedFormatter::LevelBasedFormatter() {
  using spdlog::level::level_enum;
  using spdlog::pattern_formatter;

  // --- Define Patterns ---

  // 1. INFO Level: SHORT message + Fixed Beijing Time Offset (+0800)
  // Pattern: [Level] YYYY/MM/DD HH:MM:SS +0800 Message
  std::string info_pattern = "[%^%L%$] %Y/%m/%d %H:%M:%S +0800 %v";

  // 2. WARNING Level: Adds Source File
  // Pattern: [Level] YYYY/MM/DD HH:MM:SS +0800 [Source File] Message
  std::string warn_pattern = "[%^%L%$] %Y/%m/%d %H:%M:%S +0800 [%s] %v";

  // 3. DEBUG/ERROR/TRACE/CRITICAL Level: Adds File, Line, Function, Thread ID [Ti]
  // Pattern: [Level] YYYY/MM/DD HH:MM:SS +0800 [File:Line] [Function] [Ti:ThreadID] Message
  // %s = source file, %# = line number, %! = function name, %t = thread id
  std::string detail_pattern = "[%^%L%$] %Y/%m/%d %H:%M:%S +0800 [%s:%#] [%!] [Ti:%t] %v";

  // --- Initialize Formatters ---

  // Note: spdlog::pattern_formatter::compile() is used to create the concrete formatter.
  level_formatters_[level_enum::info]     = std::make_unique<pattern_formatter>(info_pattern);
  level_formatters_[level_enum::warn]     = std::make_unique<pattern_formatter>(warn_pattern);
  level_formatters_[level_enum::debug]    = std::make_unique<pattern_formatter>(detail_pattern);
  level_formatters_[level_enum::err]      = std::make_unique<pattern_formatter>(detail_pattern);

  // Also include trace and critical for completeness, using the detailed pattern
  level_formatters_[level_enum::trace]    = std::make_unique<pattern_formatter>(detail_pattern);
  level_formatters_[level_enum::critical] = std::make_unique<pattern_formatter>(detail_pattern);
}

void LevelBasedFormatter::format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) {
  // Find the appropriate formatter for the current log message's level.
  auto it = level_formatters_.find(msg.level);

  if (it != level_formatters_.end()) {
    // Use the custom formatter for this level.
    it->second->format(msg, dest);
  } else {
    // Fallback: Use the INFO formatter as a safe default.
    level_formatters_[spdlog::level::info]->format(msg, dest);
  }
}

std::unique_ptr<spdlog::formatter> LevelBasedFormatter::clone() const {
  // spdlog requires this method to create a copy when a logger is cloned.
  return std::make_unique<LevelBasedFormatter>();
}
} // namespace avs_analyzer