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
  explicit Analyzer(Scheduler* scheduler, Monitor* monitor);
  ~Analyzer() = default;

  std::vector<cv::Rect> ProcessVideoFrame(const cv::Mat& image) const;

 private:
  Scheduler* scheduler_;
  Monitor* monitor_;
  std::unique_ptr<Algorithm> algorithm_;
  int minimum_num{1};

};
}  // namespace avs_analyzer
#endif  // ANALYZER_ANALYZER_H
