#ifndef ANALYZER_ANALYZER_H
#define ANALYZER_ANALYZER_H
#include <opencv2/opencv.hpp>
#include <vector>

namespace avs_analyzer {
struct Monitor;
class Config;
class Scheduler;
class Algorithm;
struct Detection;

class Analyzer {
 public:
  Analyzer(Monitor* monitor, const Config& config, float conf_th);
  ~Analyzer() = default;

  [[nodiscard]] std::vector<cv::Rect> ProcessVideoFrame(const cv::Mat& image) const;

 private:
  Monitor* monitor_;
  std::unique_ptr<Algorithm> algorithm_;
  int minimum_num_{1};
  float conf_th_;

};
}  // namespace avs_analyzer
#endif  // ANALYZER_ANALYZER_H
