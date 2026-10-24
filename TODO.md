# Analyzer TODO and Maintenance Checklist

**Last reconciled:** 2026-10-24
**Authoritative summary:** [PROJECT_STATUS.md](PROJECT_STATUS.md)
**Detailed evidence:** [CODEBASE_REVIEW_REPORT.md](CODEBASE_REVIEW_REPORT.md)

This checklist is deliberately separate from the historical design plan. A
source change can be complete while its long-running or hardware validation
remains pending. Check an item only when the required evidence is recorded in
[PROJECT_STATUS.md](PROJECT_STATUS.md).

## 1. Release-blocking validation

These items must be completed before claiming production readiness for a
multi-video deployment.

- [ ] **VAL-01 — 8-stream endurance run.** Run eight local 1080p25 streams
      (`ffmpeg -re` loop files or an RTSP test server) for 30 minutes. Record
      per-camera decoded/admitted/result cadence, maximum service gap, CPU,
      queue age, reconnect count, and errors. Acceptance: every camera
      receives results at its configured major cadence and no camera is
      starved for more than `2 * major_stride_ms`.
- [ ] **VAL-02 — API latency under load.** Measure `GET /api/metrics` p95 while
      adding/removing a stream and while another stream reconnects. Target:
      less than 200 ms, with no event-loop stalls or unbounded deferred
      replies.
- [ ] **VAL-03 — Memory stability.** During VAL-01 measure per-worker and
      process RSS, event-buffer raw/encoded bytes, packet queues, mailbox
      frames, push queues, and encoder allocations. Record a fixed-overhead
      model; show no monotonic growth.
- [ ] **VAL-04 — Alarm integrity.** Generate alarms with the admin service
      available. Verify every accepted critical job has a playable output,
      uploaded JPEGs, correct event ID, and a successful/failed final status.
      Record dispatcher drops, encoder failures, upload failures, and cleanup
      failures.
- [ ] **VAL-05 — Reconnect geometry.** Stop and restart a source at a
      different resolution and pixel format. Verify frames resume, conversion
      state is rebuilt, and push/annotation/output dimensions match the new
      geometry.
- [ ] **VAL-06 — CPU distribution.** Compare throughput and queue wait with
      `inferenceWorkersPerModel` values 1, 2, and the deployment default.
      Confirm no session or task queue starves another video.
- [ ] **VAL-07 — ASan.** Configure a separate address-sanitized build and run
      at least `monitor_registry_test`, `alarm_job_test`,
      `event_buffer_test`, `detector_pool_test`, and the scheduler lifecycle
      tests.
- [ ] **VAL-08 — package smoke test.** Install into a clean staging prefix,
      run the packaged binary with a minimal config, verify shared-library
      resolution, model/resource paths, metrics startup, and output creation.
- [ ] **VAL-09 — aarch64 smoke test.** Build with the documented toolchain and
      run the installed package on the target or a compatible aarch64 runtime.
      Record FFmpeg codecs, ONNX Runtime ABI, C++ runtime, and HLS availability.
- [ ] **VAL-10 — Ascend hardware gate.** Block production Ascend enablement
      until a real device validates ACL context lifetime, model loading,
      device input/output synchronization, DVPP/VDEC, geometry changes,
      failure reporting, and multi-stream throughput.

## 2. P0 correctness and operability

- [x] **F-01a — typed backend outcome.** `AscendInferenceBackend::RunWithStatus`
      now distinguishes `ready`, `unavailable`, `invalid_input`, and
      `execution_error`; skeleton tests cover unavailable and invalid-input
      behavior. Remaining work is generic status propagation, degraded metrics,
      explicit CPU fallback policy, and hardware execution.
- [x] **F-15a — known configuration type validation.** `Config` now reports
      malformed known numeric, boolean, array, and HLS fields through
      `validation_errors` and sets `valid=false`; tests cover malformed types.
- [ ] **F-01b — backend integration policy.** Propagate typed backend outcomes
      through the generic pipeline, expose readiness/degraded state in health
      and metrics, and define explicit CPU fallback behavior.
