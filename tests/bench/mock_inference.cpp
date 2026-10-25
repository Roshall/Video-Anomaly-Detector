#include "mock_inference.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace avsbench {

namespace {
constexpr auto kReadyWait = std::chrono::milliseconds(200);
}  // namespace

MockInference::MockInference(MockInferenceConfig cfg,
                             std::shared_ptr<avs_analyzer::ModelMetrics> metrics)
    : cfg_(std::move(cfg)),
      metrics_(std::move(metrics)),
      rng_state_(cfg_.seed ? cfg_.seed : 1u) {}

MockInference::~MockInference() { Stop(); }

MockInference::AddTaskFunc MockInference::Register(unsigned long analyzer_id,
                                                   Callback cb,
                                                   std::size_t depth) {
  {
    std::lock_guard<std::mutex> lk(cb_mutex_);
    callbacks_[analyzer_id] = std::move(cb);
  }
  mailboxes_.Add(analyzer_id, depth);

  return [this](unsigned long id1, unsigned long id2, unsigned long id3,
                ImagePtr image, std::shared_ptr<SharedInference> shared) {
    AddTask(id1, id2, id3, std::move(image), std::move(shared));
  };
}

void MockInference::Deregister(unsigned long analyzer_id) {
  {
    std::lock_guard<std::mutex> lk(cb_mutex_);
    callbacks_.erase(analyzer_id);
  }
  const auto dropped = mailboxes_.Remove(analyzer_id);
  if (metrics_ && dropped > 0) {
    metrics_->queue_depth.fetch_sub(static_cast<std::uint64_t>(dropped),
                                    std::memory_order_relaxed);
  }
}

void MockInference::Start() {
  if (cfg_.mode != MockMode::kQueued) return;  // sync 模式不需要线程
  if (running_.load(std::memory_order_acquire)) return;
  running_.store(true, std::memory_order_release);

  const unsigned n = cfg_.worker_threads ? cfg_.worker_threads : 1u;
  for (unsigned i = 0; i < n; ++i) {
    workers_.emplace_back([this] { RunLoop(); });
  }
}

void MockInference::Stop() {
  running_.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(ready_mutex_);
    ready_ids_.push_back(kStopSentinel);
  }
  ready_cv_.notify_all();
  for (auto& t : workers_) {
    if (t.joinable()) t.join();
  }
  workers_.clear();
}

void MockInference::AddTask(unsigned long id1, unsigned long id2,
                            unsigned long id3, ImagePtr image,
                            std::shared_ptr<SharedInference> shared) {
  submitted_.fetch_add(1, std::memory_order_relaxed);

  // ---------------------------------------------------------------- queued
  if (cfg_.mode == MockMode::kQueued) {
    MailboxTask task;
    task.id = {id1, id2, id3};
    task.image = std::move(image);
    task.shared = std::move(shared);

    const auto outcome = mailboxes_.Push(std::move(task));
    if (outcome.result == avs_analyzer::AnalyzerMailboxes::PushResult::kRejected) {
      return;
    }
    if (metrics_ && outcome.buffered_delta > 0) {
      metrics_->queue_depth.fetch_add(
          static_cast<std::uint64_t>(outcome.buffered_delta),
          std::memory_order_relaxed);
    } else if (metrics_ && outcome.buffered_delta < 0) {
      metrics_->queue_depth.fetch_sub(
          static_cast<std::uint64_t>(-outcome.buffered_delta),
          std::memory_order_relaxed);
    }

    const auto buffered =
        static_cast<std::uint64_t>(mailboxes_.BufferedTasks());
    auto peak = depth_peak_.load(std::memory_order_relaxed);
    while (buffered > peak &&
           !depth_peak_.compare_exchange_weak(peak, buffered,
                                              std::memory_order_relaxed)) {
    }

    if (outcome.result == avs_analyzer::AnalyzerMailboxes::PushResult::kSchedule) {
      {
        std::lock_guard<std::mutex> lk(ready_mutex_);
        ready_ids_.push_back(id1);
      }
      ready_cv_.notify_one();
    }
    return;
  }

  // ---------------------------------------------------------------- sync
  Callback cb;
  bool has_callback = false;
  {
    std::lock_guard<std::mutex> lk(cb_mutex_);
    if (auto it = callbacks_.find(id1); it != callbacks_.end()) {
      cb = it->second;
      has_callback = true;
    }
  }
  if (!has_callback) return;

  const std::uint64_t frame_ts = id3;
  const bool stale = frame_ts < cb.second();

  std::vector<DL_RESULT> results;
  if (!stale) {
    results = Produce(image, shared);
  } else {
    stale_dropped_.fetch_add(1, std::memory_order_relaxed);
  }
  Deliver(cb, std::move(results), frame_ts, stale);
}

