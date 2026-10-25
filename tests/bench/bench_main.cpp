// 自带 main 的独立压测可执行文件。
//
// 刻意**不链接 gtest**：这样能脱离测试框架单独跑（便于用 /usr/bin/time -v
// 或读 /proc/self/statm 采集 RSS），也不会把 gtest 的静态初始化算进内存基线。
#include <getopt.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "bench_harness.h"

namespace {

using namespace avsbench;

void PrintUsage(const char* argv0) {
  std::printf(
      "用法: %s [选项]\n"
      "\n"
      "帧与节奏:\n"
      "  --frames N              每路流的帧数（总投递 = N × --streams，默认 20000）\n"
      "  --streams N             并发流数，做容量测试时调大（默认 1）\n"
      "  --warmup N              预热帧数，不计入统计（默认 2000）\n"
      "  --phases N              稳态分段数，用于区分缓存增长与真泄漏（默认 2）\n"
      "  --fps F                 帧时间戳步长（默认 25）\n"
      "  --pacing none|realtime  是否按墙钟对齐 fps（默认 none = 尽快跑）\n"
      "\n"
      "合成帧:\n"
      "  --width W --height H    分辨率（默认 1920 1080）\n"
      "  --reuse-image 0|1       0=每帧 clone（把图像生命周期纳入测量，默认 1）\n"
      "\n"
      "mock 推理:\n"
      "  --mode sync|queued      sync=直连测下限；queued=走真实 mailbox 测队列/背压\n"
      "  --depth D               queued 的 mailbox 深度（1=live，4=offline）\n"
      "  --threads T             queued 的取任务线程数（默认 1）\n"
      "  --sim-cost-us U         模拟模型耗时（微秒，默认 2000）\n"
      "  --sim-jitter-us J       叠加均匀抖动上限（默认 0）\n"
      "  --sim-mode spin|sleep|hybrid  耗时注入方式（默认 spin）\n"
      "  --boxes N               每帧产出框数（默认 2）\n"
      "  --move-boxes 0|1        框是否随帧平移（默认 1；0=恒定框，ByteTrack 对照）\n"
      "  --seed S                确定性 PRNG 种子（默认 12345）\n"
      "\n"
      "接入的真实组件（默认关）:\n"
      "  --convert 0|1           真实 FormatConvertor 做 YUV->BGR（每帧）\n"
      "  --preprocess 0|1        真实 OpenCvImageProcessor 做 letterbox+NCHW+/255\n"
      "                          （只在帧被接纳时做，属推理的一部分）\n"
      "  开启后报告会多出「分阶段耗时」一节，用于定位瓶颈。\n"
      "\n"
      "被测链路:\n"
      "  --analyzer probe|general|bunkoff   默认 probe\n"
      "  --task breakin|wander|bunkoff|sleep|phone|climb  默认 breakin\n"
      "  --timeout-ms N          inference_result_timeout_ms（默认 300，别用 5000）\n"
      "  --event 0|1             是否挂 EventDetector（默认 1）\n"
      "  --admission 0|1         是否走背压判定（默认 1；0=每帧都投，用来压满系统）\n"
      "  --major-stride-ms N     事件机主检查间隔（默认 2000）\n"
      "  --minor-stride-ms N     事件机次检查间隔（默认 250）\n"
      "  --window N --min-vote N 投票窗口与门槛（默认 5 / 3）\n"
      "  提示：把两个 stride 调到约 1000/fps（如 30fps -> 33）可让系统\n"
      "        尝试分析每一帧，是做容量摸底的正确姿势。\n"
      "\n"
      "输出:\n"
      "  --rss-every N           每 N 帧采一次 RSS（默认 100）\n"
      "  --format text|json      报告格式（默认 text）\n"
      "  --dump-samples PATH     导出端到端逐帧延迟样本（CSV）\n"
      "  --strict                按阈值判定，不通过则非零退出\n"
      "  --max-wall-ms N         看门狗上限（0=关闭）\n",
      argv0);
}

bool ParseTask(const std::string& s, avs_analyzer::Task& out) {
  if (s == "breakin") { out = avs_analyzer::Task::kBreakIn; return true; }
  if (s == "wander") { out = avs_analyzer::Task::kWandering; return true; }
  if (s == "bunkoff") { out = avs_analyzer::Task::kBunkOff; return true; }
  if (s == "sleep") { out = avs_analyzer::Task::kSleepWork; return true; }
  if (s == "phone") { out = avs_analyzer::Task::kPhonePlaying; return true; }
  if (s == "climb") { out = avs_analyzer::Task::kWallClimbing; return true; }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
#ifndef NDEBUG
  std::fputs(
      "WARNING: 该可执行文件未定义 NDEBUG（非 Release 构建），延迟数据无参考价值，"
      "请用 -DCMAKE_BUILD_TYPE=Release 重新构建。\n",
      stderr);
#endif

  BenchOptions opt;
  std::string mode = "sync";
  std::string sim_mode = "spin";
  std::string pacing = "none";
  std::string analyzer_kind = "probe";
  std::string task = "breakin";
  std::string format = "text";
  std::string dump_path;
  bool strict = false;
  bool dump = false;

  static option long_opts[] = {
      {"frames", required_argument, nullptr, 'f'},
      {"streams", required_argument, nullptr, 'N'},
      {"warmup", required_argument, nullptr, 'w'},
      {"phases", required_argument, nullptr, 'P'},
      {"width", required_argument, nullptr, 'W'},
      {"height", required_argument, nullptr, 'H'},
      {"reuse-image", required_argument, nullptr, 'R'},
      {"fps", required_argument, nullptr, 'F'},
      {"pacing", required_argument, nullptr, 'p'},
      {"mode", required_argument, nullptr, 'm'},
      {"depth", required_argument, nullptr, 'd'},
      {"threads", required_argument, nullptr, 't'},
      {"sim-cost-us", required_argument, nullptr, 'c'},
      {"sim-jitter-us", required_argument, nullptr, 'j'},
      {"sim-mode", required_argument, nullptr, 'M'},
      {"event", required_argument, nullptr, 'e'},
      {"admission", required_argument, nullptr, 'a'},
      {"timeout-ms", required_argument, nullptr, 'T'},
      {"analyzer", required_argument, nullptr, 'A'},
      {"task", required_argument, nullptr, 'k'},
      {"boxes", required_argument, nullptr, 'b'},
      {"move-boxes", required_argument, nullptr, 'g'},
      {"seed", required_argument, nullptr, 's'},
      {"rss-every", required_argument, nullptr, 'r'},
      {"format", required_argument, nullptr, 'o'},
      {"dump-samples", required_argument, nullptr, 'D'},
      {"max-wall-ms", required_argument, nullptr, 'x'},
      {"convert", required_argument, nullptr, 'C'},
      {"preprocess", required_argument, nullptr, 'G'},
      {"major-stride-ms", required_argument, nullptr, '0'},
      {"minor-stride-ms", required_argument, nullptr, '1'},
      {"window", required_argument, nullptr, '2'},
      {"min-vote", required_argument, nullptr, '3'},
      {"strict", no_argument, nullptr, 'S'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };

  int c = 0;
  while ((c = getopt_long(argc, argv, "hf:N:w:P:W:H:R:F:p:m:d:t:c:j:M:e:a:T:A:k:b:g:s:r:o:D:x:C:G:0:1:2:3:Sh",
                          long_opts, nullptr)) != -1) {
    switch (c) {
      case 'f': opt.frames = std::strtoull(optarg, nullptr, 10); break;
      case 'N': opt.streams = static_cast<std::uint32_t>(std::atoi(optarg)); break;
      case 'w': opt.warmup_frames = std::strtoull(optarg, nullptr, 10); break;
      case 'P': opt.phases = std::strtoull(optarg, nullptr, 10); break;
      case 'W': opt.width = std::atoi(optarg); break;
      case 'H': opt.height = std::atoi(optarg); break;
      case 'R': opt.reuse_image = std::atoi(optarg) != 0; break;
      case 'F': opt.fps = std::atof(optarg); break;
      case 'p': pacing = optarg; break;
      case 'm': mode = optarg; break;
      case 'd': opt.mock.mailbox_depth = std::strtoull(optarg, nullptr, 10); break;
      case 't': opt.mock.worker_threads = static_cast<unsigned>(std::atoi(optarg)); break;
      case 'c': opt.mock.cost_ns = std::strtoull(optarg, nullptr, 10) * 1000ULL; break;
      case 'j': opt.mock.jitter_ns = std::strtoull(optarg, nullptr, 10) * 1000ULL; break;
      case 'M': sim_mode = optarg; break;
      case 'e': opt.use_event_detector = std::atoi(optarg) != 0; break;
      case 'a': opt.use_admission = std::atoi(optarg) != 0; break;
      case 'T': opt.inference_result_timeout_ms = std::atoi(optarg); break;
      case 'A': analyzer_kind = optarg; break;
      case 'k': task = optarg; break;
      case 'b': opt.mock.num_boxes = std::atoi(optarg); break;
      case 'g': opt.mock.move_boxes = std::atoi(optarg) != 0; break;
      case 's': opt.mock.seed = std::strtoull(optarg, nullptr, 10); break;
      case 'r': opt.rss_every = std::strtoull(optarg, nullptr, 10); break;
      case 'o': format = optarg; break;
      case 'D': dump = true; dump_path = optarg; break;
      case 'x': opt.max_wall_ms = std::strtoull(optarg, nullptr, 10); break;
      case '0': opt.event_cfg.major_stride_ms = std::atoi(optarg); break;
      case '1': opt.event_cfg.minor_stride_ms = std::atoi(optarg); break;
      case '2': opt.event_cfg.window_size = std::atoi(optarg); break;
      case '3': opt.event_cfg.min_vote = std::atoi(optarg); break;
      case 'C': opt.use_convert = std::atoi(optarg) != 0; break;
      case 'G': opt.use_preprocess = std::atoi(optarg) != 0; break;
      case 'S': strict = true; break;
      case 'h': PrintUsage(argv[0]); return 0;
      default: PrintUsage(argv[0]); return 2;
    }
  }

  opt.mock.mode = (mode == "queued") ? MockMode::kQueued : MockMode::kSync;
  opt.mock.cost_mode = (sim_mode == "sleep") ? CostMode::kSleep
                       : (sim_mode == "hybrid") ? CostMode::kHybrid
                                                : CostMode::kSpin;
  opt.realtime_pacing = (pacing == "realtime");
  opt.analyzer_kind = (analyzer_kind == "general")
                          ? AnalyzerKind::kGeneral
                          : (analyzer_kind == "bunkoff") ? AnalyzerKind::kBunkOff
                                                         : AnalyzerKind::kProbe;
  if (!ParseTask(task, opt.task)) {
    std::fprintf(stderr, "未知 --task: %s\n", task.c_str());
    return 2;
  }

  const BenchReport report = RunBench(opt);

  if (format == "json") {
    std::cout << report.ToJson().dump(2) << std::endl;
  } else {
    std::cout << report.ToText() << std::endl;
  }

  if (dump) {
    std::ofstream out(dump_path);
    out << "frame_index,e2e_ns\n";
    const auto& samples = report.latency.e2e_ns.samples();
    for (std::size_t i = 0; i < samples.size(); ++i) {
      out << i << "," << samples[i] << "\n";
    }
    std::printf("已导出 %zu 条延迟样本到 %s\n", samples.size(), dump_path.c_str());
  }

  if (!report.completed) return 1;

  if (strict) {
    const auto verdict = Evaluate(report, BenchThresholds{});
    if (!verdict.pass) {
      std::fprintf(stderr, "\n判定：不通过\n");
      for (const auto& f : verdict.failures) std::fprintf(stderr, "  - %s\n", f.c_str());
      return 1;
    }
    std::printf("\n判定：通过\n");
  }
  return 0;
}
