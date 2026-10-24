# Analyzer Design Weakness Remediation Plan

> **Status:** This is the historical seven-phase design record. Current
> implementation and validation status is maintained in
> [`PROJECT_STATUS.md`](../../PROJECT_STATUS.md), with actionable unchecked
> items in [`TODO.md`](../../TODO.md). Do not infer that an unchecked
> acceptance item passed merely because the corresponding code phase is
> implemented.

## 1. Summary

This plan fixes the design weaknesses found in the Analyzer module review, prioritized for **multi-stream performance** and **maintainability**. Security is explicitly out of scope (private LAN).

The work is split into seven phases. Each phase is independently buildable and testable, and can be committed/reverted separately. The HTTP API request/response contract, alarm payloads, output file naming, and HLS segment naming remain unchanged.

Target outcome:

- Inference scales with camera count instead of being serialized by task.
- No blocking inference wait while holding the monitor registry lock.
- API add/remove no longer blocks the libevent loop on RTSP connect or model loading.
- Per-stream memory is bounded by a configurable budget instead of an 8-second raw-frame window.
- Reconnect, worker deletion, and alarm finalization have defined ownership and no cross-thread mutation of shared objects.
- `Worker` becomes a small orchestrator composed of focused modules.
- Build/test/repository hygiene is improved.

---

## 2. Scope and Non-Goals

### In scope

All findings from the review:

1. One inference thread/queue per task shared by all cameras, plus duplicate ONNX sessions for the same model.
2. No admission control before frame clone/enqueue; duplicate inference for multiple monitors of the same task.
3. Detection awaited while holding `monitors_mutex_`.
4. Synchronous RTSP connect + model load on the single HTTP event loop.
5. Unbounded per-stream raw-frame memory (event buffer and push queue).
6. One 3-thread pool shared by heavy media work and latency-sensitive alarm I/O.
7. RTSP reconnect races and stale `FormatConvertor`.
8. `worker_map_` accessed without its mutex; lock-invariant comments instead of code.
9. Shared mutable `Alarm` mutated from detect and cleanup threads.
10. Smaller bugs: non-atomic monitor IDs, colliding alarm IDs, HLS fps using numerator only, decode-error busy loop, infinite reconnect, fragile unbounded result wait.
11. `Worker` god object.
12. `const_cast`-based detector pool and raw-pointer callback ownership.
13. Build/test/repo hygiene.

### Out of scope

- Security hardening, authentication, TLS, network isolation.
- Rewriting the event detection algorithms or ByteTrack.
- Changing the admin/media service API contract.
- Rewriting git history.

---

## 3. Assumptions and Decisions

These decisions are fixed for the implementation; no further choices are needed.

| Topic | Decision |
|---|---|
| Language/standard | Keep C++20, CMake, FFmpeg, OpenCV, ONNX Runtime, libevent |
| HTTP contract | Keep endpoint paths, JSON response codes (`code`/`msg`), and add/cancel blocking semantics. Only move execution off the event loop |
| Inference parallelism | N ONNX sessions per model, static round-robin assignment of analyzers to sessions. Default 2 sessions/model on CPU, 1 when `acceleratorInference=true` |
| Live frame policy | One in-flight frame + one latest pending frame per analyzer (pipeline depth 1) |
| Offline frame policy | Same mailbox with configurable depth (default 4) to pipeline file analysis |
| Fairness | Per-analyzer mailbox + FIFO ready queue inside each session. No global cross-camera drop |
| Model reuse | Pool keyed by `(model file, target_id)`, so `yolov8n.onnx` is loaded once for all tasks that use it |
| Memory policy | Idle pre-event frames stored as in-memory JPEG; active event frames stored raw. Hard per-worker byte budget; oldest frames evicted |
| Push queue | Bounded by frame count (default 2 seconds at 25 fps = 50 frames), drop newest and increment a metric |
| Thread pools | `control` (stream lifecycle/API work), `media` (offline analysis), `alarm` (alarm I/O + HLS reencode). No shared 3-thread pool |
| Async HTTP | Handler offloads blocking work to `control` pool, replies on the libevent loop via `event_base_once` + `evhttp_request_own` |
| Reconnect | Bounded attempts with exponential backoff; generation-tagged packets; resources swapped atomically |
| Alarm IDs | Process-global monotonic generator seeded from epoch ms; slices share one event ID |
| Repo hygiene | Untrack build directories and large generated scripts (`git rm --cached` only; files stay on disk). No history rewrite |
| Compatibility | No feature flags/backward-compat shims. Phases are merged incrementally |

### New configuration (defaults preserve or improve current behavior)

Add to `conf/config.json` and `Analyzer/Core/config.h/.cpp`:

```json
"application": {
  "threadPoolSize": 3,
  "offlineThreads": 2,
  "alarmThreads": 2,
  "maxReconnectAttempts": 10,
  "reconnectBaseDelayMs": 1000,
  "reconnectMaxDelayMs": 30000,
  "inferenceResultTimeoutMs": 5000
},
"performance": {
  "inferenceWorkersPerModel": 2,
  "inferenceWorkersPerModelAccelerated": 1,
  "eventBufferBudgetMb": 256,
  "eventBufferJpegQuality": 7,
  "pushQueueMaxFrames": 50,
  "maxPendingAlarmJobs": 128,
  "offlinePipelineDepth": 4,
  "preloadModels": [
    "yolov8n.onnx",
    "sleeping_m.onnx",
    "climbing_m.onnx",
    "phone_playing_m.onnx"
  ]
}
```

Validation rules in `Config`:

- `inferenceWorkersPerModel`: clamp `[1, 8]`.
- `eventBufferBudgetMb`: clamp `[32, 4096]`.
- `pushQueueMaxFrames`: clamp `[10, 300]`.
- `offlinePipelineDepth`: clamp `[1, 16]`.
- `maxReconnectAttempts`: `0` means unlimited (documented); otherwise `[1, 100]`.
- `preloadModels`: unknown files are logged and skipped, not fatal.

---

## 4. Current State Analysis (evidence)

