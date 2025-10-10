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
  Alarm(int64_t happenTimestamp, std::string_view controlCode,
        std::unique_ptr<Event> event,
        const std::shared_ptr<Recorder>& recorder);
  ~Alarm() = default;

  bool Finalize(Config& config);

  int64_t start_ts = 0;      // 发生事件的时间戳（毫秒级）
  std::string control_code;  // 布控编号
  std::shared_ptr<Recorder> recorder;
  std::unique_ptr<Event> event;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_GENERATEALARMVIDEO_H