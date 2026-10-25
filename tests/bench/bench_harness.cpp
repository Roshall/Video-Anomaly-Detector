#include "bench_harness.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "analyzer_registration.h"
#include "config.h"
#include "metrics.h"
#include "pipeline_stages.h"

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

std::string FormatPercent(double ratio) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f%%", ratio * 100.0);
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

/// 一路流的全部状态。多路时每路跑在自己的线程上。
struct StreamCtx {
  std::shared_ptr<avs_analyzer::Analyzer> analyzer;
  std::unique_ptr<avs_analyzer::EventDetector> detector;
  LatencyBuckets latency;
  std::uint64_t admitted{0};
  std::uint64_t event_starts{0};
  std::int64_t max_lag_ms{0};
  std::unique_ptr<ConvertStage> convert;
  std::unique_ptr<PreprocessStage> preprocess;
};

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
  double expected_ms = 0.0;  // 理想墙钟：按时序"应该"花多久
  auto wall_ms_now = [&t_start]() -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                t_start)
        .count();
  };

  try {
    // 1) Config：传入不存在的文件 -> 只 LOGE 并保留头文件缺省值。
    avs_analyzer::Config cfg("bench_no_such_config.json");
    cfg.inference_result_timeout_ms = opt.inference_result_timeout_ms;

    const std::uint32_t streams = opt.streams ? opt.streams : 1u;
    const std::uint64_t per_stream = opt.frames;
    const std::uint64_t offered = per_stream * streams;

    auto worker_metrics = std::make_shared<avs_analyzer::WorkerMetrics>();
    auto model_metrics = std::make_shared<avs_analyzer::ModelMetrics>();

    // 2) mock 必须先于 analyzers 声明：Analyzer 持有 AnalyzerRegistration，
    //    其析构回调会调 mock.Deregister()，所以 mock 必须比它们活得久。
    MockInferenceConfig mock_cfg = opt.mock;
    mock_cfg.frame_size = cv::Size(opt.width, opt.height);
    MockInference mock(mock_cfg, model_metrics);

    // 3) 每路流一套 Monitor / Analyzer / Registration / EventDetector。
    std::vector<avs_analyzer::Monitor> monitors(streams);
    std::vector<StreamCtx> ctxs(streams);

    for (std::uint32_t s = 0; s < streams; ++s) {
      auto& monitor = monitors[s];
      monitor.task = opt.task;
      monitor.conf_threshold = 0.5f;
      monitor.levels.first = {0};  // kWandering 读 [0]，空则 UB
      monitor.levels.second = {"low"};

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
      analyzer->SetMetrics(worker_metrics);

      // 注入点：自己构造 registration，把 mock 的 AddTaskFunc 塞进去。
      const auto analyzer_id = analyzer->id();
      auto registration = std::make_shared<avs_analyzer::AnalyzerRegistration>(
          analyzer_id,
          mock.Register(analyzer_id, analyzer->detector_callback(),
                        mock_cfg.mailbox_depth),
          [&mock, analyzer_id]() { mock.Deregister(analyzer_id); });
      analyzer->SetRegistration(registration);

      ctxs[s].analyzer = std::move(analyzer);
      if (opt.use_event_detector) {
        ctxs[s].detector = std::make_unique<avs_analyzer::EventDetector>(
            opt.event_cfg, *ctxs[s].analyzer);
      }
      ctxs[s].latency.ReserveAll(static_cast<std::size_t>(
          per_stream > opt.warmup_frames ? per_stream - opt.warmup_frames + 1
                                         : 1));

      if (opt.use_convert) {
        ctxs[s].convert = std::make_unique<ConvertStage>(opt.width, opt.height,
                                                        opt.width, opt.height);
      }
      if (opt.use_preprocess) {
        ctxs[s].preprocess = std::make_unique<PreprocessStage>(opt.preprocess_spec);
      }
    }

    mock.Start();

    const auto shared_image =
        std::make_shared<const cv::Mat>(MakeSyntheticFrame(opt.width, opt.height));

    // 4) 采样器：单独线程按墙钟采样（多路时主线程只负责等待）。
    const std::uint64_t every = opt.rss_every ? opt.rss_every : 1;
    RssSampler sampler(static_cast<std::size_t>(offered / every + 4096));
    report.latency.ReserveAll(static_cast<std::size_t>(offered + 1));

    const std::uint64_t steady =
        per_stream > opt.warmup_frames
            ? (per_stream - opt.warmup_frames) * streams
            : 0;
    const std::uint64_t phases = opt.phases ? opt.phases : 1;
    const std::uint64_t warmup_total = opt.warmup_frames * streams;
    std::vector<std::uint64_t> boundaries{warmup_total};
    for (std::uint64_t k = 1; k < phases; ++k) {
      boundaries.push_back(warmup_total + steady * k / phases);
    }
    std::size_t next_boundary = 0;
    std::vector<std::uint64_t> boundary_rss;

    std::atomic<std::uint64_t> progress{0};
    std::atomic<bool> stop_sampling{false};
    std::atomic<bool> abort{false};

    std::jthread sampler_thread([&] {
      while (!stop_sampling.load(std::memory_order_acquire)) {
        const auto p = progress.load(std::memory_order_relaxed);
        while (next_boundary < boundaries.size() &&
               p >= boundaries[next_boundary]) {
          RssSampler::TrimHeap();
          sampler.Sample(p, wall_ms_now());
          boundary_rss.push_back(sampler.points().back().rss_bytes);
          ++next_boundary;
        }
        if ((p % every) == 0) sampler.Sample(p, wall_ms_now());
        if (opt.max_wall_ms &&
            wall_ms_now() > static_cast<std::int64_t>(opt.max_wall_ms)) {
          report.error = "看门狗触发：运行时间超过 max_wall_ms";
          abort.store(true, std::memory_order_release);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });

    // 5) 每路流一个线程，各自按 fps 节奏送帧。
    const double step_ms = opt.fps > 0.0 ? (1000.0 / opt.fps) : 40.0;
    expected_ms = static_cast<double>(per_stream) * step_ms;
    std::vector<std::jthread> stream_threads;
    for (std::uint32_t s = 0; s < streams; ++s) {
      stream_threads.emplace_back([&, s] {
        auto& c = ctxs[s];
        auto deadline = Clock::now();
        for (std::uint64_t tick = 0; tick < per_stream; ++tick) {
          if (abort.load(std::memory_order_acquire)) break;

          const auto ts_ms =
              static_cast<std::int64_t>(static_cast<double>(tick) * step_ms);
          const bool measured = tick >= opt.warmup_frames;
          progress.fetch_add(1, std::memory_order_relaxed);

          bool admitted = true;
          if (opt.use_admission) {
            // 与 AdmitFrame 同样的判定，但按流独立执行（多路线程下不便做
            // 跨流分组）。代价：**不建模**同任务流的 SharedInference 去重，
            // 因此这是容量上的保守（悲观）估计。
            const auto wanted = c.analyzer->WantsFrame();
            admitted = static_cast<std::uint64_t>(ts_ms) >= wanted;
            // 记录滞后。wanted 可能是"停止哨兵"极大值，需排除。
            if (wanted < (1ULL << 62)) {
              const auto lag = static_cast<std::int64_t>(wanted) - ts_ms;
              if (lag > c.max_lag_ms) c.max_lag_ms = lag;
            }
          }

          // 阶段 1：YUV -> BGR（每帧都做，与真实解码后行为一致）
          const auto t0 = Clock::now();
          cv::Mat bgr;
          if (c.convert) {
            const auto yuv = MakeYuvFrame(opt.width, opt.height, tick);
            bgr = c.convert->Run(yuv);
            if (measured) c.latency.convert_ns.Add(ToNs(Clock::now() - t0));
          }

          // 阶段 2：BGR -> 模型张量。只在帧被接纳时做——它属于推理的一部分，
          // 真实系统里被跳过的帧不会产生预处理开销。
          if (c.preprocess && admitted) {
            if (bgr.empty()) bgr = MakeSyntheticFrame(opt.width, opt.height);
            const auto tp0 = Clock::now();
            (void)c.preprocess->Run(bgr);
            const auto tp1 = Clock::now();
            if (measured) c.latency.preprocess_ns.Add(ToNs(tp1 - tp0));
          }

          std::shared_ptr<const cv::Mat> image =
              opt.reuse_image
                  ? shared_image
                  : std::make_shared<const cv::Mat>(
                        MakeSyntheticFrame(opt.width, opt.height));
          if (c.convert && !bgr.empty()) {
            image = std::make_shared<const cv::Mat>(bgr);
          }

          const auto ts0 = Clock::now();
          if (admitted) {
            ++c.admitted;
            c.analyzer->AddVideoFrame(static_cast<std::uint64_t>(s) + 1,
                                      static_cast<std::uint64_t>(ts_ms), image,
                                      nullptr);
          }
          const auto ts1 = Clock::now();
          if (c.detector) {
            const auto state = c.detector->ProcessFrame(ts_ms, ts_ms);
            if (state == avs_analyzer::EventDetector::kEventStart) {
              ++c.event_starts;
            }
          } else if (admitted) {
            (void)c.analyzer->RetrieveDetection();
          }
          const auto t2 = Clock::now();

          if (measured && admitted) {
            c.latency.e2e_ns.Add(ToNs(t2 - t0));
            c.latency.submit_ns.Add(ToNs(ts1 - ts0));
            c.latency.wait_ns.Add(ToNs(t2 - ts1));
            c.latency.mock_ns.Add(
                static_cast<std::int64_t>(mock.last_cost_ns()));
          }

          if (opt.realtime_pacing) {
            deadline += std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double, std::milli>(step_ms));
            std::this_thread::sleep_until(deadline);
          }
        }
      });
    }

    for (auto& t : stream_threads) t.join();
    stop_sampling.store(true, std::memory_order_release);
    sampler_thread.join();

    // 6) 汇总
    for (auto& c : ctxs) {
      for (const auto v : c.latency.convert_ns.samples())
        report.latency.convert_ns.Add(v);
      for (const auto v : c.latency.preprocess_ns.samples())
        report.latency.preprocess_ns.Add(v);
      for (const auto v : c.latency.e2e_ns.samples())
        report.latency.e2e_ns.Add(v);
      for (const auto v : c.latency.submit_ns.samples())
        report.latency.submit_ns.Add(v);
      for (const auto v : c.latency.wait_ns.samples())
        report.latency.wait_ns.Add(v);
      for (const auto v : c.latency.mock_ns.samples())
        report.latency.mock_ns.Add(v);
      report.frames_submitted += c.admitted;
      report.event_starts += c.event_starts;
      if (c.max_lag_ms > report.max_lag_ms) report.max_lag_ms = c.max_lag_ms;
    }

    RssSampler::TrimHeap();
    sampler.Sample(progress.load(std::memory_order_relaxed), wall_ms_now());

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
        sampler.SlopeBytesPerFrame(warmup_total, warmup_total + steady);

    report.streams = streams;
    report.frames_offered = offered;
    report.frames_total = offered;
    report.frames_skipped = offered - report.frames_submitted;
    report.inferences = mock.inferred();
    report.stale_dropped = mock.stale_dropped();
    report.frames_lost = mock.dropped();
    report.mailbox_depth_peak = mock.queue_depth_peak();
    report.detect_wait_timeout =
        worker_metrics->detect_wait_timeout.load(std::memory_order_relaxed);

    if (report.frames_submitted > 0) {
      report.drop_rate =
          static_cast<double>(report.stale_dropped + report.frames_lost) /
          static_cast<double>(report.frames_submitted);
    }
    if (report.frames_offered > 0) {
      report.admission_skip_rate =
          static_cast<double>(report.frames_offered - report.frames_submitted) /
          static_cast<double>(report.frames_offered);
    }
    report.target_fps = opt.fps * static_cast<double>(streams);

    // 收尾顺序：先停 mock，再停各 analyzer。
    mock.Stop();
    for (auto& c : ctxs) {
      if (c.analyzer) c.analyzer->Stop();
    }

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
          ? static_cast<double>(report.frames_total) / report.wall_ms * 1000.0
          : 0.0;
  report.effective_infer_fps =
      report.wall_ms > 0.0
          ? static_cast<double>(report.inferences) / report.wall_ms * 1000.0
          : 0.0;
  report.wall_expected_ms = expected_ms;
  report.behind_factor =
      expected_ms > 0.0 ? report.wall_ms / expected_ms : 1.0;
  report.demand_fps =
      report.wall_ms > 0.0
          ? static_cast<double>(report.frames_submitted) / report.wall_ms * 1000.0
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
  if (t.max_drop_rate > 0.0 && r.drop_rate > t.max_drop_rate) {
    v.failures.push_back("丢帧率 " + FormatPercent(r.drop_rate) + " 超过阈值 " +
                         FormatPercent(t.max_drop_rate));
  }
  v.pass = v.failures.empty();
  return v;
}

nlohmann::json BenchReport::ToJson() const {
  const auto e2e = latency.e2e_ns.Summarize();
  const auto submit = latency.submit_ns.Summarize();
  const auto wait = latency.wait_ns.Summarize();
  const auto mock_s = latency.mock_ns.Summarize();
  const auto convert_s = latency.convert_ns.Summarize();
  const auto preprocess_s = latency.preprocess_ns.Summarize();
  return {
      {"completed", completed},
      {"error", error},
      {"streams", streams},
      {"framesOffered", frames_offered},
      {"framesSubmitted", frames_submitted},
      {"framesSkipped", frames_skipped},
      {"inferences", inferences},
      {"staleDropped", stale_dropped},
      {"framesLost", frames_lost},
      {"mailboxDepthPeak", mailbox_depth_peak},
      {"detectWaitTimeout", detect_wait_timeout},
      {"eventStarts", event_starts},
      {"dropRate", drop_rate},
      {"admissionSkipRate", admission_skip_rate},
      {"targetFps", target_fps},
      {"effectiveInferFps", effective_infer_fps},
      {"behindFactor", behind_factor},
      {"demandFps", demand_fps},
      {"wallExpectedMs", wall_expected_ms},
      {"maxLagMs", max_lag_ms},
      {"latencyMs",
       {{"e2eP50", e2e.p50_ns / 1e6},
        {"e2eP95", e2e.p95_ns / 1e6},
        {"e2eP99", e2e.p99_ns / 1e6},
        {"e2eMax", e2e.max_ns / 1e6},
        {"submitP50", submit.p50_ns / 1e6},
        {"waitP50", wait.p50_ns / 1e6},
        {"convertP50", convert_s.p50_ns / 1e6},
        {"preprocessP50", preprocess_s.p50_ns / 1e6},
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
  out += "并发流数    : " + std::to_string(streams) + "\n";
  out += "墙钟        : " + std::to_string(wall_ms) + " ms\n";

  out += "\n--- 容量与丢帧（重点）---\n";
  out += "投递帧数    : " + std::to_string(frames_offered) + "\n";
  out += "  接纳      : " + std::to_string(frames_submitted) + "   （跳过 " +
         std::to_string(frames_skipped) + "，" +
         FormatPercent(admission_skip_rate) + " 属低采样设计，非容量问题）\n";
  out += "  已推理    : " + std::to_string(inferences) + "\n";
  out += "  stale 过期: " + std::to_string(stale_dropped) + "\n";
  out += "  队列覆盖丢: " + std::to_string(frames_lost) +
         "   （mailbox latest-wins，静默丢失）\n";
  out += ">>> 丢帧率  : " + FormatPercent(drop_rate) +
         "   = (stale + 覆盖) / 接纳\n";
  out += "速率对比    : 入流 " + std::to_string(target_fps) + " fps -> 需求 " +
         std::to_string(demand_fps) + " fps -> 实际推理 " +
         std::to_string(effective_infer_fps) + " fps\n";
  out += "             （需求才是容量基准：入流被低采样大幅削减）\n";
  out += "跑慢倍数    : " + std::to_string(behind_factor) +
         "x   （实际墙钟 " + std::to_string(wall_ms) + " ms / 理想 " +
         std::to_string(wall_expected_ms) + " ms）\n";
  out += "最大滞后    : " + std::to_string(max_lag_ms) +
         " ms（>> 一个 stride 即说明饱和，而非低采样设计）\n";
  out += "队列峰值    : " + std::to_string(mailbox_depth_peak) +
         "   空等超时 " + std::to_string(detect_wait_timeout) +
         "   事件起始 " + std::to_string(event_starts) + "\n";

  out += "\n--- 内存 ---\n";
  out += "warmup 末   : " + FormatBytes(rss_warmup_end_bytes) + "\n";
  out += "phase1 末   : " + FormatBytes(rss_phase1_end_bytes) + "  (增长 " +
         FormatBytes(static_cast<std::uint64_t>(
             std::max<std::int64_t>(0, phase1_growth))) +
         ")\n";
  out += "终值        : " + FormatBytes(rss_final_bytes) + "  (增长 " +
         FormatBytes(static_cast<std::uint64_t>(
             std::max<std::int64_t>(0, phase2_growth))) +
         ")\n";
  out += "峰值        : " + FormatBytes(rss_peak_bytes) + "\n";
  out += "稳态斜率    : " + std::to_string(slope_bytes_per_frame) +
         " bytes/frame\n";

  // 分阶段归因：直接回答"瓶颈在哪"。占比以端到端 p50 为分母。
  out += "\n--- 分阶段耗时（p50 / 占端到端比例）---\n";
  const auto stage_line = [&](const char* name, const LatencyStats& bucket) {
    const auto s = bucket.Summarize();
    if (s.count == 0) {
      out += std::string(name) + " : （未启用）\n";
      return;
    }
    const double pct = e2e.p50_ns > 0
                           ? static_cast<double>(s.p50_ns) /
                                 static_cast<double>(e2e.p50_ns) * 100.0
                           : 0.0;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%-12s: %10s  %5.1f%%   (p99 %s)\n", name,
                  FormatNs(s.p50_ns).c_str(), pct,
                  FormatNs(s.p99_ns).c_str());
    out += buf;
  };
  stage_line("YUV->BGR", latency.convert_ns);
  stage_line("预处理", latency.preprocess_ns);
  stage_line("提交", latency.submit_ns);
  stage_line("等待/推理", latency.wait_ns);

  out += "\n--- 延迟（仅统计 warmup 之后）---\n";
  out += "端到端      : p50 " + FormatNs(e2e.p50_ns) + "  p95 " +
         FormatNs(e2e.p95_ns) + "  p99 " + FormatNs(e2e.p99_ns) + "  max " +
         FormatNs(e2e.max_ns) + "\n";
  out += "  提交段    : p50 " + FormatNs(submit.p50_ns) + "\n";
  out += "  等待段    : p50 " + FormatNs(wait.p50_ns) + "\n";
  out += "mock 自检   : mean " +
         FormatNs(static_cast<std::int64_t>(mock_s.mean_ns)) +
         "（应接近配置的模拟耗时）\n";
  return out;
}

}  // namespace avsbench
