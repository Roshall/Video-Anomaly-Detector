#ifndef ANALYZER_ALGORITHM_H
#define ANALYZER_ALGORITHM_H

#include <opencv2/opencv.hpp>  //opencv header file
#include <string>
#include <vector>

namespace avs_analyzer {
struct Detection {
  std::vector<cv::Rect> bbox;
  std::vector<float> confidences;
};

class Algorithm {
 public:
  Algorithm() = default;
  virtual ~Algorithm();

 public:
  virtual Detection DetectObjects(const cv::Mat& image) = 0;
  bool CreateState() const;

 protected:
  bool create_state_{false};  // 创建状态，默认false
};

}  // namespace avs_analyzer
#endif  // ANALYZER_ALGORITHM_H
