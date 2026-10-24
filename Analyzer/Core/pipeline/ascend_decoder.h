// VDEC 硬解码器：实现 pipeline::IDecoder。
//
// 与 CPU 解码器的关键差异：解码输出**留在 NPU 上**（NV12 半平面），
// 后续 VPC 缩放与 ACL 推理可直接零拷贝消费，避免每帧 D2H。
// 这也是 AscendPipelineFactory 必须整族替换组件的原因——混用 CPU 解码
// 会把硬件解码的收益全部吃回去。
#ifndef ANALYZER_PIPELINE_ASCEND_DECODER_H
#define ANALYZER_PIPELINE_ASCEND_DECODER_H

#include <memory>

#include "ascend/acl_runtime.h"
#include "ascend/ascend_types.h"
#include "pipeline/pipeline_factory.h"

namespace avs_analyzer::pipeline {

class AscendDecoder final : public IDecoder {
 public:
  explicit AscendDecoder(ascend::VdecChannelConfig cfg);
  ~AscendDecoder() override;

  [[nodiscard]] BackendKind backend() const noexcept override {
    return BackendKind::kNpu;
  }

  [[nodiscard]] bool Open(const StreamDesc& desc) override;

  [[nodiscard]] int channel_id() const noexcept {
    return channel_ ? channel_->channel_id() : -1;
  }
  [[nodiscard]] bool valid() const noexcept {
    return channel_ && channel_->valid();
  }

 private:
  ascend::VdecChannelConfig cfg_;
  std::unique_ptr<ascend::VdecChannel> channel_;
};

}  // namespace avs_analyzer::pipeline

#endif
