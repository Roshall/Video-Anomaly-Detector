// Ascend 专属值类型：驱动 310P 硬件所需的配置与错误码。
//
// 与中性契约层 io/ 的分工：
//   - io/        描述"图像处理引擎与推理引擎之间交换的数据"（谁都能消费）
//   - 本文件     描述"驱动 Ascend 硬件所必需的参数"（只有 Ascend 实现需要）
//
// 刻意**不**重复定义检测结果类型——统一复用 io::DetectionResult，避免代码库里
// 出现第二套 DetectionResult（ ascend-hardware 分支曾定义过一份，已废弃）。
#ifndef ANALYZER_ASCEND_ASCEND_TYPES_H
#define ANALYZER_ASCEND_ASCEND_TYPES_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "io/common.h"

namespace avs_analyzer::ascend {

using io::PixelFormat;
using io::Size;

/// VDEC 解码后留在 NPU 上的一帧（NV12 半平面）。
struct AscendFrameDesc {
  const void* ptr{nullptr};
  std::size_t bytes{0};
  Size size{};
  PixelFormat format{PixelFormat::kNv12};
  int stride{0};
  int device_id{-1};
  std::int64_t pts{0};
};

/// VDEC 通道配置。
struct VdecChannelConfig {
  std::string codec{"h264"};  // "h264" / "h265"
  Size input{1920, 1080};     // 码流分辨率
  Size output{1920, 1080};    // VPC 缩放目标（等于 input 表示不缩放）
  int device_id{0};
  int channel_id{-1};         // -1 = 由运行时在合法区间内自动分配
};

/// .om 模型加载配置（由 atc 转换得到，见迁移计划的 atc 命令）。
struct OmModelConfig {
  std::string model_path;  // .om 文件路径
  Size input{640, 640};
  bool use_dynamic_aipp{true};  // 动态 AIPP：归一化/色域转换下沉到推理节点
  float conf_threshold{0.25f};
  float iou_threshold{0.45f};
};

enum class AscendError {
  kSuccess = 0,
  kDeviceNotFound,
  kChannelAllocationFailed,
  kModelLoadFailed,
  kInferenceFailed,
  kDecodeFailed,
  kMemoryError,
  kInvalidParameter,
  kNotInitialized,
  kSdkUnavailable,  // 未链接 CANN / 运行期硬件不可用
};

const char* AscendErrorToString(AscendError err) noexcept;

/// Atlas 300V Pro 的 SoC 版本；atc 转换模型时必须用匹配值，否则加载失败。
inline constexpr const char* kDefaultSocVersion = "Ascend310P3";

/// 310P 的模块通道号区间：VDEC 占 [256,511]，VPC 占 [0,255]。
/// 自动分配 VDEC 通道时必须落在这个区间，否则与 VPC 冲突。
inline constexpr int kVdecChannelMin = 256;
inline constexpr int kVdecChannelMax = 511;

}  // namespace avs_analyzer::ascend

#endif
