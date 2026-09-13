# Fovea architecture

Fovea is a local video monitoring application: connect existing CCTV cameras,
watch and record, search recordings in natural language, and raise alerts with
evidence clips when user rules fire. Everything runs on one machine; no cloud.

## Processes

Three executables. Modules do not get their own services.

| Process | Binary | Role |
| --- | --- | --- |
| Desktop Console | `fovea` | Qt Widgets UI. Renders frames, edits settings, reviews events, searches. Never decodes video, never runs models, never touches SQLite directly. |
| Headless Core | `fovea-core` | Camera connections (GStreamer), recording, frame distribution, metadata (SQLite), index jobs, search, rule engine, alert dispatch. Keeps running when the console closes. |
| Model Worker | `fovea-worker` (Python) | Detection, embedding, VLM inference. Separate process from the first inference integration so a model crash cannot stop recording. |

Console -> Core: loopback HTTP JSON API (`127.0.0.1`, random port, bearer token
stored in a 0600 file inside the data directory). Core -> Worker: loopback HTTP
JSON API with the same token scheme, plus frame payloads referenced by shared
memory or file paths, never embedded base64 at high rate.

Frames Core -> Console: one shared-memory ring per channel (live camera or
playback session). Bounded: fixed slot count and slot size, single writer,
any number of readers, seqlock per slot. Readers copy out; they never block the
writer. See "Frame ring" below.

The console starts `fovea-core` if it is not running and leaves it running on
exit unless the operator chooses "Stop service".

## Core internals

```
CameraManager
  +-- one CameraPipeline per enabled camera (GStreamer)
        source (rtspsrc | filesrc+demux) -> depay/parse -> tee
          +-- record branch: queue -> splitmuxsink (no re-encode, keyframe cuts)
          +-- view branch: queue(leaky) -> decode -> convert/scale -> appsink -> FrameRing
          +-- (M3) analysis branch: queue(leaky) -> sampled frames -> worker
PlaybackManager: file -> decode -> FrameRing (same widget path as live)
Store: SQLite (WAL), owns all metadata writes
ApiServer: QtHttpServer routes under /v1
Metrics: per camera fps, latency, drops, queue depth, reconnects, disk
```

One GLib main loop thread services all pipeline buses. Decoding runs inside
GStreamer streaming threads. SQLite writes happen on a single store thread via
queued calls. Nothing heavy runs on the API thread.

## Time model

Three clocks, kept apart in every record:

- `pts_ns`: media presentation time from the stream (GStreamer running time
  within a StreamSession).
- `recv_mono_ns`: local monotonic clock when the compressed packet arrived
  (measured before decoding). Processing timestamps use the same clock. It is
  C++ `std::chrono::steady_clock`; Python code must use
  `fovea_worker.clock.mono_ns()` because on macOS `time.monotonic` excludes
  sleep time and differs from `steady_clock` by the total sleep duration.
- `capture_utc_ms`: sender capture time in UTC, only when it can be verified
  (RTCP sender reports / reference timestamp meta). Zero means unknown, and the
  UI then reports "delay since receive" only.

Every connect or reconnect creates a new `StreamSession` with its own
pts -> UTC mapping (`session_start_utc_ms`, `first_pts_ns`). Segments, frames
and (later) observations always carry the session id, so two sessions are never
stitched into one timeline by accident. A PTS that goes backwards inside a
session closes the session and opens a new one.

## Frame ring

Shared memory segment per channel: header + N slots (default 3) of a fixed
maximum size (1280x720 BGRA, 3.7 MB). Slot header: `seq`, `session_id`,
`pts_ns`, `recv_mono_ns`, `capture_utc_ms`, `width`, `height`, `stride`,
`format`, `flags`. Writer increments `seq` to odd before writing and to even
after. Readers pick the newest even slot, copy, and re-check `seq`; a mismatch
is a torn read and is retried. Larger sources are scaled down in the core to fit
the slot; the wall never needs more than 720p per tile. Memory is bounded by
`channels * slots * slot_bytes` and is reported in metrics.

Copying is allowed now. Zero-copy (GPU textures) is deferred until profiling
shows the copy is the bottleneck.

## Recording

The compressed elementary stream is written without re-encoding by
`splitmuxsink`, cut at keyframes into fixed-duration segments. Container choice
and crash behaviour are recorded in `docs/DECISIONS.md` after the M0 spike.
Segment states: `recording` -> `finalized` | `damaged`. On startup, every
segment left in `recording` is probed; unreadable files are marked `damaged`,
readable ones `finalized` with the measured duration. Receive gaps (no packets
for longer than the configured threshold) are stored per session and shown in
the UI.

Disk: the core refuses to start new segments below a free-space floor, marks
the camera `recording_paused_disk`, and shows it. Retention (M2) is separate
for ordinary recordings and for event evidence.

## Console

Qt Widgets with QSS, design tokens from `docs/design/handoff-map.md` (the design
handoff source files are kept out of the repository). Display
code never contains model or storage logic. Screens: Monitor (M1), Camera
settings (M1), Search (M4), Alerts & Analytics (M3/M5), Model Train (later).
Screens that have no real data render an explicit "not implemented" state.

## Model worker (M0 protocol, M3+ use)

`POST /v1/jobs` with `{kind: detect|embed|vlm_clip, inputs: [...frame refs with
frame_id, pts_ns, capture_utc_ms...], clip_range, gaps, limits, deadline_ms}`.
Result carries `observations`, `evidence` (frame ids + ranges that must exist in
the input), `uncertainty`, `status`. The core validates every evidence id and
range against what it sent before storing anything.