| # | Weakness | Evidence |
|---|---|---|
| 1 | One detector per Task, one thread, one 100-slot queue shared by all cameras | `Core/detector/constant_yolo.cpp:21-46`, `Core/detector/yolo_detector_onnx.h:60-68`, `Core/detector/yolo_detector_onnx.cpp:41-89` |
| 2 | Clone/enqueue before `WantsFrame()` check; stale frames dropped in detector thread | `Core/worker.cpp:639-660`, `Core/detector/yolo_detector_onnx.cpp:54-64` |
| 3 | `monitors_mutex_` shared lock held across blocking inference result wait | `Core/worker.cpp:743-788`, `Core/algorithm.cpp:7-11` |
| 4 | Add/cancel handled on the single libevent thread; `start()` does open_input/find_stream_info and model load synchronously | `Core/server.cpp:22-53`, `Core/server_apis/api_monitor.cpp:191-483`, `Core/av_pull_stream.cpp:22-92`, `Core/scheduler.cpp:148-199` |
| 5 | 8 s raw-frame event window + 300-frame push queue per stream; per-inference 4.9 MB blob allocation | `Core/combined_event_manager.cpp:20-33`, `Core/video_processing/av_push_stream.cpp:38-45`, `Core/detector/inference.cpp:69-84` |
| 6 | One 3-thread global pool for offline analysis, HLS reencode, alarm POST, joins | `Core/scheduler.cpp:18`, `Core/worker.cpp:492-524`, `Core/alarm.cpp:216-239` |
| 7 | Reconnect swaps `fmt_ctx`/`codec_ctx` while decode thread uses them; convertor never rebuilt | `Core/av_pull_stream.cpp:94-159`, `Core/worker.cpp:926-949`, `Core/worker.cpp:230-237` |
| 8 | `worker_map_` mutated without mutex from pull thread; lock invariant only in comments | `Core/scheduler.cpp:283-339`, `Core/av_pull_stream.cpp:152` |
| 9 | Shared `Alarm` mutated by detect thread and finalized by cleanup thread | `Core/alarm.h:72-74`, `Core/worker.cpp:791-822`, `Core/alarm.cpp:99-138`, `Core/scheduler.cpp:42-47` |
| 10 | Non-atomic `Monitor::NextId`; ms-seeded IDs; `framerate.num` only; decode errors spin; unbounded result wait | `Core/monitor.h:162-165`, `Core/combined_event_manager.h:69`, `Core/worker.cpp:800`, `Core/worker.cpp:720-723`, `Core/analyzer.cpp:28-40` |
| 11 | `Worker` owns pull/decode/detect/annotate/HLS/push/alarm/offline in 1177 lines | `Core/worker.h:34-156`, `Core/worker.cpp` |
| 12 | `ConstPool` + `const_cast`; callbacks capture raw pointers | `Core/detector/constant_yolo.cpp:44-105`, `Core/analyzer.h:34-43` |
| 13 | Build outputs and large scripts tracked; per-test manual source lists; only 4 `add_test` | `CMakeLists.txt:218-360`, `git ls-files` |

---

## 5. Target Architecture

```
HTTP (libevent loop, non-blocking)
  |  add/cancel/probe -> control pool -> reply posted back to loop
  v
Scheduler
  |- worker_map_ : shared_ptr<Worker> (mutex-protected)
  |- ModelPool (per model+target_id)
  |    |- InferenceSession x N (ORT session + thread)
  |         |- per-analyzer mailbox (latest frame, 1 in flight)
  |- AlarmDispatcher (bounded queue, N threads)
  v
Worker (orchestrator)
  |- StreamSession: AvPullStream + decode + VideoContext + convertor + push output
  |- MonitorRegistry: shared_ptr<MonitorContext> snapshots, add/remove
  |- EventPipeline: per-frame detect -> EventDetector -> CombinedEventManager
  |- AlarmPipeline: HLS writer (detect thread) + immutable alarm jobs
```

Key invariant: **blocking waits never happen while holding a registry lock**, and **no shared mutable object crosses thread boundaries without an owner**.

---

## 6. Phase 0 — Baseline, Metrics, and Test Safety Net

### Goal

Make current behavior measurable and buildable before changing it.

### Changes

1. **Baseline build/test**
   - Run `cmake --preset debug`, `cmake --build --preset debug -j8`, `ctest --test-dir build --output-on-failure`.
   - Record current warnings and test results in the phase PR description.

2. **Metrics primitives** — new `Analyzer/Core/metrics.h`
   - `struct WorkerMetrics` (atomics): `frames_decoded`, `frames_submitted`, `frames_skipped`, `frames_dropped`, `detect_wait_timeout`, `reconnects`, `alarm_jobs_enqueued`, `alarm_jobs_dropped`.
   - `struct ModelMetrics`: `inferences_total`, `inference_ns_total`, `queue_depth`, `pending_analyzers`, `max_inference_ns`.
   - `struct AlarmMetrics`: `pending_jobs`, `upload_failures`, `reencode_failures`.
   - Add JSON serialization helper in `Utils/common` or `metrics.h`.
   - Expose via a new `GET /api/metrics` route in `Core/server.cpp` returning per-worker and per-model counters. Keep it read-only and cheap.
   - Wire counters at: `Worker::AddTasks`, `YOLODetector::AddTask/Run`, `Analyzer::FetchRawDetection`, `AlarmDispatcher`, `AvPullStream::Reconnect`.

3. **Hot-loop logging cleanup**
   - Change per-frame `LOGD` in `Analyzer::FetchRawDetection` and `Worker::DetectEvent` to `LOG_TRACE` or remove.
   - Rate-limit repeated decode/reconnect failure logs (first 5, then every 100).

4. **Test registration and sanitizer option**
   - Add `add_test(NAME <target> COMMAND <target>)` for every existing test executable in `CMakeLists.txt`.
   - Add `option(ANALYZER_SANITIZE "Enable sanitizer" OFF)` with `thread`/`address` variants via `CMAKE_CXX_FLAGS`.
   - Do not change test sources in this phase.

