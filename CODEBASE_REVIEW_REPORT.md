# Video Anomaly Detector — Full Codebase Review

**Review scope:** current `refactor/worker-extraction` worktree  
**Review focus:** maintainability, multi-video performance, event admission and
detection, recording/alarm reliability, aarch64 portability, and Ascend
readiness  
**Review date:** 2026-10-24

## 1. Executive summary

This is a substantial C++20 service rather than a prototype. The current
design has several good foundations for a small maintenance team:

- one `Worker` owns the lifecycle of one logical video source;
- stream, event, alarm, and offline work have been extracted into focused
  components;
- monitor removal uses shared snapshots and an `active` flag, so in-flight
  frames do not use freed monitor state;
- detection admission is timestamp-driven rather than decode-rate-driven;
- live detector mailboxes are bounded and use newest-frame replacement;
- same-task monitors can share one inference result;
- event pre-roll has a configurable JPEG/byte budget;
- HTTP control work is moved away from the libevent callback thread;
- metrics, unit tests, sanitizers, and explicit Ascend abstraction seams
  already exist.

The main remaining risks are not the absence of an architecture. They are
cross-cutting correctness and operability details:

1. The CPU hot path still performs several full-frame copies/conversions, and
   the model scheduler has no global fairness or queue-latency view.
2. Shutdown and reconnect depend on blocking FFmpeg operations and need
   long-running validation, especially with multiple streams.
3. Recording and alarm code does not consistently propagate encoder/write
   failures; the alarm dispatcher intentionally drops work when full.
4. The wall-clock fallback does not implement the behavior its comments
   describe.
5. The Ascend path is an explicit skeleton, but an unavailable backend can
   produce an empty detection result that is too similar to a valid
   no-detection result.
6. The HTTP/file boundary is permissive and has no clear authentication,
   body-size, or request-timeout policy. This is a deployment/security
   prerequisite even though it is separate from the core detection design.
7. Documentation and status records lag behind the current refactoring.

The recommended order is: make failure/degraded states observable; make
shutdown, timestamp, and output failure behavior deterministic; measure the
multi-stream hot path; then optimize copies and scheduling. Do not begin with
GPU/NPU optimization before those measurements exist.

## 2. System map and data flow

The service is organized around the following flow:

```text
HTTP/API or startup config
        |
        v
Scheduler
  └── Worker per video_code
        ├── MonitorRegistry
        │     └── MonitorContext -> Analyzer/EventDetector/registration
        ├── StreamSession
        │     ├── AvPullStream -> decode queue -> timestamp/geometry handling
        │     └── AvPushStream -> bounded output queue -> encoder
        ├── EventPipeline
        │     ├── admission by analyzer wanted timestamp
        │     ├── task grouping/shared inference
        │     └── CombinedEventManager
        ├── EventRunner
        └── AlarmPipeline -> AlarmIoDispatcher -> HLS/MP4/JPEG/admin HTTP

DetectorPool
  └── ModelRuntime per (model file, target task)
        └── several ONNX sessions, analyzer mailboxes, worker thread(s)

Optional backend seams:
  FFmpeg/OpenCV CPU path
  Ascend ACL/VDEC/DVPP/OM-model path (mostly skeleton/stub)
```

`Worker` is now a composition root rather than a monolithic implementation.
That is a good direction, but it also makes lifecycle contracts between
`StreamSession`, `EventPipeline`, `AlarmPipeline`, and `Scheduler` the most
important maintenance surface.

## 3. What is already implemented correctly

### 3.1 Admission and stride behavior

The project already implements the intended major/minor cadence:

- `EventDetector` starts in `kMajorCheck`.
- Idle monitors request the next frame at `major_stride_ms`.
- A positive major detection moves the detector to `kMinorCheck`.
- Verification and active-event checks use `minor_stride_ms`.
- `window_size` and `min_vote` implement a bounded vote window.
- `Analyzer::SetNextFrameTimestamp()` publishes the next requested timestamp.
- `EventPipeline::SubmitFrame()` creates admission entries from those
  timestamps.
