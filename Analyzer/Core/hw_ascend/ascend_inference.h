//
// Ascend Inference Engine - Hardware inference using Huawei Atlas ACL
//
#ifndef ANALYZER_ASCEND_INFERENCE_ENGINE_H
#define ANALYZER_ASCEND_INFERENCE_ENGINE_H

#include "inference_interface.h"
#include "ascend_common.h"
#include "ascend_pipeline.h"

#include <memory>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>

namespace avs_analyzer {
namespace ascend {

//
// Ascend Inference Engine - Full implementation with ACL
//
// Features:
// - Hardware inference on Atlas 300V Pro NPU
// - Dynamic AIPP for preprocessing (resize, color conversion)
// - Zero-copy input from VDEC decoded frames
// - Async inference with callback support
// - Batch inference for improved throughput
//
class AscendInferenceEngineImpl : public IInferenceEngine {
 public:
  explicit AscendInferenceEngineImpl(int device_id = 0);
  ~AscendInferenceEngineImpl() override;

  bool Initialize(const InferenceConfig& config) override;
  
  std::vector<DetectionResult> Infer(const cv::Mat& bgr_image) override;
  std::vector<DetectionResult> InferDevice(void* device_ptr, 
                                           int width, int height) override;
  void InferAsync(const InferenceResourceID& id,
                  const cv::Mat& bgr_image,
                  InferenceResultCallback callback) override;

  bool SupportsDeviceMemory() const override { return true; }
  int InputWidth() const override { return config_.input_width; }
  int InputHeight() const override { return config_.input_height; }
  std::string ModelPath() const override { return config_.model_path; }

  // Link to VDEC channel for automatic zero-copy inference
  void LinkToVdecChannel(int channel_id);
  void UnlinkFromVdecChannel(int channel_id);

  // Set result callback (alternative to InferAsync)
  void SetResultCallback(InferenceResultCallback callback);

 private:
  // Internal inference task
  struct InferTask {
    InferenceResourceID id;
    cv::Mat image;                    // For host memory input
    void* device_ptr{nullptr};        // For device memory input
    int width{0};
    int height{0};
    InferenceResultCallback callback;
  };

  // Preprocessing: letterbox resize + padding
  struct PreprocInfo {
    float scale{1.0f};
    int pad_x{0};
    int pad_y{0};
  };

  // Preprocess image for inference (host memory version)
  PreprocInfo Preprocess(const cv::Mat& bgr_image, cv::Mat& output);

  // Postprocess inference output to detections
  std::vector<DetectionResult> Postprocess(void* output_buffer, 
                                           size_t output_size,
                                           const PreprocInfo& preproc);

  // Apply NMS to detections
  void ApplyNMS(std::vector<DetectionResult>& detections);

  // Worker thread for async inference
  void AsyncWorker();

  int device_id_;
  InferenceConfig config_;
  std::shared_ptr<AscendPipeline> pipeline_;
  int model_id_{-1};

  // Async processing
  std::atomic<bool> running_{false};
  std::thread worker_thread_;
  std::queue<InferTask> task_queue_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;

  InferenceResultCallback default_callback_;
};

//
// Factory function for Ascend inference engine
//
std::unique_ptr<IInferenceEngine> CreateAscendInferenceEngine(int device_id = 0);

}  // namespace ascend
}  // namespace avs_analyzer

#endif  // ANALYZER_ASCEND_INFERENCE_ENGINE_H
