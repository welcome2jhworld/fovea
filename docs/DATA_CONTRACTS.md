# Data contracts

IDs are UUIDv4 strings issued by the application. Times: `*_utc_ms` are UTC
milliseconds, `*_mono_ns` are local monotonic nanoseconds (valid only within one
core process run), `*_pts_ns` are media times within one StreamSession.

Only entities used by the current milestone are implemented in SQLite. The rest
are listed so names stay stable.

## Implemented in M1

### Camera
`id, name, group_name, kind (rtsp|file), main_url, sub_url, transport (tcp|udp),
timeout_ms, jitter_ms, analytics_enabled, record_enabled, enabled, created_utc_ms,
updated_utc_ms`. Credentials live in the secret store keyed by camera id and are
never returned by the API or written to logs.

### StreamSession
`id, camera_id, started_mono_ns, started_utc_ms, first_pts_ns, ended_utc_ms,
end_reason (eos|error|pts_backwards|stopped|shutdown), codec, width, height, fps,
transport, capture_clock (none|rtcp)`.

### RecordingSegment
`id, camera_id, session_id, path, state (recording|finalized|damaged|deleted),
start_pts_ns, end_pts_ns, start_utc_ms, end_utc_ms, bytes, keyframe_start,
created_utc_ms, finalized_utc_ms`.

### ReceiveGap
`id, camera_id, session_id, from_utc_ms, to_utc_ms, reason (timeout|reconnect|eos)`.

### Setting
`key, value_json`.

## M3 (schema version 2)

Zone/ZoneRevision, Rule/RuleRevision, RuleEvaluation, Event, EventReview,
AlertDelivery, EvidenceRef, AnalysisCoverage. Columns and lifecycles are in
`docs/M3_DESIGN.md`.

## Later milestones (names reserved)

ObservationWindow, IndexJob, EmbeddingRecord, Observation, SearchSession.

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
