#include "bench_harness.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

#include <spdlog/spdlog.h>

#include "analyzer_registration.h"
#include "config.h"
#include "metrics.h"

namespace avsbench {

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t ToNs(Clock::duration d) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
}

std::string FormatBytes(std::uint64_t bytes) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.2f MiB",
                static_cast<double>(bytes) / (1024.0 * 1024.0));
  return buf;
}

std::string FormatNs(std::int64_t ns) {
  char buf[64];
  // 亚毫秒用微秒显示：纯管线基线只有几微秒，用 ms 显示会一律变成 0.000。
  if (std::fabs(static_cast<double>(ns)) < 1e6) {
    std::snprintf(buf, sizeof(buf), "%.2f us", static_cast<double>(ns) / 1e3);
  } else {
    std::snprintf(buf, sizeof(buf), "%.3f ms", static_cast<double>(ns) / 1e6);
  }
  return buf;
}

/// 只实现最小 Analyzer，用来测"管线本身"（analyzer_timeout_test 的同类手法）。
class ProbeAnalyzer : public avs_analyzer::Analyzer {
 public:
  ProbeAnalyzer(avs_analyzer::Monitor* monitor, const avs_analyzer::Config& cfg)
      : Analyzer(monitor, cfg) {}

  std::vector<cv::Rect> RetrieveDetection() const override {
    return FetchRawDetection().bbox;
  }
  void SetROIs(const std::vector<std::vector<cv::Point>>&) override {}
};

cv::Mat MakeSyntheticFrame(int w, int h) {
  cv::Mat img(h, w, CV_8UC3);
  // 内容不重要，但不能是全零（避免任何"空图"捷径）。用 randu 是为了快：
  // reuse_image=false 时每帧都要造一张，逐像素填充会让压测本身成为瓶颈。
  cv::randu(img, cv::Scalar(0, 0, 0), cv::Scalar(255, 255, 255));
  return img;
}

}  // namespace

avs_analyzer::EventDetectorConfig DefaultEventConfig() {
  return avs_analyzer::EventDetectorConfig{
      .major_stride_ms = 2000,
      .minor_stride_ms = 250,
      .window_size = 5,
      .min_vote = 3,
      .max_event_duration_ms = 60000,
      .latest_duration_ms = 0,
      .minimum_num = 1,
  };
}

