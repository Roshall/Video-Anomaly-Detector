#ifndef ANALYZER_CONFIG_H
#define ANALYZER_CONFIG_H

#include <string>
#include <vector>

#include "video_processing/frame_recorder.h"

namespace avs_analyzer {
struct Config {
  explicit Config(const char* file);

  bool state = false;
  int thread_pool_size{3};
  // Dedicated pools: offline file analysis and alarm I/O.
  int offline_threads{2};
  int alarm_threads{2};
  // Bound on queued alarm side effects; newest jobs are dropped when full.
  int max_pending_alarm_jobs{128};
  u_int32_t file_cache_timeout{3600}; // 1 hour
  int rtsp_probe_timeout_ms{5000}; // RTSP probe timeout in milliseconds
  int inference_workers_per_model{2};
  int inference_workers_per_model_accelerated{1};
  int inference_result_timeout_ms{5000};
  int offline_pipeline_depth{4};
  // Per-worker cap for buffered pre-roll / event frames.
  int event_buffer_budget_mb{256};
  // Bounded per-stream push queue; newest frames are dropped when full.
  int push_queue_max_frames{50};
  // Reconnect policy for pull streams.
  int stream_reconnect_max_attempts{50};
  int stream_reconnect_backoff_ms{500};
  int stream_reconnect_backoff_max_ms{10000};
  std::vector<std::string> preload_models{"yolov8n.onnx", "sleeping_m.onnx",
                                          "climbing_m.onnx",
                                          "phone_playing_m.onnx"};
  void Show();

  std::string admin_host{};  // 后台管理服务地址 http://192.168.1.4:9001
                             // 后台管理服务端口 9001
  std::string analyzer_host;         // 分析服务端口 9002
  std::string media_http_host;       // 80
  std::string media_rtsp_host;       // 554

  std::string upload_dir{};
  std::string offline_video_dir{};
  std::string model_dir{};
  bool use_accelerator{false};
  std::string log_level{"info"};
  std::string alarm_sound_path{};
  int alarm_sound_cooldown_sec{10};
  std::string font_path{"/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc"};
  std::string legend_position{"top-right"};
  int legend_prealloc_max_labels{6};
  int legend_prealloc_max_label_len{10};
  double bunkoff_overlap_ratio{0.3};
  // std::string video_file_name_format{};//%Y%m%d%H%M%S
  
  // HLS configuration for event recording
  Recorder::HlsConfig hls_config{};
};
}  // namespace avs_analyzer
#endif  // ANALYZER_CONFIG_H