#ifndef ANALYZER_CONFIG_H
#define ANALYZER_CONFIG_H

#include <string>
#include <vector>

namespace avs_analyzer {
struct Config {
  explicit Config(const char* file);
  ~Config() = default;

  bool state = false;
  void Show();

  std::string host{};        // 主机IP地址 192.168.1.4
  std::string admin_host{};  // 后台管理服务地址 http://192.168.1.4:9001
  int admin_port;            // 后台管理服务端口 9001
  int analyzer_port;         // 分析服务端口 9002
  int media_http_port;       // 80
  int media_rtsp_port;       // 554

  std::string upload_dir{};
  std::string model_dir{};
  // std::string video_file_name_format{};//%Y%m%d%H%M%S
};
}  // namespace avs_analyzer
#endif  // ANALYZER_CONFIG_H