BenchReport RunBench(const BenchOptions& opt) {
  BenchReport report;
  const auto prev_level = spdlog::get_level();
  // 全局开了 SPDLOG_LEVEL_TRACE；几万帧的状态迁移日志会刷爆磁盘并污染延迟。
  spdlog::set_level(spdlog::level::off);

  const auto t_start = Clock::now();
  auto wall_ms_now = [&t_start]() -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                t_start)
        .count();
  };

  try {
    // 1) Config：传入不存在的文件 -> 只 LOGE 并保留头文件缺省值。
    //    注意 clamp 仅在文件成功打开时才执行，所以这里可以设任意超时。
    avs_analyzer::Config cfg("bench_no_such_config.json");
    cfg.inference_result_timeout_ms = opt.inference_result_timeout_ms;

    // 2) Monitor：levels.first 必须非空（kWandering 读 [0]）。
    avs_analyzer::Monitor monitor;
    monitor.task = opt.task;
    monitor.conf_threshold = 0.5f;
    monitor.levels.first = {0};
    monitor.levels.second = {"low"};

    // 3) Analyzer 必须由 shared_ptr 持有：detector_callback() 内部用
    //    weak_from_this()，unique_ptr 会让回调静默失效。
    std::shared_ptr<avs_analyzer::Analyzer> analyzer;
    if (opt.analyzer_kind == AnalyzerKind::kProbe) {
      analyzer = std::make_shared<ProbeAnalyzer>(&monitor, cfg);
    } else {
      analyzer = avs_analyzer::CreateAnalyzer(&monitor, cfg);
    }
    if (!analyzer) {
      report.error = "analyzer 创建失败";
      spdlog::set_level(prev_level);
      return report;
    }
    if (opt.analyzer_kind != AnalyzerKind::kProbe) {
      std::vector<cv::Point> full{{0, 0},
                                  {opt.width, 0},
                                  {opt.width, opt.height},
                                  {0, opt.height}};
      analyzer->SetROIs({full});
    }

    auto worker_metrics = std::make_shared<avs_analyzer::WorkerMetrics>();
    analyzer->SetMetrics(worker_metrics);

    // 4) 注入 mock 推理（不改任何生产代码）。
    //
    // 上游重构后 Analyzer 不再直接持有 AddTask 回调，而是持有
    // AnalyzerRegistration（RAII：析构时自动注销）。所以注入点变成
    // 自己构造一个 registration，把 MockInference 的 AddTaskFunc 塞进去。
    MockInferenceConfig mock_cfg = opt.mock;
    mock_cfg.frame_size = cv::Size(opt.width, opt.height);
    auto model_metrics = std::make_shared<avs_analyzer::ModelMetrics>();
    MockInference mock(mock_cfg, model_metrics);

    const auto analyzer_id = analyzer->id();
    auto registration = std::make_shared<avs_analyzer::AnalyzerRegistration>(
        analyzer_id,
        mock.Register(analyzer_id, analyzer->detector_callback(),
                      mock_cfg.mailbox_depth),
        [&mock, analyzer_id]() { mock.Deregister(analyzer_id); });
    analyzer->SetRegistration(registration);
    mock.Start();

    std::unique_ptr<avs_analyzer::EventDetector> detector;
    if (opt.use_event_detector) {
      detector =
          std::make_unique<avs_analyzer::EventDetector>(opt.event_cfg, *analyzer);
    }

    const auto shared_image =
        std::make_shared<const cv::Mat>(MakeSyntheticFrame(opt.width, opt.height));

    // 5) 采样器与统计桶：全部预分配，测量期间零分配。
    const std::uint64_t every = opt.rss_every ? opt.rss_every : 1;
    RssSampler sampler(static_cast<std::size_t>(opt.frames / every + 16));
    const std::uint64_t measured_count =
        opt.frames > opt.warmup_frames ? opt.frames - opt.warmup_frames : 0;
    report.latency.ReserveAll(static_cast<std::size_t>(measured_count + 1));

    // 阶段边界：warmup 末 + 各稳态段末（最后一段的末点就是循环结束）。
    const std::uint64_t steady = measured_count;
    const std::uint64_t phases = opt.phases ? opt.phases : 1;
    std::vector<std::uint64_t> boundaries{opt.warmup_frames};
    for (std::uint64_t k = 1; k < phases; ++k) {
      boundaries.push_back(opt.warmup_frames + steady * k / phases);
    }
    std::size_t next_boundary = 0;
    std::vector<std::uint64_t> boundary_rss;

    const double step_ms = opt.fps > 0.0 ? (1000.0 / opt.fps) : 40.0;
    auto next_deadline = Clock::now();

    // 6) 帧循环
    for (std::uint64_t i = 0; i < opt.frames; ++i) {
      const auto ts_ms = static_cast<std::int64_t>(static_cast<double>(i) * step_ms);
      const bool measured = i >= opt.warmup_frames;

      std::shared_ptr<avs_analyzer::SharedInference> shared;
      bool admitted = true;
      if (opt.use_admission) {
        std::vector<avs_analyzer::AdmissionEntry> entries{
            {analyzer->id(), monitor.task, analyzer->WantsFrame()}};
        const auto adm =
            avs_analyzer::AdmitFrame(entries, static_cast<std::uint64_t>(ts_ms));
        if (adm.groups.empty()) {
          admitted = false;
        } else {
          shared = adm.groups.front().shared;
        }
        report.frames_skipped += adm.skipped;
      }
      if (admitted) ++report.frames_submitted;

      std::shared_ptr<const cv::Mat> image =
          opt.reuse_image
              ? shared_image
              : std::make_shared<const cv::Mat>(
                    MakeSyntheticFrame(opt.width, opt.height));

      const auto t0 = Clock::now();
      if (admitted) {
        analyzer->AddVideoFrame(1, static_cast<std::uint64_t>(ts_ms), image,
                                shared);
      }
      const auto t1 = Clock::now();
      if (detector) {
        const auto state = detector->ProcessFrame(ts_ms, ts_ms);
        if (state == avs_analyzer::EventDetector::kEventStart) {
          ++report.event_starts;
        }
      } else if (admitted) {
        (void)analyzer->RetrieveDetection();
      }
      const auto t2 = Clock::now();

      if (measured && admitted) {
        report.latency.e2e_ns.Add(ToNs(t2 - t0));
        report.latency.submit_ns.Add(ToNs(t1 - t0));
        report.latency.wait_ns.Add(ToNs(t2 - t1));
        report.latency.mock_ns.Add(
            static_cast<std::int64_t>(mock.last_cost_ns()));
      }

      // 阶段边界：先 trim 再采样，让"真泄漏"从分配器水位里浮出来。
      while (next_boundary < boundaries.size() && i >= boundaries[next_boundary]) {
        RssSampler::TrimHeap();
        sampler.Sample(i, wall_ms_now());
        boundary_rss.push_back(sampler.points().back().rss_bytes);
        ++next_boundary;
      }
      if ((i % every) == 0) sampler.Sample(i, wall_ms_now());

      if (opt.realtime_pacing) {
        next_deadline += std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double, std::milli>(step_ms));
        std::this_thread::sleep_until(next_deadline);
      }

      if (opt.max_wall_ms &&
          wall_ms_now() > static_cast<std::int64_t>(opt.max_wall_ms)) {
        report.error = "看门狗触发：运行时间超过 max_wall_ms";
        break;
      }
    }

    // 7) 收尾：trim 后取终值
    RssSampler::TrimHeap();
    sampler.Sample(opt.frames, wall_ms_now());

    const auto& points = sampler.points();
    report.rss_final_bytes = points.back().rss_bytes;
    std::uint64_t peak = 0;
    for (const auto& p : points) peak = std::max(peak, p.peak_bytes);
    report.rss_peak_bytes = peak;

    if (!boundary_rss.empty()) report.rss_warmup_end_bytes = boundary_rss[0];
    report.rss_phase1_end_bytes =
        boundary_rss.size() > 1 ? boundary_rss[1] : report.rss_final_bytes;
    report.phase1_growth =
        static_cast<std::int64_t>(report.rss_phase1_end_bytes) -
        static_cast<std::int64_t>(report.rss_warmup_end_bytes);
    report.phase2_growth = static_cast<std::int64_t>(report.rss_final_bytes) -
                           static_cast<std::int64_t>(report.rss_phase1_end_bytes);
    report.slope_bytes_per_frame =
        sampler.SlopeBytesPerFrame(opt.warmup_frames, opt.frames);

    report.inferences = mock.inferred();
    report.stale_dropped = mock.stale_dropped();
    report.mailbox_depth_peak = mock.queue_depth_peak();
    report.detect_wait_timeout =
        worker_metrics->detect_wait_timeout.load(std::memory_order_relaxed);
    report.frames_total = opt.frames;

    // 收尾顺序：先停 mock，再停 analyzer（Stop 会 PushResult 唤醒等待方）。
    mock.Stop();
    analyzer->Stop();

    if (report.error.empty()) report.completed = true;
  } catch (const std::exception& e) {
    report.error = std::string("异常: ") + e.what();
  }

  report.wall_ms =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
          Clock::now() - t_start)
          .count();
  report.achieved_fps =
      report.wall_ms > 0.0
          ? static_cast<double>(opt.frames) / report.wall_ms * 1000.0
          : 0.0;
  spdlog::set_level(prev_level);
  return report;
}

