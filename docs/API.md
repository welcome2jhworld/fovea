# Core local API (v1)

Bind: `127.0.0.1` on a random free port. Discovery file: `<data>/core.json`
`{ "port": 43123, "pid": 12345, "started_utc_ms": ..., "version": "0.1.0" }`.
Token file: `<data>/core.token` (0600). On Windows the data directory lives under `%LOCALAPPDATA%`, whose inherited ACL already limits access to the user, SYSTEM and administrators; Qt file permissions cannot express ACLs, so the POSIX mode checks in the tests are skipped there. Every request carries
`Authorization: Bearer <token>`; missing or wrong token returns 401.
Bodies and responses are JSON. Errors: `{ "error": { "code": "...", "message": "..." } }`.

| Method | Path | Purpose |
| --- | --- | --- |
| GET | /v1/health | `{status, version, uptime_ms, data_dir}` |
| GET | /v1/cameras | list Camera + CameraStatus |
| POST | /v1/cameras | create Camera (body: Camera fields + optional `username`, `password`) |
| GET | /v1/cameras/{id} | Camera + status |
| PUT | /v1/cameras/{id} | update; fields not in the body keep their values (restarts the pipeline if connection fields changed; `retention_days` or `max_bytes` start a retention pass) |
| DELETE | /v1/cameras/{id} | delete camera; segments stay on disk and are marked `deleted` in metadata, its evidence refs become `deleted` (thumbnails removed), its rules are deleted (open events clear with note `camera_deleted`) and its zones too |
| POST | /v1/cameras/{id}/enable, /disable | start / stop pipeline |
| POST | /v1/cameras/test | probe a connection for up to `timeout_ms`; returns codec, width, height, fps, bitrate_kbps, handshake_ms, or an error |
| GET | /v1/cameras/{id}/sessions | StreamSessions, newest first |
| GET | /v1/cameras/{id}/segments?from_utc_ms&to_utc_ms&limit | RecordingSegments |
| GET | /v1/cameras/{id}/gaps?from_utc_ms&to_utc_ms | ReceiveGaps |
| GET | /v1/segments/{id} | one segment |
| POST | /v1/playback | `{segment_id}` or `{camera_id, at_utc_ms}` -> PlaybackChannel |
| GET | /v1/playback/{id} | state: position_pts_ns, duration, playing, rate, frame ring |
| POST | /v1/playback/{id}/play, /pause, /seek `{pts_ns}`, /rate `{rate}` | control |
| DELETE | /v1/playback/{id} | close channel |
| GET | /v1/metrics | per camera + process metrics |
| GET | /v1/storage | free space, disk floor, recorded bytes per camera, last retention pass |
| POST | /v1/service/shutdown | stop the core (console asks first) |

CameraStatus:
```
{ camera_id, state: connecting|online|reconnecting|offline|disabled,
  session_id, since_utc_ms, last_frame_recv_mono_ns, last_frame_age_ms,
  stale, codec, width, height, fps_new, latency_ms: {p50, p95, p99},
  drops, queue_depth, reconnects, recording: recording|paused_disk|disabled|error,
  current_segment_id, frame_ring: { name, slots, slot_bytes, max_width, max_height, format } }
```
`last_frame_age_ms` measures from the last frame received in any session of the camera during this core run, so it stays meaningful while reconnecting; -1 means no frame since the core started. `fps_new` counts distinct new frames only; repeated display of a held frame is
never included.

Camera (as returned, and accepted by POST/PUT):
```
{ id, code, name, group_name, kind: rtsp|file, main_url, sub_url, transport: tcp|udp,
  timeout_ms, jitter_ms, segment_seconds (5..600), analytics_enabled, record_enabled, enabled,
  retention_days (1..3650, default 7), max_bytes (0 = no limit, default 0),
  created_utc_ms, updated_utc_ms }
```

Metrics (`GET /v1/metrics`):
```
{ cameras: [{ camera_id, state, session_id, fps_new, latency_ms, drops, queue_depth, queue_max,
              reconnects, recording, stale, ring_bytes, bytes_received, bitrate_kbps, frames,
              ring_write_errors, open_segments, jitterbuffer }],
  process: { rss_bytes, cpu_percent, cpu_time_ms, ring_bytes, pipelines },
  playback_channels, uptime_ms }
```
A camera without a running pipeline carries only `camera_id` and `state`.
`queue_max` is the bound of the view queue (leaky; `queue_depth` never exceeds it).
`cpu_percent` is process CPU time (user + system, all threads) over the interval
since the previous metrics request, 100 = one core; a request less than a second
after the previous one repeats the previous value. `cpu_time_ms` is the
cumulative CPU time, so a caller can average over any interval it chooses.

