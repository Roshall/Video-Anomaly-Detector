#ifndef ANALYZER_STREAM_SESSION_H
#define ANALYZER_STREAM_SESSION_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>

#include "av_pull_stream.h"
#include "av_push_stream.h"
#include "frame.h"
#include "frame_walk_time.h"
#include "metrics.h"
#include "video_processing/format_convertor.h"
#include "video_processing/video_context.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace avs_analyzer {
class Scheduler;

// Result of one decode attempt from StreamSession::GrabFrame.
enum class GrabResult {
  kOk,
  kAgain,         // Retryable decoder state; back off before the next attempt.
  kReconnecting,  // Woken without data while a reconnect is in progress.
  kEof,
  kDecodeError,
  kFatal,
};

// One decoded frame handed to the worker's per-frame handler. The frame is
// borrowed: it is only valid for the duration of the callback.
struct DecodedFrame {
  AVFrame* frame{nullptr};
  int64_t timestamp{0};     // resolved PTS in the stream time base
  int64_t timestamp_ms{0};  // resolved PTS in milliseconds
};

// Callbacks driving the decode loop. StreamSession owns the loop mechanics
// (reconnect backoff, generation changes, error budget); the Worker owns what
// happens with each decoded frame.
struct DecodeLoopHandlers {
  // Returns false to stop the loop.
  std::function<bool(const DecodedFrame&)> on_frame;
  // First frame of a new stream generation: decoder flushed, timer reset.
  std::function<void()> on_new_generation;
  // Decoded geometry changed (e.g. reconnect at another resolution).
  std::function<void(int width, int height, int fps, int out_width,
                     int out_height)>
      on_geometry_changed;
  // Decode errors exhausted the budget; the worker should remove itself.
  std::function<void()> on_error_budget_exhausted;
};

// Owns one input/output stream pair: pull/decode resources, the published
// video context, the format convertor, and the push encoder thread.
//
// It deliberately has no event or monitor knowledge. The Worker supplies the
// stream URLs and callbacks for lifecycle transitions.
class StreamSession final : public PullStreamHost, public PushStreamHost {
 public:
  struct Options {
    Scheduler* scheduler{nullptr};
    std::string video_code;
    std::string stream_url;
    std::string push_stream_url;
    bool file{false};
    // Lifetime is owned by the Worker; Stop() is requested by clearing it.
    std::atomic<bool>* state{nullptr};
    // Wall-clock mapping used by the alarm pipeline; re-initialised when the
    // stream generation changes.
    FrameWallClock* timer{nullptr};
    std::shared_ptr<WorkerMetrics> metrics;
    std::function<void()> on_stream_error;
    std::function<void(const VideoContext&)> on_stream_opened;
  };

  explicit StreamSession(Options options);
  ~StreamSession();
  StreamSession(const StreamSession&) = delete;
  StreamSession& operator=(const StreamSession&) = delete;

  // Opens the input stream and (when requested) the push stream, then builds
  // the first convertor. Threads are not started until StartThreads().
  bool Open(std::string& msg);
  void StartThreads();
  // Signals both loops to stop. Never joins, so it is safe to call from the
  // pull thread itself.
  void Stop();

  // Starts or closes the push output as the monitor set changes. `detach`
  // moves the join to the control pool so API callers never block.
  void SetPushRequested(bool requested, std::string push_url);
  void EnsurePush();
  void ClosePush(bool detach);

  [[nodiscard]] GrabResult GrabFrame(AVFrame* frame,
                                     StreamResourcesPtr& resources);
  // Runs the decode loop on the calling thread until stopped, EOF or a fatal
  // error. Resolves timestamps (synthesising missing PTS) and reports stream
  // generation/geometry changes through the handlers.
  void RunDecodeLoop(const DecodeLoopHandlers& handlers);
  void PushFrame(std::shared_ptr<const Frame> frame);

