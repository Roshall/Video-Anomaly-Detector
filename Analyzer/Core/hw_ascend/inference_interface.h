//
// Inference Interface - Abstract interface for YOLOv8 inference
// Allows switching between ONNX Runtime (CPU/CUDA) and Ascend ACL backends
//
#ifndef ANALYZER_INFERENCE_INTERFACE_H
#define ANALYZER_INFERENCE_INTERFACE_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

namespace avs_analyzer {

// Detection result (same as DL_RESULT from inference.h)
struct DetectionResult {
  int class_id;
  float confidence;
  cv::Rect box;
};

// Configuration for inference engine
struct InferenceConfig {
  std::string model_path;
  float conf_threshold{0.25f};
  float iou_threshold{0.45f};
  int input_width{640};
  int input_height{640};
  int device_id{0};                  // Device ID (GPU/NPU)
  bool use_device_memory{false};     // Input is in device memory
  int target_class_id{0};            // For filtering specific class (0 = all)
};

// Resource identifier for async inference: (output_id, video_id, frame_ts)
using InferenceResourceID = std::array<uint64_t, 3>;

// Callback for async inference results
using InferenceResultCallback = std::function<void(
    const InferenceResourceID& id,
    const std::vector<DetectionResult>& results)>;

//
// Abstract interface for inference engines
//
class IInferenceEngine {
 public:
  virtual ~IInferenceEngine() = default;

  // Initialize the inference engine with config
  virtual bool Initialize(const InferenceConfig& config) = 0;

  // Run synchronous inference on BGR image (host memory)
  virtual std::vector<DetectionResult> Infer(const cv::Mat& bgr_image) = 0;

  // Run synchronous inference on device memory (for zero-copy)
  // device_ptr: pointer to device memory (YUV or RGB depending on model)
  // width, height: dimensions of the image
  virtual std::vector<DetectionResult> InferDevice(void* device_ptr, 
                                                   int width, int height) = 0;

  // Async inference with callback
  virtual void InferAsync(const InferenceResourceID& id,
                          const cv::Mat& bgr_image,
                          InferenceResultCallback callback) = 0;

  // Check if engine supports device memory input
  virtual bool SupportsDeviceMemory() const = 0;

  // Get input dimensions
  virtual int InputWidth() const = 0;
  virtual int InputHeight() const = 0;

  // Get model info
  virtual std::string ModelPath() const = 0;
};

//
// Inference Engine Factory
//
enum class InferenceBackend {
  kOnnxCpu,      // ONNX Runtime with CPU provider
  kOnnxCuda,     // ONNX Runtime with CUDA provider
  kAscendAcl     // Huawei Ascend ACL
};

// Create inference engine based on backend type
std::unique_ptr<IInferenceEngine> CreateInferenceEngine(InferenceBackend backend);

// Auto-detect best available backend
InferenceBackend DetectBestBackend();

}  // namespace avs_analyzer

#endif  // ANALYZER_INFERENCE_INTERFACE_H
