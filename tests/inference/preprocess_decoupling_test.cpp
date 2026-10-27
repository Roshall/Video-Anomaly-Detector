// TDD：图像预处理与推理引擎解耦的契约测试。
//
// 目标：预处理（letterbox + 像素格式 + 归一化）可以从推理引擎里拿出来，
// 选择跑在 CPU（OpenCV）或 NPU（DVPP + AIPP）上；推理引擎只消费中性契约
// io::ModelInput，不关心像素在哪、谁做的预处理。
//
// 红灯阶段：backend/onnx_inference_backend.h 以及 YOLOv8Detect 的张量入口
// 尚不存在，本文件应当编译失败。

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>

#include "backend/inference_backend.h"
#include "backend/onnx_inference_backend.h"
#include "detector/inference.h"
#include "imaging/frame.h"
#include "imaging/frame_storage.h"
#include "imaging/opencv_image_processor.h"
#include "io/tensor.h"
#include "pipeline/pipeline_factories.h"

namespace {

using avs_analyzer::io::BackendKind;
using avs_analyzer::io::HostTensor;
using avs_analyzer::io::ModelInput;
using avs_analyzer::io::ModelSpec;
using avs_analyzer::io::PixelFormat;
using avs_analyzer::io::Size;

constexpr int kSrcW = 1280;
constexpr int kSrcH = 720;
constexpr int kDst = 640;

cv::Mat MakeBgr(int w, int h, int seq) {
  cv::Mat m(h, w, CV_8UC3);
  for (int y = 0; y < h; ++y) {
    auto* row = m.ptr<cv::Vec3b>(y);
    for (int x = 0; x < w; ++x) {
      row[x] = cv::Vec3b(static_cast<uchar>((x * 3 + seq) & 0xFF),
                         static_cast<uchar>((y * 5 + seq * 2) & 0xFF),
                         static_cast<uchar>((x + y + seq) & 0xFF));
    }
  }
  return m;
}

std::shared_ptr<avs_analyzer::imaging::Frame> ToHostFrame(const cv::Mat& bgr) {
  std::vector<std::byte> bytes(static_cast<std::size_t>(bgr.total()) *
                               bgr.elemSize());
  std::memcpy(bytes.data(), bgr.data, bytes.size());
  auto storage = std::make_shared<avs_analyzer::imaging::HostFrameStorage>(
      std::move(bytes), Size{bgr.cols, bgr.rows}, PixelFormat::kBgr24, 0);
  return std::make_shared<avs_analyzer::imaging::Frame>(
      storage, avs_analyzer::imaging::MemoryLocation::kHost, nullptr);
}

}  // namespace

// ---------------------------------------------------------------------------
// 契约 1：预处理必须是可独立调用的一步（不是焊在 RunSession 内部）
// ---------------------------------------------------------------------------

TEST(PreprocessDecoupling, YoloV8DetectExposesStandalonePreprocess) {
  YOLOv8Detect yolo;  // 未加载模型也应可用（预处理不依赖 session）

  const cv::Mat bgr = MakeBgr(kSrcW, kSrcH, 7);
  const auto tensor = yolo.Preprocess(bgr);

  // letterbox 到 640x640，NCHW
  ASSERT_EQ(tensor.dims[0], 1);
  ASSERT_EQ(tensor.dims[1], 3);
  EXPECT_EQ(tensor.dims[2], kDst);
  EXPECT_EQ(tensor.dims[3], kDst);
  EXPECT_EQ(tensor.data.size(),
            static_cast<std::size_t>(3) * kDst * kDst);
}

TEST(PreprocessDecoupling, PreprocessNormalizesToUnitRange) {
  YOLOv8Detect yolo;
  const auto tensor = yolo.Preprocess(MakeBgr(320, 240, 3));

  ASSERT_FALSE(tensor.data.empty());
  for (const float v : tensor.data) {
    EXPECT_GE(v, 0.0f);
    EXPECT_LE(v, 1.0f);
  }
}

// ---------------------------------------------------------------------------
// 契约 2：外部图像引擎的产物必须与 YOLOv8Detect 内部预处理像素级等价
//
// 这条是解耦成立的前提：如果两条路径数值不一致，那么"换到 NPU 做预处理"
// 就会静默改变检测结果，解耦也就没有意义。
// ---------------------------------------------------------------------------

