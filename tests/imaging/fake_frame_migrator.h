// 测试替身：不接触真实 NPU，用 memcpy 模拟跨端搬运，并统计搬运次数。
// 用于验证 Frame 的"按需搬运 + 只搬一次"语义（Bridge 的核心契约）。
#ifndef ANALYZER_TESTS_IMAGING_FAKE_FRAME_MIGRATOR_H
#define ANALYZER_TESTS_IMAGING_FAKE_FRAME_MIGRATOR_H

#include <atomic>
#include <cstring>
#include <vector>

#include "imaging/frame_storage.h"

namespace avs_analyzer::imaging::test {

class FakeFrameMigrator final : public IFrameMigrator {
 public:
  [[nodiscard]] std::shared_ptr<const IFrameStorage> ToHost(
      const IFrameStorage& src) const override {
    ++to_host_calls;
    std::vector<std::byte> buf(src.byte_size());
    std::memcpy(buf.data(), src.data(), buf.size());
    return std::make_shared<HostFrameStorage>(std::move(buf), src.size(),
                                              src.format(), src.pts());
  }

  [[nodiscard]] std::shared_ptr<const IFrameStorage> ToDevice(
      const IFrameStorage& src, int device_id) const override {
    ++to_device_calls;
    last_device_id = device_id;
    owned_.push_back(std::vector<std::byte>(src.byte_size()));
    auto& backing = owned_.back();
    std::memcpy(backing.data(), src.data(), backing.size());
    return std::make_shared<DeviceFrameStorage>(backing.data(), backing.size(),
                                                src.size(), src.format(),
                                                src.pts(), device_id);
  }

  mutable int to_host_calls = 0;
  mutable int to_device_calls = 0;
  mutable int last_device_id = -1;

 private:
  // DeviceFrameStorage 不持有内存，这里替它保活
  mutable std::vector<std::vector<std::byte>> owned_;
};

}  // namespace avs_analyzer::imaging::test

#endif
