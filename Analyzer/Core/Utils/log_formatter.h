#ifndef ANALYZER_LOG_FORMATTER_H
#define ANALYZER_LOG_FORMATTER_H

#include <spdlog/spdlog.h>
#include <spdlog/pattern_formatter.h>
#include <string>
#include <map>
namespace avs_analyzer {

/**
 * @brief Custom spdlog formatter that uses different patterns based on the log level.
 * * Implements the spdlog::formatter interface.
 */
class LevelBasedFormatter final : public spdlog::formatter {
 public:
  /**
     * @brief Constructor. Initializes the patterns for each log level.
   */
  LevelBasedFormatter();

  /**
     * @brief Formats the log message based on its level.
     * @param msg The log message to format.
     * @param dest The output buffer to write the formatted message to.
   */
  void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override;

  /**
     * @brief Creates a copy of the formatter (required by spdlog::formatter interface).
     * @return std::unique_ptr<spdlog::formatter> A pointer to the copied object.
   */
  std::unique_ptr<spdlog::formatter> clone() const override;

 private:
  // Map to store pre-configured pattern formatters for each log level.
  std::map<spdlog::level::level_enum, std::unique_ptr<spdlog::formatter>> level_formatters_;

  /**
     * @brief Creates a PatternFormatter with a specific pattern and time zone offset.
     * @param pattern The spdlog pattern string.
     * @param tz_offset_hours The time zone offset in hours (e.g., 8 for UTC+8/Beijing time).
     * @return std::unique_ptr<spdlog::formatter> The configured formatter.
   */
  static std::unique_ptr<formatter> create_formatter(const std::string& pattern, int tz_offset_hours);
};


} // namespace avs_analyzer

#endif // ANALYZER_LOG_FORMATTER_H