### Acceptance

- Build and all existing tests pass.
- `/api/metrics` returns valid JSON with zero counters when no cameras are configured.
- No behavior change in event detection or output files.

---

## 7. Phase 1 — Inference Scaling and Fairness

### Goal

Replace the per-task serial inference queue with a per-model, multi-session, per-analyzer-mailbox design.

### Changes

1. **Refactor `YOLODetector` into an inference session with per-analyzer mailboxes**
   Files: `Core/detector/yolo_detector_onnx.h/.cpp`
   - Remove `task_queue_`, `task_sem_`, `kMaxQueueSize`, and `DropOverflowTasks()`.
   - Add:
     - `struct Mailbox { ImagePtr image; ResourceID id; bool in_flight{false}; bool pending{false}; uint64_t frame_ts{0}; Callback cb; size_t depth{1}; std::deque<Task> backlog; }`
     - `std::unordered_map<unsigned long, Mailbox> mailboxes_` keyed by analyzer id.
     - `moodycamel::BlockingConcurrentQueue<unsigned long> ready_ids_`.
   - `AddTask(id, image)`:
     - Lock `mailbox_mutex_`; if the analyzer is not `in_flight`, store the task and enqueue the id; otherwise replace the pending task (live) or append to `backlog` (offline depth > 1).
     - Dropping an older pending image releases its `shared_ptr` immediately.
   - `Run()`:
     - Pop an id, move the pending task to local, mark `in_flight`, release the lock.
     - Check `cb.second()` / `WantsFrame()`; skip inference if stale, but still invoke the callback with an empty result if the analyzer is waiting (reuse existing semantics).
     - Run `model_->RunSession(...)`, `InvokeCallback`, then under the lock mark not-in-flight and re-enqueue if pending/backlog remains.
   - `RegisterCallback(id, cb, depth=1)` stores the callback and depth.
   - `DeregisterCallback(id)` removes the mailbox; if no mailboxes remain, `Stop()`.
   - Keep `Stop()` releasing the blocking queue (push a sentinel id or use `BlockingConcurrentQueue::enqueue` + `running_` check).

2. **Model-keyed pool with N sessions**
   Files: `Core/detector/constant_yolo.h/.cpp`, rename internals to `ModelPool`/`ModelRuntime` (keep public `DetectorPool` name only if it minimizes churn; otherwise update `Scheduler` member type).
   - Map key: `model_name + "#" + std::to_string(target_id)`.
   - `ModelRuntime` owns `std::vector<std::unique_ptr<YOLODetector>> sessions_`, an atomic round-robin index, and `LoadSessionCount(Config, task)`:
     - `use_accelerator ? performance.inferenceWorkersPerModelAccelerated : performance.inferenceWorkersPerModel`.
   - `RegisterToDetector(task, analyzer_id, cb)` selects a session round-robin and returns an `AddTaskFunc` bound to that session.
   - `DeregisterToDetector(task, analyzer_id)` removes the callback from the correct session; erase the `ModelRuntime` when no callbacks remain and all sessions are stopped.
   - Remove all `const_cast`; pool stores `shared_ptr<ModelRuntime>` and returns non-const pointers.
   - Keep `detector_mutex_` only for pool map operations; never hold it while loading a model or running inference.

3. **Model preload and lazy fallback**
   - In `Scheduler` constructor (or `main` after config load), call `ModelPool::Preload(config.performance.preload_models)`.
   - Preload creates runtimes/sessions but does not start worker threads until the first callback registration (or start them immediately if warm-up is required by ONNX Runtime).
   - If a model is missing, log and skip; a later analyzer registration fails gracefully (no-op adder + error log), preserving current behavior.
   - Add a startup duration log per model.

4. **Reuse inference buffers**
   File: `Core/detector/inference.cpp`, `inference.h`
   - Add members to `YOLOv8Detect`: `cv::Mat rgb_`, `cv::Mat canvas_`, `std::vector<float> blob_`, and `std::array<int64_t,4> input_dims_`.
   - `letterbox_bgr_to_rgb_` writes into `rgb_` (allocate only when size/type changes).
   - `blob_` is resized once to `3 * H * W`; `hwc_to_nchw_norm_` writes into it.
   - `Ort::MemoryInfo` is a static/member constant, not recreated per frame.
   - Document that one `YOLOv8Detect` is owned by one session thread, so buffers are not shared across threads.

5. **Bounded result wait and stop semantics**
   Files: `Core/algorithm.h/.cpp`, `Core/detection_algorithm.*`, `Core/track_algorithm.*`, `Core/analyzer.h/.cpp`
   - Add `bool Algorithm::TryProcessDetections(Detection& out, std::chrono::milliseconds timeout)`; default implementation uses `wait_dequeue_timed`.
   - `Analyzer::SetResultTimeout(ms)` from config; `Analyzer::Stop()` sets `stopped_=true` and pushes a sentinel result.
   - `Analyzer::FetchRawDetection()`:
     - Return an empty `Detection` immediately if `stopped_`.
     - Use the timed wait; on timeout, log once per second, return empty detection with `frame_ts = WantsFrame()`, and increment `detect_wait_timeout`.
     - Remove the unbounded `while(true)`.
   - Ordering rule for removal: call `analyzer->Stop()` **before** `DeregisterToDetector`, so a blocked wait is released even if the detector is stopping.

6. **Offline pipeline depth**
   - Add `Analyzer::SetPipelineDepth(size_t)`; `Worker::OfflineAnalyze` sets `config.performance.offline_pipeline_depth` before the loop.
   - Offline loop submits up to K frames ahead, then consumes results in order (one `RetrieveDetection` per submitted frame).
   - Keep the live path at depth 1.

### Tests

- New `tests/inference_mailbox_test.cpp`:
  - Fake `InferenceSession` seam (introduce `IInferenceSession` with `Run(const cv::Mat&)` and configurable latency) to avoid ONNX in unit tests.
  - 8 analyzers submit frames at different rates; assert every analyzer receives a result at least every 2× its configured stride, mailbox memory is bounded, and per-analyzer result timestamps are monotonic.
  - Overwrite semantics: submitting 10 newer frames while one is in flight must run at most 2 inferences and deliver the newest result.
