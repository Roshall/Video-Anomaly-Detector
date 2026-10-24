# TODO — remaining work

Status of this branch (`dev/phase6-maintainability`, HEAD `f066537`): all seven
remediation phases from `.trae/documents/analyzer-design-remediation-plan.md`
are implemented; `ctest` is 23/23 green (4 manual tests disabled) and the TSan
suite is clean. The items below are what is still open.

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
- [ ] **ASan pass** — only TSan has been run. Configure
      `cmake -B build-asan -DANALYZER_SANITIZE=address -DBUILD_TESTS=ON` and
      run at least `monitor_registry_test`, `alarm_job_test`,
      `event_buffer_test`, `detector_pool_test`.

## 2. Test gaps

- [ ] **EventPipeline has no tests.** The config→manager quality wiring
      (`Analyzer/Core/event/event_pipeline.cpp:96`) and the once-per-monitor
      invalid-levels warning (`event_pipeline.cpp:287`, reset via
      `level_log_reset_`) are only covered indirectly. Needs a harness with a
      `nullptr` Scheduler and a fake monitor context.
- [ ] **ONNX-backed paths are not under TSan.** Model loading / session start
      are covered by `detector_pool_test` only in normal builds (the test
      skips via `ANALYZER_TSAN` because ONNX Runtime is uninstrumented).
      Either accept this or add the planned `IInferenceSession` fake seam.
- [ ] **Independent review of the config + JPEG lens** — the adversarial
      reviewer for that area failed on a network timeout and never produced a
      report; re-run it if you want parity with the other review lenses.
- [ ] (Optional) Pin the shared-inference fallback: when the claimer takes
      longer than the wait budget (capped at 10 s), a waiter re-runs inference
      by design. Add a test documenting the duplicate-inference trade-off.

## 3. Deferred code work (LOW severity, intentionally not fixed)

- [ ] **JPEG range correctness** (`Analyzer/Core/video_processing/jpeg_encoder.cpp:126,135,265`).
      Limited-range YUV420P is encoded as a full-range JPEG without pixel
      conversion, so the round trip is pixel-exact but a standards-compliant
      viewer (browser) may show stretched contrast on alarm JPEGs. Proper fix:
      convert limited→full on encode and full→limited on decode via
      `sws_setColorspaceDetails`, then verify with a synthetic limited-range
      gradient (MAE) and a visual check of alarm clips.
- [x] **Worker extraction** — done in `refactor/worker-extraction`
      (`c372ba5`): the decode loop moved to `StreamSession::RunDecodeLoop`,
      frame admission/submission to `EventPipeline::SubmitFrame`, offline
      analysis to `Core/offline/offline_pipeline`, and monitor lifecycle
      helpers to `MonitorRegistry`. `worker.cpp` is 398 lines (from 830).
- [ ] **DetectorPool shutdown vs in-flight loads** —
      `DetectorPool::StopAllDetectors` (`Analyzer/Core/detector/constant_yolo.cpp:250`)
      drains `runtimes_` but not `loading_`; a load that finishes after
      shutdown repopulates the pool. Add a shutdown flag checked before the
      final `emplace`, or drain `loading_` waiters.
- [ ] **GetMonitorTasks is O(n²)** over distinct tasks
      (`Analyzer/Core/worker.cpp:793`) — negligible for ≤6 tasks, but the plan
      asked for an `unordered_set`; switch for consistency.
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
