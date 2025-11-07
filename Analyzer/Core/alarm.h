#ifndef ANALYZER_GENERATEALARMVIDEO_H
#define ANALYZER_GENERATEALARMVIDEO_H
#include <memory>
#include <string>

#include "event.h"

namespace avs_analyzer {

struct Alarm {
  Alarm() = delete;
  Alarm(std::chrono::time_point<std::chrono::system_clock> happenTimestamp,
        std::string controlCode, std::unique_ptr<Event> event, int width,
        int height, int fps = 25);
  ~Alarm() = default;
  Alarm(const Alarm&) = delete;
  Alarm& operator=(const Alarm&) = delete;
  Alarm(Alarm&&) noexcept = default;
  Alarm& operator=(Alarm&&) noexcept = default;

  bool Finalize(const std::string& upload_dir, const std::string& admin_host) const;

  std::chrono::time_point<std::chrono::system_clock> start_ts{}; // ms
  std::string control_code;
  std::unique_ptr<Event> event;
  int width, height, fps;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_GENERATEALARMVIDEO_H