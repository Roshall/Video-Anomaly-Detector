#ifndef ANALYZER_GENERATEALARMVIDEO_H
#define ANALYZER_GENERATEALARMVIDEO_H

#include <memory>
#include <string>
#include <chrono>
#include "event.h"

namespace avs_analyzer {

struct Alarm {
  Alarm() = delete;
  Alarm(std::string upload_dir, std::string admin_host, int width, int height,
        int fps = 25);
  ~Alarm() = default;
  Alarm(const Alarm&) = delete;
  Alarm& operator=(const Alarm&) = delete;
  Alarm(Alarm&&) noexcept = default;
  Alarm& operator=(Alarm&&) noexcept = default;

  void Finalize(std::unique_ptr<Event> event,
                std::chrono::time_point<std::chrono::system_clock> start_ts,
                const std::string& monitor_code) const;

  std::string upload_dir;
  std::string admin_host;
  int width, height, fps;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_GENERATEALARMVIDEO_H