TEST(PreprocessDecoupling, ImageProcessorMatchesYoloInternalPreprocess) {
  const cv::Mat bgr = MakeBgr(kSrcW, kSrcH, 11);

  YOLOv8Detect yolo;
  const auto internal = yolo.Preprocess(bgr);

  avs_analyzer::imaging::OpenCvImageProcessor proc;
  auto frame = ToHostFrame(bgr);

  ModelSpec spec;
  spec.input_size = Size{kDst, kDst};
  spec.input_format = PixelFormat::kRgb24;
  spec.normalize = true;
  spec.planar_nchw = true;

  const ModelInput external_input = proc.PrepareModelInput(*frame, spec);
  ASSERT_TRUE(std::holds_alternative<HostTensor>(external_input));
  const auto& external = std::get<HostTensor>(external_input);

  EXPECT_EQ(external.dims, internal.dims);
  ASSERT_EQ(external.data.size(), internal.data.size());
  for (std::size_t i = 0; i < external.data.size(); ++i) {
    EXPECT_FLOAT_EQ(external.data[i], internal.data[i]) << "index " << i;
  }
}

// ---------------------------------------------------------------------------
// 契约 3：张量必须自带 letterbox 元信息
//
// 预处理外置后，推理引擎拿到的是 640x640 的张量，它不知道原图尺寸，
// 因此坐标还原所需的 scale/pad 只能随张量一起传递。
// ---------------------------------------------------------------------------

TEST(PreprocessDecoupling, PreparedTensorCarriesLetterboxInfo) {
  YOLOv8Detect yolo;
  const auto tensor = yolo.Preprocess(MakeBgr(kSrcW, kSrcH, 5));

  // 1280x720 -> 640x640: r = min(0.5, 0.888..) = 0.5
  // new_w=640, new_h=360 -> pad_x=0, pad_y=140
  EXPECT_FLOAT_EQ(tensor.letterbox.scale, 0.5f);
  EXPECT_EQ(tensor.letterbox.pad_x, 0);
  EXPECT_EQ(tensor.letterbox.pad_y, 140);
}

TEST(PreprocessDecoupling, DeletterboxRoundTripsCanvasToSource) {
  YOLOv8Detect yolo;
  const auto tensor = yolo.Preprocess(MakeBgr(kSrcW, kSrcH, 5));
  const auto& lb = tensor.letterbox;

  // 原图 (100,100) 经 letterbox 后落在 canvas 的：
  const float cx = 100.0f * lb.scale + lb.pad_x;
  const float cy = 100.0f * lb.scale + lb.pad_y;

  const cv::Rect back =
      YOLOv8Detect::DeletterboxXYXY(cx, cy, cx + 10, cy + 10, lb);
  EXPECT_EQ(back.x, 100);
  EXPECT_EQ(back.y, 100);
}

// ---------------------------------------------------------------------------
// 契约 4：推理引擎只认张量，不再触碰 cv::Mat
// ---------------------------------------------------------------------------

TEST(PreprocessDecoupling, OnnxBackendIsAnInferenceBackend) {
  auto backend =
      avs_analyzer::backend::CreateOnnxInferenceBackendForTest();

  ASSERT_NE(backend, nullptr);
  const avs_analyzer::backend::IInferenceBackend* iface = backend.get();
  EXPECT_EQ(iface->backend(), BackendKind::kCpu);
}

// 未加载模型时（压测/单测常见），Run 必须安全返回空而不是崩溃。
TEST(PreprocessDecoupling, OnnxBackendWithoutModelReturnsEmpty) {
  auto backend =
      avs_analyzer::backend::CreateOnnxInferenceBackendForTest();

  HostTensor t;
  t.dims = {1, 3, 640, 640};
  t.data.resize(static_cast<std::size_t>(3) * 640 * 640, 0.5f);
  t.letterbox = {1.0f, 0, 0};

  const auto results = backend->Run(ModelInput{t});
  EXPECT_TRUE(results.empty());
}

// DeviceTensor 分支（NPU 预处理产物）在无硬件时应优雅降级，不得崩溃。
TEST(PreprocessDecoupling, OnnxBackendRejectsDeviceTensorGracefully) {
  auto backend =
      avs_analyzer::backend::CreateOnnxInferenceBackendForTest();

  avs_analyzer::io::DeviceTensor d;
  d.ptr = nullptr;
  d.bytes = 0;
  d.device_id = -1;
  d.size = Size{kDst, kDst};
  d.format = PixelFormat::kNv12;

  const auto results = backend->Run(ModelInput{d});
  EXPECT_TRUE(results.empty());
}

// ---------------------------------------------------------------------------
// 契约 5：工厂按族成组提供两个引擎，避免跨族混搭
// ---------------------------------------------------------------------------

TEST(PreprocessDecoupling, CpuFactoryProvidesBothEngines) {
  avs_analyzer::pipeline::CpuPipelineFactory factory;

  auto processor = factory.CreateImageProcessor();
  auto inference = factory.CreateInference(ModelSpec{}, 0);

  ASSERT_NE(processor, nullptr);
  EXPECT_EQ(processor->backend(), BackendKind::kCpu);

  // 此前 CPU 工厂返回 nullptr + 一条 LOGW；解耦后必须给出可用的 CPU 后端。
  ASSERT_NE(inference, nullptr);
  EXPECT_EQ(inference->backend(), BackendKind::kCpu);
}
