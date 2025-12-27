//
// Huawei Ascend Common Definitions
//
#ifndef ANALYZER_ASCEND_COMMON_H
#define ANALYZER_ASCEND_COMMON_H

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

#include <opencv2/opencv.hpp>

namespace avs_analyzer {
namespace ascend {

// Forward declarations for Ascend types (defined when SDK is available)
struct AscendContext;
struct DeviceBuffer;

// Result from Ascend inference (compatible with DL_RESULT)
struct AscendDetection {
  int class_id;
  float confidence;
  cv::Rect box;
};

// Decoded frame information from VDEC
struct DecodedFrame {
  void* device_ptr{nullptr};       // Pointer to device memory (YUV420SP)
  int width{0};
  int height{0};
  int stride{0};
  int64_t pts{0};
  int64_t timestamp_ms{0};
  
  // For zero-copy sharing between models
  bool is_device_memory{true};
};

// Configuration for Ascend pipeline
struct AscendPipelineConfig {
  int device_id{0};                  // NPU device ID
  int vdec_channel_id{-1};           // VDEC channel (-1 for auto-allocation)
  std::string codec_type{"h264"};    // h264, h265
  int input_width{1920};
  int input_height{1080};
  int output_width{640};             // For model input after resize
  int output_height{640};
  bool enable_vpc_resize{true};      // Use VPC for hardware resize
  bool enable_dynamic_aipp{true};    // Dynamic AIPP for preprocessing
};

// Model configuration for Ascend inference
struct AscendModelConfig {
  std::string model_path;            // Path to .om file
  int input_width{640};
  int input_height{640};
  float conf_threshold{0.25f};
  float iou_threshold{0.45f};
  bool use_dynamic_aipp{true};
};

// Callback types
using FrameCallback = std::function<void(std::unique_ptr<DecodedFrame>)>;
using InferenceCallback = std::function<void(uint64_t frame_id, 
                                             const std::vector<AscendDetection>&)>;

// Error codes
enum class AscendError {
  kSuccess = 0,
  kDeviceNotFound,
  kChannelAllocationFailed,
  kModelLoadFailed,
  kInferenceFailed,
  kDecodeFailed,
  kMemoryError,
  kInvalidParameter,
  kNotInitialized
};

inline const char* AscendErrorToString(AscendError err) {
  switch (err) {
    case AscendError::kSuccess: return "Success";
    case AscendError::kDeviceNotFound: return "Device not found";
    case AscendError::kChannelAllocationFailed: return "Channel allocation failed";
    case AscendError::kModelLoadFailed: return "Model load failed";
    case AscendError::kInferenceFailed: return "Inference failed";
    case AscendError::kDecodeFailed: return "Decode failed";
    case AscendError::kMemoryError: return "Memory error";
    case AscendError::kInvalidParameter: return "Invalid parameter";
    case AscendError::kNotInitialized: return "Not initialized";
    default: return "Unknown error";
  }
}

}  // namespace ascend
}  // namespace avs_analyzer

#endif  // ANALYZER_ASCEND_COMMON_H