- New `tests/analyzer_timeout_test.cpp`: no result + `Stop()` unblocks within timeout; `FetchRawDetection` does not hang.
- Existing `yolo_test`, `tracker_test`, `detect_test` must pass unchanged.

### Acceptance

- Model loaded once per `(model,target_id)`; log confirms session count.
- Under a fake 100 ms inference latency, 8 live analyzers each receive a detection result at their configured cadence (no global starvation).
- No use of `const_cast` in detector code; `rg "const_cast" Core/detector` returns nothing.

---

## 8. Phase 2 — Pipeline Decoupling and Admission Control

### Goal

Never hold registry locks while waiting on inference; do the cheap `WantsFrame()` check before cloning; deduplicate inference for multiple monitors of the same task.

### Changes

1. **Shared monitor contexts with stable lifetime**
   Files: `Core/worker.h/.cpp`
   - Change `MonitorContextPtr` from `std::unique_ptr<MonitorContext>` to `std::shared_ptr<MonitorContext>`.
   - Add `std::atomic<bool> active{true}` to `MonitorContext`.
   - `monitors_` becomes `std::vector<std::shared_ptr<MonitorContext>>`.
   - `DetectEvent`:
     ```text
     snapshot = copy monitors_ under shared_lock
     release lock
     for ctx in snapshot:
         if (!ctx->active) continue
         ... detector->ProcessFrame(...)
     ```
   - `AddMonitorsBatch` / `RemoveMonitorsBatch`:
     - Add under unique lock.
     - Remove sets `active=false` and erases under unique lock, then calls `Stop()` + `DeregisterToDetector` outside the lock.
     - Snapshots keep removed contexts alive until the detect loop finishes; `active=false` prevents further use.
   - `GetAllMonitor` copies `Monitor` values from a snapshot, not while holding the lock during string copies.

2. **Admission control and deduplication in `AddTasks`**
   File: `Core/worker.cpp`
   - Under a shared snapshot lock, for each active context:
     - `if (timestamp_ms < ctx->analyzer->WantsFrame()) { metrics.frames_skipped++; continue; }`
   - Group admitted contexts by `Task`. For each distinct `(Task, timestamp)`:
     - Convert the frame once and clone once.
     - Submit one inference task and fan out the result to all analyzers in that group.
   - Implementation approach:
     - Extend `YOLODetector::Task` with `std::vector<unsigned long> sinks` (or create `InferenceRequest` with sinks).
     - `YOLODetector::InvokeCallback` invokes every sink's callback for the same timestamp.
     - Mailbox handles a shared request by marking each sink's mailbox in flight; a single shared image is owned by the request.
     - If a sink's `WantsFrame()` is newer than the request timestamp, deliver an empty result to that sink only, so it does not wait for a stale frame.
   - If no analyzers want the frame, do not convert or clone at all.
   - Handle `AV_NOPTS_VALUE`:
     - Try `best_effort_timestamp`, then `frame->pts`; if invalid, use `last_frame_ts_ms_ + round(1000 / fps)`; if fps unknown, skip the frame and log once.
   - Increment `frames_decoded`, `frames_submitted`, `frames_skipped`.

3. **Event manager / annotator without a hot-path mutex**
   Files: `Core/combined_event_manager.h/.cpp`, `Core/worker.cpp`
   - Add `static_assert(std::is_trivially_copyable_v<CombinedEventConfig>)`.
   - Replace `config_` with `std::atomic<CombinedEventConfig> config_`; `UpdateConfig` stores it; `ProcessFrame` loads one snapshot at entry.
   - Replace `const DynamicAnnotator* annotator_` + `std::mutex* annotator_mutex_` with `std::atomic<std::shared_ptr<DynamicAnnotator>> annotator_` (C++20) or `std::shared_ptr` protected by a short mutex if the compiler/library combination does not support atomic shared_ptr.
   - `SetAnnotator(std::shared_ptr<DynamicAnnotator>)` stores a snapshot; `ProcessFrame` loads a local `shared_ptr` so an old annotator stays alive until the frame finishes.
   - `Worker::EnsureAnnotatorReady`/`RebuildEventManager` construct a new annotator and swap it; no longer hold `event_manager_mutex_` across `ProcessFrame`.
   - Keep `event_manager_mutex_` only for `FlushPending`/destructor serialization if needed; remove it from the per-frame path.

4. **Per-context highest level**
   - Replace `SetMonitorCurrentHighestLevel(code, level)` map lookup with `ctx->current_highest_level.store(level)`, found from the snapshot.
   - This removes another `monitors_mutex_` writer from the detect loop.

5. **Offline analysis rewrite**
   File: `Core/worker.cpp`
   - Guard `monitors_.empty()` before `front()`.
   - Use the same snapshot/admission/event-manager path as live detection, but with pipeline depth from config.
   - Finalize pending events and notify admin on file completion/error.
   - Do not access `monitors_` without the lock, even in file mode.

### Tests

- New `tests/monitor_registry_test.cpp`:
  - Add/remove monitors while a detect loop snapshots at high frequency; under ASAN, no use-after-free.
  - Remove the last monitor while a fake inference is blocked; `Stop()` releases it; removal completes within 5 s.
- New `tests/admission_test.cpp`:
  - 3 monitors of the same task on one frame: exactly one inference request and three result deliveries.
  - Frames older than `WantsFrame()` are not cloned or submitted.
- Existing `tests/event_test.cpp`, `detect_test`, `tracker_test` pass.

### Acceptance

- With a fake 2 s inference latency, `/api/control/add` on another monitor completes without waiting for inference.
- `frames_skipped` grows during idle periods; `frames_submitted` matches the configured detection stride.
- No `monitors_mutex_` is held across `ProcessDetections` (verified by code inspection and TSan).

---

## 9. Phase 3 — Memory Bounds

### Goal

