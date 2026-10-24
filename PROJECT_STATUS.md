# Analyzer Project Status

**Status record date:** 2026-10-24  
**Repository:** `Roshall/Video-Anomaly-Detector`  
**Working branch at review:** `refactor/worker-extraction` (current review worktree)  
**Purpose:** current implementation, validation, and maintenance status

This file is the authoritative status summary for the Analyzer directory.
The detailed review in [CODEBASE_REVIEW_REPORT.md](CODEBASE_REVIEW_REPORT.md)
contains the evidence and recommendations; [TODO.md](TODO.md) contains the
action checklist. The historical design plan in
[.trae/documents/analyzer-design-remediation-plan.md](.trae/documents/analyzer-design-remediation-plan.md)
records the original seven-phase decisions and should not be read as proof
that every acceptance test has passed.

## 1. Status legend

| Marker | Meaning |
|---|---|
| Done | Implemented and supported by current source/tests |
| Partial | Implemented in part, or only component-tested |
| Pending | Required work or validation has not been completed |
| Blocked | Cannot be completed until an external dependency is available |

## 2. Executive status

| Area | Status | Current position |
|---|---|---|
| Core architecture | **Done** | `Worker` composes stream, monitor, event, offline, and alarm pipelines |
| Stride/admission | **Done / Partial validation** | Major/minor stride admission, bounded mailboxes, and shared inference are implemented; multi-stream fairness is not certified |
| Live/offline lifecycle | **Done / Partial validation** | Registry snapshots, reconnect generations, decode-loop policy, and shared live/offline event flow exist; long-running shutdown/reconnect tests remain |
| Event buffering | **Done / Partial validation** | Idle JPEG buffering and byte budget exist; total worker RSS is not yet certified |
| Alarm/media output | **Partial** | HLS/MP4/JPEG paths and bounded dispatch exist; output failure propagation and critical-job guarantees remain |
| Metrics/API | **Partial** | Metrics and asynchronous control endpoints exist; service-gap/backend/output-health metrics and deployment hardening remain |
| CPU inference | **Done / Partial validation** | ONNX CPU path is the validated implementation; copy/preprocess cost and cross-video fairness need measurement |
| aarch64 build | **Partial** | Cross-build/package documentation and architecture conditionals exist; target runtime smoke test remains |
| Ascend NPU | **Blocked / Skeleton** | ACL/VDEC/DVPP/model/inference seams compile and have contract tests; no real device validation and several execution paths are stubs |
| Typed Ascend run status | **Partial** | `AscendInferenceBackend::RunWithStatus()` distinguishes ready, unavailable, invalid input, and execution error; hardware execution remains unavailable |
| Configuration validation | **Partial** | Known field types are checked and exposed through `Config::valid`/`validation_errors`; required-field schema and startup refusal at the application boundary remain |
| Documentation | **Done for R7** | README, status record, TODO, and review report are aligned by this change |
| CI/release gates | **Pending** | No repository CI and no completed 8-stream acceptance record |

## 3. Implemented capabilities

### Detection and event processing

- Major/minor stride state machine in `EventDetector`.
- Timestamp-based frame admission before clone/submit.
- Same-task grouping and shared inference for monitors in one worker.
- Live depth-one mailbox with newest pending frame replacement.
- Bounded offline pipeline depth.
- Vote window, event start/active/end transitions, and configurable levels.
- Combined event buffering with JPEG idle pre-roll and raw active-event frames.
- Dynamic annotation with ROI and legend support.

### Stream and lifecycle

- One `Worker` per logical `video_code`.
- Shared-pointer monitor snapshots with an `active` removal flag.
- Extracted `StreamSession` decode/reconnect/geometry/push responsibilities.
- Generation-tagged pull resources and bounded reconnect policy.
- Decode-loop policy seam with unit tests for error budget/backoff behavior.
- Separate control, offline/media, and alarm work paths.

### Outputs and observability

- JPEG snapshots, MP4 event files, HLS event output, and suggested MP4
  re-encoding.
- Bounded alarm I/O dispatch with drop counters.
- Frame, inference, reconnect, push, alarm, upload, and re-encode metrics.
- Asynchronous monitor add/remove operations away from the HTTP event loop.

## 4. Validation record

The following is the current evidence state. A check is not marked complete
because a test exists; it is marked complete only when the required run has
been recorded.

