// Snapshot/lifetime semantics for the worker's monitor registry.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "monitor/monitor_registry.h"

namespace avs_analyzer {
namespace {

std::shared_ptr<MonitorContext> MakeContext(const std::string& code) {
  auto ctx = std::make_shared<MonitorContext>();
  ctx->monitor = std::make_unique<Monitor>();
  ctx->monitor->code = code;
  return ctx;
}

TEST(MonitorRegistry, SnapshotKeepsRemovedContextsAlive) {
  MonitorRegistry registry;
  registry.AddBatch({MakeContext("a"), MakeContext("b")});

  const auto snapshot = registry.Snapshot();
  ASSERT_EQ(snapshot.size(), 2u);

  const auto removed = registry.RemoveBatch({"a"});
  ASSERT_EQ(removed.size(), 1u);
  EXPECT_FALSE(removed[0]->active.load(std::memory_order_acquire));
  EXPECT_EQ(removed[0]->monitor->code, "a");

  // The detect loop may still be using its snapshot: the context must remain
  // valid after removal (no use-after-free).
  const auto it = std::find_if(
      snapshot.begin(), snapshot.end(),
      [](const auto& ctx) { return ctx->monitor->code == "a"; });
  ASSERT_NE(it, snapshot.end());
  EXPECT_EQ((*it)->monitor->code, "a");

  EXPECT_EQ(registry.Size(), 1u);
  EXPECT_TRUE(registry.Contains("b"));
  EXPECT_FALSE(registry.Contains("a"));
  EXPECT_FALSE(registry.Empty());
}

TEST(MonitorRegistry, TaskLabelsKeepsOneLabelPerTask) {
  auto wandering_a = MakeContext("a");
  wandering_a->monitor->task = Task::kWandering;
  auto wandering_b = MakeContext("b");
  wandering_b->monitor->task = Task::kWandering;
  auto sleeping = MakeContext("c");
  sleeping->monitor->task = Task::kSleepWork;
  sleeping->monitor->behavior = u8"custom-label";

  MonitorRegistry registry;
  registry.AddBatch({wandering_a, wandering_b, sleeping});

  const auto labels = registry.TaskLabels();
  ASSERT_EQ(labels.size(), 2u);
  EXPECT_EQ(labels[0].first, Task::kWandering);
  EXPECT_EQ(labels[0].second, avs_analyzer::toChinese(Task::kWandering));
  EXPECT_EQ(labels[1].first, Task::kSleepWork);
  EXPECT_EQ(std::string(labels[1].second.begin(), labels[1].second.end()),
            "custom-label");
}

TEST(MonitorRegistry, AnyPushStreamFollowsTheMonitorSet) {
  auto plain = MakeContext("a");
  MonitorRegistry registry;
  registry.AddBatch({plain});
  EXPECT_FALSE(registry.AnyPushStream());

  auto pushing = MakeContext("b");
  pushing->monitor->push_stream = true;
  registry.AddBatch({pushing});
  EXPECT_TRUE(registry.AnyPushStream());

  registry.RemoveBatch({"b"});
  EXPECT_FALSE(registry.AnyPushStream());
}

TEST(MonitorRegistry, RemoveUnknownCodeLeavesRegistryIntact) {
  MonitorRegistry registry;
  registry.AddBatch({MakeContext("a")});
  EXPECT_TRUE(registry.RemoveBatch({"missing"}).empty());
  EXPECT_EQ(registry.Size(), 1u);
}

TEST(MonitorRegistry, RemoveLastDoesNotWaitForInFlightSnapshot) {
  MonitorRegistry registry;
  registry.AddBatch({MakeContext("only")});

  // Emulates the detect loop holding a snapshot while blocked on inference.
  const auto snapshot = registry.Snapshot();
  std::mutex mutex;
  std::condition_variable cv;
  bool release = false;
  std::thread detect([&] {
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return release; });
    EXPECT_FALSE(snapshot.empty());
    EXPECT_EQ(snapshot.front()->monitor->code, "only");
  });

  const auto started = std::chrono::steady_clock::now();
  const auto removed = registry.RemoveBatch({"only"});
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_EQ(removed.size(), 1u);
  EXPECT_LT(elapsed, std::chrono::seconds(5));
  EXPECT_TRUE(registry.Empty());

  {
    std::lock_guard lock(mutex);
    release = true;
  }
  cv.notify_all();
  detect.join();
}

TEST(MonitorRegistry, GetAllCopiesMonitorsFromSnapshot) {
  MonitorRegistry registry;
  registry.AddBatch({MakeContext("a"), MakeContext("b")});
  const auto monitors = registry.GetAll();
  ASSERT_EQ(monitors.size(), 2u);
  EXPECT_EQ(monitors[0].code, "a");
  EXPECT_EQ(monitors[1].code, "b");
  EXPECT_FALSE(monitors[0].code.empty());
}

TEST(MonitorRegistry, ConcurrentSnapshotsAndRemovalsAreSafe) {
  MonitorRegistry registry;
  constexpr int kCount = 32;
  std::vector<MonitorRegistry::Ptr> contexts;
  contexts.reserve(kCount);
  for (int i = 0; i < kCount; ++i) {
    contexts.push_back(MakeContext("m" + std::to_string(i)));
  }
  registry.AddBatch(std::move(contexts));

  std::atomic<bool> start{false};
  std::atomic<size_t> snapshots{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 3; ++i) {
    readers.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int n = 0; n < 2000; ++n) {
        const auto snapshot = registry.Snapshot();
        for (const auto& ctx : snapshot) {
          EXPECT_NE(ctx->monitor->code.find('m'), std::string::npos);
        }
        snapshots.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::thread remover([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int i = 0; i < kCount; ++i) {
      registry.RemoveBatch({"m" + std::to_string(i)});
    }
  });

  start.store(true, std::memory_order_release);
  for (auto& reader : readers) {
    reader.join();
  }
  remover.join();

  EXPECT_TRUE(registry.Empty());
  EXPECT_GT(snapshots.load(), 0u);
}

}  // namespace
}  // namespace avs_analyzer
