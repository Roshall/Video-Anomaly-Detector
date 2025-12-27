//
// Ascend VDEC Decoder - Hardware video decoding using Huawei Atlas VDEC
//
#ifndef ANALYZER_ASCEND_DECODER_H
#define ANALYZER_ASCEND_DECODER_H

#include "decoder_interface.h"
#include "ascend_common.h"
#include "ascend_pipeline.h"

#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>

namespace avs_analyzer {
namespace ascend {

//
// Ascend VDEC Decoder Implementation
// Uses hardware VDEC on Atlas 300V Pro for video decoding
//
// Features:
// - Hardware H.264/H.265 decoding
// - Output in device memory (YUV420SP NV12)
// - Zero-copy sharing with inference models
// - Supports up to 128+ concurrent channels
//
class AscendDecoder : public IVideoDecoder {
 public:
  explicit AscendDecoder(int device_id = 0);
  ~AscendDecoder() override;

  bool Initialize(AVCodecParameters* codec_params, AVRational time_base) override;
  bool SendPacket(AVPacket* packet) override;
  AVFrame* ReceiveFrame() override;
  bool HasPendingFrames() const override;
  void Flush() override;
  
  int Width() const override;
  int Height() const override;
  AVPixelFormat PixelFormat() const override;
  bool IsDeviceMemory() const override { return true; }
  void* GetDevicePtr(AVFrame* frame) const override;

  // Get the underlying VDEC channel ID for linking with models
  int VdecChannelId() const { return vdec_channel_id_; }

  // Set callback for decoded frames (for async processing)
  void SetFrameCallback(FrameCallback callback);

 private:
  void OnFrameDecoded(std::unique_ptr<DecodedFrame> frame);

  int device_id_;
  int vdec_channel_id_{-1};
  std::shared_ptr<AscendPipeline> pipeline_;
  
  int width_{0};
  int height_{0};
  AVRational time_base_{1, 1};
  
  // Frame buffer for ReceiveFrame()
  std::queue<std::unique_ptr<DecodedFrame>> decoded_frames_;
  mutable std::mutex frame_mutex_;
  std::condition_variable frame_cv_;
  
  // AVFrame wrapper for returning to caller
  AVFrame* output_frame_{nullptr};
  
  FrameCallback user_callback_;
};

}  // namespace ascend
}  // namespace avs_analyzer

#endif  // ANALYZER_ASCEND_DECODER_H