- [ ] **F-15b — complete configuration schema.** Add required-field/path
      validation, complete legacy-key coverage, effective-config logging, and
      application-level startup refusal when `Config::valid` is false.
- [ ] **F-04 — wall-clock anchoring.** Implement the documented
      `FrameWallClock::ObservedAnchor` behavior: capture the first valid PTS
      and wall-clock observation, then calculate later timestamps from the
      stream time base. Add tests for missing, duplicate, backward, and
      reconnect timestamps.
- [ ] **F-02 — recorder result propagation.** Make frame add, flush, trailer,
      close, filesystem, and codec failures observable in a typed alarm/output
      result. Do not notify the admin service as successful for an incomplete
      artifact.
- [ ] **F-03 — critical alarm delivery.** Separate critical final alarm jobs
      from best-effort previews/re-encodes. Define retry/backpressure behavior
      and expose accepted, finalized, dropped, and failed states.

## 3. P1 performance and lifecycle work

- [ ] **F-05 — cancellation-aware FFmpeg I/O.** Tie an interrupt callback or
      equivalent cancellation deadline to pull/probe/read operations. Test
      stop during DNS, connect, blocked read, reconnect backoff, and EOF.
- [ ] **F-06 — CPU hot-path profile.** Measure clone, conversion, letterbox,
      BGR/RGB conversion, tensor packing, inference, annotation, and output
      enqueue time using admitted-frame rates. Optimize only measured stages;
      reuse scratch buffers and avoid copies when ownership is safe.
- [ ] **F-07 — model fairness.** Add per-model/session utilization, queue wait,
      queue age, result age, shared-inference hit rate, duplicate fallback,
      and per-video maximum service-gap metrics. If VAL-06 shows starvation,
      replace static assignment or add fair queue scheduling.
- [ ] **F-09 — keep media I/O off detection latency.** Measure HLS/event
      encoding and filesystem stalls against admission latency. Move or bound
      work that blocks detection, preserving event ordering and output
      finalization.
- [ ] **F-13 — byte-bounded packet queues.** Add byte watermarks in addition
      to packet-count limits. Export queue bytes and drop reasons, especially
      for large keyframes or bitrate changes.
- [ ] **F-08 — total memory envelope.** Extend accounting beyond idle JPEG
      frames to active raw frames, detector mailboxes, packet queues,
      conversion buffers, push queues, and encoder state. Define process and
      per-worker overload behavior.

## 4. P1 API and deployment hardening

The historical remediation plan marked security out of scope, but these items
are required for a safe operational contract.

- [ ] **F-10 — request limits and timeouts.** Set a maximum body size before
      multipart upload buffering, finite HTTP request timeouts, and bounded
      deferred work. Add tests for oversized and slow clients.
- [ ] **F-11 — filesystem roots.** Constrain snapshot, clip, cleanup, upload,
      alarm, and offline paths to configured roots. Add tests for symlinks,
      traversal, absolute paths, and non-regular files.
- [ ] **F-12 — outbound HTTP security.** Restore TLS certificate and host
      verification by default. If insecure mode is needed for development,
      make it an explicit configuration option and expose its state.
- [ ] **F-16 — reproducible builds.** Remove unconditional build-type/global
      flag side effects, make CUDA/provider selection explicit, use
      target-scoped architecture options, and add x86_64/aarch64 release
      configure checks.

## 5. P2 tests and maintainability

- [x] **TEST-01a — EventPipeline guard tests.** Added
      `tests/event_pipeline_test.cpp` and registered `event_pipeline_test`.
      It covers empty configuration, null-frame handling, invalid video
      context rejection, manager initialization, and refresh behavior.
- [ ] **TEST-01b — EventPipeline behavioral integration.** Extend the test
      harness to cover config-to-manager wiring, invalid-level warning/reset
      behavior, inactive monitor snapshots, admission counts, and shared
      inference with fake monitor/analyzer contexts.
- [x] **TEST-02a — Mailbox/shared-result contracts.** Existing mailbox and
      shared-inference tests now also cover eight independently schedulable
      analyzers, ordering, timeout, overwrite, and bounded buffering.