Storage (`GET /v1/storage`):
```
{ free_bytes, floor_bytes, headroom_bytes,
  per_camera: [{ camera_id, bytes, segments, oldest_utc_ms, retention_days, max_bytes, age_limit_ms }],
  last_run_utc_ms, deleted_last_run, deleted_bytes_last_run, held_last_run, floor_unreachable,
  retention_seconds_override }
```
`bytes`, `segments` and `oldest_utc_ms` cover finalized, damaged and recording
segments (a recording segment counts 0 bytes until it is finalized).

Retention runs in the core when it starts, every 60 s, within 5 s of free space
dropping below `floor_bytes + headroom_bytes`, and after a PUT that changes
retention fields. It deletes finalized and damaged segments only: those whose
content ended more than `retention_days` ago, the oldest of a camera whose
bytes exceed `max_bytes`, and, while free space is below floor plus headroom,
the oldest of any camera until the headroom is restored. That last step is
skipped (and `floor_unreachable` is true) when deleting every deletable
segment could not reach the target. A segment with an active evidence hold is
never deleted (`held_last_run` counts the ones skipped). Deleted segments keep
their row with state `deleted`; each deletion is written to the audit log
(actor `retention`, action `segment.delete`).

Environment: `FOVEA_MIN_FREE_MB` overrides `--min-free-mb`. `FOVEA_RETENTION_SECONDS`
is for tests only: when set, every camera's age limit is that many seconds
instead of `retention_days` (reported as `age_limit_ms` and
`retention_seconds_override`).

## Analytics (M3)

Schema and lifecycles are in `docs/M3_DESIGN.md`. Every write returns the stored
object; validation failures return 400 with `error.code` `invalid_zone`,
`invalid_rule`, `invalid_label` or `bad_json`, unknown ids return 404.

| Method | Path | Purpose |
| --- | --- | --- |
| GET | /v1/zones?camera_id= | zones with their current revision (all cameras without `camera_id`) |
| POST | /v1/zones | create `{camera_id, name, points, anchor, ref_width, ref_height}` |
| PUT | /v1/zones/{id} | new revision (fields not in the body keep their values); rules on the zone move to it with a new rule revision |
| DELETE | /v1/zones/{id} | soft delete; enabled rules on it get a disabled revision (open events clear) |
| GET | /v1/rules | rules with their current revision and `runtime [{camera_id, condition, quality, open_event_id}]` |
| POST | /v1/rules | create (body: `name` plus RuleRevision fields) |
| PUT | /v1/rules/{id} | new revision (fields not in the body keep their values); an open event stays open |
| DELETE | /v1/rules/{id} | soft delete; an open event is cleared (evaluation note `rule_deleted`) |
| POST | /v1/rules/{id}/enable, /disable | new revision with `enabled` set |
| GET | /v1/events?state=&camera_id=&from_utc_ms=&to_utc_ms=&limit= | newest first; `state` is an operator state, `unresolved`, or a condition; `limit` 1..1000 (100) |
| GET | /v1/events/counts | `{unresolved, acknowledged, dismissed}` over all events: `unresolved` and `acknowledged` are those operator states without a latest review `false_alarm`; `dismissed` holds resolved events and false alarms |
| GET | /v1/events/{id} | event plus `evaluations` from the start of the occupancy to the clear |
| POST | /v1/events/{id}/acknowledge, /resolve | optional body `{operator}`; idempotent; resolve starts the 30-day evidence retention |
| POST | /v1/events/{id}/review | `{label: confirmed|false_alarm|undecided, note, operator}` (note up to 2000 characters) |
| GET | /v1/alerts/pending?console_id= | pending and failed deliveries of unresolved events, oldest first, each with an `event` summary |
| POST | /v1/alerts/{id}/delivered | `{console_id}`; idempotent |
| GET | /v1/cameras/{id}/detections/latest | newest known detection frame; 404 `analytics_off` or `no_detections` |
| GET | /v1/analysis | worker supervisor state and per-camera scheduler statistics |
| GET | /v1/evidence/{id}/thumbnail | trigger frame JPEG of the evidence ref; 404 `evidence_deleted` once the ref is deleted (its file is removed) |

