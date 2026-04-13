#ifndef ANALYZER_CONFIG_H
#define ANALYZER_CONFIG_H

#include <string>
#include "video_processing/frame_recorder.h"

namespace avs_analyzer {
struct Config {
  explicit Config(const char* file);

  bool state = false;
  int thread_pool_size{3};
  u_int32_t file_cache_timeout{3600}; // 1 hour
  int rtsp_probe_timeout_ms{5000}; // RTSP probe timeout in milliseconds
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