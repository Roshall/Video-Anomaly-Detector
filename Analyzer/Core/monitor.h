#ifndef ANALYZER_MONITOR_H
#define ANALYZER_MONITOR_H

#include <cctype>
#include <nlohmann/json.hpp>
#include <memory>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

#include "Utils/common.h"
#include "Utils/log.h"
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
  // 对于某些任务（如 kBunkOff）支持多个多边形 ROI
  std::vector<std::vector<cv::Point>> roi_polygons{};
  // 通过计算获得的参数

  LevelMode level_mode; // 0 -> number of people, 1 -> time
  int64_t start_timestamp = 0;  // 执行器启动时毫秒级时间戳（13位）

  bool operator==(const Monitor& other) const {
    return code == other.code;
  }
  static std::string trim_ws(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
      ++b;
    }
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
      --e;
    }
    return s.substr(b, e - b);
  }

  bool ParseRecognitionRegion(int video_width, int video_height) {
    bool res = false;
    roi_points.clear();
    roi_polygons.clear();
    if (video_width > 0 && video_height > 0) {
      // kBunkOff: roi_str can be a JSON array of polygon strings, e.g.
      // ["0.1,0.2,0.3,0.4,...","..."]
      const auto parse_one_polygon = [&](const std::string& polygon_str,
                                         std::vector<cv::Point>& out_points) -> bool {
        out_points.clear();
        std::vector<std::string> xys = split(polygon_str, ",");
        const size_t point_count = xys.size() / 2;
        if (point_count <= 3 || (xys.size() % 2) != 0) {
          return false;
        }
        for (size_t i = 0; i < point_count; i++) {
          const size_t x_index = i * 2;
          double x = stod(xys[x_index]) * video_width;
          double y = stod(xys[x_index + 1]) * video_height;
          out_points.emplace_back(x, y);
        }
        return true;
      };

      try {
        if (task == Task::kBunkOff) {
          // Detect JSON array input.
          const std::string trimmed = trim_ws(roi_str);
          if (!trimmed.empty() && trimmed.front() == '[') {
            nlohmann::json root;
            try {
              root = nlohmann::json::parse(trimmed);
            } catch (const std::exception& e) {
              LOGE("ParseRecognitionRegion: invalid JSON array for kBunkOff: {}\nroi_str: {}", e.what(), roi_str);
              return false;
            }
            if (!root.is_array()) {
              LOGE("ParseRecognitionRegion: invalid JSON array for kBunkOff\nroi_str: {}", roi_str);
              return false;
            }
            for (const auto& item : root) {
              if (!item.is_string()) {
                LOGE("ParseRecognitionRegion: non-string polygon entry for kBunkOff\nroi_str: {}", roi_str);
                return false;
              }
              std::vector<cv::Point> poly;
              if (!parse_one_polygon(item.get<std::string>(), poly)) {
                LOGE("ParseRecognitionRegion: invalid polygon entry for kBunkOff\nroi_str: {}", roi_str);
                return false;
              }
              roi_polygons.emplace_back(std::move(poly));
            }
            if (!roi_polygons.empty()) {
              res = true;
            }
            return res;
          }
          // Fall back to single polygon format for kBunkOff if not JSON.
        }

        // Default: single polygon string "x1,y1,x2,y2,..."
        std::vector<cv::Point> poly;
        if (parse_one_polygon(roi_str, poly)) {
          roi_points = poly;
          roi_polygons.emplace_back(std::move(poly));
          res = true;
        } else {
          res = false;
        }
      } catch (const std::exception& e) {
        LOGE("ParseRecognitionRegion exception: {}\nroi_str: {}", e.what(), roi_str);
        res = false;
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
