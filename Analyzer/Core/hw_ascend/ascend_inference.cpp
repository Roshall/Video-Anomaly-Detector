//
// Ascend Inference Engine Implementation
//
#include "ascend_inference.h"
#include "../Utils/log.h"

#include <algorithm>
#include <cmath>

namespace avs_analyzer {
namespace ascend {

AscendInferenceEngineImpl::AscendInferenceEngineImpl(int device_id)
    : device_id_(device_id) {}

AscendInferenceEngineImpl::~AscendInferenceEngineImpl() {
  running_.store(false);
  queue_cv_.notify_all();
  
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }

  if (model_id_ >= 0 && pipeline_) {
    pipeline_->UnloadModel(model_id_);
  }
}

bool AscendInferenceEngineImpl::Initialize(const InferenceConfig& config) {
  config_ = config;

#ifdef ENABLE_ASCEND
  // Get pipeline from manager
  pipeline_ = AscendPipelineManager::Instance().GetPipeline(device_id_);
  if (!pipeline_) {
    LOGE("AscendInferenceEngine: failed to get pipeline for device {}", 
         device_id_);
    return false;
  }

  // Load model
  AscendModelConfig model_config;
  model_config.model_path = config.model_path;
  model_config.input_width = config.input_width;
  model_config.input_height = config.input_height;
  model_config.conf_threshold = config.conf_threshold;
  model_config.iou_threshold = config.iou_threshold;
  model_config.use_dynamic_aipp = true;

  model_id_ = pipeline_->LoadModel(model_config);
  if (model_id_ < 0) {
    LOGE("AscendInferenceEngine: failed to load model from {}", 
         config.model_path);
    return false;
  }

  // Start async worker thread
  running_.store(true);
  worker_thread_ = std::thread(&AscendInferenceEngineImpl::AsyncWorker, this);

  LOGI("AscendInferenceEngine: initialized model {} from {} ({}x{})", 
       model_id_, config.model_path, config.input_width, config.input_height);
  return true;
#else
  LOGW("AscendInferenceEngine: MindX SDK not available");
  return false;
#endif
}

std::vector<DetectionResult> AscendInferenceEngineImpl::Infer(
    const cv::Mat& bgr_image) {
#ifdef ENABLE_ASCEND
  if (!pipeline_ || model_id_ < 0) {
    return {};
  }

  // Preprocess
  cv::Mat preprocessed;
  auto preproc = Preprocess(bgr_image, preprocessed);

  // Run inference via pipeline
  auto ascend_results = pipeline_->RunInference(model_id_, preprocessed);

  // Convert to DetectionResult
  std::vector<DetectionResult> results;
  results.reserve(ascend_results.size());
  
  for (const auto& r : ascend_results) {
    // De-letterbox the coordinates
    int x1 = static_cast<int>((r.box.x - preproc.pad_x) / preproc.scale);
    int y1 = static_cast<int>((r.box.y - preproc.pad_y) / preproc.scale);
    int x2 = static_cast<int>((r.box.x + r.box.width - preproc.pad_x) / preproc.scale);
    int y2 = static_cast<int>((r.box.y + r.box.height - preproc.pad_y) / preproc.scale);
    
    // Clamp to image bounds
    x1 = std::max(0, std::min(x1, bgr_image.cols - 1));
    y1 = std::max(0, std::min(y1, bgr_image.rows - 1));
    x2 = std::max(0, std::min(x2, bgr_image.cols));
    y2 = std::max(0, std::min(y2, bgr_image.rows));
    
    if (x2 > x1 && y2 > y1) {
      results.push_back({
        r.class_id, 
        r.confidence, 
        cv::Rect(x1, y1, x2 - x1, y2 - y1)
      });
    }
  }

  // Apply NMS
  ApplyNMS(results);

  return results;
#else
  (void)bgr_image;
  return {};
#endif
}

std::vector<DetectionResult> AscendInferenceEngineImpl::InferDevice(
    void* device_ptr, int width, int height) {
#ifdef ENABLE_ASCEND
  if (!pipeline_ || model_id_ < 0 || !device_ptr) {
    return {};
  }

  // Create DecodedFrame wrapper
  DecodedFrame frame;
  frame.device_ptr = device_ptr;
  frame.width = width;
  frame.height = height;
  frame.is_device_memory = true;

  // With Dynamic AIPP, the model will handle resize internally
  // No need to preprocess on host
  auto ascend_results = pipeline_->RunInference(model_id_, frame);

  // Calculate preproc info for de-letterboxing
  PreprocInfo preproc;
  float scale_x = static_cast<float>(config_.input_width) / width;
  float scale_y = static_cast<float>(config_.input_height) / height;
  preproc.scale = std::min(scale_x, scale_y);
  int new_w = static_cast<int>(width * preproc.scale);
  int new_h = static_cast<int>(height * preproc.scale);
  preproc.pad_x = (config_.input_width - new_w) / 2;
  preproc.pad_y = (config_.input_height - new_h) / 2;

  // Convert results
  std::vector<DetectionResult> results;
  results.reserve(ascend_results.size());
  
  for (const auto& r : ascend_results) {
    int x1 = static_cast<int>((r.box.x - preproc.pad_x) / preproc.scale);
    int y1 = static_cast<int>((r.box.y - preproc.pad_y) / preproc.scale);
    int x2 = static_cast<int>((r.box.x + r.box.width - preproc.pad_x) / preproc.scale);
    int y2 = static_cast<int>((r.box.y + r.box.height - preproc.pad_y) / preproc.scale);
    
    x1 = std::max(0, std::min(x1, width - 1));
    y1 = std::max(0, std::min(y1, height - 1));
    x2 = std::max(0, std::min(x2, width));
    y2 = std::max(0, std::min(y2, height));
    
    if (x2 > x1 && y2 > y1) {
      results.push_back({r.class_id, r.confidence, cv::Rect(x1, y1, x2 - x1, y2 - y1)});
    }
  }

  ApplyNMS(results);
  return results;
#else
  (void)device_ptr;
  (void)width;
  (void)height;
  return {};
#endif
}

void AscendInferenceEngineImpl::InferAsync(
    const InferenceResourceID& id,
    const cv::Mat& bgr_image,
    InferenceResultCallback callback) {
#ifdef ENABLE_ASCEND
  InferTask task;
  task.id = id;
  task.image = bgr_image.clone();  // Clone for async processing
  task.callback = std::move(callback);

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    task_queue_.push(std::move(task));
  }
  queue_cv_.notify_one();
#else
  (void)id;
  (void)bgr_image;
  (void)callback;
#endif
}