Validation: `points` 3..32 `[x, y]` pairs inside [0, 1]; `anchor` foot|center;
`ref_width`/`ref_height` both 0 (no frame size check) or both 1..16384 (the
size of the analysed frame, which is the camera's frame ring size);
`camera_id` names an existing camera and cannot change on PUT; a rule's
`zone_id` names a zone of the same camera, and `zone_revision` is always set by
the core to that zone's current revision. `dwell_ns` 1..3600 s,
`max_observation_gap_ns` 0.5..60 s, `clear_after_ns` 0..3600 s, `rearm_ns`
0..86400 s, `result_ttl_ns` 0.5..60 s, `evidence_pre_ns`/`evidence_post_ns`
0..600 s, `min_confidence` 0..1, `time_zone` a valid IANA id, `target_class`
person|car|truck|bus|motorcycle|bicycle, `severity` critical|review|info,
`vlm_role` none (describe and verify are M5), `schedule` up to 16 windows with
`days` 1..127 (bit 0 = Monday), `start_minute` 0..1439, `end_minute` 1..1440
(end <= start wraps past midnight; an empty schedule means always).

Rule (list entry, POST/PUT response):
```
{ id, name, revision, enabled, camera_id, zone_id, zone_revision,
  schedule: [{days, start_minute, end_minute}], time_zone, target_class, min_confidence,
  dwell_ns, max_observation_gap_ns, clear_after_ns, rearm_ns, result_ttl_ns,
  evidence_pre_ns, evidence_post_ns, vlm_role, severity, actions: {sound, pop_to_main_view},
  created_utc_ms, revision_utc_ms, runtime: [{camera_id, condition, quality, open_event_id}] }
```

Event:
```
{ id, rule_id, rule_revision, camera_id, session_id, severity, condition: active|clearing|cleared,
  operator_state: new|acknowledged|resolved, opened_utc_ms, opened_pts_ns, trigger_pts_ns,
  cleared_utc_ms, title, detail, late,
  review: {id, event_id, label, note, operator, utc_ms} | null,
  evidence: {id, event_id, camera_id, from_utc_ms, to_utc_ms, state, reason, segment_ids,
             has_thumbnail, updated_utc_ms} | null,
  deliveries: [{id, event_id, channel: console|sound, state: pending|delivered|failed,
                attempts, last_error, created_utc_ms, delivered_utc_ms}] }
```
`opened_utc_ms` is the session-clock time of the trigger frame, `opened_pts_ns`
the first in-zone sighting of the occupancy and `trigger_pts_ns` the trigger
frame. `GET /v1/events/{id}` adds `evaluations [{id, utc_ms, rule_revision,
transition, before, after, quality, dwell_ns, track_ids, event_id, note,
session_id, generation, window_start_pts_ns, window_end_pts_ns}]`.

Detections (`/detections/latest`):
```
{ camera_id, session_id, frame_id, pts_ns, utc_ms, width, height, generation,
  detections: [{track_id | null, cls, confidence, bbox: [x1,y1,x2,y2], anchor_foot: [x,y], anchor_center: [x,y]}] }
```
Coordinates are normalized; `width`/`height` are the analysed frame (frame ring) size.

Analysis (`GET /v1/analysis`; `/v1/metrics` carries the same camera object as
`analysis` on each analysed camera and the worker object as `worker`):
```
{ worker: { state: starting|ready|failed|stopped|disabled, source: env|app|dev, pid, port, restarts,
            last_error, detector_model, detector_state, detector_device, detector_load_ms,
            detector_load_error, detector_busy_ms, detector_turnaround_ms: {jobs, p50, p95} },
  cameras: [{ camera_id, analyzing, session_id, generation, detect_fps_target, achieved_fps,
              frames_sent, frames_known, frames_unknown, unknown_ratio, skips,
              in_flight: idle|encoding|ready|posted, quality, last_unknown_reason, last_result_utc_ms,
              turnaround_ms: {p50, p95, samples}, request_ms: {p50, p95, samples} }] }
```
`achieved_fps` counts known results over the last 10 s. `turnaround_ms` runs
from taking the frame to the end of its request (JPEG encode included),
`request_ms` covers the HTTP request only; both count every posted request
that finished, whether with detections, an error reply or the 3 s timeout
(counted as its elapsed time), and exclude the first job after each worker
start. `detector_busy_ms` is how long the worker's detector lane has been
inside its current job (0 when idle); above 30 s, or with `detector_state`
`failed` in 3 consecutive health checks, the core restarts the worker.
Detector jobs carry `threshold` (the lowest `min_confidence` of the camera's
enabled rules within [0.1, 0.3]) and `max_gap_ns` (their longest
`max_observation_gap_ns`, which the worker's tracker bridges without new ids). `last_unknown_reason` is one of
`worker_unavailable` (no worker found), `worker_starting|failed|stopped`,
`detector_loading|failed`, `timeout`, `worker_error: ...`,
`worker_http_<status>`, `worker_status_<status>`, `contract_violation: ...`,
`encode_failed: ...`.

Environment: `FOVEA_WORKER_CMD` is a command line that runs the worker CLI
(for example `"worker/.venv/bin/python" -m fovea_worker.cli`); the core appends
`serve --backend dry --detector rfdetr --port 0 --token-file <data>/worker.token
--info-file <data>/worker.json --warmup --exit-on-stdin-eof`. Without it the
core looks for `<app dir>/worker/.venv/bin/fovea-worker`
(`Scripts\fovea-worker.exe` on Windows), then for a development tree above the
executable. Worker output goes to `<data>/logs/worker.log`.