Cut per-stream memory from hundreds of MB to a bounded budget without losing required pre-event footage.

### Changes

1. **In-memory JPEG encode/decode**
   Files: `Core/video_processing/jpeg_encoder.h/.cpp`
   - Add:
     - `std::vector<uint8_t> EncodeToBuffer(AVFrame* in)`
     - `AVFramePtr DecodeToFrame(const std::vector<uint8_t>& data)`
   - `EncodeToBuffer` reuses the existing `AV_CODEC_ID_MJPEG` encoder with `AV_PIX_FMT_YUV420P`; `saveFrame` becomes `EncodeToBuffer` + file write.
   - `DecodeToFrame` uses a lazily created MJPEG decoder; convert `yuvj420p` output to `AV_PIX_FMT_YUV420P` when needed with a cached `SwsContext`.
   - Protect encoder/decoder state with the existing `recursive_mutex`; one instance is used by the detect thread, one per alarm worker.
   - Add a unit test that round-trips a synthetic YUV420P frame and verifies dimensions, pixel format, and mean absolute error below a threshold.

2. **Combined event buffer with encoded idle frames and a byte budget**
   Files: `Core/combined_event_manager.h/.cpp`, `Core/frame.h`
   - Extend `Frame` with `std::vector<uint8_t> jpeg; bool encoded{false};`.
   - `ProcessFrame`:
     - If the combined state is idle and there are no detections, create the annotated/cloned frame, encode it to JPEG, and store an encoded `Frame` (no raw `AVFrame` retained).
     - If the state is active or detections exist, store the raw annotated frame.
     - If annotation is not needed (no detections, no active event), encode the cloned source frame directly.
   - `FlushEvent`:
     - Materialize encoded entries to `AVFramePtr` in order using `JPEGEncoder::DecodeToFrame`.
     - Frames are materialized only when an event slice is actually flushed.
   - Track `buffer_bytes_`; add `UpdateMemoryBudget(size_t bytes)` from `performance.eventBufferBudgetMb` per worker.
   - `TrimFrameBuffer` evicts from the front while either the time window or byte budget is exceeded. Log `buffer_evictions` when a frame is evicted before its normal window.
   - Dynamic windows by state:
     - idle: `pre_event_buffer_ms + guard_window_ms + 1000`
     - active: `slice_ms + 1000`
     - ending: `post_event_buffer_ms + 1000`
   - Keep the config-level `max_buffer_time_ms_` as a hard upper bound for safety.

3. **Push queue bound**
   Files: `Core/video_processing/av_push_stream.cpp`, `Core/worker.cpp`
   - Replace the hardcoded `30 * 10` with `performance.pushQueueMaxFrames`.
   - Drop newest when full and increment `push_frames_dropped`; log at most once per second.
   - Keep the queue consumer unchanged.

4. **Alarm image memory**
   - Move the alarm JPEG encode before enqueueing the alarm job (see Phase 5, AlarmDispatcher); do not retain a full annotated frame in a pending job.
   - If not yet implemented, at minimum reduce the shared frame lifetime by cloning only the JPEG bytes needed.

### Tests

- New `tests/event_buffer_test.cpp`:
  - Feed 30 s of idle frames; assert `buffer_bytes_ <= budget` and encoded entries are used (no raw AVFrame retained).
  - Start an event; assert pre-roll frames are materialized and the first/last timestamps are correct.
  - Verify memory does not grow monotonically over 10 minutes of idle input.
- New `tests/jpeg_roundtrip_test.cpp` as above.
- Manual: run one 1080p25 stream idle for 10 minutes; RSS stays within `eventBufferBudgetMb + fixed overhead`.

### Acceptance

- Idle 1080p stream memory is dominated by the configured budget, not by 8 s of raw frames.
- Alarm HLS/MP4 output still includes the configured pre-roll and post-roll.
- No visible pixel-format/color regression in alarm clips.

---

## 10. Phase 4 — Lifecycle and Concurrency Correctness

### Goal

Make worker removal, reconnect, and alarm finalization race-free.

### Changes

1. **`worker_map_` ownership and locking**
   Files: `Core/scheduler.h/.cpp`, `Core/worker.cpp`, `Core/av_pull_stream.cpp`
   - Change `worker_map_` to `std::unordered_map<std::string, std::shared_ptr<Worker>>`.
   - `RemoveWorker` always locks `worker_map_mutex_`; remove the "caller must hold the lock" comment and invariant.
   - `MonitorBatchRemove`:
     - Under the map lock, group codes by video and take `shared_ptr<Worker>`.
     - Release the lock, then call `RemoveMonitorsBatch` on each worker.
   - `to_be_deleted_workers_queue_` holds `std::shared_ptr<Worker>`; `CleanUpLoop` pops and releases the last reference.
   - `AddWorker`, `GetWorkerSize`, `ClearWorker` lock the map.
   - `ContainsMonitor` and `GetAllMonitors` take a `shared_ptr` snapshot and call the worker without the map lock.
   - `MonitorBatchAdd`:
     - Under the map lock, find or create+insert the worker.
     - Release the lock, call `start()` for the creator, and add monitors to an existing worker.
     - Add `Worker::WaitStarted(timeout)` backed by `std::shared_future<bool>` so concurrent add requests return accurate results.
     - If `start()` fails, remove the worker and return the current error code/message.

