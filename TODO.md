# TODO — remaining work

Status of this branch (`refactor/worker-extraction`, HEAD `c1856d5`): all seven
remediation phases from `.trae/documents/analyzer-design-remediation-plan.md`
are implemented; `ctest` is 26/26 green (4 manual tests disabled) and the TSan
suite is clean. The items below are what is still open. Untracked findings from
`CODEBASE_REVIEW_REPORT.md` (2026-10-24) that are not listed elsewhere are
tracked in §5.

## 1. Validation owed by the remediation plan (§13)

- [ ] **8-stream 30-minute integration run** — 8 local 1080p25 streams
      (`ffmpeg -re` loop files or an RTSP test server); verify every camera
      receives a detection result at least once per `major_stride_ms` and no
      camera is starved for more than 2× its stride.
- [ ] **API latency** — `GET /api/metrics` p95 < 200 ms while a stream is being
      added/removed and while a stream is reconnecting.
- [ ] **Memory** — per-worker RSS stays within `eventBufferBudgetMb` plus a
      documented fixed overhead over 30 minutes; no monotonic growth.
- [ ] **Alarm integrity** — all generated alarm jobs produce playable HLS/MP4
      and uploaded JPEGs; no dropped jobs with the admin service available.
- [ ] **Reconnect with resolution change** — kill/restart a source at a
      different resolution; frames continue and push/annotation dimensions
      match the new geometry.
- [ ] **CPU distribution** — inference threads spread across sessions; no
      single-thread saturation attributable to one task queue (plan §13).
- [ ] **ASan pass** — only TSan has been run. Configure
      `cmake -B build-asan -DANALYZER_SANITIZE=address -DBUILD_TESTS=ON` and
      run at least `monitor_registry_test`, `alarm_job_test`,
      `event_buffer_test`, `detector_pool_test`.

## 2. Test gaps

- [ ] **EventPipeline integration tests** — the guard contracts of
      `SubmitFrame` (`Analyzer/Core/event/event_pipeline.cpp:168`) and the
      once-per-monitor invalid-levels warning
      (`event_pipeline.cpp:366`, reset via `level_log_reset_`) are covered
      only at the guard level (`tests/event_submit_test.cpp`); the
      config→manager wiring (`event_pipeline.cpp:99`) and the warning behaviour
      still need a harness with a fake monitor context.
- [ ] **ONNX-backed paths are not under TSan.** Model loading / session start
      are covered by `detector_pool_test` only in normal builds (the test
      skips via `ANALYZER_TSAN` because ONNX Runtime is uninstrumented).
      Either accept this or add the planned `IInferenceSession` fake seam.
- [ ] **Independent review of the config + JPEG lens** — the adversarial
      reviewer for that area failed on a network timeout and never produced a
      report; re-run it if you want parity with the other review lenses. The
      config findings themselves are now tracked in §5.
- [ ] (Optional) Pin the shared-inference fallback: when the claimer takes
      longer than the wait budget (capped at 10 s), a waiter re-runs inference
      by design. Add a test documenting the duplicate-inference trade-off.

## 3. Deferred code work (LOW severity, intentionally not fixed)

- [x] **Worker extraction** — done in `refactor/worker-extraction`
      (`c372ba5`): the decode loop moved to `StreamSession::RunDecodeLoop`,
      frame admission/submission to `EventPipeline::SubmitFrame`, offline
      analysis to `Core/offline/offline_pipeline`, monitor lifecycle helpers
      to `MonitorRegistry`, and the shared per-frame event flow to
      `Core/event/event_runner.{h,cpp}` (`c1856d5`). `worker.cpp` is 393
      lines (from 830).