- `AdmitFrame()` rejects frames older than the requested timestamp.

This is not a missing-feature recommendation. The remaining question is how
well this admission policy behaves under timestamp gaps, inference overload,
reconnects, and many concurrent videos. Those cases require measurements of
requested timestamp, decoded timestamp, admitted timestamp, and result age.

### 3.2 Bounded inference work

Live analyzer registrations use depth-one mailboxes. When inference is
already in flight, a newer pending frame replaces an older pending frame.
Offline analysis uses a bounded pipeline depth. Same-task analyzers in one
worker can share a converted frame and a `SharedInference` result. These are
the correct basic policies for bounded memory and avoiding redundant
inference.

The implementation does not yet provide a global fairness policy: model
sessions are assigned round-robin and there is no cross-video queue-age,
starvation, or per-model utilization metric.

### 3.3 Lifecycle and registry extraction

`MonitorRegistry::Snapshot()` returns shared context pointers, and removal
marks a context inactive before erasing it. Blocking analyzer stop and
registration cleanup happen outside the registry lock. This is materially
safer than holding a monitor lock across inference or destroying a context
while a decode callback still references it.

`StreamSession` also cleanly owns decode-loop mechanics, generation changes,
timestamp resolution, conversion state, and push output. This extraction
reduces the amount of code that must be understood before changing one
subsystem.

### 3.4 Existing observability and tests

The code has metrics for decoded/submitted/skipped/dropped frames, inference
timing, queue depth, reconnects, push drops, alarm jobs, upload failures, and
re-encode failures. Tests cover admission, detector pooling, mailboxes,
timeouts, event buffering, JPEG round trips, HLS recording, registry
snapshots, alarm dispatch, scheduler lifecycle, and asynchronous APIs.

The important limitation is that the metrics do not yet explain *why* a
frame was dropped or how far behind an analyzer is. Existing tests are
primarily component tests; the multi-stream and long-running acceptance
checks remain outstanding.

## 4. Findings

Severity is based on impact to a small operations/maintenance team, not only
on code complexity.

