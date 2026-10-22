// YOLOv8-detect ONNXRuntime C++20 (Linux-only, detect-only)
#ifndef ANALYZER_INFERENCE_H
#define ANALYZER_INFERENCE_H
#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>
#include <array>
#include <string>
#include <vector>

#include "inference_types.h"

class YOLOv8Detect {
 public:
  YOLOv8Detect() = default;
  ~YOLOv8Detect() = default;

  // Linux only, detect-only
  const char* CreateSession(const DL_INIT_PARAM& p);

  std::vector<DL_RESULT> RunSession(const cv::Mat& bgr);

 private:
  struct PreprocInfo {
    float scale = 1.f;
    int pad_x = 0;
    int pad_y = 0;
  };

  // centered letterbox BGR->RGB
  PreprocInfo letterbox_bgr_to_rgb_(const cv::Mat& src_bgr, int W, int H);

  static void hwc_to_nchw_norm_(const cv::Mat& rgb, float* out);

  static cv::Rect deletterbox_xyxy_(float x1, float y1, float x2, float y2, const PreprocInfo& pp);

  static cv::Rect deletterbox_xywh_(float cx, float cy, float w, float h, const PreprocInfo& pp);

  void rebuild_name_ptrs_();

  // ORT
  Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "yolov8-detect"};
  std::unique_ptr<Ort::Session> session_;
  Ort::RunOptions run_opts_{nullptr};

  // names
  std::vector<std::string> input_names_s_, output_names_s_;
  std::vector<const char*> input_names_, output_names_;

  // cfg
  float conf_th_ = 0.25f;
  float iou_th_  = 0.45f;
  std::array<int,2> img_size_{640, 640};
  bool cuda_ = false;

  // Preprocessing scratch buffers. One YOLOv8Detect instance is owned by a
  // single inference thread, so these are never shared across threads.
  cv::Mat rgb_;
  cv::Mat canvas_;
  cv::Mat resized_;
  std::vector<float> blob_;
  std::array<int64_t, 4> input_dims_{1, 3, 0, 0};
};
#endif