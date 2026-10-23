// Bridge 的实现侧：像素到底放在哪（Host 内存 / NPU Device 内存）。
//
// 业务层（event.cpp 的帧缓冲、analyzer.cpp 的 ROI 判定）只应通过 Frame 访问，
// 不该知道像素在 Host 还是 Device；跨端搬运由 IFrameMigrator 负责，
// 且 Frame 保证同一帧只搬一次。
#ifndef ANALYZER_IMAGING_FRAME_STORAGE_H
#define ANALYZER_IMAGING_FRAME_STORAGE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "pixel_format.h"

namespace avs_analyzer::imaging {

class IFrameStorage {
 public:
  virtual ~IFrameStorage() = default;

  [[nodiscard]] virtual MemoryLocation location() const noexcept = 0;
  [[nodiscard]] virtual Size size() const noexcept = 0;
  [[nodiscard]] virtual PixelFormat format() const noexcept = 0;
  [[nodiscard]] virtual std::int64_t pts() const noexcept = 0;

  /// 本机可读的数据地址。Device 存储返回的是 device 虚拟地址，
  /// Host 侧不能直接解引用，必须先 Materialize(kHost)。
  [[nodiscard]] virtual const std::byte* data() const noexcept = 0;
  [[nodiscard]] virtual std::size_t byte_size() const noexcept = 0;

  /// 所属 NPU 编号；Host 存储返回 -1。
  [[nodiscard]] virtual int device_id() const noexcept { return -1; }
};

/// 持有自己的内存（AVFrame / cv::Mat / 裸缓冲都可转成它）
class HostFrameStorage final : public IFrameStorage {
 public:
  HostFrameStorage(std::vector<std::byte> bytes, Size size, PixelFormat format,
                   std::int64_t pts)
      : bytes_(std::move(bytes)), size_(size), format_(format), pts_(pts) {}

  [[nodiscard]] MemoryLocation location() const noexcept override {
    return MemoryLocation::kHost;
  }
  [[nodiscard]] Size size() const noexcept override { return size_; }
  [[nodiscard]] PixelFormat format() const noexcept override { return format_; }
  [[nodiscard]] std::int64_t pts() const noexcept override { return pts_; }
  [[nodiscard]] const std::byte* data() const noexcept override {
    return bytes_.data();
  }
  [[nodiscard]] std::size_t byte_size() const noexcept override {
    return bytes_.size();
  }

 private:
  std::vector<std::byte> bytes_;
  Size size_;
  PixelFormat format_;
  std::int64_t pts_;
};

/// 不持有内存：指向由 DVPP（hi_mpi_dvpp_malloc）分配的 device 缓冲。
/// 生命周期由 NpuResourceManager / DvppBufferPool 负责，本类只是一个视图。
class DeviceFrameStorage final : public IFrameStorage {
 public:
  DeviceFrameStorage(const std::byte* ptr, std::size_t bytes, Size size,
                     PixelFormat format, std::int64_t pts, int device_id)
      : ptr_(ptr), bytes_(bytes), size_(size), format_(format), pts_(pts),
        device_id_(device_id) {}

  [[nodiscard]] MemoryLocation location() const noexcept override {
    return MemoryLocation::kDevice;
  }
  [[nodiscard]] Size size() const noexcept override { return size_; }
  [[nodiscard]] PixelFormat format() const noexcept override { return format_; }
  [[nodiscard]] std::int64_t pts() const noexcept override { return pts_; }
  [[nodiscard]] const std::byte* data() const noexcept override { return ptr_; }
  [[nodiscard]] std::size_t byte_size() const noexcept override { return bytes_; }
  [[nodiscard]] int device_id() const noexcept override { return device_id_; }

 private:
  const std::byte* ptr_;
  std::size_t bytes_;
  Size size_;
  PixelFormat format_;
  std::int64_t pts_;
  int device_id_;
};

/// 跨端搬运（D2H / H2D）。真实实现走 aclrtMemcpy；测试里换成 Fake 即可。
class IFrameMigrator {
 public:
  virtual ~IFrameMigrator() = default;

  [[nodiscard]] virtual std::shared_ptr<const IFrameStorage> ToHost(
      const IFrameStorage& src) const = 0;

  [[nodiscard]] virtual std::shared_ptr<const IFrameStorage> ToDevice(
      const IFrameStorage& src, int device_id) const = 0;
};

}  // namespace avs_analyzer::imaging

#endif