2. **Reconnect-safe stream resources**
   Files: `Core/av_pull_stream.h/.cpp`, `Core/worker.cpp`, `Core/video_processing/video_context.h`
   - Introduce `struct StreamResources { AVFormatContextPtr fmt; AVCodecContextPtr codec; AVStream* video_stream; VideoContext ctx; uint64_t generation; }`.
   - `AvPullStream` keeps `std::shared_ptr<StreamResources> current_` accessed with `std::atomic_load/atomic_store` (or a short `std::shared_mutex`).
   - `Read()` on failure:
     - Opens a new connection locally with `avformat_open_input` + `avformat_find_stream_info` **without holding locks**.
     - On success, atomically stores the new resources, increments generation, tags subsequent packets with the generation, and calls `Worker::OnStreamOpened(ctx)`.
     - On failure, retries with exponential backoff up to `maxReconnectAttempts`; then calls `worker_->Remove()` and notifies admin.
   - Packet type becomes `struct StreamPacket { AVPacketPtr pkt; uint64_t generation; }`.
   - `GrabFrame`:
     - Loads `current_`; if the packet generation differs, discards stale packets and calls `avcodec_flush_buffers` on the new codec.
     - Returns an enum `GrabResult { kOk, kAgain, kEof, kDecodeError, kReconnecting, kFatal }`.
   - `Worker::DetectEvent` handles the enum:
     - `kAgain`/`kDecodeError`: backoff 10 ms → 500 ms; after 50 consecutive errors, remove the worker.
     - `kEof`: flush events and exit.
     - `kReconnecting`: wait for the next generation, do not spin.
     - Remove the current `if (!GrabFrame) continue;` tight loop.

3. **Convertor and output reinitialization on stream change**
   Files: `Core/worker.cpp`
   - `OnStreamOpened` takes `video_context_mutex_` exclusively, updates `video_context_` and atomics, and rebuilds `FormatConvertor` for the new `VideoContext`.
   - `AddTasks` takes the same mutex in shared mode for the convert/clone step, so a frame cannot use a converter for a different resolution/format.
   - If width/height/pixel format changed, mark `push_reinit_needed_`; `EnsureAllPipelinesReady` closes and restarts the push thread with the new `VideoConfig`.
   - Compute fps with `av_guess_frame_rate(fmt, stream, nullptr)` and round to an int; clamp to `>=1`; pass it to `Recorder::VideoConfig`. Do not use `framerate.num` alone.

4. **Alarm ownership**
   Files: `Core/alarm.h/.cpp`, `Core/scheduler.h/.cpp`, `Core/worker.cpp`
   - Replace the shared mutable `Alarm` with an immutable job value:
     - `struct AlarmJob { int64_t id; std::string video_code; std::string monitor_code; std::string level; std::vector<FramePtr> frames; uint64_t start_pts; std::optional<Recorder::HlsPaths> hls_paths; std::chrono::system_clock::time_point start_ts; }`.
     - `Alarm::Finalize` becomes a const/static writer that takes `AlarmJob` and does not mutate shared state.
   - HLS recorder ownership:
     - Add `HlsEventWriter` owned by the detect thread (`std::shared_ptr<HlsEventWriter> active_hls_`), created on `kBecameActive`.
     - Detect thread writes slice frames to the HLS writer at flush time.
     - On `kBecameIdle`/max duration, close the writer and put `HlsPaths` into the final `AlarmJob`.
     - Remove `Alarm::AddFrameToHls` calls from the cleanup thread.
   - Alarm image:
     - Encode JPEG bytes on the detect thread (`EncodeToBuffer`) and enqueue `{path, bytes, metadata}` to the alarm dispatcher.
     - Do not retain a full `Frame` in the pending upload queue.
   - Alarm ID:
     - Add `int64_t NextAlarmId()` in `Utils/common` using atomic CAS: `next = max(now_ms, last+1)`.
     - `CombinedEventManager` obtains a new ID at event start; slices share it. Remove the constructor-seeded counter and the `worker.cpp:401` FIXME.
   - Sound/HTTP/reencode all operate on the immutable job; `mutable` members disappear from `Alarm`.

5. **Small correctness fixes**
   - `Monitor::NextId`: `static std::atomic<uint64_t>`.
   - `GetMonitorTasks`: use `std::unordered_set<std::string>` instead of the O(n²) duplicate loop.
   - `OfflineAnalyze`: guard empty monitor list; return a meaningful error.
   - `Reconnect`: increment `connect_count` on every attempt, apply exponential backoff, and honor `maxReconnectAttempts`.
   - `Scheduler::CleanUpLoop`: use `try_acquire_for(100 ms)` and drain both queues before exit; `Scheduler::~Scheduler` performs a final drain.
   - `Worker::~Worker`: set `state_=false`, signal streams, call `Stop()` on all analyzers, join threads, then destroy contexts.
   - Ensure `main` stops the scheduler and waits for workers before destroying thread pools.

### Tests

- New `tests/scheduler_lifecycle_test.cpp`:
  - Add/remove workers concurrently from 8 threads; TSAN clean; no duplicate worker creation for the same `video_code`.
  - Remove from the pull-stream thread simulation; no unlocked map access.
- New `tests/alarm_job_test.cpp`:
  - Concurrent finalize of two jobs with different IDs; no shared state; HLS paths and IDs are correct.
  - Verify monotonic unique IDs under 8 threads.
- Manual reconnect test:
  - Start a local RTSP/file source, interrupt the sender, restart with a different resolution; verify no crash, convertor rebuild, and frames continue with the new size.
  - Verify bounded reconnect attempts and admin stream-end notification.

### Acceptance

- TSan reports no races in `scheduler_lifecycle_test` and registry tests.
- `rg "const_cast"` is empty.
- Reconnect with a resolution change produces correctly sized annotated frames and push output.

---

## 11. Phase 5 — Thread Pools and Non-Blocking API

### Goal

Remove the single 3-thread pool bottleneck and stop blocking the libevent loop.

### Changes

1. **Split thread pools**
   Files: `Core/scheduler.h/.cpp`, `Core/main.cpp`
   - Replace `g_th_pool` with:
     - `g_control_pool` (size `application.threadPoolSize`, default 3): worker start/stop, push-thread joins, admin notifications, RTSP probe offload.
     - `g_media_pool` (size `application.offlineThreads`, default 2): offline file analysis.
     - `g_alarm_pool` (size `application.alarmThreads`, default 2): alarm JPEG write/POST and HLS reencode.
   - Initialize all pools in `main` after `Config` is loaded, before `Scheduler` starts. `0` still means `hardware_concurrency` for `threadPoolSize`.
   - Shutdown order: stop scheduler → drain alarm dispatcher → `wait()` media/alarm/control pools.
   - Update all `g_th_pool` call sites:
     - `scheduler.cpp:151,196` → `g_media_pool`.
     - `worker.cpp:492` → alarm dispatcher (next step).
     - `worker.cpp:1092,1139` → `g_control_pool`.
     - `alarm.cpp:216` → `g_alarm_pool` gated by `std::counting_semaphore reencode_slots_{1}`.
     - `api_rtsp.cpp:72-100` → async offload (next step).

