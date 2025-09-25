#ifndef ANALYZER_YOLOALGORITHM_H
#define ANALYZER_YOLOALGORITHM_H

#include <onnxruntime_cxx_api.h>

#include <mutex>
#include <string>
#include <vector>

#include "algorithm.h"

namespace avs_analyzer {
class Config;
class OnnxRuntimeEngine {
 public:
  explicit OnnxRuntimeEngine(Config* config, std::string& modelPath,
                             std::vector<std::string>& classNames);
  ~OnnxRuntimeEngine();


  bool RunInference(cv::Mat& image, std::vector<Detection>& detects);

 private:
  Config* config_;
  std::vector<std::string> class_names_;
  std::string model_path_;
  Ort::Env env_{nullptr};
  Ort::SessionOptions session_options_{nullptr};
  Ort::Session session_{nullptr};
};
class YoloAlgorithm : public Algorithm {
 public:
  YoloAlgorithm(Config* config, std::string& modelPath,
                std::vector<std::string>& classNames);
  virtual ~YoloAlgorithm();


  virtual bool DetectObjects(cv::Mat& image, std::vector<Detection>& detects);

 private:
  std::vector<std::string> class_names_;
  std::mutex mutex_;
  OnnxRuntimeEngine* engine_;
};
}  // namespace avs_analyzer
#endif  // ANALYZER_YOLOALGORITHM_H
