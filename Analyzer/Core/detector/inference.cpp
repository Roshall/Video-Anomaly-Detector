//
// Created by lg on 10/11/25.
//
#include "inference.h"

YOLOv8Detect::PreprocInfo YOLOv8Detect::letterbox_bgr_to_rgb_(
    const cv::Mat& src_bgr, int W, int H, cv::Mat& dst_rgb) {
  const float r = std::min(static_cast<float>(W) / src_bgr.cols,
                           static_cast<float>(H) / src_bgr.rows);
  const int new_w = static_cast<int>(std::round(src_bgr.cols * r));
  const int new_h = static_cast<int>(std::round(src_bgr.rows * r));
  const int pad_x = (W - new_w) / 2;
  const int pad_y = (H - new_h) / 2;

  cv::Mat resized;
  cv::resize(src_bgr, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);
  cv::Mat canvas(H, W, CV_8UC3, cv::Scalar(114,114,114));
  resized.copyTo(canvas(cv::Rect(pad_x, pad_y, new_w, new_h)));
  cv::cvtColor(canvas, dst_rgb, cv::COLOR_BGR2RGB);
  return {r, pad_x, pad_y};
}
void YOLOv8Detect::hwc_to_nchw_norm_(const cv::Mat& rgb, float* out) {
  const int H = rgb.rows, W = rgb.cols;
  const int HW = H * W;
  float* c0 = out;
  float* c1 = out + HW;
  float* c2 = out + 2*HW;
  constexpr float inv255 = 1.f/255.f;

  for (int y = 0; y < H; ++y) {
    const auto* row = rgb.ptr<cv::Vec3b>(y);
    for (int x = 0; x < W; ++x) {
      const cv::Vec3b px = row[x]; // RGB
      const float r = px[0] * inv255;
      const float g = px[1] * inv255;
      const float b = px[2] * inv255;
      const int i = y*W + x;
      c0[i] = r; c1[i] = g; c2[i] = b;
    }
  }
}
cv::Rect YOLOv8Detect::deletterbox_xyxy_(float x1, float y1, float x2, float y2,
                                         const YOLOv8Detect::PreprocInfo& pp) {
  const float nx1 = (x1 - pp.pad_x) / pp.scale;
  const float ny1 = (y1 - pp.pad_y) / pp.scale;
  const float nx2 = (x2 - pp.pad_x) / pp.scale;
  const float ny2 = (y2 - pp.pad_y) / pp.scale;
  int l = static_cast<int>(std::round(nx1));
  int t = static_cast<int>(std::round(ny1));
  int r = static_cast<int>(std::round(nx2));
  int b = static_cast<int>(std::round(ny2));
  return cv::Rect(cv::Point(l, t), cv::Point(r, b));
}
cv::Rect YOLOv8Detect::deletterbox_xywh_(float cx, float cy, float w, float h,
                                         const YOLOv8Detect::PreprocInfo& pp) {
  return deletterbox_xyxy_(cx - 0.5f*w, cy - 0.5f*h, cx + 0.5f*w, cy + 0.5f*h, pp);
}
void YOLOv8Detect::rebuild_name_ptrs_() {
  input_names_.clear(); output_names_.clear();
  input_names_.reserve(input_names_s_.size());
  output_names_.reserve(output_names_s_.size());
  for (auto& s : input_names_s_)  input_names_.push_back(s.c_str());
  for (auto& s : output_names_s_) output_names_.push_back(s.c_str());
}
std::vector<DL_RESULT> YOLOv8Detect::RunSession(const cv::Mat& bgr) {
  if (bgr.empty() || bgr.type() != CV_8UC3) return {};

  // preprocess (centered letterbox to target WxH, RGB, keep scale/pad)
  cv::Mat rgb;
  const auto pp = letterbox_bgr_to_rgb_(bgr, img_size_[0], img_size_[1], rgb);

  // NCHW FP32, normalized [0,1]
  const int W = rgb.cols, H = rgb.rows;
  const size_t HW = static_cast<size_t>(H) * W;
  std::vector<float> blob(3 * HW);
  hwc_to_nchw_norm_(rgb, blob.data());

  std::array<int64_t,4> in_dims{1, 3, H, W};
  Ort::Value input = Ort::Value::CreateTensor<float>(
      Ort::MemoryInfo::CreateCpu(OrtAllocatorType::OrtDeviceAllocator, OrtMemTypeCPU),
      blob.data(), blob.size(), in_dims.data(), in_dims.size());

  auto outs = session_->Run(run_opts_, input_names_.data(), &input, 1,
                            output_names_.data(), output_names_.size());

  // assume 1 output (Ultralytics default)
  auto& outv = outs.front();
  const auto shape = outv.GetTypeInfo().GetTensorTypeAndShapeInfo().GetShape();
  float* raw_ptr = outv.GetTensorMutableData<float>();

  // case A: end2end with NMS in-graph → [1, N, 6] (x1,y1,x2,y2,score,cls)
  if (shape.size() == 3 && shape[0] == 1 && shape[2] == 6) {
    const int N = static_cast<int>(shape[1]);
    cv::Mat det(N, 6, CV_32F, raw_ptr); // do not clone; read-only
    std::vector<DL_RESULT> out;
    out.reserve(N);
    for (int i = 0; i < N; ++i) {
      const float* d = det.ptr<float>(i);
      const float score = d[4];
      if (score < conf_th_) continue;
      const int cls = static_cast<int>(d[5]);
      cv::Rect box = deletterbox_xyxy_(d[0], d[1], d[2], d[3], pp);
      if (box.width <= 0 || box.height <= 0) continue;
      out.push_back({cls, score, box});
    }
    return out;
  }

  // case B: raw head → normalize to [84,8400] then transpose to [8400,84]
  cv::Mat M;
  if (shape.size() == 3 && shape[0] == 1) {
    M = cv::Mat((int)shape[1], (int)shape[2], CV_32F, raw_ptr); // [84,8400]
  } else if (shape.size() == 2) {
    M = cv::Mat((int)shape[0], (int)shape[1], CV_32F, raw_ptr); // [84,8400] or [8400,84]
  } else {
    throw std::runtime_error("Unsupported YOLOv8 output shape");
  }

  cv::Mat head = M;
  if (head.rows < head.cols) {
    // [84,8400] -> ok
  } else {
    head = head.t(); // [8400,84] -> transpose back later
  }
  // Now force to [84,8400]
  if (head.rows != 84) head = head.t(); // if [8400,84] => transpose
  if (head.rows != 84) throw std::runtime_error("Unexpected head rows, expect 84");

  cv::Mat dets = head.t(); // [8400,84], each row: [cx,cy,w,h, p0..p{num_cls-1}]
  const int num_preds = dets.rows;
  const int num_cols  = dets.cols;
  const int num_cls   = num_cols - 4;

  std::vector<cv::Rect> boxes;
  std::vector<float> scores;
  std::vector<int>   cls_ids;
  boxes.reserve(num_preds); scores.reserve(num_preds); cls_ids.reserve(num_preds);

  for (int i = 0; i < num_preds; ++i) {
    const float* d = dets.ptr<float>(i);
    const float cx = d[0], cy = d[1], w = d[2], h = d[3];
    // max class
    int best_id = 0;
    float best_sc = d[4];
    for (int c = 1; c < num_cls; ++c) {
      const float sc = d[4 + c];
      if (sc > best_sc) { best_sc = sc; best_id = c; }
    }
    if (best_sc < conf_th_) continue;

    cv::Rect box = deletterbox_xywh_(cx, cy, w, h, pp);
    if (box.width <= 0 || box.height <= 0) continue;

    boxes.push_back(box);
    scores.push_back(best_sc);
    cls_ids.push_back(best_id);
  }

  // NMS
  std::vector<int> keep;
  cv::dnn::NMSBoxes(boxes, scores, conf_th_, iou_th_, keep);

  std::vector<DL_RESULT> out;
  out.reserve(keep.size());
  for (int idx : keep) out.push_back({cls_ids[idx], scores[idx], boxes[idx]});
  return out;
}
const char* YOLOv8Detect::CreateSession(const DL_INIT_PARAM& p) {
  conf_th_  = p.rectConfidenceThreshold;
  iou_th_   = p.iouThreshold;
  img_size_ = p.imgSize;
  cuda_     = p.cudaEnable;

  Ort::SessionOptions sess_opt{};

  sess_opt.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  sess_opt.SetIntraOpNumThreads(p.intraOpNumThreads);
  sess_opt.SetLogSeverityLevel(p.logSeverityLevel);

#ifdef USE_CUDA
  if (cuda_) {
      OrtCUDAProviderOptions cuda_opt{};
      cuda_opt.device_id = 0;
      sess_opt.AppendExecutionProvider_CUDA(cuda_opt);
      sess_opt.SetIntraOpNumThreads(1);
    }
#else
  (void)cuda_; // unused if not built with CUDA
#endif

  session_ = std::make_unique<Ort::Session>(env_, p.modelPath.c_str(), sess_opt);

  // cache IO names safely
  Ort::AllocatorWithDefaultOptions alloc;
  size_t in_n  = session_->GetInputCount();
  size_t out_n = session_->GetOutputCount();
  input_names_s_.reserve(in_n);
  output_names_s_.reserve(out_n);
  for (size_t i = 0; i < in_n; ++i) {
    Ort::AllocatedStringPtr nm = session_->GetInputNameAllocated(i, alloc);
    input_names_s_.emplace_back(nm.get());
  }
  for (size_t i = 0; i < out_n; ++i) {
    Ort::AllocatedStringPtr nm = session_->GetOutputNameAllocated(i, alloc);
    output_names_s_.emplace_back(nm.get());
  }
  rebuild_name_ptrs_();

  // warm-up
  cv::Mat dummy(img_size_[1], img_size_[0], CV_8UC3, cv::Scalar(0,0,0));
  (void)RunSession(dummy);

  return "OK";
}