void AscendInferenceEngineImpl::LinkToVdecChannel(int channel_id) {
#ifdef ENABLE_ASCEND
  if (pipeline_ && model_id_ >= 0) {
    pipeline_->LinkModelToChannel(model_id_, channel_id);
    LOGI("AscendInferenceEngine: linked model {} to VDEC channel {}", 
         model_id_, channel_id);
  }
#else
  (void)channel_id;
#endif
}

void AscendInferenceEngineImpl::UnlinkFromVdecChannel(int channel_id) {
#ifdef ENABLE_ASCEND
  if (pipeline_ && model_id_ >= 0) {
    pipeline_->UnlinkModelFromChannel(model_id_, channel_id);
  }
#else
  (void)channel_id;
#endif
}

void AscendInferenceEngineImpl::SetResultCallback(
    InferenceResultCallback callback) {
  default_callback_ = std::move(callback);
}

AscendInferenceEngineImpl::PreprocInfo 
AscendInferenceEngineImpl::Preprocess(const cv::Mat& bgr_image, cv::Mat& output) {
  PreprocInfo info;
  
  int img_w = bgr_image.cols;
  int img_h = bgr_image.rows;
  int target_w = config_.input_width;
  int target_h = config_.input_height;

  // Calculate scale and padding for letterbox
  float scale_x = static_cast<float>(target_w) / img_w;
  float scale_y = static_cast<float>(target_h) / img_h;
  info.scale = std::min(scale_x, scale_y);
  
  int new_w = static_cast<int>(img_w * info.scale);
  int new_h = static_cast<int>(img_h * info.scale);
  info.pad_x = (target_w - new_w) / 2;
  info.pad_y = (target_h - new_h) / 2;

  // Resize
  cv::Mat resized;
  cv::resize(bgr_image, resized, cv::Size(new_w, new_h));

  // Create letterbox with padding
  output = cv::Mat(target_h, target_w, CV_8UC3, cv::Scalar(114, 114, 114));
  resized.copyTo(output(cv::Rect(info.pad_x, info.pad_y, new_w, new_h)));

  return info;
}

