#ifndef ANALYZER_MONITOR_H
#define ANALYZER_MONITOR_H

#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

#include "Utils/common.h"
#include "task.h"

namespace avs_analyzer {

enum class LevelMode{
  kNumPeople,
  kDuration,
};

struct Monitor {
  // 布控请求必需参数
  uint64_t id{NextId()};
  std::string code;           // 布控编号
  std::string video_code;
  std::string source_type;   // stream or file
  std::string stream_url;      // 拉流地址
  bool push_stream = false;    // 是否推流
  std::string push_stream_url;  // 推流地址
  Task task;
  float conf_threshold = 0.5f; // 置信度阈值

  std::string roi_str;

  std::pair<std::vector<int>, std::vector<std::string>> levels;
  // 算法识别区域坐标点 x1, y1, x2, y2, x3, y3, x4, y4
  std::vector<cv::Point> roi_points{};  // 算法识别区域
  // 通过计算获得的参数

  LevelMode level_mode; // 0 -> number of people, 1 -> time
  int64_t start_timestamp = 0;  // 执行器启动时毫秒级时间戳（13位）

  bool operator==(const Monitor& other) const {
    return code == other.code;
  }

  bool ParseRecognitionRegion(int video_width, int video_height) {
    bool res = false;
    if (video_width > 0 && video_height > 0) {
      std::vector<std::string> xys = split(roi_str, ",");
      size_t lineCount = xys.size() / 2;  // 多边形边数
      if (lineCount > 3) {
        for (size_t i = 0; i < lineCount; i++) {
          size_t x_index = i * 2;
          double x = stod(xys[x_index]) * video_width;
          double y = stod(xys[x_index + 1]) * video_height;
          roi_points.emplace_back(x, y);
        }
        res = true;
      }
    }

    return res;
  }
  bool ValidateAdd(std::string& result_msg) const {
    if (code.empty() || stream_url.empty() || roi_str.empty()) {
      result_msg = "validate parameter error";
      return false;
    }
    if (push_stream) {
      if (push_stream_url.empty()) {
        result_msg =
            "validate parameter pushStreamUrl is error: " + push_stream_url;
        return false;
      }
    }
    result_msg = "validate success";
    return true;
  }
  bool ValidateCancel(std::string& result_msg) const {
    if (code.empty()) {
      result_msg = "validate parameter error";
      return false;
    }
    result_msg = "validate success";
    return true;
  }
  static uint64_t NextId() {
    static uint64_t id = 0;
    return id++;
  }
};
}  // namespace avs_analyzer
#endif  // ANALYZER_MONITOR_H
