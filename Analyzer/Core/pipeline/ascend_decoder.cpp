#include "pipeline/ascend_decoder.h"

#include <string>

#include "Utils/log.h"

namespace avs_analyzer::pipeline {
namespace {

// 从 URL/文件名猜编码类型，作为 VDEC 通道属性的初始 hint。
// 真正的码流参数以后应由 demux 出的 AVCodecParameters 填充，这里只保证
// h265 不会被误配成 h264（配错会导致 VDEC 直接拒绝码流）。
const char* GuessCodec(const StreamDesc& desc) {
  const std::string& u = desc.url;
  const auto has = [&u](const char* needle) {
    return u.find(needle) != std::string::npos;
  };
  if (has("hevc") || has("h265") || has("265")) return "h265";
  return "h264";
}

}  // namespace

AscendDecoder::AscendDecoder(ascend::VdecChannelConfig cfg)
    : cfg_(std::move(cfg)) {}

AscendDecoder::~AscendDecoder() = default;

bool AscendDecoder::Open(const StreamDesc& desc) {
  if (cfg_.codec.empty() || cfg_.codec == "auto") cfg_.codec = GuessCodec(desc);

  if (ascend::AclRuntime::Instance().Initialize() != ascend::AscendError::kSuccess) {
    LOGW("Ascend: 运行时不可用，VDEC 解码器无法打开 url={}", desc.url);
    return false;
  }

  channel_ = std::make_unique<ascend::VdecChannel>(cfg_.device_id, cfg_);
  const auto err = channel_->Create();
  if (err != ascend::AscendError::kSuccess) {
    LOGW("Ascend: VDEC 通道创建失败 url={} err={}", desc.url,
         ascend::AscendErrorToString(err));
    channel_.reset();
    return false;
  }
  return true;
}

}  // namespace avs_analyzer::pipeline