std::vector<DetectionResult> AscendInferenceEngineImpl::Postprocess(
    void* output_buffer, size_t output_size, const PreprocInfo& preproc) {
  // YOLOv8 output parsing
  // Format: [batch, 84, 8400] or [batch, num_classes+4, num_predictions]
  // Row 0-3: cx, cy, w, h
  // Row 4+: class scores
  
  (void)output_buffer;
  (void)output_size;
  (void)preproc;
  
  // Real implementation would parse the output tensor
  // This is a placeholder
  return {};
}

void AscendInferenceEngineImpl::ApplyNMS(std::vector<DetectionResult>& detections) {
  if (detections.size() <= 1) return;

  // Sort by confidence
  std::sort(detections.begin(), detections.end(),
            [](const auto& a, const auto& b) { 
              return a.confidence > b.confidence; 
            });

  std::vector<bool> suppressed(detections.size(), false);
  std::vector<DetectionResult> result;
  result.reserve(detections.size());

  for (size_t i = 0; i < detections.size(); ++i) {
    if (suppressed[i]) continue;
    result.push_back(detections[i]);

    for (size_t j = i + 1; j < detections.size(); ++j) {
      if (suppressed[j]) continue;
      if (detections[i].class_id != detections[j].class_id) continue;

      // Calculate IoU
      auto& a = detections[i].box;
      auto& b = detections[j].box;
      
      int x1 = std::max(a.x, b.x);
      int y1 = std::max(a.y, b.y);
      int x2 = std::min(a.x + a.width, b.x + b.width);
      int y2 = std::min(a.y + a.height, b.y + b.height);
      
      if (x2 <= x1 || y2 <= y1) continue;
      
      float intersection = (x2 - x1) * (y2 - y1);
      float union_area = a.width * a.height + b.width * b.height - intersection;
      float iou = intersection / union_area;

      if (iou > config_.iou_threshold) {
        suppressed[j] = true;
      }
    }
  }

  detections = std::move(result);
}

void AscendInferenceEngineImpl::AsyncWorker() {
#ifdef ENABLE_ASCEND
  while (running_.load()) {
    InferTask task;
    
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] { 
        return !task_queue_.empty() || !running_.load(); 
      });
      
      if (!running_.load() && task_queue_.empty()) {
        break;
      }
      
      if (task_queue_.empty()) continue;
      
      task = std::move(task_queue_.front());
      task_queue_.pop();
    }

    // Process task
    std::vector<DetectionResult> results;
    if (task.device_ptr) {
      results = InferDevice(task.device_ptr, task.width, task.height);
    } else if (!task.image.empty()) {
      results = Infer(task.image);
    }

    // Invoke callback
    if (task.callback) {
      task.callback(task.id, results);
    } else if (default_callback_) {
      default_callback_(task.id, results);
    }
  }
#endif
}

std::unique_ptr<IInferenceEngine> CreateAscendInferenceEngine(int device_id) {
  return std::make_unique<AscendInferenceEngineImpl>(device_id);
}

}  // namespace ascend
}  // namespace avs_analyzer
