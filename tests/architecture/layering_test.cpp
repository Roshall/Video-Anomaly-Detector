// 架构分层测试：把"推理引擎 / 图像处理引擎解耦"变成可执行断言。
//
// 背景：为支持未来更多平台/NPU，两个引擎必须能独立编译、独立替换，
// 只通过一层中性契约（Analyzer/Core/io/）通信，彼此不得 include。
//
// 本测试扫描库头文件的 #include，越界即失败。它在真实 CMake 构建里生效
// （通过 -DANALYZER_SOURCE_ROOT 注入源码根）；缺该宏时自动跳过，不破坏
// 临时手动编译。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef ANALYZER_SOURCE_ROOT
#define ANALYZER_SOURCE_ROOT ""
#endif

namespace fs = std::filesystem;
namespace {

std::string Root() { return ANALYZER_SOURCE_ROOT; }
bool HasRoot() { return !Root().empty(); }

std::string Slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// 提取 "..." include 的项目内目标；忽略 <...> 系统头。
std::vector<std::string> QuotedIncludes(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream is(text);
  std::string line;
  while (std::getline(is, line)) {
    const auto pos = line.find("#include");
    if (pos == std::string::npos) continue;
    const auto q1 = line.find('"', pos);
    if (q1 == std::string::npos) continue;
    const auto q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos) continue;
    out.push_back(line.substr(q1 + 1, q2 - q1 - 1));
  }
  return out;
}

std::vector<fs::path> SourcesUnder(const fs::path& dir) {
  std::vector<fs::path> out;
  if (!fs::exists(dir)) return out;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    const auto ext = e.path().extension();
    if (ext == ".h" || ext == ".hpp" || ext == ".cpp") out.push_back(e.path());
  }
  return out;
}

bool StartsWith(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

}  // namespace

// 推理引擎不得 include 图像处理引擎的模块。
TEST(ArchitectureLayering, BackendMustNotIncludeImaging) {
  if (!HasRoot()) GTEST_SKIP() << "ANALYZER_SOURCE_ROOT 未定义";
  const auto dir = fs::path(Root()) / "Analyzer" / "Core" / "backend";
  bool saw_any = false;
  for (const auto& f : SourcesUnder(dir)) {
    saw_any = true;
    for (const auto& inc : QuotedIncludes(Slurp(f))) {
      EXPECT_FALSE(StartsWith(inc, "imaging/"))
          << f.filename().string() << " 不得依赖成像引擎，却 include 了 " << inc;
    }
  }
  EXPECT_TRUE(saw_any) << "未找到 backend 源文件，检查 ANALYZER_SOURCE_ROOT";
}

// 中性契约层 io 不得依赖任何引擎。
TEST(ArchitectureLayering, IoContractIsEngineAgnostic) {
  if (!HasRoot()) GTEST_SKIP() << "ANALYZER_SOURCE_ROOT 未定义";
  const auto dir = fs::path(Root()) / "Analyzer" / "Core" / "io";
  for (const auto& f : SourcesUnder(dir)) {
    for (const auto& inc : QuotedIncludes(Slurp(f))) {
      EXPECT_FALSE(StartsWith(inc, "imaging/") || StartsWith(inc, "backend/"))
          << f.filename().string() << " 契约层不得依赖引擎，却 include 了 " << inc;
    }
  }
}

// 图像处理引擎不得依赖推理引擎。
TEST(ArchitectureLayering, ImagingMustNotIncludeBackend) {
  if (!HasRoot()) GTEST_SKIP() << "ANALYZER_SOURCE_ROOT 未定义";
  const auto dir = fs::path(Root()) / "Analyzer" / "Core" / "imaging";
  for (const auto& f : SourcesUnder(dir)) {
    for (const auto& inc : QuotedIncludes(Slurp(f))) {
      EXPECT_FALSE(StartsWith(inc, "backend/"))
          << f.filename().string() << " 不得依赖推理引擎，却 include 了 " << inc;
    }
  }
}
