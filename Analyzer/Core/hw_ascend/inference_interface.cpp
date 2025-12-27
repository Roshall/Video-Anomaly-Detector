//
// Inference Interface Implementation
// Provides ONNX Runtime adapter and factory for creating inference engines
//
#include "inference_interface.h"
#include "../detector/inference.h"
#include "../Utils/log.h"

#include <mutex>
#include <thread>

namespace avs_analyzer {

//
// ONNX Runtime Adapter - Wraps existing YOLOv8Detect class
//
class OnnxInferenceEngine : public IInferenceEngine {
 public:
  explicit OnnxInferenceEngine(bool use_cuda) : use_cuda_(use_cuda) {}
  
  ~OnnxInferenceEngine() override = default;

  bool Initialize(const InferenceConfig& config) override {
    config_ = config;
    
    DL_INIT_PARAM params;
    params.modelPath = config.model_path;
    params.rectConfidenceThreshold = config.conf_threshold;
    params.iouThreshold = config.iou_threshold;
    params.imgSize = {config.input_width, config.input_height};
    params.cudaEnable = use_cuda_;
    params.target_id = config.target_class_id;

    detector_ = std::make_unique<YOLOv8Detect>();
    const char* error = detector_->CreateSession(params);
    
    if (error && error[0] != '\0') {
      LOGE("OnnxInferenceEngine: Failed to create session: {}", error);
      detector_.reset();
      return false;
    }

    LOGI("OnnxInferenceEngine: Initialized {} model from {}", 
         use_cuda_ ? "CUDA" : "CPU", config.model_path);
    return true;
  }

  std::vector<DetectionResult> Infer(const cv::Mat& bgr_image) override {
    if (!detector_) {
      return {};
    }

    auto dl_results = detector_->RunSession(bgr_image);
    
    std::vector<DetectionResult> results;
    results.reserve(dl_results.size());
    
    for (const auto& r : dl_results) {
      results.push_back({r.classId, r.confidence, r.box});
    }
    
    return results;
  }

  std::vector<DetectionResult> InferDevice(void* device_ptr, 
                                           int width, int height) override {
    // ONNX Runtime doesn't support direct device memory input in our wrapper
    // This would require copying to host first
    (void)device_ptr;
    (void)width;
    (void)height;
    LOGW("OnnxInferenceEngine: Device memory input not supported, use Infer() with cv::Mat");
    return {};
  }

  void InferAsync(const InferenceResourceID& id,
                  const cv::Mat& bgr_image,
                  InferenceResultCallback callback) override {
    // Simple async implementation using thread
    // Note: The existing YOLODetector class has a more sophisticated queue-based async
    std::thread([this, id, bgr_image, callback]() {
      auto results = Infer(bgr_image);
      if (callback) {
        callback(id, results);
      }
    }).detach();
  }

  bool SupportsDeviceMemory() const override { return false; }
  
  int InputWidth() const override { return config_.input_width; }
  int InputHeight() const override { return config_.input_height; }
  std::string ModelPath() const override { return config_.model_path; }

 private:
  bool use_cuda_;
  InferenceConfig config_;
  std::unique_ptr<YOLOv8Detect> detector_;
};

//
// Ascend ACL Inference Engine (stub - requires MindX SDK)
//
class AscendInferenceEngine : public IInferenceEngine {
 public:
  AscendInferenceEngine() = default;
  ~AscendInferenceEngine() override = default;

  bool Initialize(const InferenceConfig& config) override {
    config_ = config;
    
#ifdef ENABLE_ASCEND
    // Real implementation would:
    // 1. Get AscendPipeline from manager
    // 2. Load .om model
    // 3. Configure dynamic AIPP if enabled
    LOGI("AscendInferenceEngine: Initialized from {}", config.model_path);
    model_id_ = 0;  // Would be set by LoadModel
    return true;
#else
    LOGW("AscendInferenceEngine: MindX SDK not available");
    return false;
#endif
  }

  std::vector<DetectionResult> Infer(const cv::Mat& bgr_image) override {
#ifdef ENABLE_ASCEND
    // Real implementation would:
    // 1. Copy image to device
    // 2. Run inference
    // 3. Parse results
    (void)bgr_image;
    return {};
#else
    (void)bgr_image;
    return {};
#endif
  }

  std::vector<DetectionResult> InferDevice(void* device_ptr, 
                                           int width, int height) override {
#ifdef ENABLE_ASCEND
    // Real implementation would:
    // 1. Create input tensor from device pointer
    // 2. Set AIPP parameters for resize
    // 3. Run inference
    // 4. Parse output tensor
    (void)device_ptr;
    (void)width;
    (void)height;
    return {};
#else
    (void)device_ptr;
    (void)width;
    (void)height;
    return {};
#endif
  }

  void InferAsync(const InferenceResourceID& id,
                  const cv::Mat& bgr_image,
                  InferenceResultCallback callback) override {
#ifdef ENABLE_ASCEND
    // Real implementation would use ACL async APIs
    std::thread([this, id, bgr_image, callback]() {
      auto results = Infer(bgr_image);
      if (callback) {
        callback(id, results);
      }
    }).detach();
#else
    (void)id;
    (void)bgr_image;
    (void)callback;
#endif
  }

  bool SupportsDeviceMemory() const override { 
#ifdef ENABLE_ASCEND
    return true;
#else
    return false;
#endif
  }
  
  int InputWidth() const override { return config_.input_width; }
  int InputHeight() const override { return config_.input_height; }
  std::string ModelPath() const override { return config_.model_path; }

 private:
  InferenceConfig config_;
  int model_id_{-1};
};

//
// Factory Implementation
//

std::unique_ptr<IInferenceEngine> CreateInferenceEngine(InferenceBackend backend) {
  switch (backend) {
    case InferenceBackend::kOnnxCpu:
      return std::make_unique<OnnxInferenceEngine>(false);
    
    case InferenceBackend::kOnnxCuda:
      return std::make_unique<OnnxInferenceEngine>(true);
    
    case InferenceBackend::kAscendAcl:
      return std::make_unique<AscendInferenceEngine>();
    
    default:
      LOGE("CreateInferenceEngine: Unknown backend {}", static_cast<int>(backend));
      return nullptr;
  }
}

InferenceBackend DetectBestBackend() {
#ifdef ENABLE_ASCEND
  // Check if Ascend is available
  // Real implementation would call aclrtGetDeviceCount()
  return InferenceBackend::kAscendAcl;
#else
  // Check if CUDA is available (simplified check)
  // Real implementation would check CUDA runtime
  #ifdef __linux__
    // Try CUDA first on Linux
    return InferenceBackend::kOnnxCuda;
  #else
    return InferenceBackend::kOnnxCpu;
  #endif
#endif
}

}  // namespace avs_analyzer