- [ ] **TEST-02b — Fake inference seam.** Keep ONNX-heavy tests out of TSan
      when the dependency is uninstrumented, and add a reusable fake
      inference-session seam for result ordering, fairness, and duplicate
      fallback at the detector-worker layer.
- [ ] **TEST-03 — output failure tests.** Inject missing codec, unwritable
      directory, short write, trailer failure, failed upload, and failed
      re-encode; assert job status and metrics.
- [ ] **TEST-04 — shutdown/reconnect tests.** Exercise concurrent monitor
      removal, blocked inference, blocked pull, push shutdown, worker cleanup,
      and replacement worker creation under ASan/TSan.
- [ ] **TEST-05 — configuration tests.** Cover missing, malformed, out-of-
      range, and valid values; assert defaults, clamps, errors, and effective
      configuration logging.
- [ ] **TEST-06 — JPEG range correctness.** Convert limited-range YUV420P to
      full-range JPEG correctly and back; use a limited-range gradient with an
      MAE threshold and inspect a representative alarm image.
- [ ] **TEST-07 — recorder PTS ownership.** Ensure recording does not mutate
      shared caller frame metadata, or make the ownership/copy contract
      explicit and test it.
- [ ] **TEST-08 — test registration.** Resolve orphan sources:
      `tests/event_test.cpp`, `tests/strategic_detect_test.cpp`,
      `tests/annotator_test.cpp`, and the unused dynamic annotator stub.
      Register valid tests or remove obsolete sources deliberately.
- [ ] **TEST-09 — CI.** Add clean Debug/Release, enabled CTest, ASan, and
      TSan jobs. Document tool versions, the CTest invocation for older CMake,
      and any `setarch` requirement.

## 6. Deferred or accepted trade-offs

These are not release blockers by themselves, but they must remain visible:

- [ ] **LOW-01 — DetectorPool shutdown race.** Prevent an in-flight model load
      from repopulating the pool after shutdown, or drain loading waiters.
- [ ] **LOW-02 — Shared inference wait cap.** The 10-second wait cap can cause
      a waiter to duplicate inference. Keep the cap, or make it configurable,
      and document the duplicate-work trade-off with a test.
- [ ] **LOW-03 — Active-event memory policy.** Document the intentional choice
      not to evict active frames under the idle JPEG byte budget; add a
      separate maximum event duration/overload policy if needed.
- [ ] **LOW-04 — Dynamic annotator benchmarks.** Turn claimed legend, ROI, and
      alpha-blending improvements into reproducible benchmarks.
- [ ] **LOW-05 — generated build trees.** Prune stale `build/`,
      `build-*/`, `build-review/`, `build-work/`, `build-tsan/`, and
      `cmake-build-debug/` directories when disk usage matters.

## 7. Completed work

- [x] **DOC-01 — documentation/status record.** README, project status, TODO,
      and historical remediation-plan links are aligned. See
      [PROJECT_STATUS.md](PROJECT_STATUS.md).
- [x] **ARCH-01 — worker extraction.** Decode, event submission, offline
      analysis, monitor registry, and shared event flow were extracted from
      the former monolithic worker.
- [x] **ARCH-02 — stride/admission.** Major/minor stride scheduling,
      timestamp admission, bounded live/offline mailboxes, and same-task
      shared inference are implemented.
- [x] **ARCH-03 — monitor lifetime.** Snapshot-based registry removal and
      inactive context guards protect in-flight detection work.
- [x] **ARCH-04 — decode policy seam.** Error budget, backoff, reconnect
      waiting, and related unit tests are present.
- [x] **ARCH-05 — event memory controls.** Idle JPEG buffering, event budget
      configuration, and bounded push/offline queues are present.

## 8. Updating this file

When completing an item:

1. Change only the relevant checkbox.
2. Record the command, environment, date, and result in
   [PROJECT_STATUS.md](PROJECT_STATUS.md).
3. Add or update the focused test/documentation.
4. Do not mark integration or hardware gates complete based only on static
   inspection or a component test.
