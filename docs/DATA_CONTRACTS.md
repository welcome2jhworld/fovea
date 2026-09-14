# Data contracts

IDs are UUIDv4 strings issued by the application. Times: `*_utc_ms` are UTC
milliseconds, `*_mono_ns` are local monotonic nanoseconds (valid only within one
core process run), `*_pts_ns` are media times within one StreamSession.

Only entities used by the current milestone are implemented in SQLite. The rest
are listed so names stay stable.

## Implemented in M1 and M2

### Camera
`id, code, name, group_name, kind (rtsp|file), main_url, sub_url, transport (tcp|udp),
timeout_ms, jitter_ms, segment_seconds, analytics_enabled, record_enabled, enabled,
retention_days (default 7), max_bytes (0 = unlimited), created_utc_ms,
updated_utc_ms`. Credentials live in the secret store keyed by camera id and
are never returned by the API or written to logs.

### StreamSession
`id, camera_id, started_mono_ns, started_utc_ms, first_pts_ns, ended_utc_ms,
end_reason (eos|error|pts_backwards|stopped|shutdown), codec, width, height, fps,
transport, capture_clock (none|rtcp|imported)`. An imported session (M4) has
`first_pts_ns` 0 and `started_utc_ms` the time given for media time zero of
the file.

### RecordingSegment
`id, camera_id, session_id, path, state (recording|finalized|damaged|deleted),
start_pts_ns, end_pts_ns, start_utc_ms, end_utc_ms, bytes, keyframe_start,
created_utc_ms, finalized_utc_ms, deleted_utc_ms, delete_reason, purged_utc_ms`.

`delete_reason` is empty for segments of a deleted camera (their files stay on
disk), `age | max_bytes | disk_floor` for retention and `import_failed` for the
segments of an import that failed or was interrupted (M4). Retention commits the
state change first and removes the file afterwards; `purged_utc_ms` is set once
the file is gone (or was outside the recordings directory and left alone). A
row with a reason and `purged_utc_ms = 0` is a removal still owed, finished by
the next retention pass or at startup.

### EvidenceHold (schema version 2)
`segment_id, until_utc_ms, reason`. Any number per segment. A hold is active
while `until_utc_ms` is 0 or in the future; retention never deletes a segment
with an active hold. M3 writes these for segments covered by evidence refs.

### ReceiveGap
`id, camera_id, session_id, from_utc_ms, to_utc_ms, reason (timeout|reconnect|eos)`.

### Setting
`key, value_json`.

## M3

Schema version 2 (M2) added the camera retention columns, the segment deletion
columns and `evidence_holds`; schema version 3 adds the M3 tables below. Zone/ZoneRevision, Rule/RuleRevision, RuleEvaluation, Event, EventReview,
AlertDelivery, EvidenceRef, AnalysisCoverage. Columns and lifecycles are in
`docs/M3_DESIGN.md`.

## M4 (schema version 4)

Columns and lifecycles are in `docs/M4_DESIGN.md`; API shapes in `docs/API.md`.

- Camera gains `index_enabled` (default 1).
- `index_versions`: `hash (PK), name, model_id, model_revision, dims, dtype,
  sample_interval_ms, descriptor_json, created_utc_ms, deleted_utc_ms`.
- `index_jobs`: `id (integer), segment_id, camera_id, index_version,
  state (queued|running|done|failed|skipped), reason, attempts, generation,
  sample_interval_ms, frames_expected, frames_indexed, next_attempt_utc_ms,
  compute_ms, footage_ms, created_utc_ms, updated_utc_ms`, unique
  `(segment_id, index_version)`.
- `embedding_records`: `id (integer), index_version, camera_id, session_id,
  segment_id, job_id, generation, pts_ns, utc_ms, vector_file, vector_offset,
  thumbnail_path, deleted`. The vector lives in
  `<data>/index/<index_version>/<camera_id>/<yyyymmdd>[-<n>].vec` at
  `vector_offset`; a deleted row loses its thumbnail path.
- `search_sessions`: `id, query, filters_json, index_version, model,
  created_utc_ms, stats_json, results_json`.
- `imports`: `id, camera_id, session_id, source_path, start_utc_ms,
  state (queued|running|done|failed), error, codec, duration_ns, progress,
  segments, bytes, created_utc_ms, started_utc_ms, finished_utc_ms`.
- Settings: `search.active_index_version`, `search.previous_index_version`
  (JSON strings), `index.sample_interval_ms` (number, 200..60000, default 1000).

## Later milestones (names reserved)

ObservationWindow, Observation.

## Rule and event states (M3, fixed now)

- Condition: `inactive | pending | active | clearing`
- Observation quality: `known | unknown`
- Operator: `new | acknowledged | resolved`
- Review label: `confirmed | false_alarm | undecided`
- Alert delivery: `pending | delivered | failed`
- Evidence availability: `pending | available | partial | deleted`

Timers advance only on `known` observations measured in media time of the
tracked object; `unknown` freezes them. Results carry camera, session, rule
revision, observation window and job generation; stale results cannot roll back
current state.
