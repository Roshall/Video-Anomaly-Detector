#ifndef ANALYZER_GENERATEALARMVIDEO_H
#define ANALYZER_GENERATEALARMVIDEO_H
#include <cstdint>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "event.h"
#include "video_processing/frame_recorder.h"

namespace avs_analyzer {

struct AlarmJob;

// Owns the HLS recorder for one combined event. Created, fed, and closed by
// the detect thread so finalization never touches recorder state.
class HlsEventWriter {
 public:
  HlsEventWriter(int width, int height, int fps, std::string base_path,
                 const Recorder::HlsConfig& hls_config);
  ~HlsEventWriter() = default;
  HlsEventWriter(const HlsEventWriter&) = delete;
  HlsEventWriter& operator=(const HlsEventWriter&) = delete;

  [[nodiscard]] bool valid() const noexcept { return recorder_ != nullptr; }
  [[nodiscard]] std::optional<Recorder::HlsPaths> paths() const;
  void AddFrames(const Event& event);
  // Closes the recorder and returns the finalized HLS paths.
  std::optional<Recorder::HlsPaths> Close();

 private:
  std::unique_ptr<Recorder> recorder_;
  std::optional<Recorder::HlsPaths> paths_;
};

// Immutable alarm output settings shared by all jobs of a worker.
struct Alarm {
  enum class OutputMode { kAuto, kMp4Only };

  Alarm() = delete;
  Alarm(std::string upload_dir, std::string admin_host, int width, int height,
        int64_t id = 0, int fps = 25, OutputMode mode = OutputMode::kAuto,
        bool report_delta = false);
  ~Alarm() = default;
  Alarm(const Alarm&) = delete;
  Alarm& operator=(const Alarm&) = delete;

  // Writes/reports one immutable alarm job. Safe to call concurrently for
  // different jobs.
  void Finalize(AlarmJob&& job) const;

  // Report HLS stream .m3u8 path to admin for live playback
  void ReportHlsToAdmin(const std::string& playlist_path,
                        std::chrono::time_point<std::chrono::system_clock> start_ts,
                        const std::string& video_code) const;

 private:
  void FinalizeMp4Event(
      const Event* event,
      std::chrono::time_point<std::chrono::system_clock> start_ts,
      const std::string& video_code) const;
  // Helper function to post alarm video to admin
  void PostAlarmVideoToAdmin(const std::string& video_code,
                             const std::string& video_type,
                             const std::string& video_path,
                             const std::string& create_time,
                             int64_t file_size = 0) const;

 public:
  const std::string upload_dir;
  const std::string admin_host;
  const int width;
  const int height;
  const int fps;
  const int64_t id;
  const bool report_delta;
  const OutputMode output_mode;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_GENERATEALARMVIDEO_H
