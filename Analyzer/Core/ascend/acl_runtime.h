// ACL 运行时与硬件资源管理（迁移计划 Phase 1.3）。
//
// 职责边界：只管"设备 / 上下文 / 流 / 通道 / 模型句柄"的生命周期，不关心
// 图像语义，也不 include 任何引擎目录——它是被 backend/ 与 pipeline/ 共同
// 使用的平台层。
//
// 真实 ACL 调用仅在 ENABLE_ASCEND 下编译；未链接 CANN 时走桩并 LOGW，
// 保证 x86 无硬件环境下仍能编译与回归（与抽象层既有测试一致）。
#ifndef ANALYZER_ASCEND_ACL_RUNTIME_H
#define ANALYZER_ASCEND_ACL_RUNTIME_H

#include <memory>
#include <mutex>
#include <unordered_map>

#include "ascend_types.h"

namespace avs_analyzer::ascend {

/// 单个 NPU 设备上的 ACL 资源：device / context / stream。
///
/// ACL 句柄以 void* 持有，因此本头文件在**没有** CANN SDK 时也能编译；
/// 只有在 .cpp 的 ENABLE_ASCEND 分支里才会 cast 成真实类型。
class AclDeviceContext {
 public:
  explicit AclDeviceContext(int device_id) noexcept;
  ~AclDeviceContext();
  AclDeviceContext(const AclDeviceContext&) = delete;
  AclDeviceContext& operator=(const AclDeviceContext&) = delete;

  AscendError Initialize();
  void Finalize() noexcept;

  [[nodiscard]] bool initialized() const noexcept { return initialized_; }
  [[nodiscard]] int device_id() const noexcept { return device_id_; }
  /// ACL stream 句柄（真实 SDK 下为 aclrtStream）；桩模式下为 nullptr。
  [[nodiscard]] void* stream() const noexcept { return stream_; }

 private:
  int device_id_{0};
  bool initialized_{false};
  void* context_{nullptr};
  void* stream_{nullptr};
};

/// 一路码流的 VDEC 硬解通道。
///
/// 通道号由运行时在 310P 的合法区间 [kVdecChannelMin, kVdecChannelMax] 内
/// 自动分配，避免与 VPC 的 [0,255] 冲突。
class VdecChannel {
 public:
  VdecChannel(int device_id, VdecChannelConfig config);
  ~VdecChannel();
  VdecChannel(const VdecChannel&) = delete;
  VdecChannel& operator=(const VdecChannel&) = delete;

  AscendError Create();
  void Destroy() noexcept;

  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] int channel_id() const noexcept { return channel_id_; }
  [[nodiscard]] int device_id() const noexcept { return device_id_; }
  [[nodiscard]] const VdecChannelConfig& config() const noexcept {
    return config_;
  }

 private:
  int device_id_{0};
  int channel_id_{-1};
  VdecChannelConfig config_{};
  bool valid_{false};
};

/// 已加载的 .om 模型句柄。
class OmModel {
 public:
  OmModel(int device_id, OmModelConfig config);
  ~OmModel();
  OmModel(const OmModel&) = delete;
  OmModel& operator=(const OmModel&) = delete;

  AscendError Load();
  void Unload() noexcept;

  [[nodiscard]] bool loaded() const noexcept { return loaded_; }
  [[nodiscard]] std::uint32_t model_id() const noexcept { return model_id_; }
  [[nodiscard]] const OmModelConfig& config() const noexcept { return config_; }

 private:
  int device_id_{0};
  OmModelConfig config_{};
  std::uint32_t model_id_{0};
  bool loaded_{false};
};

/// 进程级运行时单例：aclInit/aclFinalize 只做一次，按 device_id 复用上下文。
class AclRuntime {
 public:
  static AclRuntime& Instance();

  AscendError Initialize();
  void Finalize() noexcept;

  [[nodiscard]] bool IsAvailable() const noexcept;
  [[nodiscard]] int DeviceCount() const noexcept;

  /// 取得（必要时创建）指定设备的上下文；失败返回 nullptr。
  [[nodiscard]] std::shared_ptr<AclDeviceContext> Context(int device_id);

 private:
  AclRuntime() = default;

  mutable std::mutex mu_;
  bool initialized_{false};
  bool available_{false};
  int device_count_{0};
  std::unordered_map<int, std::shared_ptr<AclDeviceContext>> contexts_;
};

}  // namespace avs_analyzer::ascend

#endif