BenchVerdict Evaluate(const BenchReport& r, const BenchThresholds& t) {
  BenchVerdict v;
  if (!r.completed) {
    v.failures.push_back("压测未完成: " + r.error);
    return v;
  }
  if (r.phase2_growth > static_cast<std::int64_t>(t.max_phase2_growth_bytes)) {
    v.failures.push_back("phase2 内存增长 " + FormatBytes(r.phase2_growth) +
                         " 超过阈值 " + FormatBytes(t.max_phase2_growth_bytes));
  }
  if (r.slope_bytes_per_frame > t.max_slope_bytes_per_frame) {
    v.failures.push_back("稳态斜率 " + std::to_string(r.slope_bytes_per_frame) +
                         " B/frame 超过阈值 " +
                         std::to_string(t.max_slope_bytes_per_frame));
  }
  if (r.rss_final_bytes >
      r.rss_warmup_end_bytes + t.max_final_over_warmup_bytes) {
    v.failures.push_back(
        "终值相对 warmup 末增长 " +
        FormatBytes(r.rss_final_bytes - r.rss_warmup_end_bytes) + " 超过阈值 " +
        FormatBytes(t.max_final_over_warmup_bytes));
  }
  if (r.rss_peak_bytes > r.rss_final_bytes + t.max_peak_over_final_bytes) {
    v.failures.push_back("峰值高出终值 " +
                         FormatBytes(r.rss_peak_bytes - r.rss_final_bytes) +
                         "，疑似瞬时无界堆积");
  }
  if (t.max_p99_e2e_ns > 0 &&
      r.latency.e2e_ns.Summarize().p99_ns > t.max_p99_e2e_ns) {
    v.failures.push_back("p99 端到端 " +
                         FormatNs(r.latency.e2e_ns.Summarize().p99_ns) +
                         " 超过阈值 " + FormatNs(t.max_p99_e2e_ns));
  }
  if (r.detect_wait_timeout > t.max_detect_wait_timeout) {
    v.failures.push_back("发生 " + std::to_string(r.detect_wait_timeout) +
                         " 次空等超时（EventDetector 与送帧节奏不同步）");
  }
  v.pass = v.failures.empty();
  return v;
}

