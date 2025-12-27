//
// Stream Processor - Unified interface for video stream processing
// Supports both CPU (FFmpeg) and hardware (Ascend) backends
//
#ifndef ANALYZER_STREAM_PROCESSOR_H
#define ANALYZER_STREAM_PROCESSOR_H

#include <memory>
#include <functional>
#include <string>
#include <vector>
#include <atomic>

#include <opencv2/opencv.hpp>

#include "decoder_interface.h"
#include "inference_interface.h"
#include "ascend_common.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

// Forward declaration
struct SwsContext;

namespace avs_analyzer {

// Forward declarations
class Worker;

//
// Processed frame with detection results
//
struct ProcessedFrame {
  cv::Mat bgr_image;                          // Frame in BGR format (for annotation)
  int64_t pts{0};                             // Presentation timestamp
  int64_t timestamp_ms{0};                    // Timestamp in milliseconds
  std::vector<DetectionResult> detections;    // Detection results from inference
  bool is_keyframe{false};
  
  // For hardware path: raw decoded frame
  AVFrame* raw_frame{nullptr};                // May point to device memory
  void* device_ptr{nullptr};                  // Device memory pointer (if applicable)
};

using ProcessedFrameCallback = std::function<void(std::unique_ptr<ProcessedFrame>)>;

//
// StreamProcessor Configuration
//
struct StreamProcessorConfig {
  bool use_hardware{false};           // Use Ascend hardware if available
  int device_id{0};                   // NPU/GPU device ID
  
  // Decoder settings
  bool enable_hardware_decode{false}; // Use VDEC for decoding
  
  // Inference settings
  std::string model_path;
  float conf_threshold{0.25f};
  float iou_threshold{0.45f};
  int input_width{640};
  int input_height{640};
  int target_class_id{0};
  
  // Output settings
  int output_width{0};                // 0 = same as input
  int output_height{0};               // 0 = same as input
};

//
// StreamProcessor - Unified stream processing with backend switching
//
// This class provides a unified interface for:
// - Video decoding (FFmpeg CPU or Ascend VDEC)
// - Image preprocessing (Software or VPC hardware)
// - Model inference (ONNX CPU/CUDA or Ascend ACL)
//
// Usage:
//   auto processor = StreamProcessor::Create(config);
//   processor->Initialize(codec_params, time_base);
//   processor->SetFrameCallback([](auto frame) { /* process */ });
//   while (running) {
//     processor->SendPacket(packet);
//   }
//
class StreamProcessor {
 public:
  virtual ~StreamProcessor() = default;

  // Factory method - creates appropriate backend based on config
  static std::unique_ptr<StreamProcessor> Create(const StreamProcessorConfig& config);

  // Initialize with stream parameters
  virtual bool Initialize(AVCodecParameters* codec_params, 
                          AVRational time_base) = 0;

  // Send compressed packet for decoding
  virtual bool SendPacket(AVPacket* packet) = 0;

  // Receive processed frame with detections (synchronous)
  // Returns nullptr if no frame available
  virtual std::unique_ptr<ProcessedFrame> ReceiveFrame() = 0;

  // Set callback for async frame processing
  virtual void SetFrameCallback(ProcessedFrameCallback callback) = 0;

  // Model management (for multi-model support)
  virtual int AddModel(const InferenceConfig& config) = 0;
  virtual void RemoveModel(int model_id) = 0;

  // Get decoder info
  virtual int Width() const = 0;
  virtual int Height() const = 0;
  virtual AVPixelFormat PixelFormat() const = 0;

  // Check backend type
  virtual bool IsHardwareAccelerated() const = 0;

  // Flush decoder
  virtual void Flush() = 0;
};

//
// CPU Stream Processor - Uses FFmpeg for decoding, ONNX for inference
//
class CpuStreamProcessor : public StreamProcessor {
 public:
  explicit CpuStreamProcessor(const StreamProcessorConfig& config);
  ~CpuStreamProcessor() override;

  bool Initialize(AVCodecParameters* codec_params, 
                  AVRational time_base) override;
  bool SendPacket(AVPacket* packet) override;
  std::unique_ptr<ProcessedFrame> ReceiveFrame() override;
  void SetFrameCallback(ProcessedFrameCallback callback) override;
  int AddModel(const InferenceConfig& config) override;
  void RemoveModel(int model_id) override;
  int Width() const override;
  int Height() const override;
  AVPixelFormat PixelFormat() const override;
  bool IsHardwareAccelerated() const override { return false; }
  void Flush() override;

 private:
  StreamProcessorConfig config_;
  std::unique_ptr<FFmpegDecoder> decoder_;
  std::vector<std::unique_ptr<IInferenceEngine>> inference_engines_;
  ProcessedFrameCallback frame_callback_;
  AVRational time_base_{1, 1};

  // Format conversion for BGR output
  ::SwsContext* sws_ctx_{nullptr};
  AVFrame* bgr_frame_{nullptr};
};

//
// Hardware Stream Processor - Uses Ascend VDEC for decoding, ACL for inference
//
class HardwareStreamProcessor : public StreamProcessor {
 public:
  explicit HardwareStreamProcessor(const StreamProcessorConfig& config);
  ~HardwareStreamProcessor() override;

  bool Initialize(AVCodecParameters* codec_params, 
                  AVRational time_base) override;
  bool SendPacket(AVPacket* packet) override;
  std::unique_ptr<ProcessedFrame> ReceiveFrame() override;
  void SetFrameCallback(ProcessedFrameCallback callback) override;
  int AddModel(const InferenceConfig& config) override;
  void RemoveModel(int model_id) override;
  int Width() const override;
  int Height() const override;
  AVPixelFormat PixelFormat() const override;
  bool IsHardwareAccelerated() const override { return true; }
  void Flush() override;

 private:
  StreamProcessorConfig config_;
  std::unique_ptr<IVideoDecoder> decoder_;
  std::vector<std::pair<int, std::unique_ptr<IInferenceEngine>>> inference_engines_;
  ProcessedFrameCallback frame_callback_;
  AVRational time_base_{1, 1};
  int next_model_id_{0};
};

}  // namespace avs_analyzer

#endif  // ANALYZER_STREAM_PROCESSOR_H
