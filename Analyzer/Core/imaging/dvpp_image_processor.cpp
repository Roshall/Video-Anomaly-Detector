#include "imaging/dvpp_image_processor.h"

#include "Utils/log.h"

namespace avs_analyzer::imaging {

DvppImageProcessor::DvppImageProcessor(int device_id)
    : cpu_fallback_(), device_id_(device_id) {}

DvppImageProcessor::DvppImageProcessor(
    std::shared_ptr<const IFrameMigrator> migrator, int device_id)
    : migrator_(std::move(migrator)),
      cpu_fallback_(migrator_),
      device_id_(device_id) {}

std::unique_ptr<Frame> DvppImageProcessor::ResizeAndConvert(
    const Frame& in, Size out, PixelFormat fmt) {
  if (in.preferred() == MemoryLocation::kDevice) {
    // TODO(硬件联调)：hi_mpi_vpc_resize / hi_mpi_vpc_convert_color，
    // 输入与输出都留在 device 上，输出包成 DeviceFrameStorage + Frame。
    LOGW("Ascend: VPC 缩放待接入（{}x{} → {}x{}，device={}）", in.size().w,
         in.size().h, out.w, out.h, device_id_);
    return nullptr;
  }
  // Host 帧：VPC 尚不可用时用 CPU 实现兜底，保证展示/抓图链路不断。
  return cpu_fallback_.ResizeAndConvert(in, out, fmt);
}

ModelInput DvppImageProcessor::PrepareModelInput(const Frame& in,
                                                 const ModelSpec& spec) {
  const auto storage = in.pixels();

  if (storage->location() == MemoryLocation::kDevice) {
    // 零拷贝路径：把 device 缓冲原样包成 DeviceTensor 交给推理引擎。
    // 不做 letterbox 到 host、也不做 /255——这两件事分别由 VPC 与 AIPP 承担。
    DeviceTensor t;
    t.ptr = storage->data();
    t.bytes = storage->byte_size();
    t.device_id = storage->device_id();
    t.size = storage->size();
    t.format = storage->format();
    return t;
  }

  // Host 帧：暂由 CPU 预处理兜底（产出 HostTensor），推理端会走 H2D 分支。
  LOGW("Ascend: 输入为 host 帧，VPC letterbox 待接入，暂用 CPU 预处理兜底");
  return cpu_fallback_.PrepareModelInput(in, spec);
}

bool DvppImageProcessor::Supports(Op op) const noexcept {
  switch (op) {
    case Op::kResize:
    case Op::kPixelFormatConvert:
    case Op::kLetterbox:
    case Op::kCrop:
      return true;
    case Op::kNormalize:
      // 归一化交给 AIPP，本模块刻意不承担。
      return false;
  }
  return false;
}

}  // namespace avs_analyzer::imaging
