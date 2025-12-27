//
// Ascend Pipeline - MindX SDK Integration
// Manages VDEC decoding, VPC preprocessing, and model inference on Huawei Atlas 300V Pro
//
#ifndef ANALYZER_ASCEND_PIPELINE_H
#define ANALYZER_ASCEND_PIPELINE_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ascend_common.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace avs_analyzer {
namespace ascend {

// Forward declaration of implementation details
class AscendPipelineImpl;

//
// AscendPipeline: High-level interface for Huawei Atlas 300V Pro video processing
//
// This class encapsulates the MindX SDK pipeline for:
// 1. Hardware video decoding (VDEC) - H.264/H.265 to YUV420SP
// 2. Hardware image preprocessing (VPC) - Resize, crop, color conversion
// 3. Hardware inference (ACL) - YOLOv8 detection with .om models
//
// The pipeline supports:
// - Dynamic stream addition/removal (up to 128+ concurrent channels)
// - Zero-copy frame sharing between multiple models
// - Dynamic AIPP for per-model input preprocessing
//
class AscendPipeline {
 public:
  // Create pipeline on specified device
  explicit AscendPipeline(int device_id = 0);
  ~AscendPipeline();

  // Non-copyable
  AscendPipeline(const AscendPipeline&) = delete;
  AscendPipeline& operator=(const AscendPipeline&) = delete;

  // Initialize the Ascend device and context
  AscendError Initialize();

  // Shutdown and release all resources
  void Shutdown();

  //
  // VDEC Channel Management
  //

  // Create a new VDEC channel for a stream
  // Returns channel_id on success, -1 on failure
  int CreateVdecChannel(const AscendPipelineConfig& config);

  // Destroy a VDEC channel
  void DestroyVdecChannel(int channel_id);

  // Send compressed packet to VDEC for decoding
  // The packet should contain H.264/H.265 NAL units
  AscendError SendPacket(int channel_id, AVPacket* packet);

  // Set callback for decoded frames from a channel
  void SetFrameCallback(int channel_id, FrameCallback callback);

  //
  // Model Management
  //

  // Load an .om model and return model_id
  // Returns model_id on success, -1 on failure
  int LoadModel(const AscendModelConfig& config);

  // Unload a model
  void UnloadModel(int model_id);

  // Link a model to receive frames from a VDEC channel
  // This enables zero-copy inference on decoded frames
  void LinkModelToChannel(int model_id, int channel_id);

  // Unlink a model from a channel
  void UnlinkModelFromChannel(int model_id, int channel_id);

  // Set callback for inference results
  void SetInferenceCallback(int model_id, InferenceCallback callback);

  //
  // Direct Inference (for manual frame feeding)
  //

  // Run inference on a decoded frame (zero-copy if from VDEC)
  std::vector<AscendDetection> RunInference(int model_id, 
                                            const DecodedFrame& frame);

  // Run inference on host memory (CPU Mat) - will copy to device
  std::vector<AscendDetection> RunInference(int model_id, 
                                            const cv::Mat& bgr_image);

  //
  // VPC Preprocessing
  //

  // Resize frame using hardware VPC
  // Returns new DecodedFrame with resized data in device memory
  std::unique_ptr<DecodedFrame> VpcResize(const DecodedFrame& input,
                                          int output_width, 
                                          int output_height);

  // Convert YUV420SP to BGR using hardware VPC
  cv::Mat VpcYuvToBgr(const DecodedFrame& frame);

  //
  // Status and Info
  //

  bool IsInitialized() const;
  int DeviceId() const { return device_id_; }
  int ActiveChannelCount() const;
  int LoadedModelCount() const;

 private:
  int device_id_;
  std::unique_ptr<AscendPipelineImpl> impl_;
  std::atomic<bool> initialized_{false};
};

//
// AscendPipelineManager: Singleton manager for all Ascend pipelines
// Handles device initialization and provides global resource management
//
class AscendPipelineManager {
 public:
  static AscendPipelineManager& Instance();

  // Initialize Ascend runtime (call once at startup)
  AscendError InitializeRuntime();

  // Finalize Ascend runtime (call once at shutdown)
  void FinalizeRuntime();

  // Get or create pipeline for a device
  std::shared_ptr<AscendPipeline> GetPipeline(int device_id = 0);

  // Check if Ascend hardware is available
  static bool IsAscendAvailable();

  // Get device count
  static int GetDeviceCount();

 private:
  AscendPipelineManager() = default;
  ~AscendPipelineManager();

  std::mutex mutex_;
  std::unordered_map<int, std::shared_ptr<AscendPipeline>> pipelines_;
  std::atomic<bool> runtime_initialized_{false};
};

}  // namespace ascend
}  // namespace avs_analyzer

#endif  // ANALYZER_ASCEND_PIPELINE_H