| Validation | Status | Evidence/next action |
|---|---|---|
| Enabled unit tests | **Previously recorded green** | `TODO.md` records 26/26 green on the earlier branch state; rerun after future source changes |
| EventPipeline guard tests | **Done** | `event_pipeline_test` added and passes; deeper fake-context behavioral coverage remains in `TODO.md` |
| Mailbox maintainability tests | **Done / Partial** | `inference_mailbox_test` now covers eight independently schedulable analyzers; detector-worker fake-session seam remains |
| TSan lifecycle/registry coverage | **Previously recorded clean** | Preserve as a required gate; ONNX-backed paths may remain excluded |
| ASan | **Pending** | Run focused registry, event buffer, detector pool, and alarm tests |
| 8 streams, 1080p25, 30 minutes | **Pending** | Measure cadence, starvation, CPU, queue age, and memory |
| API p95 under add/remove/reconnect | **Pending** | Target is under 200 ms for `GET /api/metrics` |
| RSS stability | **Pending** | Report event budget, fixed overhead, queue memory, and slope |
| Alarm artifact integrity | **Pending** | Verify playable HLS/MP4 and uploaded JPEGs; record dropped jobs |
| Reconnect with resolution change | **Pending** | Verify resumed frames, push dimensions, and annotation geometry |
| aarch64 package smoke test | **Pending** | Install/run on target or compatible runtime and verify shared libraries/codecs |
| Ascend hardware test | **Blocked** | Requires a real Ascend NPU, CANN, and target model/runtime |

## 5. Open engineering risks

These are intentionally not presented as completed:

1. **Backend integration:** typed Ascend outcomes now distinguish unavailable,
   invalid input, and execution error, but the generic backend consumer still
   needs to propagate status and real device execution is unvalidated.
2. **Configuration validation:** known field types now produce
   `validation_errors`; required paths, complete schema coverage, and startup
   refusal at the application boundary remain.
3. **Queue/fairness contract:** detector, push, event, and alarm queues have
   different limits and drop policies. Add queue-age/service-gap metrics and
   document critical versus best-effort work.
4. **Output failure propagation:** recorder/add-frame/close failures need to
   affect job state before notifying the admin service.
5. **Timestamp fallback:** `FrameWallClock::ObservedAnchor` still needs a
   first-valid-PTS anchor implementation and tests.
6. **Shutdown latency:** FFmpeg blocking operations need interrupt/timeout
   validation during stop and reconnect.
7. **Memory accounting:** event budget does not cover every `cv::Mat`,
   mailbox, packet, conversion, push, and encoder allocation.
8. **API/deployment boundary:** authentication, body-size limits, path roots,
   finite HTTP timeouts, and secure TLS defaults are not yet a documented
   production contract.
9. **Build reproducibility:** global compiler flags, dependency layouts, and
   package runtime assumptions need CI or install smoke tests.

## 6. Maintenance workflow

For every behavior change:

1. Update the focused source/test documentation.
2. Run the smallest relevant unit test set.
3. Update the matching checkbox in [TODO.md](TODO.md).
4. If the validation state changes, update this file with date, command,
   environment, and result.
5. Update [CODEBASE_REVIEW_REPORT.md](CODEBASE_REVIEW_REPORT.md) only when the
   finding or recommendation changes; do not use it as a test log.

For every release candidate:

1. Build Debug and Release from a clean build directory.
2. Run enabled tests, ASan, and TSan as applicable.
3. Run the eight-stream acceptance scenario.
4. Verify alarm artifacts and reconnect geometry.
5. Run the package smoke test on the target architecture.
6. Record all exceptions and unverified gates here.

## 7. R7 documentation change record

R7 was the documentation/status gap identified in the review: the README and
status records did not describe the current pipeline, configuration knobs,
validation boundaries, or unresolved remediation work.

This update:

- replaces the minimal README with build, configuration, runtime, packaging,
  architecture, test, and limitation documentation;
- adds this current status record as the single status summary;
- keeps TODO items grouped by validation, tests, code, and housekeeping;
- clarifies that the seven-phase remediation plan is historical context;
- records the distinction between implemented CPU behavior, pending runtime
  validation, and hardware-blocked Ascend work.

## 8. TDD changes recorded on 2026-10-24

- Added typed Ascend status tests for unavailable backends, invalid device
  inputs, and stable status strings in
  `tests/ascend/ascend_skeleton_test.cpp`.
- Added configuration tests for malformed numeric/array types and valid
  effective state in `tests/config_test.cpp`.
- Implemented the smallest production changes needed to satisfy those tests:
  `AscendInferenceBackend::RunWithStatus()` and `Config::valid`/
  `Config::validation_errors`.
