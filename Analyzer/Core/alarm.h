#ifndef ANALYZER_GENERATEALARMVIDEO_H
#define ANALYZER_GENERATEALARMVIDEO_H
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "event.h"
#include "frame_recorder.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}

namespace avs_analyzer {

class Config;
struct Frame;

struct Alarm {
  Alarm() = delete;
  Alarm(std::chrono::time_point<std::chrono::system_clock> happenTimestamp,
        std::string_view controlCode, std::unique_ptr<Event> event, int width,
        int height, int fps = 25);
  ~Alarm() = default;

  bool Finalize(Config& config);

  std::chrono::time_point<std::chrono::system_clock> start_ts{};      // 发生事件的时间戳（毫秒级）
  std::string control_code;  // 布控编号
  std::unique_ptr<Event> event;
  int width, height, fps;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_GENERATEALARMVIDEO_H