| ID | Priority | Area | Finding | Evidence / consequence |
|---|---|---|---|---|
| F-01 | P0 | Ascend/health | Unavailable Ascend execution can look like a valid empty detection | `AscendInferenceBackend::Run()` returns an empty vector for unavailable/device/host paths. A typed `unavailable` or `degraded-to-CPU` state is needed; otherwise operators may see “no event” instead of “inference is not running.” |
| F-02 | P1 | Recording | Encoder and file-write failures are not consistently propagated | `HlsEventWriter::AddFrames()` ignores `Recorder::AddVideoFrame()` results; `Alarm::FinalizeMp4Event()` also ignores frame-add and close outcomes, then reports the output. This can create success-shaped admin records for incomplete media. |
| F-03 | P1 | Alarm throughput | The bounded alarm dispatcher drops jobs when full, including final alarm side effects | `Scheduler::AddAlarm()` increments `jobs_dropped` and returns when `TrySubmit()` fails. This is a valid overload policy for optional images, but not automatically valid for final alarm videos/admin notifications. Separate critical and best-effort queues or an explicit retry policy is needed. |
| F-04 | P1 | Time correctness | `FrameWallClock::ObservedAnchor` never captures an observed PTS | The implementation returns `system_clock::now()` whenever the mode is `ObservedAnchor`, despite comments saying it should anchor on the first valid frame. Live streams without `start_time_realtime` can therefore receive wall-clock times based on processing time rather than frame time. |
| F-05 | P1 | Shutdown | FFmpeg read/probe operations do not have a clearly enforced interrupt callback tied to service shutdown | `StreamSession::Stop()` signals state, but blocking demux/probe calls can still depend on configured/network timeouts. Validate worst-case stop latency and add an interrupt callback if the FFmpeg call path permits it. |
| F-06 | P1 | CPU hot path | An admitted frame may be cloned, resized, color-converted, and packed multiple times | `EventPipeline::SubmitFrame()` clones a converted view; `YOLOv8Detect::RunSession()` performs letterboxing, BGR/RGB conversion, and FP32 NCHW packing. Optimize only after measuring copy time and admitted-frame rate per task/model. |
| F-07 | P1 | Scheduling | Round-robin model-session assignment has no cross-video fairness or starvation control | `ModelRuntime` assigns analyzers to sessions, while mailboxes are local to analyzers. Under many streams or slow models, there is no queue-age or per-video service-level metric to prove every camera receives a result within its stride. |
| F-08 | P1 | Memory | The configured event buffer budget does not represent all retained frame memory | The byte budget primarily governs idle JPEG entries. Active/ending raw frames, detector mailbox `cv::Mat` ownership, conversion buffers, push queues, and encoder buffers are separate allocations. RSS acceptance must measure the whole worker, not equate RSS to `eventBufferBudgetMb`. |
| F-09 | P1 | Output latency | HLS/event frame encoding is performed in paths that can delay detection | `HlsEventWriter::AddFrames()` adds all event frames to a recorder, and alarm/event transitions are connected to the detect-side pipeline. A long encode or filesystem stall can delay subsequent admission unless this is demonstrated otherwise with timing metrics. |
| F-10 | P1 | Network/API boundary | The server binds broadly and request resource limits are not a documented contract | `server.cpp` binds to `0.0.0.0`; the HTTP event timeout is disabled; multipart upload is read into a complete string without an obvious configured body cap. This can turn slow clients or large requests into memory/thread pressure. |
| F-11 | P1 | File/API boundary | File-serving and snapshot paths need an explicit trust-boundary policy | `/api/clip` and cleanup validate relative paths, but `/api/snapshot` opens a requested `videoPath` through OpenCV. The deployment must either authenticate this API and constrain paths to an allowlisted root, or document it as a trusted-local interface. |
| F-12 | P1 | Outbound HTTP | TLS certificate/host verification is disabled | `Utils/request.cpp` configures libcurl without certificate and host verification. This is acceptable only behind a controlled trusted network; otherwise alarm metadata can be intercepted or redirected. |
| F-13 | P2 | Decode memory | Packet queue capacity is count-based rather than byte-based | A queue limit in packets bounds packet count but not memory, because packet sizes vary greatly across codecs/bitrates. Add a byte watermark and expose queue bytes/drops. |
| F-14 | P2 | Recorder API | `Recorder::AddVideoFrame()` rewrites the caller’s `AVFrame::pts` | The recorder reconstructs monotonic PTS by assigning `frame->pts`. This hidden mutation is risky when the same frame is shared with push, event, or another output. Copy metadata or use a local frame/reference before rewriting. |
| F-15 | P2 | Config | Parsing is resilient but can hide operator mistakes | `json_get_or` catches conversion failures and returns defaults. Keep the fallback for optional fields, but report invalid type/value with key and source, and distinguish “missing” from “malformed.” Validate model, offline-video, and output directories at startup. |
| F-16 | P2 | Deployment | Build configuration is not yet a reproducible portability contract | The main CMake file forces `CMAKE_BUILD_TYPE DEBUG`, mutates global flags, enables `-march=native` on x86, and uses architecture-specific prebuilt dependency layouts. This can produce binaries that work on the build host but not on another x86 host. |
| F-17 | P2 | Ascend | Device lifetime, zero-copy synchronization, and throughput are unvalidated | ACL initialization has conditional real setup, but VDEC, OM loading, DVPP device conversion, and Ascend inference execution remain stubs. CPU fallback tests prove interfaces, not hardware correctness or performance. |
| F-18 | P2 | Documentation | The README is not an operational guide and status documents are stale | The README only describes a basic CMake/make build. It omits current knobs, metrics, test presets, packaging, reconnect behavior, and Ascend limitations. `TODO.md` and the historical remediation plan do not fully agree with the current branch/test registration. |
| F-19 | P2 | Validation | No repository CI or required multi-stream acceptance test is present | The remaining checklist calls for an 8-stream/30-minute run, API p95, RSS stability, alarm integrity, resolution-change reconnect, and ASan. These are precisely the tests needed to validate the project’s stated operating model. |
| F-20 | P3 | Maintainability | Several policies are implicit in constants or comments | Examples include the 10-second shared-inference wait cap, one global re-encode semaphore, queue capacities, and reconnect/probe timeouts. Promote them to named configuration/metrics and document the intentional trade-offs. |