  [[nodiscard]] std::shared_ptr<const ConvertorState> Convertor() const {
    return std::atomic_load(&convertor_state_);
  }
  [[nodiscard]] StreamResourcesPtr Current() const {
    return in_stream_ ? in_stream_->Current() : nullptr;
  }
  [[nodiscard]] const std::string& StreamUrlValue() const {
    return stream_url_;
  }
  [[nodiscard]] const std::string& PushUrlValue() const {
    return push_stream_url_;
  }
  [[nodiscard]] int Fps() const;
  [[nodiscard]] VideoContext VideoContextSnapshot() const;
  [[nodiscard]] int VideoStreamIndex() const {
    return video_index_.load(std::memory_order_acquire);
  }
  [[nodiscard]] int VideoWidth() const {
    return video_width_.load(std::memory_order_acquire);
  }
  [[nodiscard]] int VideoHeight() const {
    return video_height_.load(std::memory_order_acquire);
  }
  // Geometry of the push output; falls back to the input geometry while no
  // push stream is open.
  [[nodiscard]] int PushWidth() const {
    const auto out = std::atomic_load(&out_stream_);
    return out ? out->width() : VideoWidth();
  }
  [[nodiscard]] int PushHeight() const {
    const auto out = std::atomic_load(&out_stream_);
    return out ? out->height() : VideoHeight();
  }

  // PullStreamHost
  [[nodiscard]] std::string_view StreamUrl() const override {
    return stream_url_;
  }
  [[nodiscard]] bool State() const override {
    return state_ && state_->load(std::memory_order_acquire);
  }
  void OnStreamOpened(const VideoContext& context) override;
  void OnStreamError() override;
  [[nodiscard]] const Config& GetConfig() const override;
  void RecordReconnect() override;

  // PushStreamHost
  [[nodiscard]] int PushQueueMaxFrames() const override;
  void RecordPushFrameDrop() const override;
  [[nodiscard]] std::string_view PushStreamUrl() const override {
    return push_stream_url_;
  }
  void OnPushStreamError() override;

 private:
  [[nodiscard]] bool NeedPushStream() const {
    return push_requested_.load(std::memory_order_acquire);
  }
  void StartPushThreadIfNeeded();
  void RebuildConvertor(const VideoContext& context, int out_width,
                        int out_height);
  // Resolves the frame timestamp, synthesising one when PTS is missing.
  // Returns false when the timestamp cannot be determined.
  bool ResolveTimestamps(AVFrame* frame, AVRational time_base,
                         AVRational fps_hint, int64_t& timestamp,
                         int64_t& timestamp_ms);
  void JoinPushThread(std::unique_ptr<std::thread> thread, bool detach);

  Options options_;
  Scheduler* scheduler_{nullptr};
  std::string stream_url_;
  std::string push_stream_url_;
  bool file_{false};
  std::atomic<bool>* state_{nullptr};
  std::shared_ptr<WorkerMetrics> metrics_;
  std::function<void()> on_stream_error_;
  std::function<void(const VideoContext&)> on_stream_opened_;

  std::unique_ptr<AvPullStream> in_stream_;
  std::atomic<bool> push_requested_{false};
  std::shared_ptr<AvPushStream> out_stream_{};
  std::unique_ptr<std::thread> read_thread_;
  std::unique_ptr<std::thread> push_thread_;
  std::mutex push_mutex_;

  mutable std::shared_mutex video_context_mutex_;
  VideoContext video_context_{.format = AV_PIX_FMT_NONE,
                              .width = 0,
                              .height = 0,
                              .video_index = -1};
  std::atomic<int> video_index_{-1};
  std::atomic<int> video_width_{0};
  std::atomic<int> video_height_{0};
  std::shared_ptr<const ConvertorState> convertor_state_;
  // Detect thread only: generation whose decoder buffers are current.
  uint64_t decoded_generation_{0};
  // Detect thread only: last resolved timestamp, used to synthesise PTS.
  std::atomic<int64_t> last_frame_ts_ms_{-1};
  std::atomic<bool> nopts_logged_{false};
};

}  // namespace avs_analyzer

#endif  // ANALYZER_STREAM_SESSION_H