nlohmann::json BenchReport::ToJson() const {
  const auto e2e = latency.e2e_ns.Summarize();
  const auto submit = latency.submit_ns.Summarize();
  const auto wait = latency.wait_ns.Summarize();
  const auto mock_s = latency.mock_ns.Summarize();
  return {
      {"completed", completed},
      {"error", error},
      {"framesTotal", frames_total},
      {"framesSubmitted", frames_submitted},
      {"framesSkipped", frames_skipped},
      {"inferences", inferences},
      {"staleDropped", stale_dropped},
      {"mailboxDepthPeak", mailbox_depth_peak},
      {"detectWaitTimeout", detect_wait_timeout},
      {"eventStarts", event_starts},
      {"latencyMs",
       {{"e2eP50", e2e.p50_ns / 1e6},
        {"e2eP95", e2e.p95_ns / 1e6},
        {"e2eP99", e2e.p99_ns / 1e6},
        {"e2eMax", e2e.max_ns / 1e6},
        {"submitP50", submit.p50_ns / 1e6},
        {"waitP50", wait.p50_ns / 1e6},
        {"mockMean", mock_s.mean_ns / 1e6}}},
      {"rssMiB",
       {{"warmupEnd", rss_warmup_end_bytes / (1024.0 * 1024.0)},
        {"phase1End", rss_phase1_end_bytes / (1024.0 * 1024.0)},
        {"final", rss_final_bytes / (1024.0 * 1024.0)},
        {"peak", rss_peak_bytes / (1024.0 * 1024.0)},
        {"phase1Growth", phase1_growth / (1024.0 * 1024.0)},
        {"phase2Growth", phase2_growth / (1024.0 * 1024.0)}}},
      {"slopeBytesPerFrame", slope_bytes_per_frame},
      {"wallMs", wall_ms},
      {"achievedFps", achieved_fps},
  };
}

std::string BenchReport::ToText() const {
  const auto e2e = latency.e2e_ns.Summarize();
  const auto submit = latency.submit_ns.Summarize();
  const auto wait = latency.wait_ns.Summarize();
  const auto mock_s = latency.mock_ns.Summarize();

  std::string out;
  out += "=========== 端到端压测报告（mock YOLO）===========\n";
  out += "完成: " + std::string(completed ? "是" : "否");
  if (!error.empty()) out += "  错误: " + error;
  out += "\n";
  out += "帧数        : 总 " + std::to_string(frames_total) + " / 提交 " +
         std::to_string(frames_submitted) + " / 跳过 " +
         std::to_string(frames_skipped) + "\n";
  out += "推理次数    : " + std::to_string(inferences) + "   stale 丢弃 " +
         std::to_string(stale_dropped) + "\n";
  out += "队列峰值    : " + std::to_string(mailbox_depth_peak) +
         "   空等超时 " + std::to_string(detect_wait_timeout) +
         "   事件起始 " + std::to_string(event_starts) + "\n";
  out += "墙钟        : " + std::to_string(wall_ms) + " ms   吞吐 " +
         std::to_string(achieved_fps) + " fps\n";

  out += "\n--- 内存 ---\n";
  out += "warmup 末   : " + FormatBytes(rss_warmup_end_bytes) + "\n";
  out += "phase1 末   : " + FormatBytes(rss_phase1_end_bytes) + "  (增长 " +
         FormatBytes(static_cast<std::uint64_t>(std::max<std::int64_t>(0, phase1_growth))) +
         ")\n";
  out += "终值        : " + FormatBytes(rss_final_bytes) + "  (增长 " +
         FormatBytes(static_cast<std::uint64_t>(std::max<std::int64_t>(0, phase2_growth))) +
         ")\n";
  out += "峰值        : " + FormatBytes(rss_peak_bytes) + "\n";
  out += "稳态斜率    : " + std::to_string(slope_bytes_per_frame) +
         " bytes/frame\n";

  out += "\n--- 延迟（仅统计 warmup 之后）---\n";
  out += "端到端      : p50 " + FormatNs(e2e.p50_ns) + "  p95 " +
         FormatNs(e2e.p95_ns) + "  p99 " + FormatNs(e2e.p99_ns) + "  max " +
         FormatNs(e2e.max_ns) + "\n";
  out += "  提交段    : p50 " + FormatNs(submit.p50_ns) + "\n";
  out += "  等待段    : p50 " + FormatNs(wait.p50_ns) + "\n";
  out += "mock 自检   : mean " + FormatNs(static_cast<std::int64_t>(mock_s.mean_ns)) +
         "（应接近配置的模拟耗时）\n";
  return out;
}

}  // namespace avsbench
