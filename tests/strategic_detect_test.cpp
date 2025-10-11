#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "../Analyzer/Core/event.h"
#include "../Analyzer/Core/frame.h"

// Shared queue for the simulation
std::queue<avs_analyzer::Frame> g_frame_queue;
std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
bool g_producer_finished = false;

// Simulates an object detection model by checking frame data.
std::vector<Detection> Detect(const avs_analyzer::Frame& frame) {
  if (frame.data.find("object") != std::string::npos) {
    return {Detection{}};
  }
  return {};
}

// Frame producer simulation (same as your original, but using global queue)
void FrameProducer() {
  using namespace std::chrono_literals;
  auto start_time = std::chrono::steady_clock::now();
  const double simulation_duration_s = 35.0;

  auto should_have_object = [&](double current_time_s) {
    return (current_time_s >= 3.0 && current_time_s <= 8.0) ||         // Normal event
           (current_time_s >= 15.0 && current_time_s <= 16.0) ||      // Too short to trigger
           (current_time_s >= 20.0 && current_time_s <= 35.0);       // Long event to test max_duration
  };

  int frame_id = 0;
  while (true) {
    auto now = std::chrono::steady_clock::now();
    double timestamp = std::chrono::duration<double>(now - start_time).count();
    if (timestamp > simulation_duration_s) break;

    avs_analyzer::Frame new_frame;
    new_frame.timestamp = timestamp;
    new_frame.data = "frame_" + std::to_string(frame_id++);
    if (should_have_object(timestamp)) {
      new_frame.data += "_with_object";
    }
    // The detector will set has_object itself based on Detect().

    {
      std::lock_guard<std::mutex> lock(g_queue_mutex);
      g_frame_queue.push(new_frame);
    }
    g_queue_cv.notify_one();
    std::this_thread::sleep_for(33ms); // ~30 FPS
  }
  {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_producer_finished = true;
  }
  g_queue_cv.notify_one();
  std::cout << "[Producer] Finished generating frames." << std::endl;
}

int main() {
  std::cout << "Starting event detection simulation..." << std::endl;
  std::cout << std::fixed << std::setprecision(2);

  std::jthread producer_thread(FrameProducer);

  avs_analyzer::EventDetectorConfig config; // Use default config
  avs_analyzer::EventDetector detector(config, <#initializer #>, nullptr,
                                       std::vector<cv::Point>(), nullptr);
  std::vector<avs_analyzer::Event> all_events;

  while (true) {
    std::unique_lock<std::mutex> lock(g_queue_mutex);
    g_queue_cv.wait(lock, [] { return !g_frame_queue.empty() || g_producer_finished; });

    if (g_producer_finished && g_frame_queue.empty()) {
      if (auto event = detector.Flush()) {
        all_events.push_back(*event);
      }
      break;
    }

    avs_analyzer::Frame frame = g_frame_queue.front();
    g_frame_queue.pop();
    lock.unlock();

    // Simulate detection
    frame.object_count = !Detect(frame).empty();

    if (auto event = detector.ProcessFrame(frame)) {
      all_events.push_back(*event);
    }
  }

  std::cout << "\n--- Detected Events Summary ---" << std::endl;
  for (size_t i = 0; i < all_events.size(); ++i) {
    const auto& ev = all_events[i];
    std::cout << "Event " << i + 1 << ": Start=" << ev.start_timestamp
              << "s, End=" << ev.end_timestamp << "s, Duration=" << ev.GetDuration()
              << "s, Frames=" << ev.frames.size() << std::endl;
  }
  return 0;
}