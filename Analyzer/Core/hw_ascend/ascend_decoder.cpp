//
// Ascend VDEC Decoder Implementation
//
#include "ascend_decoder.h"
#include "../Utils/log.h"

namespace avs_analyzer {
namespace ascend {

AscendDecoder::AscendDecoder(int device_id) : device_id_(device_id) {
  output_frame_ = av_frame_alloc();
}

AscendDecoder::~AscendDecoder() {
  if (vdec_channel_id_ >= 0 && pipeline_) {
    pipeline_->DestroyVdecChannel(vdec_channel_id_);
  }
  if (output_frame_) {
    av_frame_free(&output_frame_);
  }
}

bool AscendDecoder::Initialize(AVCodecParameters* codec_params, 
                               AVRational time_base) {
  if (!codec_params) {
    LOGE("AscendDecoder: null codec params");
    return false;
  }

  width_ = codec_params->width;
  height_ = codec_params->height;
  time_base_ = time_base;

  // Determine codec type
  std::string codec_type;
  switch (codec_params->codec_id) {
    case AV_CODEC_ID_H264:
      codec_type = "h264";
      break;
    case AV_CODEC_ID_HEVC:
      codec_type = "h265";
      break;
    default:
      LOGE("AscendDecoder: unsupported codec_id {}", 
           static_cast<int>(codec_params->codec_id));
      return false;
  }

#ifdef ENABLE_ASCEND
  // Get pipeline from manager
  pipeline_ = AscendPipelineManager::Instance().GetPipeline(device_id_);
  if (!pipeline_) {
    LOGE("AscendDecoder: failed to get pipeline for device {}", device_id_);
    return false;
  }

  // Create VDEC channel
  AscendPipelineConfig config;
  config.device_id = device_id_;
  config.codec_type = codec_type;
  config.input_width = width_;
  config.input_height = height_;

  vdec_channel_id_ = pipeline_->CreateVdecChannel(config);
  if (vdec_channel_id_ < 0) {
    LOGE("AscendDecoder: failed to create VDEC channel");
    return false;
  }

  // Set frame callback
  pipeline_->SetFrameCallback(vdec_channel_id_, 
      [this](std::unique_ptr<DecodedFrame> frame) {
        OnFrameDecoded(std::move(frame));
      });

  LOGI("AscendDecoder: initialized {}x{} {} on device {} channel {}", 
       width_, height_, codec_type, device_id_, vdec_channel_id_);
  return true;
#else
  LOGW("AscendDecoder: MindX SDK not available, cannot initialize");
  return false;
#endif
}

bool AscendDecoder::SendPacket(AVPacket* packet) {
#ifdef ENABLE_ASCEND
  if (!pipeline_ || vdec_channel_id_ < 0) {
    return false;
  }

  auto err = pipeline_->SendPacket(vdec_channel_id_, packet);
  if (err != AscendError::kSuccess) {
    LOGE("AscendDecoder: SendPacket failed: {}", AscendErrorToString(err));
    return false;
  }
  return true;
#else
  (void)packet;
  return false;
#endif
}

AVFrame* AscendDecoder::ReceiveFrame() {
#ifdef ENABLE_ASCEND
  std::unique_lock<std::mutex> lock(frame_mutex_);
  
  // Wait for a frame with timeout
  if (decoded_frames_.empty()) {
    frame_cv_.wait_for(lock, std::chrono::milliseconds(100), 
        [this] { return !decoded_frames_.empty(); });
  }

  if (decoded_frames_.empty()) {
    return nullptr;
  }

  auto decoded = std::move(decoded_frames_.front());
  decoded_frames_.pop();
  lock.unlock();

  // Populate output_frame_ with decoded frame info
  av_frame_unref(output_frame_);
  
  output_frame_->width = decoded->width;
  output_frame_->height = decoded->height;
  output_frame_->format = AV_PIX_FMT_NV12;  // VDEC outputs NV12
  output_frame_->pts = decoded->pts;
  output_frame_->best_effort_timestamp = decoded->pts;
  output_frame_->time_base = time_base_;
  
  // Store device pointer in data[0]
  // Note: This is a non-standard usage; normally data contains host pointers
  // The caller should use GetDevicePtr() to retrieve the device memory
  output_frame_->data[0] = static_cast<uint8_t*>(decoded->device_ptr);
  output_frame_->linesize[0] = decoded->stride;
  
  // Store ownership in opaque for cleanup
  // Real implementation would need proper lifetime management
  output_frame_->opaque = nullptr;

  return output_frame_;
#else
  return nullptr;
#endif
}

bool AscendDecoder::HasPendingFrames() const {
  std::lock_guard<std::mutex> lock(frame_mutex_);
  return !decoded_frames_.empty();
}

void AscendDecoder::Flush() {
#ifdef ENABLE_ASCEND
  // Flush VDEC channel
  // Real implementation would call appropriate VDEC flush API
  std::lock_guard<std::mutex> lock(frame_mutex_);
  while (!decoded_frames_.empty()) {
    decoded_frames_.pop();
  }
#endif
}

int AscendDecoder::Width() const { return width_; }
int AscendDecoder::Height() const { return height_; }

AVPixelFormat AscendDecoder::PixelFormat() const {
  return AV_PIX_FMT_NV12;  // VDEC outputs NV12
}

void* AscendDecoder::GetDevicePtr(AVFrame* frame) const {
  if (!frame || !IsDeviceMemory()) {
    return nullptr;
  }
  return frame->data[0];
}

void AscendDecoder::SetFrameCallback(FrameCallback callback) {
  user_callback_ = std::move(callback);
}

void AscendDecoder::OnFrameDecoded(std::unique_ptr<DecodedFrame> frame) {
  if (user_callback_) {
    // User wants async handling
    user_callback_(std::move(frame));
  } else {
    // Queue for ReceiveFrame()
    std::lock_guard<std::mutex> lock(frame_mutex_);
    decoded_frames_.push(std::move(frame));
    frame_cv_.notify_one();
  }
}

}  // namespace ascend
}  // namespace avs_analyzer
