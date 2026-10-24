# Video Anomaly Detector

Video anomaly analysis service for live streams and offline video files. The
service decodes video with FFmpeg, admits frames according to each monitor's
major/minor detection stride, runs ONNX-based detection, tracks event state,
annotates frames, and writes alarm media.

This repository also contains portability seams for aarch64 and Ascend NPU
integration. The CPU/FFmpeg path is the validated runtime path. The Ascend
implementation is currently an integration skeleton and must not be treated
as production hardware support without the hardware validation described in
[PROJECT_STATUS.md](PROJECT_STATUS.md).

## Documentation map

| Document | Purpose |
|---|---|
| [PROJECT_STATUS.md](PROJECT_STATUS.md) | Current implementation status, validation evidence, known risks, and ownership-oriented next steps |
| [TODO.md](TODO.md) | Action checklist, grouped by validation, test gaps, code work, and housekeeping |
| [CODEBASE_REVIEW_REPORT.md](CODEBASE_REVIEW_REPORT.md) | Detailed maintainability and performance review |
| [.trae/documents/analyzer-design-remediation-plan.md](.trae/documents/analyzer-design-remediation-plan.md) | Historical seven-phase design plan and its acceptance criteria |
| [cross_compile_aarch64.md](cross_compile_aarch64.md) | aarch64 toolchain, dependency, packaging, and deployment guide |
| [DYNAMIC_ANNOTATOR_GUIDE.md](DYNAMIC_ANNOTATOR_GUIDE.md) | Dynamic ROI/legend annotation design and usage |

`PROJECT_STATUS.md` is the current status record. The remediation plan is
historical design context; unresolved acceptance items are copied into
`TODO.md` rather than being implied to be complete.

## Architecture

The executable is assembled around one `Worker` per logical `video_code`:

```text
Scheduler
  └── Worker
        ├── MonitorRegistry
        ├── StreamSession
        │     ├── AvPullStream
        │     └── AvPushStream
        ├── EventPipeline
        │     └── CombinedEventManager
        ├── EventRunner
        └── AlarmPipeline
              └── AlarmIoDispatcher

DetectorPool
  └── ModelRuntime per (model file, target task)
        └── bounded analyzer mailboxes and ONNX sessions
```

### Frame admission and detection

Detection is not run at decode FPS:

1. An idle monitor requests its next frame at `major_stride_ms`.
2. A positive major check enters verification/active processing.
3. Verification and active processing request frames at `minor_stride_ms`.
4. `EventPipeline` compares the requested timestamp with the decoded
   timestamp before cloning or submitting a frame.
5. Monitors using the same task can share one converted frame and inference
   result.
6. Live mailboxes retain one in-flight request and replace an older pending
   request with the newest frame. Offline depth is bounded by
   `offlinePipelineDepth`.

The configured stride is therefore a scheduling/admission policy, not a
promise that every decoded frame is inferred. Validate service cadence with
the per-camera metrics and the multi-stream test plan in
[PROJECT_STATUS.md](PROJECT_STATUS.md).

### Event and output flow

The event manager keeps idle pre-roll frames as JPEG data when possible and
keeps active event frames raw. Alarm output may include JPEG snapshots, HLS,
and MP4. Output work is dispatched asynchronously, but output finalization
must still be monitored; an accepted alarm is not proof that every artifact
was successfully written.

## Requirements

### Runtime/build dependencies

The exact dependency layout is deployment-specific. The normal build expects:

- CMake and Ninja;
- a C++20 compiler and standard library;
- FFmpeg development libraries;
- OpenCV;
- ONNX Runtime;
- libevent, libcurl, nlohmann-json, spdlog, and the queue/thread-pool
  dependencies configured by `CMakeLists.txt`;
- model files and runtime resources (font and alarm sound).

For aarch64, use the documented GCC 13 cross toolchain and sysroot in
[cross_compile_aarch64.md](cross_compile_aarch64.md). Confirm that the target
runtime provides the required FFmpeg codecs, HLS muxers, OpenCV libraries, and
ONNX Runtime ABI.

## Build

Configure a Debug build with tests:

```bash
cmake --preset debug
cmake --build --preset debug
```

Configure/build Release:

```bash
cmake --preset release
cmake --build --preset release
```

Run the enabled tests from the build directory:

```bash
cd build
ctest --output-on-failure
```

The repository also contains manual tests that are disabled by default because
they require local media, models, or an external stream. Do not interpret a
green enabled CTest run as proof that the full runtime has passed the
8-stream, reconnect, alarm, or package acceptance gates.

### Sanitizers

Use a separate build directory:

```bash
cmake -S . -B build-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DANALYZER_SANITIZE=address
cmake --build build-asan
cd build-asan
ctest --output-on-failure
```

For ThreadSanitizer, replace `address` with `thread`. ONNX Runtime may be
excluded from TSan coverage when it is not instrumented; use the fake/session
tests for concurrency contracts.

## Configuration

Copy `conf/config.json` and change paths for the deployment. Important groups:

| Group | Examples | Meaning |
|---|---|---|
| `server` | `admin.host`, `media.http`, `analyzer.host` | Service endpoints used by the analyzer |
| `application` | `threadPoolSize`, `offlineThreads`, `alarmThreads` | Control/media/alarm execution capacity and timeouts |
| `performance` | `inferenceWorkersPerModel`, `offlinePipelineDepth` | Inference parallelism and bounded offline admission |
| `performance` | `eventBufferBudgetMb`, `pushQueueMaxFrames` | Per-worker event memory and push queue limits |
| `performance` | `streamReconnectMaxAttempts`, `streamReconnectBackoff*` | Reconnect budget and exponential backoff |
| `performance` | `preloadModels` | Startup model preload list; an empty list disables preloading |
| `DLModel` | `dir`, `acceleratorInference` | Model directory and backend selection |
| `uploadDir` | filesystem path | Alarm/media output root |
| `display` | font and legend settings | Annotation resources and layout |
| `hls_config` | segment/playlist settings | HLS output behavior |

Configuration values are clamped in `Config`. Known numeric, boolean, array,
and selected deployment fields are type-checked during startup. Malformed
configuration is reported in `Config::validation_errors` and makes
`Config::valid` false; callers must refuse startup rather than silently
continuing with defaults. Inspect startup logs and the effective configuration
before production use.

Do not commit credentials or private service tokens. The sample configuration
contains deployment-specific paths and should be replaced for another host.

## Runtime operations

### Startup checklist

1. Verify model files exist under `DLModel.dir`.
2. Verify font, alarm sound, upload, and offline-video paths.
3. Verify FFmpeg can load the required input and output codecs.
4. Start with CPU inference unless the Ascend backend has passed its hardware
   gate.
5. Confirm worker/backend state and metrics before adding many streams.
6. Check disk capacity for HLS segments, MP4 files, JPEGs, and offline input.

### Shutdown/reconnect expectations

Stopping a worker signals stream and push loops, flushes pending event state,
and removes the worker from the scheduler. Network reads can still be bounded
by FFmpeg/network timeouts; shutdown latency must be measured in deployment.
Reconnects are bounded by the configured attempt count and backoff.

Reconnect at a different resolution should rebuild conversion and annotation
state. This behavior is a required integration test, not a claim that every
codec/source combination has been certified.

### Metrics and health

Use `GET /api/metrics` for current counters and queue information. Important
signals include decoded/submitted/skipped/dropped frames, inference timing,
reconnects, push drops, pending/dropped alarm jobs, upload failures, and
re-encode failures.

For capacity work, also record requested-versus-admitted timestamps, result
age, queue wait, per-video service gaps, event-buffer bytes, and backend
readiness. These measurements are required before tuning copies or adding
hardware acceleration.

## Packaging

Install into a staging directory using the CMake install rules, then package
the staging directory:

```bash
cmake --install build --prefix ./staging
./pack.sh ./staging
```

Use `./pack.sh --help` for inclusion and stripping options. The default packer
includes the binary, shared libraries, resources, models, tests, and supported
configuration assets. Review paths embedded in `conf/config.json` before
deploying the generated installer.

For aarch64, follow [cross_compile_aarch64.md](cross_compile_aarch64.md) and
use the target strip/runtime libraries rather than host binaries.

## Development rules

- Keep blocking work out of the libevent callback thread.
- Preserve bounded queues and make drop policy explicit.
- Do not hold registry locks while waiting for inference or doing media I/O.
- Keep backend failure distinct from a valid empty detection result.
- Add a focused unit test for lifecycle, queue, timestamp, and output policy
  changes.
- Update `PROJECT_STATUS.md` and `TODO.md` when validation state changes.
- Keep historical design decisions in the remediation plan; do not silently
  mark unverified integration work complete.

## Known limitations

- No physical Ascend NPU validation has been performed.
- The required 8-stream/30-minute, reconnect-resolution, RSS, API-p95,
  alarm-integrity, ASan, and package acceptance runs are outstanding unless
  explicitly recorded in [PROJECT_STATUS.md](PROJECT_STATUS.md).
- Alarm output failure propagation and critical/best-effort queue semantics
  remain open engineering work.
- API authentication, request-size limits, filesystem trust boundaries, and
  secure outbound HTTP defaults require deployment hardening.
