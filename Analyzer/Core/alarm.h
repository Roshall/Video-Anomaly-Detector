#ifndef ANALYZER_GENERATEALARMVIDEO_H
#define ANALYZER_GENERATEALARMVIDEO_H
#include <cstdint>
#include <memory>
#include <string>
#include <chrono>
#include <optional>
#include "event.h"
#include "video_processing/frame_recorder.h"

namespace avs_analyzer {

struct Alarm {
  Alarm() = delete;
  Alarm(std::string upload_dir, std::string admin_host, int width, int height, int64_t id = 0,
        int fps = 25);
  ~Alarm() = default;
  Alarm(const Alarm&) = delete;
  Alarm& operator=(const Alarm&) = delete;
  Alarm(Alarm&&) noexcept = default;
  Alarm& operator=(Alarm&&) noexcept = default;

  void Finalize(std::unique_ptr<Event> event,
                std::chrono::time_point<std::chrono::system_clock> start_ts,
                const std::string& video_code) const;
  
  void SetId(int64_t new_id) { id = new_id; }
  
  void SetHlsRecorder(std::unique_ptr<Recorder> recorder) {
    hls_recorder = std::move(recorder);
  }
  
  // Add frames from an intermediate event slice to the active HLS recorder
  void AddFrameToHls(const Event& event) const;
  
  // Finalize HLS recording: close, reencode to MP4, and report (async)
  void FinializeHlsEvent(std::unique_ptr<Event> event,
                         std::chrono::time_point<std::chrono::system_clock> start_ts,
                         const std::string& video_code) const;

  std::string upload_dir;
  std::string admin_host;
  int width, height, fps;
  int64_t id;
  mutable std::unique_ptr<Recorder> hls_recorder;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_GENERATEALARMVIDEO_H
