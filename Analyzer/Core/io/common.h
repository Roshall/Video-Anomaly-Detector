// 中性共享词汇：像素格式、尺寸、内存位置、后端种类。
//
// 这是"图像处理引擎"与"推理引擎"共用的最底层类型，刻意不放在任何一个
// 引擎的命名空间/目录下，也不依赖任何引擎。两个引擎都依赖它，而它谁也不
// 依赖——由 tests/architecture/layering_test.cpp 强制保证。
#ifndef ANALYZER_IO_COMMON_H
#define ANALYZER_IO_COMMON_H

#include <cstddef>
#include <cstdint>

namespace avs_analyzer::io {

enum class PixelFormat {
  kUnknown,
  kBgr24,    // 交错 BGR，每像素 3 字节
  kRgb24,    // 交错 RGB，每像素 3 字节
  kNv12,     // Y 平面 + 交错 UV 平面（UV 顺序）
  kNv21,     // Y 平面 + 交错 VU 平面
  kYuv420p,  // I420：Y、U、V 三个独立平面
};

struct Size {
  int w = 0;
  int h = 0;
  friend bool operator==(const Size&, const Size&) = default;
};

enum class MemoryLocation { kHost, kDevice };

/// 后端种类。粗粒度标记"这段跑在哪种硬件族"；跨厂商区分见各引擎的
/// 平台标识（未来扩展）。解耦后由引擎实现自报，compose 层据此校验同族。
enum class BackendKind { kCpu, kNpu };

// 给定尺寸与格式下的一帧字节数；非法输入返回 0。
constexpr std::size_t FrameBytes(Size s, PixelFormat f) noexcept {
  if (s.w <= 0 || s.h <= 0) return 0;
  const auto pixels = static_cast<std::size_t>(s.w) * static_cast<std::size_t>(s.h);
  switch (f) {
    case PixelFormat::kBgr24:
    case PixelFormat::kRgb24:
      return pixels * 3;
    case PixelFormat::kNv12:
    case PixelFormat::kNv21:
    case PixelFormat::kYuv420p:
      return pixels * 3 / 2;
    default:
      return 0;
  }
}

}  // namespace avs_analyzer::io

#endif
