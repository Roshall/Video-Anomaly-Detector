#pragma once
// YOLOv8-detect ONNXRuntime C++20 (Linux-only, detect-only)

#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>
#include <array>
#include <string>
#include <vector>
#include <numeric>
#include <algorithm>
#include <stdexcept>

struct DL_INIT_PARAM {
  std::string modelPath;
  float rectConfidenceThreshold = 0.25f;
  float iouThreshold = 0.45f;
  std::array<int,2> imgSize{640, 640}; // {W, H}
  bool cudaEnable = false;             // optional, Linux CUDA EP
  int intraOpNumThreads = 0;
  int logSeverityLevel = 2;
  int target_id = 0;
};

struct DL_RESULT {
  int classId;
  float confidence;
  cv::Rect box;
};

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
  static PreprocInfo letterbox_bgr_to_rgb_(const cv::Mat& src_bgr, int W, int H, cv::Mat& dst_rgb);

  static void hwc_to_nchw_norm_(const cv::Mat& rgb, float* out);

  static cv::Rect deletterbox_xyxy_(float x1, float y1, float x2, float y2, const PreprocInfo& pp);

  static cv::Rect deletterbox_xywh_(float cx, float cy, float w, float h, const PreprocInfo& pp);

  void rebuild_name_ptrs_();

  // ORT
  Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "yolov8-detect"};
  Ort::SessionOptions sess_opt_;
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
};
