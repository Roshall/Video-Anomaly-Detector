// Bridge 的抽象侧：业务层唯一可见的"帧"。
//
// 帧的像素可能留在 NPU 上（VDEC 解码后），也可能在 Host（画框、抓图时用）。
// Frame 记住"这个 pipeline 希望像素在哪"，首次访问时按需搬运并缓存，
// 之后重复访问不再产生 D2H。
#ifndef ANALYZER_IMAGING_FRAME_H
#define ANALYZER_IMAGING_FRAME_H

#include <memory>
#include <stdexcept>

#include "frame_storage.h"
#include "pixel_format.h"

namespace avs_analyzer::imaging {

class Frame {
 public:
  Frame(std::shared_ptr<const IFrameStorage> storage, MemoryLocation preferred,
        std::shared_ptr<const IFrameMigrator> migrator = nullptr)
      : storage_(std::move(storage)),
        preferred_(preferred),
        migrator_(std::move(migrator)) {
    if (!storage_) throw std::invalid_argument("Frame: null storage");
  }

  /// 按 preferred_ 取像素。同位置直接返回原存储；否则搬运一次并缓存。
  [[nodiscard]] std::shared_ptr<const IFrameStorage> pixels() const {
    if (storage_->location() == preferred_) return storage_;
    if (materialized_) return materialized_;
    if (!migrator_) {
      throw std::runtime_error(
          "Frame: cross-device access requires an IFrameMigrator");
    }
    materialized_ = (preferred_ == MemoryLocation::kHost)
                        ? migrator_->ToHost(*storage_)
                        : migrator_->ToDevice(*storage_, storage_->device_id());
    return materialized_;
  }

  [[nodiscard]] Size size() const { return storage_->size(); }
  [[nodiscard]] PixelFormat format() const { return storage_->format(); }
  [[nodiscard]] std::int64_t pts() const { return storage_->pts(); }
  [[nodiscard]] MemoryLocation preferred() const noexcept { return preferred_; }

 private:
  std::shared_ptr<const IFrameStorage> storage_;
  MemoryLocation preferred_;
  std::shared_ptr<const IFrameMigrator> migrator_;
  mutable std::shared_ptr<const IFrameStorage> materialized_;
};

}  // namespace avs_analyzer::imaging

#endif
