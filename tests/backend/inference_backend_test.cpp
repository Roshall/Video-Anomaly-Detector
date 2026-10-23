// Adapter 契约测试：IInferenceBackend 让两种推理后端（ORT / ACL）对业务层同形。
// 这里用 Spy 替身验证"输入原样透传 + 输出原样返回"，不依赖 ONNXRuntime 或 CANN。
#include <gtest/gtest.h>

#include <vector>

#include "backend/detection_result.h"
#include "backend/inference_backend.h"
#include "imaging/image_processor.h"

namespace avs_analyzer::backend {
namespace {

class SpyInferenceBackend final : public IInferenceBackend {
 public:
  explicit SpyInferenceBackend(std::vector<DetectionResult> canned)
      : canned_(std::move(canned)) {}

  [[nodiscard]] BackendKind backend() const noexcept override { return BackendKind::kCpu; }

  [[nodiscard]] std::vector<DetectionResult> Run(
      const imaging::ModelInput& in) override {
    last_input = in;
    ++calls;
    return canned_;
  }

  imaging::ModelInput last_input{};
  int calls = 0;

 private:
  std::vector<DetectionResult> canned_;
};

TEST(InferenceBackendTest, PassesModelInputThroughUnchanged) {
  imaging::HostTensor t;
  t.dims = {1, 3, 640, 640};
  t.data = std::vector<float>(1 * 3 * 640 * 640, 0.5f);

  SpyInferenceBackend spy({});
  (void)spy.Run(t);

  EXPECT_EQ(spy.calls, 1);
  ASSERT_TRUE(std::holds_alternative<imaging::HostTensor>(spy.last_input));
  const auto& got = std::get<imaging::HostTensor>(spy.last_input);
  EXPECT_EQ(got.dims, t.dims);
  EXPECT_EQ(got.data.size(), t.data.size());
  EXPECT_EQ(got.data.front(), 0.5f);
}

TEST(InferenceBackendTest, ReturnsCannedResultsVerbatively) {
  SpyInferenceBackend spy({{/*class_id=*/0, 0.9f, cv::Rect(1, 2, 3, 4)},
                           {/*class_id=*/2, 0.4f, cv::Rect(5, 6, 7, 8)}});

  imaging::HostTensor t;
  t.dims = {1, 3, 8, 8};
  t.data.resize(1 * 3 * 8 * 8);

  auto out = spy.Run(t);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].class_id, 0);
  EXPECT_FLOAT_EQ(out[0].confidence, 0.9f);
  EXPECT_EQ(out[0].box, cv::Rect(1, 2, 3, 4));
  EXPECT_EQ(out[1].class_id, 2);
}

TEST(InferenceBackendTest, DeviceInputIsAlsoAcceptedAsIs) {
  imaging::DeviceTensor d;
  d.bytes = 640 * 640 * 3 / 2;
  d.device_id = 3;
  d.size = imaging::Size{640, 640};
  d.format = imaging::PixelFormat::kNv12;

  SpyInferenceBackend spy({});
  (void)spy.Run(d);

  ASSERT_TRUE(std::holds_alternative<imaging::DeviceTensor>(spy.last_input));
  const auto& got = std::get<imaging::DeviceTensor>(spy.last_input);
  EXPECT_EQ(got.device_id, 3);
  EXPECT_EQ(got.format, imaging::PixelFormat::kNv12);
  EXPECT_EQ(got.bytes, d.bytes);
}

TEST(DetectionResultTest, FieldOrderAndTypes) {
  DetectionResult r{/*class_id=*/7, 0.25f, cv::Rect(10, 20, 30, 40)};
  EXPECT_EQ(r.class_id, 7);
  EXPECT_FLOAT_EQ(r.confidence, 0.25f);
  EXPECT_EQ(r.box.x, 10);
  EXPECT_EQ(r.box.width, 30);
}

}  // namespace
}  // namespace avs_analyzer::backend