void MockInference::RunLoop() {
  while (running_.load(std::memory_order_acquire)) {
    unsigned long id = 0;
    {
      std::unique_lock<std::mutex> lk(ready_mutex_);
      if (!ready_cv_.wait_for(lk, kReadyWait, [this] {
            return !ready_ids_.empty() ||
                   !running_.load(std::memory_order_acquire);
          })) {
        continue;
      }
      if (!running_.load(std::memory_order_acquire)) break;
      if (ready_ids_.empty()) continue;
      id = ready_ids_.front();
      ready_ids_.pop_front();
    }
    if (id == kStopSentinel) continue;

    MailboxTask task;
    if (!mailboxes_.Acquire(id, task)) continue;
    if (metrics_) {
      metrics_->queue_depth.fetch_sub(1, std::memory_order_relaxed);
    }

    Callback cb;
    bool has_callback = false;
    {
      std::lock_guard<std::mutex> lk(cb_mutex_);
      if (auto it = callbacks_.find(id); it != callbacks_.end()) {
        cb = it->second;
        has_callback = true;
      }
    }
    if (!has_callback) {
      mailboxes_.Complete(id);
      continue;
    }

    const std::uint64_t frame_ts = task.id[2];
    const bool stale = frame_ts < cb.second();

    std::vector<DL_RESULT> results;
    if (!stale) {
      results = Produce(task.image, task.shared);
    } else {
      stale_dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    // 关键：stale 也要回调（结果为空），否则等待方会空等到超时。
    Deliver(cb, std::move(results), frame_ts, stale);

    if (mailboxes_.Complete(id)) {  // 还有积压 -> 重新排程
      {
        std::lock_guard<std::mutex> lk(ready_mutex_);
        ready_ids_.push_back(id);
      }
      ready_cv_.notify_one();
    }
  }
}

std::vector<DL_RESULT> MockInference::Produce(
    const ImagePtr& image, std::shared_ptr<SharedInference>& shared) {
  std::vector<DL_RESULT> results;
  if (!image || image->empty()) return results;

  const std::uint64_t cost =
      cfg_.cost_ns + (cfg_.jitter_ns ? NextRand() % cfg_.jitter_ns : 0u);

  const auto t0 = std::chrono::steady_clock::now();
  BurnNs(cost);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
  last_cost_ns_.store(static_cast<std::uint64_t>(elapsed),
                      std::memory_order_relaxed);
  if (metrics_) {
    metrics_->RecordInference(static_cast<std::uint64_t>(elapsed));
  }

  results = MakeResults(seq_.fetch_add(1, std::memory_order_relaxed));
  inferred_.fetch_add(1, std::memory_order_relaxed);

  if (shared && shared->TryClaim()) {
    shared->Publish(results);
  }
  return results;
}

void MockInference::Deliver(const Callback& cb,
                            std::vector<DL_RESULT>&& results,
                            std::uint64_t frame_ts, bool stale) {
  if (stale && !cfg_.replicate_stale) return;
  cb.first({std::move(results), frame_ts}, frame_ts);
}

std::vector<DL_RESULT> MockInference::MakeResults(std::uint64_t seq) const {
  std::vector<DL_RESULT> out;
  if (cfg_.num_boxes <= 0) return out;

  const int w = cfg_.frame_size.width;
  const int h = cfg_.frame_size.height;
  const int bw = std::min(120, w > 0 ? w : 1);
  const int bh = std::min(200, h > 0 ? h : 1);
  const int max_x = std::max(0, w - bw);
  const int max_y = std::max(0, h - bh);

  out.reserve(static_cast<std::size_t>(cfg_.num_boxes));
  for (int k = 0; k < cfg_.num_boxes; ++k) {
    int x = cfg_.move_boxes
                ? static_cast<int>((seq * 2 + static_cast<std::uint64_t>(k) * 40) %
                                   static_cast<std::uint64_t>(max_x + 1))
                : k * 40;
    x = std::clamp(x, 0, max_x);

    DL_RESULT r;
    r.classId = cfg_.class_id;
    r.confidence = cfg_.confidence;
    r.box = cv::Rect(x, max_y / 2, bw, bh);
    out.push_back(r);
  }
  return out;
}

void MockInference::BurnNs(std::uint64_t ns) const {
  if (ns == 0) return;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns);

  switch (cfg_.cost_mode) {
    case CostMode::kSleep:
      std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
      break;
    case CostMode::kHybrid: {
      constexpr std::uint64_t kSpinTail = 200'000;  // 200us
      if (ns > kSpinTail) {
        std::this_thread::sleep_for(
            std::chrono::nanoseconds(ns - kSpinTail));
      }
      while (std::chrono::steady_clock::now() < deadline) {
      }
      break;
    }
    case CostMode::kSpin:
    default:
      while (std::chrono::steady_clock::now() < deadline) {
      }
      break;
  }
}

std::uint64_t MockInference::NextRand() const {
  std::uint64_t x = rng_state_.load(std::memory_order_relaxed);
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  rng_state_.store(x, std::memory_order_relaxed);
  return x;
}

}  // namespace avsbench