2. **Bounded alarm dispatcher**
   New files: `Core/alarm/alarm_io_dispatcher.h/.cpp`
   - `struct AlarmIoTask { enum Kind { kWriteAndUpload, kReencode }; ... }`.
   - Bounded `std::deque` + mutex/condition variable with `performance.maxPendingAlarmJobs`.
   - N consumer threads (`alarmThreads`); drop **newest** when full, increment `alarm_jobs_dropped`, log every 100 drops.
   - Separate `std::counting_semaphore<> reencode_slots_{1}` so a long HLS reencode does not consume all alarm I/O threads.
   - `Scheduler::AddAlarm` enqueues to the dispatcher; remove `AlarmQueue` if it becomes redundant.
   - `AlarmDispatcher::Drain()` on shutdown so queued alarms are written before exit (bounded wait).

3. **Asynchronous HTTP add/cancel/probe**
   Files: `Core/server.cpp`, `Core/server_apis/api_monitor.cpp`, `Core/server_apis/api_rtsp.cpp`, new `Core/server_apis/server_context.h`
   - Change the per-URL callback arg for blocking endpoints to:
     ```cpp
     struct ServerContext { Scheduler* scheduler; event_base* base; };
     ```
     `server.cpp` allocates one `ServerContext` on the heap (freed at shutdown) and registers `/api/control/add`, `/api/control/cancel`, and the RTSP probe route with it. Other routes keep `Scheduler*`.
   - In `ApiAddMonitor` / `ApiRemoveMonitor`:
     - Parse and validate the request synchronously (request body must be consumed in the callback).
     - Call `evhttp_request_own(req)` before returning.
     - Build a heap `PendingReply { evhttp_request* req; int code; std::string body; }`.
     - Submit the scheduler call to `g_control_pool`.
     - When done, fill `code/body`, then schedule `event_base_once(base, -1, EV_TIMEOUT, ReplyCb, reply, &zero_tv)`.
     - `ReplyCb` runs on the libevent loop: `send_json(reply->req, reply->code, reply->body); evhttp_request_free(reply->req); delete reply;`.
   - Apply the same pattern to the RTSP probe endpoint; build the JSON array in the worker thread.
   - If `evhttp_request_own`/free semantics differ in the vendored libevent version, verify against the local header and use the documented pair; add a comment with the version.
   - Keep response `code`/`msg` values and ordering exactly as today.

4. **Admin/offline callbacks**
   - Move `NotifyAdminStreamEvent` and offline-complete HTTP posts to `g_control_pool`; never perform blocking curl calls on the detect thread or event loop.
   - Add a short timeout to `Request::post` if not already present (read `Utils/request.cpp` and use the configured probe timeout or 5 s).

### Tests

- New `tests/api_async_test.py` or C++ integration test:
  - Start the server with a fake scheduler whose `MonitorBatchAdd` sleeps 2 s.
  - While add is pending, `GET /api/health` (or `/api/metrics`) responds within 200 ms.
  - Add eventually returns the same JSON as before.
- New `tests/alarm_dispatcher_test.cpp`:
  - Enqueue 1000 tasks with capacity 128; assert bounded memory, drop counter, and drain behavior.
- TSan/ASan run on lifecycle and dispatcher tests.

### Acceptance

- One slow camera add does not delay other API endpoints.
- `g_th_pool` references are gone; no blocking curl on the event loop.
- Alarm I/O queue depth is bounded and observable.

---

## 12. Phase 6 — Maintainability Refactor and Build Hygiene

### Goal

Reduce `Worker` to an orchestrator and remove fragile ownership/build patterns.

### Changes

1. **Extract `MonitorRegistry`**
   New files: `Core/monitor/monitor_registry.h/.cpp`
   - Owns `std::vector<std::shared_ptr<MonitorContext>>`, `shared_mutex`, add/remove batch, snapshots, `GetAllMonitor`, per-context ROI/level state.
   - API: `AddBatch`, `RemoveBatch`, `Snapshot`, `GetAll`, `SetHighestLevel`, `Empty`.
   - `Worker` delegates all monitor operations; detection gets a snapshot from the registry.

2. **Extract `StreamSession`**
   New files: `Core/stream/stream_session.h/.cpp`
   - Owns `AvPullStream`, decode loop helpers, `VideoContext`, `FormatConvertor`, and the push output lifecycle.
   - Exposes `Start`, `Stop`, `GrabFrame`, `OnStreamOpened`, `EnsurePush`, `ClosePush`.
   - No event/monitor knowledge.

3. **Extract `EventPipeline`**
   New files: `Core/event/event_pipeline.h/.cpp`
   - Owns the per-frame detection loop body: snapshot → detector infos → `CombinedEventManager::ProcessFrame` → transition handling.
   - Exposes `ProcessFrame(snapshot, timestamp)` returning `EventPipelineResult` (annotated frame, flushed events, transitions, alarm requests).
   - Owns annotator rebuild and combined event config updates.

4. **Extract `AlarmPipeline`**
   New files: `Core/alarm/alarm_pipeline.h/.cpp`
   - Owns HLS event writer lifecycle, alarm job construction, JPEG encoding, dispatcher submission, admin notifications.
   - Detect thread calls it synchronously for HLS/JPEG only; all blocking I/O goes to the dispatcher.

5. **Shrink `Worker`**
   - Target: `worker.h` under ~150 lines, `worker.cpp` under ~350 lines.
   - `Worker` owns `StreamSession`, `MonitorRegistry`, `EventPipeline`, `AlarmPipeline`, and the two threads.
   - Remove dead code (`MonitorEventSnapshot` if unused), duplicated helpers, and lock-order comments.

