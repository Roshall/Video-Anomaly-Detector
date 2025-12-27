//
// Stream Processor Implementation
//
#include "stream_processor.h"
#include "ascend_decoder.h"
#include "ascend_inference.h"
#include "ascend_pipeline.h"
#include "../Utils/log.h"

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

namespace avs_analyzer {

//
// Factory Method
//
std::unique_ptr<StreamProcessor> StreamProcessor::Create(
    const StreamProcessorConfig& config) {
  
  if (config.use_hardware) {
#ifdef ENABLE_ASCEND
    // Check if Ascend is available
    if (ascend::AscendPipelineManager::IsAscendAvailable()) {
      LOGI("StreamProcessor: Creating hardware-accelerated processor");
      return std::make_unique<HardwareStreamProcessor>(config);
    } else {
      LOGW("StreamProcessor: Hardware requested but Ascend not available, "
           "falling back to CPU");
    }
#else
    LOGW("StreamProcessor: Hardware requested but ENABLE_ASCEND not defined, "
         "falling back to CPU");
#endif
  }

  LOGI("StreamProcessor: Creating CPU processor");
  return std::make_unique<CpuStreamProcessor>(config);
}

//
// CPU Stream Processor Implementation
//

CpuStreamProcessor::CpuStreamProcessor(const StreamProcessorConfig& config)
    : config_(config) {}

CpuStreamProcessor::~CpuStreamProcessor() {
  if (sws_ctx_) {
    sws_freeContext(sws_ctx_);
  }
  if (bgr_frame_) {
    av_frame_free(&bgr_frame_);
  }
}

bool CpuStreamProcessor::Initialize(AVCodecParameters* codec_params,
                                    AVRational time_base) {
  time_base_ = time_base;
  
  decoder_ = std::make_unique<FFmpegDecoder>();
  if (!decoder_->Initialize(codec_params, time_base)) {
    LOGE("CpuStreamProcessor: Failed to initialize decoder");
    return false;
  }

  // Create default inference engine if model path provided
  if (!config_.model_path.empty()) {
    InferenceConfig inf_config;
    inf_config.model_path = config_.model_path;
    inf_config.conf_threshold = config_.conf_threshold;
    inf_config.iou_threshold = config_.iou_threshold;
    inf_config.input_width = config_.input_width;
    inf_config.input_height = config_.input_height;
    inf_config.target_class_id = config_.target_class_id;

    auto engine = CreateInferenceEngine(
        config_.device_id >= 0 ? InferenceBackend::kOnnxCuda 
                               : InferenceBackend::kOnnxCpu);
    if (engine && engine->Initialize(inf_config)) {
      inference_engines_.push_back(std::move(engine));
    } else {
      LOGW("CpuStreamProcessor: Failed to initialize inference engine");
    }
  }

  LOGI("CpuStreamProcessor: Initialized {}x{}", 
       decoder_->Width(), decoder_->Height());
  return true;
}

bool CpuStreamProcessor::SendPacket(AVPacket* packet) {
  return decoder_ && decoder_->SendPacket(packet);
}

std::unique_ptr<ProcessedFrame> CpuStreamProcessor::ReceiveFrame() {
  if (!decoder_) return nullptr;

  AVFrame* decoded = decoder_->ReceiveFrame();
  if (!decoded) return nullptr;

  auto result = std::make_unique<ProcessedFrame>();
  result->pts = decoded->pts;
  result->timestamp_ms = av_rescale_q(decoded->pts, time_base_, 
                                       AVRational{1, 1000});
  result->is_keyframe = (decoded->pict_type == AV_PICTURE_TYPE_I);
  result->raw_frame = decoded;

  // Convert to BGR for inference and annotation
  int width = decoder_->Width();
  int height = decoder_->Height();
  
  // Initialize swscale if needed
  if (!sws_ctx_) {
    sws_ctx_ = sws_getContext(
        width, height, decoder_->PixelFormat(),
        width, height, AV_PIX_FMT_BGR24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    
    bgr_frame_ = av_frame_alloc();
    bgr_frame_->format = AV_PIX_FMT_BGR24;
    bgr_frame_->width = width;
    bgr_frame_->height = height;
    av_frame_get_buffer(bgr_frame_, 32);
  }

  if (sws_ctx_) {
    sws_scale(sws_ctx_, decoded->data, decoded->linesize, 0, height,
              bgr_frame_->data, bgr_frame_->linesize);
    
    // Create cv::Mat wrapping the frame data
    result->bgr_image = cv::Mat(height, width, CV_8UC3, 
                                bgr_frame_->data[0], 
                                bgr_frame_->linesize[0]).clone();
  }

  // Run inference on all models
  for (auto& engine : inference_engines_) {
    if (engine && !result->bgr_image.empty()) {
      auto detections = engine->Infer(result->bgr_image);
      result->detections.insert(result->detections.end(),
                                detections.begin(), detections.end());
    }
  }

  // Call frame callback if set
  if (frame_callback_) {
    auto callback_frame = std::make_unique<ProcessedFrame>(*result);
    frame_callback_(std::move(callback_frame));
  }

  return result;
}

void CpuStreamProcessor::SetFrameCallback(ProcessedFrameCallback callback) {
  frame_callback_ = std::move(callback);
}

int CpuStreamProcessor::AddModel(const InferenceConfig& config) {
  auto engine = CreateInferenceEngine(
      config.device_id >= 0 ? InferenceBackend::kOnnxCuda 
                            : InferenceBackend::kOnnxCpu);
  if (!engine || !engine->Initialize(config)) {
    LOGE("CpuStreamProcessor: Failed to add model from {}", config.model_path);
    return -1;
  }
  
  int id = static_cast<int>(inference_engines_.size());
  inference_engines_.push_back(std::move(engine));
  return id;
}

void CpuStreamProcessor::RemoveModel(int model_id) {
  if (model_id >= 0 && model_id < static_cast<int>(inference_engines_.size())) {
    inference_engines_[model_id].reset();
  }
}

int CpuStreamProcessor::Width() const {
  return decoder_ ? decoder_->Width() : 0;
}

int CpuStreamProcessor::Height() const {
  return decoder_ ? decoder_->Height() : 0;
}

AVPixelFormat CpuStreamProcessor::PixelFormat() const {
  return decoder_ ? decoder_->PixelFormat() : AV_PIX_FMT_NONE;
}

void CpuStreamProcessor::Flush() {
  if (decoder_) decoder_->Flush();
}

//
// Hardware Stream Processor Implementation
//

HardwareStreamProcessor::HardwareStreamProcessor(
    const StreamProcessorConfig& config)
    : config_(config) {}

HardwareStreamProcessor::~HardwareStreamProcessor() {
  inference_engines_.clear();
  decoder_.reset();
}

bool HardwareStreamProcessor::Initialize(AVCodecParameters* codec_params,
                                         AVRational time_base) {
  time_base_ = time_base;

#ifdef ENABLE_ASCEND
  // Initialize Ascend runtime
  auto& manager = ascend::AscendPipelineManager::Instance();
  if (manager.InitializeRuntime() != ascend::AscendError::kSuccess) {
    LOGE("HardwareStreamProcessor: Failed to initialize Ascend runtime");
    return false;
  }

  // Create Ascend decoder
  auto ascend_decoder = std::make_unique<ascend::AscendDecoder>(config_.device_id);
  if (!ascend_decoder->Initialize(codec_params, time_base)) {
    LOGE("HardwareStreamProcessor: Failed to initialize Ascend decoder");
    return false;
  }
  
  decoder_ = std::move(ascend_decoder);

  // Create default inference engine if model path provided
  if (!config_.model_path.empty()) {
    InferenceConfig inf_config;
    inf_config.model_path = config_.model_path;
    inf_config.conf_threshold = config_.conf_threshold;
    inf_config.iou_threshold = config_.iou_threshold;
    inf_config.input_width = config_.input_width;
    inf_config.input_height = config_.input_height;
    inf_config.device_id = config_.device_id;
    inf_config.use_device_memory = true;
    inf_config.target_class_id = config_.target_class_id;

    auto engine = ascend::CreateAscendInferenceEngine(config_.device_id);
    if (engine && engine->Initialize(inf_config)) {
      // Link to VDEC channel for zero-copy
      auto* ascend_engine = dynamic_cast<ascend::AscendInferenceEngineImpl*>(
          engine.get());
      auto* asc_decoder = dynamic_cast<ascend::AscendDecoder*>(decoder_.get());
      if (ascend_engine && asc_decoder) {
        ascend_engine->LinkToVdecChannel(asc_decoder->VdecChannelId());
      }
      inference_engines_.emplace_back(next_model_id_++, std::move(engine));
    } else {
      LOGW("HardwareStreamProcessor: Failed to initialize inference engine");
    }
  }

  LOGI("HardwareStreamProcessor: Initialized {}x{} on device {}", 
       decoder_->Width(), decoder_->Height(), config_.device_id);
  return true;
#else
  (void)codec_params;
  (void)time_base;
  LOGE("HardwareStreamProcessor: ENABLE_ASCEND not defined");
  return false;
#endif
}

bool HardwareStreamProcessor::SendPacket(AVPacket* packet) {
  return decoder_ && decoder_->SendPacket(packet);
}

std::unique_ptr<ProcessedFrame> HardwareStreamProcessor::ReceiveFrame() {
#ifdef ENABLE_ASCEND
  if (!decoder_) return nullptr;

  AVFrame* decoded = decoder_->ReceiveFrame();
  if (!decoded) return nullptr;

  auto result = std::make_unique<ProcessedFrame>();
  result->pts = decoded->pts;
  result->timestamp_ms = av_rescale_q(decoded->pts, time_base_, 
                                       AVRational{1, 1000});
  result->is_keyframe = (decoded->pict_type == AV_PICTURE_TYPE_I);
  result->raw_frame = decoded;
  result->device_ptr = decoder_->GetDevicePtr(decoded);

  // Run inference using device memory (zero-copy)
  for (auto& [id, engine] : inference_engines_) {
    if (engine && result->device_ptr) {
      auto detections = engine->InferDevice(
          result->device_ptr, decoder_->Width(), decoder_->Height());
      result->detections.insert(result->detections.end(),
                                detections.begin(), detections.end());
    }
  }

  // Convert to BGR for annotation (if needed)
  // This would use VPC hardware color conversion
  auto pipeline = ascend::AscendPipelineManager::Instance().GetPipeline(
      config_.device_id);
  if (pipeline && result->device_ptr) {
    ascend::DecodedFrame dev_frame;
    dev_frame.device_ptr = result->device_ptr;
    dev_frame.width = decoder_->Width();
    dev_frame.height = decoder_->Height();
    result->bgr_image = pipeline->VpcYuvToBgr(dev_frame);
  }

  // Call frame callback if set
  if (frame_callback_) {
    auto callback_frame = std::make_unique<ProcessedFrame>(*result);
    frame_callback_(std::move(callback_frame));
  }

  return result;
#else
  return nullptr;
#endif
}

void HardwareStreamProcessor::SetFrameCallback(ProcessedFrameCallback callback) {
  frame_callback_ = std::move(callback);
}

int HardwareStreamProcessor::AddModel(const InferenceConfig& config) {
#ifdef ENABLE_ASCEND
  auto engine = ascend::CreateAscendInferenceEngine(config_.device_id);
  if (!engine || !engine->Initialize(config)) {
    LOGE("HardwareStreamProcessor: Failed to add model from {}", 
         config.model_path);
    return -1;
  }

  // Link to VDEC channel
  auto* ascend_engine = dynamic_cast<ascend::AscendInferenceEngineImpl*>(
      engine.get());
  auto* asc_decoder = dynamic_cast<ascend::AscendDecoder*>(decoder_.get());
  if (ascend_engine && asc_decoder) {
    ascend_engine->LinkToVdecChannel(asc_decoder->VdecChannelId());
  }

  int id = next_model_id_++;
  inference_engines_.emplace_back(id, std::move(engine));
  return id;
#else
  (void)config;
  return -1;
#endif
}

void HardwareStreamProcessor::RemoveModel(int model_id) {
#ifdef ENABLE_ASCEND
  auto it = std::find_if(inference_engines_.begin(), inference_engines_.end(),
                         [model_id](const auto& p) { return p.first == model_id; });
  if (it != inference_engines_.end()) {
    // Unlink from VDEC channel
    auto* ascend_engine = dynamic_cast<ascend::AscendInferenceEngineImpl*>(
        it->second.get());
    auto* asc_decoder = dynamic_cast<ascend::AscendDecoder*>(decoder_.get());
    if (ascend_engine && asc_decoder) {
      ascend_engine->UnlinkFromVdecChannel(asc_decoder->VdecChannelId());
    }
    inference_engines_.erase(it);
  }
#else
  (void)model_id;
#endif
}

int HardwareStreamProcessor::Width() const {
  return decoder_ ? decoder_->Width() : 0;
}

int HardwareStreamProcessor::Height() const {
  return decoder_ ? decoder_->Height() : 0;
}

AVPixelFormat HardwareStreamProcessor::PixelFormat() const {
  return decoder_ ? decoder_->PixelFormat() : AV_PIX_FMT_NONE;
}

void HardwareStreamProcessor::Flush() {
  if (decoder_) decoder_->Flush();
}

}  // namespace avs_analyzer
