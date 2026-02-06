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
  
  // Report HLS stream .m3u8 path to admin for live playback
  void ReportHlsToAdmin(const std::string& playlist_path,
                       std::chrono::time_point<std::chrono::system_clock> start_ts,
                       const std::string& video_code) const;
  
  void SetHlsRecorder(std::unique_ptr<Recorder> recorder) {
    hls_recorder = std::move(recorder);
  }
  
  // Add frames from an intermediate event slice to the active HLS recorder
  void AddFrameToHls(const Event& event) const;
  
  // Finalize HLS recording: close, reencode to MP4, and report (async)
  void FinializeHlsEvent(std::unique_ptr<Event> event,
                         std::chrono::time_point<std::chrono::system_clock> start_ts,
                         const std::string& video_code) const;

private:
  // Helper function to post alarm video to admin
  void PostAlarmVideoToAdmin(const std::string& video_code,
                             const std::string& video_type,
                             const std::string& video_path,
                             const std::string& create_time,
                             int64_t file_size = 0) const;

public:
  std::string upload_dir;
  std::string admin_host;
  int width, height, fps;
  int64_t id;
  mutable std::unique_ptr<Recorder> hls_recorder;
  mutable bool hls_reported{false};  // Track if HLS stream has been reported to admin
};

}  // namespace avs_analyzer

#endif  // ANALYZER_GENERATEALARMVIDEO_H
