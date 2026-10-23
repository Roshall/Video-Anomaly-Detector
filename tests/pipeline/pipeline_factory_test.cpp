// Abstract Factory 契约测试：同一工厂产出的"解码器 / 图像处理器 / 推理后端 / 编码器"
// 必须属于同一后端族。这是防止"NPU 解码配 CPU 预处理"这类会隐式引入 D2H 组合的关键断言。
// 这里用替身工厂验证契约本身；真实 CpuPipelineFactory / NpuPipelineFactory 在本文件底部
// 以同样的方式注册（需要 FFmpeg / CANN 才能编译，见 CMake 条件）。
#include <gtest/gtest.h>

#include <memory>

#include "backend/inference_backend.h"
#include "imaging/image_processor.h"
#include "pipeline/null_encoder.h"
#include "pipeline/pipeline_factory.h"

namespace avs_analyzer::pipeline {
namespace {

// ---- 替身 ----

class FakeDecoder final : public IDecoder {
 public:
  explicit FakeDecoder(BackendKind k) : k_(k) {}
  [[nodiscard]] BackendKind backend() const noexcept override { return k_; }
  [[nodiscard]] bool Open(const StreamDesc& d) override {
    opened = d;
    return !d.url.empty();
  }
  StreamDesc opened{};

 private:
  BackendKind k_;
};

class FakeEncoder final : public IEncoder {
 public:
  explicit FakeEncoder(BackendKind k) : k_(k) {}
  [[nodiscard]] BackendKind backend() const noexcept override { return k_; }
  bool Encode(const imaging::Frame&) override {
    ++encoded;
    return true;
  }
  int encoded = 0;

 private:
  BackendKind k_;
};

class FakeProcessor final : public imaging::IImageProcessor {
 public:
  explicit FakeProcessor(BackendKind k) : k_(k) {}
  [[nodiscard]] BackendKind backend() const noexcept override { return k_; }
  [[nodiscard]] bool Supports(imaging::Op) const noexcept override { return true; }
  [[nodiscard]] std::unique_ptr<imaging::Frame> ResizeAndConvert(
      const imaging::Frame&, imaging::Size, imaging::PixelFormat) override {
    return nullptr;
  }
  [[nodiscard]] imaging::ModelInput PrepareModelInput(
      const imaging::Frame&, const imaging::ModelSpec&) override {
    return imaging::HostTensor{};
  }

 private:
  BackendKind k_;
};

class FakeInference final : public IInferenceBackend {
 public:
  explicit FakeInference(BackendKind k) : k_(k) {}
  [[nodiscard]] BackendKind backend() const noexcept override { return k_; }
  [[nodiscard]] std::vector<DetectionResult> Run(
      const imaging::ModelInput&) override {
    return {};
  }

 private:
  BackendKind k_;
};

template <BackendKind K>
class FakePipelineFactory final : public IPipelineFactory {
 public:
  [[nodiscard]] BackendKind backend() const noexcept override { return K; }
  [[nodiscard]] std::unique_ptr<IDecoder> CreateDecoder(const StreamDesc&) override {
    return std::make_unique<FakeDecoder>(K);
  }
  [[nodiscard]] std::unique_ptr<imaging::IImageProcessor> CreateImageProcessor() override {
    return std::make_unique<FakeProcessor>(K);
  }
  [[nodiscard]] std::unique_ptr<IInferenceBackend> CreateInference(
      const imaging::ModelSpec&, int) override {
    return std::make_unique<FakeInference>(K);
  }
  [[nodiscard]] std::unique_ptr<IEncoder> CreateEncoder(const EncodeDesc&) override {
    return std::make_unique<FakeEncoder>(K);
  }
};

// ---- 契约测试（对所有工厂实现生效）----

struct FactoryEntry {
  std::string name;
  std::function<std::unique_ptr<IPipelineFactory>()> create;
};

class PipelineFactoryContract : public ::testing::TestWithParam<FactoryEntry> {
 protected:
  std::unique_ptr<IPipelineFactory> Make() const { return GetParam().create(); }
};

INSTANTIATE_TEST_SUITE_P(
    Factories, PipelineFactoryContract,
    ::testing::Values(
        FactoryEntry{"fake_cpu",
                     [] { return std::make_unique<FakePipelineFactory<BackendKind::kCpu>>(); }},
        FactoryEntry{"fake_npu",
                     [] {
                       return std::make_unique<FakePipelineFactory<BackendKind::kNpu>>();
                     }}
        // CpuPipelineFactory / NpuPipelineFactory 在具备完整依赖时由 CMake 追加注册
        ),
    [](const ::testing::TestParamInfo<FactoryEntry>& info) { return info.param.name; });

TEST_P(PipelineFactoryContract, ProducesCompleteFamily) {
  auto f = Make();
  StreamDesc s{"rtsp://cam/1"};
  EncodeDesc e{imaging::Size{1920, 1080}, 25, "rtsp://out/1"};
  imaging::ModelSpec m;

  EXPECT_NE(f->CreateDecoder(s), nullptr);
  EXPECT_NE(f->CreateImageProcessor(), nullptr);
  EXPECT_NE(f->CreateInference(m, 0), nullptr);
  EXPECT_NE(f->CreateEncoder(e), nullptr);
}

TEST_P(PipelineFactoryContract, FamilyMembersShareTheSameBackend) {
  auto f = Make();
  const auto expect = f->backend();
  imaging::ModelSpec m;

  EXPECT_EQ(f->CreateDecoder(StreamDesc{"rtsp://cam/1"})->backend(), expect);
  EXPECT_EQ(f->CreateImageProcessor()->backend(), expect);
  EXPECT_EQ(f->CreateInference(m, 0)->backend(), expect);
  EXPECT_EQ(f->CreateEncoder(EncodeDesc{})->backend(), expect);
}

// ---- Null Object ----

TEST(NullEncoderTest, IsSafeNoOp) {
  NullEncoder enc;
  EXPECT_EQ(enc.backend(), BackendKind::kCpu);
  // 不构造真实 Frame 也能验证接口存在；真实调用由 NullAnnotator/Recorder 侧保证
  EXPECT_NO_THROW(enc.Flush());
}

}  // namespace
}  // namespace avs_analyzer::pipeline
