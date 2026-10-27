// YOLOv8-detect ONNXRuntime C++20 (Linux-only, detect-only)
#ifndef ANALYZER_INFERENCE_H
#define ANALYZER_INFERENCE_H
#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>
#include <array>
#include <string>
#include <vector>

#include "inference_types.h"
#include "io/tensor.h"

class YOLOv8Detect {
 public:
  YOLOv8Detect() = default;
  ~YOLOv8Detect() = default;

  // Linux only, detect-only
  const char* CreateSession(const DL_INIT_PARAM& p);

  /// letterbox 的几何信息。见 io/tensor.h 中的同名类型：这里直接复用中性
  /// 层的定义，避免"同一件事两个类型"。
  using PreprocInfo = avs_analyzer::io::LetterboxInfo;

  /// 旧路径：内部完成 letterbox + BGR→RGB + HWC→NCHW + /255，然后跑模型。
  /// 保留它是为了回归对照——新路径必须逐像素等价于它。
  std::vector<DL_RESULT> RunSession(const cv::Mat& bgr);

  /// 独立的一步：只做预处理，产出带 letterbox 元信息的张量。
  /// 不触碰 session，因此未加载模型也能调用。
  [[nodiscard]] avs_analyzer::io::HostTensor Preprocess(const cv::Mat& bgr);

  /// 新路径（解耦后）：只跑模型。张量由谁产出、跑在 CPU 还是 NPU 上做的
  /// 预处理，本函数一概不关心。
  std::vector<DL_RESULT> RunSession(const avs_analyzer::io::HostTensor& input,
                                    const PreprocInfo& pp);

  /// 把模型输出的框从 letterbox 画布坐标还原回原图坐标。
  [[nodiscard]] static cv::Rect DeletterboxXYXY(float x1, float y1, float x2,
                                                float y2, const PreprocInfo& pp);
  [[nodiscard]] static cv::Rect DeletterboxXYWH(float cx, float cy, float w,
                                                float h, const PreprocInfo& pp);

 private:
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