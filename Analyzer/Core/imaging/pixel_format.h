// 兼容垫片：共享词汇已上移到中性契约层 io/common.h。
//
// 图像引擎内部与既有测试长期使用 imaging::Size / imaging::PixelFormat 等名字，
// 这里把它们 re-export 回 avs_analyzer::imaging，避免大面积改名；
// 新代码建议直接用 avs_analyzer::io:: 的原始名字。
#ifndef ANALYZER_IMAGING_PIXEL_FORMAT_H
#define ANALYZER_IMAGING_PIXEL_FORMAT_H

#include "io/common.h"

namespace avs_analyzer::imaging {

using io::BackendKind;
using io::FrameBytes;
using io::MemoryLocation;
using io::PixelFormat;
using io::Size;

}  // namespace avs_analyzer::imaging

#endif
