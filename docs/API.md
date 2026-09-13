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
| PUT | /v1/cameras/{id} | update (restarts the pipeline if connection fields changed) |
| DELETE | /v1/cameras/{id} | delete camera; segments stay on disk and are marked `deleted` in metadata |
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
| POST | /v1/service/shutdown | stop the core (console asks first) |

CameraStatus:
```
{ camera_id, state: connecting|online|reconnecting|offline|disabled,
  session_id, since_utc_ms, last_frame_recv_mono_ns, last_frame_age_ms,
  stale, codec, width, height, fps_new, latency_ms: {p50, p95, p99},
  drops, queue_depth, reconnects, recording: recording|paused_disk|disabled|error,
  current_segment_id, frame_ring: { name, slots, slot_bytes, max_width, max_height, format } }
```
`fps_new` counts distinct new frames only; repeated display of a held frame is
never included.
