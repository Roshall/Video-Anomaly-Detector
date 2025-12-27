//
// Abstract Decoder Interface
// Allows switching between FFmpeg (CPU) and Ascend VDEC (TPU) backends
//
#ifndef ANALYZER_DECODER_INTERFACE_H
#define ANALYZER_DECODER_INTERFACE_H

#include <memory>
#include <functional>
#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace avs_analyzer {

// Abstract interface for video decoders
class IVideoDecoder {
 public:
  virtual ~IVideoDecoder() = default;

  // Initialize the decoder with stream parameters
  virtual bool Initialize(AVCodecParameters* codec_params, AVRational time_base) = 0;
  
  // Send a compressed packet for decoding
  virtual bool SendPacket(AVPacket* packet) = 0;
  
  // Receive a decoded frame (returns nullptr if no frame available)
  // For CPU decoder: returns AVFrame in YUV format
  // For Ascend decoder: returns AVFrame with device memory pointer in data[0]
  virtual AVFrame* ReceiveFrame() = 0;
  
  // Check if decoder has pending frames
  virtual bool HasPendingFrames() const = 0;
  
  // Flush decoder (call when stream ends or seeks)
  virtual void Flush() = 0;
  
  // Get decoder info
  virtual int Width() const = 0;
  virtual int Height() const = 0;
  virtual AVPixelFormat PixelFormat() const = 0;
  
  // Check if frames are in device memory (GPU/NPU)
  virtual bool IsDeviceMemory() const = 0;
  
  // Get device memory pointer (if IsDeviceMemory() returns true)
  virtual void* GetDevicePtr(AVFrame* frame) const = 0;
};

// Factory function type for creating decoders
using DecoderFactory = std::function<std::unique_ptr<IVideoDecoder>(int device_id)>;

// FFmpeg software decoder implementation
class FFmpegDecoder : public IVideoDecoder {
 public:
  FFmpegDecoder() = default;
  ~FFmpegDecoder() override;

  bool Initialize(AVCodecParameters* codec_params, AVRational time_base) override;
  bool SendPacket(AVPacket* packet) override;
  AVFrame* ReceiveFrame() override;
  bool HasPendingFrames() const override;
  void Flush() override;
  
  int Width() const override;
  int Height() const override;
  AVPixelFormat PixelFormat() const override;
  bool IsDeviceMemory() const override { return false; }
  void* GetDevicePtr(AVFrame*) const override { return nullptr; }

 private:
  AVCodecContext* codec_ctx_{nullptr};
  AVFrame* frame_{nullptr};
  bool pending_{false};
};

}  // namespace avs_analyzer

#endif  // ANALYZER_DECODER_INTERFACE_H
