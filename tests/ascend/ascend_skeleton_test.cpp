// Ascend 骨架的行为测试。
//
// 目的：在没有 CANN / NPU 的机器上也能回归，钉死三件事——
//   1. 无硬件时必须**明确报告不可用**，而不是假装成功（避免静默退化）
//   2. 设备帧到 DeviceTensor 是**零拷贝**（指针原样透传，不做 D2H）
//   3. 工厂产出的组件**同族**（NPU 工厂不会混进 CPU 组件）
#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <opencv2/core.hpp>

#include "ascend/acl_runtime.h"
#include "ascend/ascend_types.h"
#include "backend/ascend_inference.h"
#include "imaging/dvpp_image_processor.h"
#include "imaging/frame.h"
#include "imaging/frame_storage.h"
#include "imaging/pixel_format.h"
#include "io/common.h"
#include "io/tensor.h"
#include "pipeline/ascend_decoder.h"
#include "pipeline/ffmpeg_decoder.h"
#include "pipeline/pipeline_factories.h"

namespace {

using namespace avs_analyzer;

// ---------------------------------------------------------------- 运行时

TEST(AscendRuntime, ReportsUnavailableInsteadOfPretendingSuccess) {
  auto& rt = ascend::AclRuntime::Instance();
  const auto err = rt.Initialize();
  // 有真实 NPU 时应为 kSuccess；无 CANN / 无设备时应明确报不可用。
  EXPECT_TRUE(err == ascend::AscendError::kSuccess ||
              err == ascend::AscendError::kSdkUnavailable ||
              err == ascend::AscendError::kDeviceNotFound)
      << "实际返回: " << ascend::AscendErrorToString(err);
  EXPECT_EQ(rt.IsAvailable(), err == ascend::AscendError::kSuccess);
}

TEST(AscendRuntime, ErrorToStringNeverReturnsNull) {
  for (int i = 0; i <= static_cast<int>(ascend::AscendError::kSdkUnavailable); ++i) {
    EXPECT_NE(ascend::AscendErrorToString(static_cast<ascend::AscendError>(i)),
              nullptr);
  }
}

// ---------------------------------------------------------------- 推理后端

TEST(AscendInference, ReportsNpuBackendAndEmptyResultsWhenUnready) {
  ascend::OmModelConfig cfg;
  cfg.model_path = "does-not-exist.om";
  backend::AscendInferenceBackend b(cfg, 0);

  EXPECT_EQ(b.backend(), io::BackendKind::kNpu);
  // 模型加载失败时必须是明确的 unavailable，而不是成功的空检测。
  io::HostTensor unavailable_input;
  unavailable_input.dims = {1, 3, 640, 640};
  unavailable_input.data.resize(3 * 640 * 640);
  const auto result =
      b.RunWithStatus(io::ModelInput{std::move(unavailable_input)});
  EXPECT_EQ(result.status, backend::AscendRunStatus::kUnavailable);
  EXPECT_TRUE(result.results.empty());
  if (!b.ready()) {
    EXPECT_NE(b.last_error(), ascend::AscendError::kSuccess);
  }
}

TEST(AscendInference, InvalidInputsAreTypedAndNeverLookLikeNoDetection) {
  ascend::OmModelConfig cfg;
  cfg.model_path = "does-not-exist.om";
  backend::AscendInferenceBackend b(cfg, 0);

  io::DeviceTensor empty_device;
  const auto result = b.RunWithStatus(io::ModelInput{empty_device});
  EXPECT_EQ(result.status, backend::AscendRunStatus::kInvalidInput);
  EXPECT_TRUE(result.results.empty());
}

TEST(AscendInference, StatusStringIsStable) {
  EXPECT_STREQ(backend::AscendRunStatusToString(
                   backend::AscendRunStatus::kUnavailable),
               "unavailable");
  EXPECT_STREQ(backend::AscendRunStatusToString(
                   backend::AscendRunStatus::kInvalidInput),
               "invalid_input");
}

TEST(AscendInference, EmptyDeviceInputIsRejected) {
  ascend::OmModelConfig cfg;
  cfg.model_path = "m.om";
  backend::AscendInferenceBackend b(cfg, 0);
  if (!b.ready()) GTEST_SKIP() << "无 CANN，后端未就绪";

  io::DeviceTensor empty;
  EXPECT_TRUE(b.Run(io::ModelInput{empty}).empty());
}

// ---------------------------------------------------------------- 解码器

TEST(AscendDecoder, OpenSucceedsOnlyIfChannelBecomesValid) {
  ascend::VdecChannelConfig cfg;
  pipeline::AscendDecoder d(cfg);
  EXPECT_EQ(d.backend(), io::BackendKind::kNpu);

  pipeline::StreamDesc desc{"rtsp://127.0.0.1/none", false};
  const bool ok = d.Open(desc);
  // 关键不变量：Open 返回 true 当且仅当通道真的可用。
  EXPECT_EQ(ok, d.valid());
}

TEST(AscendDecoder, ChannelIdStaysInsideVdecRange) {
  ascend::VdecChannelConfig cfg;
  pipeline::AscendDecoder d(cfg);
  pipeline::StreamDesc desc{"rtsp://127.0.0.1/none", false};
  d.Open(desc);
  if (!d.valid()) GTEST_SKIP() << "无 CANN，通道未真正创建";

  EXPECT_GE(d.channel_id(), ascend::kVdecChannelMin);
  EXPECT_LE(d.channel_id(), ascend::kVdecChannelMax);
}

TEST(FfmpegDecoder, StartsClosedAndYieldsNoFrame) {
  pipeline::FfmpegDecoder d;
  EXPECT_EQ(d.backend(), io::BackendKind::kCpu);
  EXPECT_FALSE(d.IsOpen());
  EXPECT_EQ(d.Width(), 0);
  EXPECT_EQ(d.Height(), 0);
  EXPECT_EQ(d.pixel_format(), io::PixelFormat::kUnknown);

  cv::Mat m;
  EXPECT_FALSE(d.ReceiveFrameTo(m));  // 未打开时不得出帧
}

// ---------------------------------------------------------------- 图像处理器

TEST(DvppImageProcessor, SupportsPreprocessOpsButLeavesNormalizeToAipp) {
  imaging::DvppImageProcessor p(0);
  EXPECT_EQ(p.backend(), io::BackendKind::kNpu);
  EXPECT_TRUE(p.Supports(imaging::Op::kResize));
  EXPECT_TRUE(p.Supports(imaging::Op::kPixelFormatConvert));
  EXPECT_TRUE(p.Supports(imaging::Op::kLetterbox));
  // 归一化由动态 AIPP 承担，本模块刻意不承担。
  EXPECT_FALSE(p.Supports(imaging::Op::kNormalize));
}

// 零拷贝的核心断言：设备帧的指针必须原样透传给推理引擎。
TEST(DvppImageProcessor, DeviceFrameBecomesDeviceTensorWithoutCopy) {
  const io::Size kSize{64, 64};
  const std::size_t bytes = io::FrameBytes(kSize, io::PixelFormat::kNv12);
  ASSERT_GT(bytes, 0u);

  std::vector<std::byte> buf(bytes, std::byte{0});
  auto storage = std::make_shared<imaging::DeviceFrameStorage>(
      buf.data(), bytes, kSize, io::PixelFormat::kNv12, /*pts=*/0,
      /*device_id=*/0);
  imaging::Frame frame(storage, io::MemoryLocation::kDevice);

  imaging::DvppImageProcessor p(0);
  io::ModelInput mi = p.PrepareModelInput(frame, io::ModelSpec{});

  ASSERT_TRUE(std::holds_alternative<io::DeviceTensor>(mi))
      << "设备帧必须产出 DeviceTensor，不能回落到 Host";
  const auto& dt = std::get<io::DeviceTensor>(mi);
  EXPECT_EQ(dt.ptr, buf.data());  // 同一个地址 = 没有拷贝
  EXPECT_EQ(dt.bytes, bytes);
  EXPECT_EQ(dt.size, kSize);
  EXPECT_EQ(dt.format, io::PixelFormat::kNv12);
}

// ---------------------------------------------------------------- 工厂预设

TEST(PipelineFactory, CpuPresetProducesCpuFamily) {
  auto f = pipeline::CreatePipelineFactory(io::BackendKind::kCpu);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->backend(), io::BackendKind::kCpu);

  auto proc = f->CreateImageProcessor();
  ASSERT_NE(proc, nullptr);
  EXPECT_EQ(proc->backend(), io::BackendKind::kCpu);
}

TEST(PipelineFactory, NpuPresetProducesHomogeneousNpuFamily) {
  auto f = pipeline::CreatePipelineFactory(io::BackendKind::kNpu, 0);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->backend(), io::BackendKind::kNpu);

  auto proc = f->CreateImageProcessor();
  ASSERT_NE(proc, nullptr);
  EXPECT_EQ(proc->backend(), io::BackendKind::kNpu);  // 不许混进 CPU 预处理

  auto inf = f->CreateInference(io::ModelSpec{}, 0);
  ASSERT_NE(inf, nullptr);
  EXPECT_EQ(inf->backend(), io::BackendKind::kNpu);
}

}  // namespace