6. **Ownership cleanup**
   - `Analyzer` callback registration returns an RAII `RegistrationGuard` (or the detector pool returns an `AddTaskHandle` whose destructor deregisters).
   - Remove `friend class Analyzer` and raw `algorithm_.get()` capture where possible; the guard keeps the callback alive only while the analyzer exists.
   - `ModelPool` returns `shared_ptr<ModelRuntime>`; no `ConstPool`, no `const_cast`.

7. **Build/test restructure**
   Files: `CMakeLists.txt`
   - Add `add_library(analyzer_core STATIC ...)` containing all `Analyzer/Core` sources and public link dependencies.
   - Main executable links `analyzer_core`.
   - Every test links `analyzer_core` + `GTest::gtest_main`, avoiding duplicated source lists.
   - Add `add_test` for every test target.
   - Keep the existing test executables and names.
   - Add an `ANALYZER_SANITIZE` option if not already added in Phase 0.

8. **Repository hygiene**
   - Append to `.gitignore`: `build/`, `build-*/`, `cmake-build-*/`, `*.ts`, `*.m4s`, `*.m3u8`, `*.mp4` (keep `resources/` and intentional fixtures).
   - `git rm --cached -r build build-aarch64 cmake-build-debug` and `git rm --cached install.sh update.sh` (files remain on disk; no history rewrite).
   - Verify `git status` does not show the removed files as deleted on disk.
   - Do not remove or rewrite any user scripts; only untrack them.

### Acceptance

- `Worker` is a composition root; no `std::unique_ptr` ordering hacks or lock-order comments.
- A new source file is added to one core library, not to N test targets.
- Clean clone builds without tracked build artifacts; `ctest` runs all tests.

---

## 13. Verification Plan

### Per-phase gates

1. Build:
   - `cmake --preset debug`
   - `cmake --build --preset debug -j8`
2. Unit tests:
   - `ctest --test-dir build --output-on-failure`
3. Sanitizers (for phases 2, 4, 5, 6):
   - Configure a separate build dir with `-DANALYZER_SANITIZE=thread` and run lifecycle/registry/dispatcher tests.
   - Repeat with `address` for registry/alarm tests.
4. Static checks:
   - `rg "const_cast" Analyzer/Core` → empty after Phase 1/6.
   - `rg "g_th_pool"` → empty after Phase 5.
   - `rg "std::unique_ptr<MonitorContext>"` → empty after Phase 2.

### Integration / performance validation

Use 8 local 1080p25 streams (loop files via `ffmpeg -re` or an RTSP test server) for 30 minutes.

Measure via `/api/metrics` and `ps`:

- Per-camera detection cadence: every camera receives at least one result per configured `major_stride_ms` (2 s), and no camera is starved for more than 2× its configured stride.
- API latency: `GET /api/metrics` p95 < 200 ms while adding/removing a stream and while a stream is reconnecting.
- Memory: per-worker RSS stays within `eventBufferBudgetMb` plus a fixed overhead (document the measured overhead); no monotonic growth over 30 minutes.
- Alarm integrity: all generated alarm jobs produce playable HLS/MP4 and uploaded JPEGs; no dropped jobs under normal admin availability.
- Reconnect: kill and restart a source with a different resolution; frames continue, push/annotation dimensions match the new resolution.
- CPU: inference threads are distributed across sessions; no single-thread saturation attributable to a task queue.

### Rollback

- Each phase is a separate commit. If a phase regresses, revert that commit; later phases must not depend on revert-incompatible APIs. Phase ordering is chosen so that reverting Phase 5/6 does not affect Phase 1–4 behavior.

---

## 14. Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Multiple ORT sessions exhaust GPU memory | Default 1 session when `acceleratorInference=true`; config override; startup log of session count and memory |
| Per-analyzer mailbox changes result ordering | Guarantee one in-flight task per analyzer; deliver shared request results to all sinks atomically; unit test monotonic timestamps |
| JPEG pre-roll introduces color/quality regressions | Round-trip test with MAE threshold; visual check of alarm clips; keep raw active-event frames |
| Async HTTP reply has libevent ownership pitfalls | Use `evhttp_request_own` + `event_base_once`; verify header semantics; add integration test; keep reply on loop thread |
| Shared_ptr monitor contexts mask logic errors | `active` flag checked in every snapshot use; ASAN test with concurrent remove |
| Reconnect generation logic drops frames | Tag packets with generation; flush decoder once; test with resolution change; keep old resources alive via shared_ptr |
| Refactor changes alarm payloads | Freeze JSON schema in tests; compare before/after payload snapshots |
| TSan false positives from ONNX Runtime | Run TSAN tests with the fake inference session; exclude ONNX-heavy tests from TSAN target |

---

## 15. Definition of Done

The checklist below is the original plan acceptance checklist. It is retained
for traceability; current evidence and outstanding runtime gates are recorded
in `PROJECT_STATUS.md`.

- [ ] All 13 review findings have a corresponding implemented change.
- [ ] Inference is per-model, multi-session, fair per analyzer, with bounded mailboxes.
- [ ] `WantsFrame()` admission happens before clone/enqueue; same-task monitors share one inference.
- [ ] No blocking wait holds `monitors_mutex_` or `event_manager_mutex_`.
- [ ] Add/cancel/probe run off the libevent loop and keep the existing JSON contract.
- [ ] Per-worker event buffer is byte-budgeted; idle pre-roll uses JPEG; push queue is frame-bounded.
- [ ] Worker map, reconnect, and alarm finalization are race-free under TSan.
- [ ] Small bugs fixed: atomic monitor IDs, unique alarm IDs, correct fps, backoff on decode errors, bounded reconnect, bounded result wait.
- [ ] `Worker` split into `StreamSession`, `MonitorRegistry`, `EventPipeline`, `AlarmPipeline`.
- [ ] No `const_cast` in detector code; RAII callback registration.
- [ ] `analyzer_core` library + all tests registered; build artifacts untracked.
- [ ] 8-stream 30-minute validation passes the metrics/memory/API targets.