- [x] **Pull-thread ROI races (found in the branch review).** `PrepareForStream`
      ran on the pull thread and mutated `ctx->roi_entries` plus the analyzers`
      ROI vectors while the detect thread read them. `MonitorContext::roi_entries`
      and the analyzer ROI state (`GeneralAnalyzer`, `BunkOffAnalyzer`) are now
      immutable snapshots published with `std::atomic` `shared_ptr` — swap on
      (re)connect, load once on the detect thread.
- [x] **Decode-loop state machine had no test seam.** The error budget, backoff
      and reconnect waiting now live in `stream/decode_loop_policy.{h,cpp}`
      with unit tests (`tests/decode_loop_policy_test.cpp`).
- [x] **TaskLabels was O(n²)** — now an `unordered_set` scan in
      `Analyzer/Core/monitor/monitor_registry.cpp:78` (was a `std::find_if`
      per monitor in the removed `Worker::GetMonitorTasks`).
- [ ] **JPEG range correctness** (`Analyzer/Core/video_processing/jpeg_encoder.cpp:126,135,265`).
      Limited-range YUV420P is encoded as a full-range JPEG without pixel
      conversion, so the round trip is pixel-exact but a standards-compliant
      viewer (browser) may show stretched contrast on alarm JPEGs. Proper fix:
      convert limited→full on encode and full→limited on decode via
      `sws_setColorspaceDetails`, then verify with a synthetic limited-range
      gradient (MAE) and a visual check of alarm clips.
- [ ] **DetectorPool shutdown vs in-flight loads** —
      `DetectorPool::StopAllDetectors` (`Analyzer/Core/detector/constant_yolo.cpp:250`)
      drains `runtimes_` but not `loading_`; a load that finishes after
      shutdown repopulates the pool. Add a shutdown flag checked before the
      final `emplace`, or drain `loading_` waiters.
- [ ] **Shared-inference wait is capped at 10 s** — a claimer that needs more
      than the cap still causes one duplicate inference per waiting sink.
      Accepted trade-off; revisit only if slow models appear.

## 4. Housekeeping

- [ ] **Orphan tests** — `tests/event_test.cpp`, `tests/strategic_detect_test.cpp`
      and `tests/annotator_test.cpp` are not registered in `CMakeLists.txt`;
      `tests/dynamic_annotator_stub.cpp` is no longer referenced. Register
      them (and fix their source lists) or delete them.
- [ ] **CI** — there is no CI workflow. Note the environment quirks if one is
      added: the system `ctest` is 3.10.2 (no `--test-dir`; run from the
      build dir or use the ctest from CMake ≥ 3.20), and TSan needs
      `setarch $(uname -m) -R`.
- [ ] **Docs** — the README does not document the new knobs
      (`performance.eventBufferJpegQuality`, empty `preloadModels` disables
      preloading, `streamReconnect*`) or the `GET /api/metrics` response.
- [ ] **Plan status** — the Definition of Done checklist in
      `.trae/documents/analyzer-design-remediation-plan.md` is unchecked.
      Either tick it against this branch or make this file the status record
      and point the plan at it.
- [ ] **Generated build trees** — `build/`, `build-*/`, `build-verify/`,
      `build-tsan/`, `build-review/`, `build-work/`, `cmake-build-debug/`
      exist on disk (gitignored); prune stale ones to cut disk noise.

## 5. Open findings from CODEBASE_REVIEW_REPORT.md (2026-10-24)

Not yet tracked above; none are addressed by the worker-extraction branch.

- [ ] **P0 — Ascend backend readiness.** The stub returns `{}` for both device
      and host inputs, so a configured-but-unavailable NPU looks like a valid
      empty detection and suppresses alarms
      (`Analyzer/Core/backend/ascend_inference.cpp`). Add a typed status
      (`ready`/`unavailable`/`invalid input`/`execution error`) or fail
      startup when Ascend is required; CPU fallback only when explicitly
      configured; emit a degraded-mode metric. Note:
      `tests/ascend/ascend_skeleton_test.cpp` currently pins the unsafe
      contract and must be updated with the fix.
- [ ] **P0 — config type fallback.** `json_get_or` swallows all conversion
      exceptions (`Analyzer/Core/config.cpp`), so a typo or wrong type in a
      safety/performance setting silently becomes a default. Clamps exist but
      do not report. Add startup schema validation (path/value/type errors),
      fail startup for required settings, keep defaults only for optional
      fields, and log the effective configuration once.
- [ ] **P1 — end-to-end queue/fairness policy.** Detector, push, event and
      alarm layers each bound queues differently
      (`Analyzer/Core/event/event_pipeline.cpp`, `detector/mailbox.h`,
      `stream/stream_session.cpp`, alarm pipeline). Define one documented
      per-video policy (decode rate, detection stride, max queue age, push and
      alarm drop policy, shutdown deadline); export queue depth, oldest-frame
      age and drop reason per video.
- [ ] **P1 — CPU hot-path copies.** `EventPipeline::SubmitFrame` converts and
      `cv::Mat::clone()`s every frame; `detector/inference.cpp` letterboxes,
      converts BGR→RGB and repacks FP32 NCHW per frame. Profile first; reuse
      per-worker scratch buffers; avoid the clone when ownership can transfer;
      consider a native-format backend input contract.
- [ ] **P1 — inference scaling.** Execution is one image at a time
      (`Analyzer/Core/detector/yolo_detector_onnx.cpp`,
      `Analyzer/Core/detector/constant_yolo.cpp`); more detector workers add
      contention, not throughput. Measure throughput versus
      `inferenceWorkersPerModel` before changing defaults.
- [ ] **P1 — recording reliability.** Hard `libx264` requirement, `std::cerr`
      errors, and `CloseOutputSegment` ignores flush/trailer failures
      (`Analyzer/Core/video_processing/frame_recorder.cpp`) — a missing
      encoder or disk failure becomes a partial alarm artifact. Use typed
      recorder errors, project logging, temp-file/finalize atomicity, and an
      output-health metric; document codec requirements per architecture.
- [ ] **P1 — no cancellation-aware FFmpeg read.** RTSP timeouts are set but no
      `interrupt_callback` is tied to shutdown/deadline
      (`Analyzer/Core/pipeline/ffmpeg_decoder.cpp`, `stream/stream_session.cpp`),
      so a blocked demux delays worker shutdown. Wire a cancellation token and
      test shutdown during connect/read.
- [ ] **P2 — memory accounting.** `eventBufferBudgetMb` governs idle JPEG
      frames only; active/ending raw frames and other queues are outside it
      (`Analyzer/Core/combined_event_manager.cpp`, `config.cpp`). Publish
      raw/encoded counts and bytes separately and set a process-wide envelope;
      validate over the §1 30-minute run.
- [ ] **P2 — build reproducibility.** `CMAKE_BUILD_TYPE` forced to DEBUG,
      global compiler flags overwritten (incl. `-march=native` on x86), CUDA
      compiled by default without a provider check (`CMakeLists.txt`). Use
      target-scoped options, make CUDA opt-in or provider-verified, and add
      documented release presets for x86_64/aarch64.