## 5. Detailed subsystem review

### 5.1 Scheduler, workers, and multi-video behavior

`Scheduler` groups stream additions by `video_code`, serializes worker creation,
and avoids holding the worker-map lock while a `Worker` constructor loads or
registers models. Concurrent adders can wait for a worker start result. Batch
removal snapshots worker pointers and performs removal unlocked. These are
strong choices for API responsiveness.

The remaining operational questions are:

- worker construction can still occupy a control-pool task while probing input
  or loading models;
- a mixed file/stream batch is explicitly non-atomic;
- a worker is removed after an error budget is exhausted, so the API needs a
  clear state/last-error response rather than only a disappearing worker;
- `GetMonitorTasks` is still an O(n²) scan over distinct tasks. It is small at
  current task counts but an `unordered_set` would make the intent clearer;
- cleanup, detector shutdown, detached alarm work, and stream/push joins need
  one documented shutdown order.

Recommended invariant: every worker should expose `running`, `degraded`,
`stopping`, `stopped`, and `last_error` states, with the reason retained in
metrics/logs and returned by the control API.

### 5.2 Detection and inference

The major/minor stride design is appropriate because it avoids running the
detector at decode FPS. Admission happens before the expensive clone/submit
step, and same-task grouping avoids duplicate inference on one frame.

The hot-path cost model should be measured as:

```text
decode
 + conversion
 + admission bookkeeping
 + clone bytes
 + preprocess bytes/CPU
 + inference queue wait
 + inference
 + result/event processing
 + annotation
 + output enqueue
```

Metrics should include, per worker and model:

- decoded, requested, admitted, submitted, mailbox-overwritten, and
  result-delivered frames;
- requested timestamp minus decoded timestamp;
- decoded timestamp minus result timestamp;
- queue wait, inference time, preprocessing time, annotation time;
- per-video maximum service gap and p95/p99 service gap;
- shared-inference hit rate and duplicate fallback count.

Without these values, a “stride” configuration can appear correct while a
model session is actually serving one video preferentially.

### 5.3 Event state, buffering, and annotation

`EventDetector` and `CombinedEventManager` provide the right separation:
per-monitor state decides whether a detection is idle/starting/active/ending,
while the combined manager owns pre-roll, event slicing, annotation, and
event IDs. Idle frames can be JPEG-buffered, while active frames remain raw to
avoid repeated lossy materialization.

The memory policy must be documented as a *soft total budget*, or expanded to
account for all ownership. In particular, active frames are intentionally not
evicted by the byte budget, and detector mailboxes can retain images outside
the event manager. Add a worker-level memory estimate and a hard overload
policy for pathological long events.

The dynamic annotator’s pre-rendered legend and batch drawing are good
maintainability/performance improvements. The documented benchmark numbers
should be moved from prose claims to reproducible benchmark tests; otherwise
they are useful hypotheses, not acceptance criteria.

### 5.4 Decode, timestamps, reconnect, and geometry

`StreamSession` centralizes reconnect generation changes and rebuilds the
conversion/annotation path when geometry changes. This is the correct place
for those responsibilities. The packet queue is bounded in count, and
reconnect metrics exist.

The most important correctness issue is the `FrameWallClock` fallback:
`ObservedAnchor` currently returns `now()` and never records the first valid
PTS. Implement either:

```text
first_frame_wall_clock = now()
first_frame_pts = valid_pts
wall_clock = first_frame_wall_clock + (pts - first_frame_pts) * time_base
```

or explicitly document that no-epoch streams use processing time. Also test:

- missing PTS followed by valid PTS;
- backwards/duplicate PTS;
- reconnect with a new time base;
- resolution and pixel-format changes;
- stop during DNS/probe/read;
- EOF versus reconnect exhaustion.

### 5.5 Recording, HLS, alarms, and storage

The project supports direct MP4, HLS, suggested MP4 re-encoding, JPEG alarm
images, and asynchronous admin notification. The single re-encode semaphore
protects the alarm pool from unlimited concurrent long re-encodes, which is a
reasonable first guard.

The output contract is currently too optimistic. Every output stage should
return a result containing at least:

```text
started, frames_received, frames_encoded, finalized, path, error
```

The admin notification must only be sent after the corresponding output has
been finalized and validated. If a best-effort artifact fails, report the
failure through metrics and the job status instead of logging and continuing
as if successful.

Separate these classes of work:

- **critical:** final event record and required admin notification;
- **best effort:** preview JPEG, optional HLS, suggested MP4;
- **cleanup:** deleting temporary segments.

The queue policy can then drop or retry best-effort work without silently
dropping a critical alarm.

### 5.6 HTTP APIs and operational boundary

Moving monitor add/remove work to a control pool avoids blocking the libevent
thread and is a meaningful improvement. The API layer still needs a written
contract for authentication, request limits, path roots, and timeouts.

At minimum:

- configure a maximum request body size before reading multipart data;
- set a finite request timeout and reject slow/incomplete bodies;
- authenticate control, upload, snapshot, clip, cleanup, and test-model APIs;
- constrain every filesystem input to an explicit configured root;
- return structured error codes for worker startup, degraded inference, and
  output failure;
- set curl connect, total, and low-speed timeouts independently;
- restore TLS verification by default, with an explicitly named insecure
  development option.

These are deployment safeguards, not a replacement for the core pipeline
review.

### 5.7 Build, packaging, and aarch64

The project uses CMake presets, sanitizer flags, manual test registration,
bundled/prebuilt dependencies, and a packaging script. It also contains
explicit aarch64 compiler settings and separate Ascend options.

Improve reproducibility by:

1. removing the unconditional build-type override from the top-level CMake
   file;
2. applying architecture flags only to targets that need them;
3. making CPU baseline, ONNX Runtime location, FFmpeg codec availability, and
   optional CUDA/Ascend features explicit configure checks;
4. adding a packaging smoke test that runs the installed binary with a
   minimal config;
5. testing the package on an aarch64 host or emulator with the same runtime
   library layout;
6. documenting whether `libx264` and the selected HLS muxers are guaranteed
   on both architectures.

`-march=native` should not be used for a distributable x86 binary unless the
package is explicitly host-specific.

### 5.8 Ascend readiness

The abstraction shape is useful: ACL runtime/device context, VDEC, DVPP
image processing, model loading, inference backend, and pipeline factory
boundaries are present. The CPU implementations and architecture tests give
the project a way to compile and exercise those contracts without hardware.

The current state is not yet “Ascend supported.” It is “Ascend-adaptable
interfaces with stubs.” Before enabling it in production:

- return typed errors from VDEC, OM loading, DVPP device operations, and
  inference;
- expose backend readiness in `/api/metrics` and startup health;
- never convert backend failure to an empty detection vector;
- validate ACL context/stream ownership and synchronization;
- validate device buffer lifetime across asynchronous inference;
- test dynamic shape/stride, pixel format, and geometry changes;
- run a hardware benchmark with multiple streams and compare CPU fallback;
- define whether CPU fallback is automatic, forbidden, or operator-selected.

### 5.9 Tests and quality gates

The registered test set is broader than the minimal README suggests, including
admission, mailbox, timeout, registry, alarm, API, HLS, and Ascend skeleton
tests. Some model/visual/media tests are intentionally disabled because they
need local assets or external streams.

The following should become required gates:

| Gate | Required evidence |
|---|---|
| Unit | All enabled CTest targets pass |
| ASan | Registry removal, event buffer, detector pool, alarm job |
| TSan | Registry, admission, scheduler lifecycle, alarm dispatcher |
| Multi-stream | 8 x 1080p25 for 30 minutes; no camera exceeds 2x configured stride |
| API | p95 metrics/control latency while add/remove/reconnect is active |
| Memory | RSS stable and explained by configured budget plus fixed overhead |
| Alarm | Every accepted critical job produces a playable artifact and status |
| Reconnect | Stream resumes after failure and geometry change |
| Package | Installed package starts and exposes health/metrics |
| Ascend | Hardware-only suite, clearly separated from CPU skeleton tests |

## 6. Prioritized improvement plan

### Phase 0 — Make failure visible

1. Add typed backend status (`ready`, `unavailable`, `degraded`, `failed`).
2. Make recorder and alarm stages return/record finalization status.
3. Add critical versus best-effort alarm queue classes.
4. Fix `FrameWallClock::ObservedAnchor`.
5. Add worker state/last-error metrics and API reporting.

### Phase 1 — Prove lifecycle and capacity

1. Add FFmpeg interrupt-driven shutdown where supported.
2. Add packet-byte limits and queue byte metrics.
3. Add the 8-stream, reconnect, resolution-change, RSS, and alarm-integrity
   integration tests.
4. Run ASan in addition to the existing TSan coverage.
5. Measure per-stage latency and per-video service gaps.

### Phase 2 — Optimize measured hot paths

1. Reduce avoidable `cv::Mat`/AVFrame clones.
2. Reuse preprocessing/tensor buffers per inference session.
3. Compare CPU color conversion with OpenCV/FFmpeg/DVPP implementations.
4. Add model-level fair scheduling if service-gap metrics show starvation.
5. Move long output work off the detection path only where measurements show
   it affects admission latency.

### Phase 3 — Operational hardening

1. Define API authentication, body limits, path roots, and timeout policy.
2. Restore secure outbound HTTP defaults.
3. Remove global CMake build/flag side effects.
4. Add packaging and aarch64 smoke tests.
5. Rewrite the README as an operator/developer guide and maintain one current
   remediation status record.

### Phase 4 — Hardware enablement

1. Implement one Ascend vertical slice end-to-end: device context, input
   transfer, model load, inference, output decode, and cleanup.
2. Add hardware tests for synchronization, lifetime, geometry, and failure.
3. Benchmark one stream, then multi-stream scaling.
4. Only then enable automatic backend selection in production.

## 7. Suggested metrics dashboard

For each worker:

- decoded FPS and admitted/submitted FPS;
- major/minor requested FPS;
- admission lag and result age;
- mailbox overwrite count and maximum queue age;
- inference/preprocess/postprocess/annotation milliseconds;
- event-buffer bytes, raw-frame count, JPEG-frame count, and evictions;
- push queue depth and drops;
- reconnect count, current generation, and last error;
- alarm accepted, dropped, finalized, upload-failed, and re-encode-failed;
- backend state and fallback count.

For each model runtime:

- session count and utilization;
- queue wait p50/p95/p99;
- inferences per session;
- shared-inference hit rate;
- duplicate fallback count;
- per-video maximum service gap.

## 8. Review limitations

No physical Ascend NPU is available, so ACL/VDEC/DVPP/model execution,
zero-copy lifetime, synchronization, and throughput remain unverified.
Likewise, this review did not claim an 8-stream/30-minute result, ASan result,
or production API p95 result. Those are validation tasks, not conclusions
that can be inferred from static code inspection.

The worktree also contains source changes from the worker/pipeline extraction
branch. The recommendations above describe the current source shape and do
not assume that historical line references in the remediation plan are still
valid.

## 9. Bottom line

The project has already completed the difficult architectural step: it has
bounded admission, bounded mailboxes, shared inference, event buffering,
pipeline extraction, and explicit portability seams. The next maintainability
step is to make every overload, backend, timestamp, shutdown, and output
failure observable and testable. Once those contracts are stable, measured
copy reduction and fair model scheduling can improve multi-video throughput
without adding opaque concurrency.
