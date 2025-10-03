#ifndef ANALYZER_ALGORITHM_H
#define ANALYZER_ALGORITHM_H

#include <opencv2/opencv.hpp>  //opencv header file
#include <string>
#include <vector>

namespace avs_analyzer {
class Config;

static const std::vector<cv::Scalar> kDetectionColors = {
    cv::Scalar(0, 0, 255),    cv::Scalar(0, 255, 0),
    cv::Scalar(255, 0, 0),    cv::Scalar(255, 100, 50),
    cv::Scalar(50, 100, 255), cv::Scalar(255, 50, 100)};

cv::Mat static PadToSquare(const cv::Mat& source) {
  int col = source.cols;
  int row = source.rows;
  int _max = MAX(col, row);
  cv::Mat result = cv::Mat::zeros(_max, _max, CV_8UC3);
  source.copyTo(result(cv::Rect(0, 0, col, row)));
  return result;
};

struct Detection {
  std::vector<cv::Rect> bbox;
  std::vector<float> confidences;
  std::vector<int> class_id;
};

class Algorithm {
 public:
  Algorithm() = delete;
  Algorithm(Config* config);
  virtual ~Algorithm();

 public:
  virtual Detection DetectObjects(cv::Mat& image) = 0;
  bool CreateState();

 protected:
  Config* config_;
  bool create_state_ = false;  // 创建状态，默认false
};

}  // namespace avs_analyzer
#endif  // ANALYZER_ALGORITHM_H
