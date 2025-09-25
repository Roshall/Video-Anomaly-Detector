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

  bool ProcessVideoFrame(int64_t frameCount, cv::Mat& image,
                         std::vector<Detection>& detection_results,
                         bool& happen, float& happenScore);

 private:
  bool PostImageToServer(int64_t frameCount, cv::Mat& image,
                         std::vector<Detection>& detection_results,
                         bool& happen, float& happenScore);

  Scheduler* scheduler_;
  Monitor* monitor_;
};
}  // namespace avs_analyzer
#endif  // ANALYZER_ANALYZER_H
