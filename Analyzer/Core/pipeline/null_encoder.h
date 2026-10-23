// Null Object：布控未开启推流时的编码器替身。
// 目的是消掉 AvPushStream / Worker 里满屏的 `if (monitor_->push_stream)`。
#ifndef ANALYZER_PIPELINE_NULL_ENCODER_H
#define ANALYZER_PIPELINE_NULL_ENCODER_H

#include "imaging/pixel_format.h"
#include "pipeline_factory.h"

namespace avs_analyzer::pipeline {

class NullEncoder final : public IEncoder {
 public:
  [[nodiscard]] BackendKind backend() const noexcept override {
    return BackendKind::kCpu;
  }
  bool Encode(const Frame&) override { return true; }
  void Flush() override {}
};

}  // namespace avs_analyzer::pipeline

#endif
