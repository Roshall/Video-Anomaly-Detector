#include "ascend/acl_runtime.h"

#include <atomic>
#include <mutex>

#include "Utils/log.h"

#ifdef ENABLE_ASCEND
#include <acl/acl.h>
#endif

namespace avs_analyzer::ascend {

const char* AscendErrorToString(AscendError err) noexcept {
  switch (err) {
    case AscendError::kSuccess: return "Success";
    case AscendError::kDeviceNotFound: return "Device not found";
    case AscendError::kChannelAllocationFailed: return "Channel allocation failed";
    case AscendError::kModelLoadFailed: return "Model load failed";
    case AscendError::kInferenceFailed: return "Inference failed";
    case AscendError::kDecodeFailed: return "Decode failed";
    case AscendError::kMemoryError: return "Memory error";
    case AscendError::kInvalidParameter: return "Invalid parameter";
    case AscendError::kNotInitialized: return "Not initialized";
    case AscendError::kSdkUnavailable: return "Ascend SDK unavailable";
    default: return "Unknown error";
  }
}

namespace {

// VDEC 通道号自动分配：严格落在 310P 的 [256,511]，绕开 VPC 的 [0,255]。
std::atomic<int> g_vdec_channel_cursor{0};

int AllocVdecChannelId() noexcept {
  const int span = kVdecChannelMax - kVdecChannelMin + 1;
  const int n = g_vdec_channel_cursor.fetch_add(1, std::memory_order_relaxed);
  return kVdecChannelMin + (n % span);
}

}  // namespace

// ---------------------------------------------------------------- AclDeviceContext

AclDeviceContext::AclDeviceContext(int device_id) noexcept
    : device_id_(device_id) {}

AclDeviceContext::~AclDeviceContext() { Finalize(); }

AscendError AclDeviceContext::Initialize() {
  if (initialized_) return AscendError::kSuccess;

#ifdef ENABLE_ASCEND
  // 顺序固定：先绑设备，再建上下文，最后建流。任一步失败都要回滚已建资源，
  // 否则设备会被占住，后续重试全部失败。
  if (aclrtSetDevice(device_id_) != ACL_SUCCESS) {
    LOGE("Ascend: aclrtSetDevice({}) 失败", device_id_);
    return AscendError::kDeviceNotFound;
  }
  aclrtContext ctx = nullptr;
  if (aclrtCreateContext(&ctx, device_id_) != ACL_SUCCESS) {
    LOGE("Ascend: aclrtCreateContext({}) 失败", device_id_);
    aclrtResetDevice(device_id_);
    return AscendError::kDeviceNotFound;
  }
  aclrtStream st = nullptr;
  if (aclrtCreateStream(&st) != ACL_SUCCESS) {
    LOGE("Ascend: aclrtCreateStream({}) 失败", device_id_);
    aclrtDestroyContext(ctx);
    aclrtResetDevice(device_id_);
    return AscendError::kMemoryError;
  }
  context_ = static_cast<void*>(ctx);
  stream_ = static_cast<void*>(st);
  initialized_ = true;
  return AscendError::kSuccess;
#else
  LOGW("Ascend: 未启用 ENABLE_ASCEND（无 CANN），AclDeviceContext 走桩实现");
  return AscendError::kSdkUnavailable;
#endif
}

void AclDeviceContext::Finalize() noexcept {
  if (!initialized_) return;
#ifdef ENABLE_ASCEND
  if (stream_) aclrtDestroyStream(static_cast<aclrtStream>(stream_));
  if (context_) aclrtDestroyContext(static_cast<aclrtContext>(context_));
  aclrtResetDevice(device_id_);
#endif
  stream_ = nullptr;
  context_ = nullptr;
  initialized_ = false;
}

// ---------------------------------------------------------------- VdecChannel

VdecChannel::VdecChannel(int device_id, VdecChannelConfig config)
    : device_id_(device_id),
      channel_id_(config.channel_id),
      config_(std::move(config)) {}

VdecChannel::~VdecChannel() { Destroy(); }

AscendError VdecChannel::Create() {
  if (valid_) return AscendError::kSuccess;
  if (channel_id_ < 0) channel_id_ = AllocVdecChannelId();

  // TODO(硬件联调)：接入真实 VDEC 通道创建。310P 上按 CANN 版本二选一：
  //   - CANN 5.x  ACL DVPP： aclvdecCreateChannel(&desc)
  //   - CANN 6+   V2 媒体：  hi_mpi_vdec_create_chn(ch, &attr)（推荐）
  // 创建前须绑定本设备的 stream：AclRuntime::Instance().Context(device_id)->stream()。
  // 输出格式固定为 NV12，与 io::PixelFormat::kNv12 对齐。
  LOGW("Ascend: VDEC 通道创建待接入（device={} ch={} codec={} {}x{}）",
       device_id_, channel_id_, config_.codec, config_.input.w,
       config_.input.h);
  return AscendError::kSdkUnavailable;
}

void VdecChannel::Destroy() noexcept {
  if (!valid_) return;
  // TODO(硬件联调)：与 Create() 配对，调用 aclvdecDestroyChannel /
  // hi_mpi_vdec_destroy_chn。
  valid_ = false;
}

// ---------------------------------------------------------------- OmModel

OmModel::OmModel(int device_id, OmModelConfig config)
    : device_id_(device_id), config_(std::move(config)) {}

OmModel::~OmModel() { Unload(); }

AscendError OmModel::Load() {
  if (loaded_) return AscendError::kSuccess;
  if (config_.model_path.empty()) return AscendError::kInvalidParameter;

  // TODO(硬件联调)：aclmdlLoadFromFile(path, &model_id) → 取 io 描述符建立
  // 输入 dataset。模型必须是用匹配 SoC 版本转换出来的 .om：
  //   atc --soc_version=Ascend310P3 ...（见迁移计划）
  LOGW("Ascend: .om 模型加载待接入（device={} path={} {}x{}）", device_id_,
       config_.model_path, config_.input.w, config_.input.h);
  return AscendError::kSdkUnavailable;
}

void OmModel::Unload() noexcept {
  if (!loaded_) return;
  // TODO(硬件联调)：aclmdlUnload(model_id_)。
  loaded_ = false;
}

// ---------------------------------------------------------------- AclRuntime

AclRuntime& AclRuntime::Instance() {
  static AclRuntime instance;
  return instance;
}

AscendError AclRuntime::Initialize() {
  std::lock_guard<std::mutex> lk(mu_);
  if (initialized_) return available_ ? AscendError::kSuccess
                                      : AscendError::kSdkUnavailable;
  initialized_ = true;

#ifdef ENABLE_ASCEND
  if (aclInit(nullptr) != ACL_SUCCESS) {
    LOGE("Ascend: aclInit 失败");
    available_ = false;
    return AscendError::kSdkUnavailable;
  }
  std::uint32_t count = 0;
  if (aclrtGetDeviceCount(&count) != ACL_SUCCESS) {
    LOGE("Ascend: aclrtGetDeviceCount 失败");
    aclFinalize();
    available_ = false;
    return AscendError::kDeviceNotFound;
  }
  device_count_ = static_cast<int>(count);
  if (device_count_ <= 0) {
    LOGW("Ascend: 未探测到可用 NPU 设备");
    aclFinalize();
    available_ = false;
    return AscendError::kDeviceNotFound;
  }
  LOGI("Ascend: 运行时就绪，设备数 {}", device_count_);
  available_ = true;
#else
  LOGW("Ascend: 未启用 ENABLE_ASCEND，运行时不可用（保持 CPU 回落路径）");
  available_ = false;
#endif
  return available_ ? AscendError::kSuccess : AscendError::kSdkUnavailable;
}

void AclRuntime::Finalize() noexcept {
  std::lock_guard<std::mutex> lk(mu_);
  if (!initialized_) return;
  contexts_.clear();
#ifdef ENABLE_ASCEND
  aclFinalize();
#endif
  initialized_ = false;
  available_ = false;
  device_count_ = 0;
}

bool AclRuntime::IsAvailable() const noexcept {
  std::lock_guard<std::mutex> lk(mu_);
  return available_;
}

int AclRuntime::DeviceCount() const noexcept {
  std::lock_guard<std::mutex> lk(mu_);
  return device_count_;
}

std::shared_ptr<AclDeviceContext> AclRuntime::Context(int device_id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = contexts_.find(device_id);
  if (it != contexts_.end()) return it->second;

  auto ctx = std::make_shared<AclDeviceContext>(device_id);
  if (ctx->Initialize() != AscendError::kSuccess) return nullptr;
  contexts_.emplace(device_id, ctx);
  return ctx;
}

}  // namespace avs_analyzer::ascend
