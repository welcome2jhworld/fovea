# M3 design: first rule, events, evidence, alerts

Scope: "during the operating schedule, alert when a person stays inside a zone
for 10 s continuously", end to end with real detection and tracking, event and
alert state, evidence clips, operator review. Natural-language rules and VLM
roles other than `none` are M5.

## Runtime path

```
CameraPipeline view branch (decoded BGRA, <=1280x720)
  -> AnalysisTap: keeps the newest frame per camera (1 slot, overwrite; the appsink
     callback copies at most one frame per 100 ms, under the tap's mutex)
AnalysisScheduler (core main thread, 1 timer)
  every 1/detect_fps per analytics-enabled camera, round robin:
    take newest frame not yet sent -> JPEG (quality 85, <=960 px wide; GStreamer
       jpegenc through gst_video_convert_sample on a 2-thread pool, off the main thread)
    -> spool file <data>/spool/<camera>/<frame_id>.jpg
    -> POST worker /v1/jobs {kind: detect_frames, frames: [1 frame], camera_id, session_id, generation,
                             threshold, max_gap_ns}
       one in-flight request per camera; a camera whose request is still open is skipped
       (latest frame wins, no queue growth); global in-flight cap = worker detector lane (1)
  <- DetectionFrame -> ObservationFrame{quality known, tracks}
     failure/timeout/worker down/detector loading/contract violation
       -> ObservationFrame{quality unknown} (every taken frame yields exactly one result)
  -> RuleEngine: one RuleEvaluator per (rule revision, camera)
  -> transitions persisted (RuleEvaluation rows only on transitions)
  -> EventService: Triggered opens Event + EvidenceRef + AlertDelivery rows
SpoolJanitor: deletes spool files once the result is processed, and (every 5 s) anything older than 30 s
```

Defaults (per camera, stored in `settings` key `analysis.<camera_id>` when overridden):
`detect_fps = 2` (the evaluator's `maxObservationGapNs` default 1.5 s requires
at least one frame per 1.5 s; the scheduler warns when the achieved rate falls
below 1/maxObservationGap), JPEG width 960, request timeout 3 s, result TTL 5 s.

Measured budget on the development Mac (RF-DETR Nano on MPS): 40 ms per frame,
so a serial detector lane serves about 25 frames/s. N cameras x 2 fps x 0.04 s
= 0.08 N; 4 cameras = 0.32 load. The scheduler reports achieved fps, queue
skips and p95 turnaround per camera in `/v1/metrics`.

### Time fields on a detection

`frame_id` (core-issued), `pts_ns` (session running time of the decoded frame),
`recv_mono_ns` (packet receive time on `steady_clock`), `utc_ms` from the
session clock. The evaluator measures dwell with `pts_ns` only. TTL is
`now_mono - recv_mono_ns`. `generation` is a per-camera counter incremented on
every rule-set change and session change, and when analysis of the camera
starts (it never goes back, so a camera whose analytics is switched off and on
again is not stuck behind its evaluators' last generation); results with an
older generation are stale. The job's `deadline_ms` is 2500, below the core's 3 s request timeout.
Before each result the rule engine raises every evaluator of the camera to the
camera's current generation, so a result from an older one is reported `stale`
by the evaluator (note `generation X below Y`) and, like a result dropped for
TTL, its media span is frozen rather than counted towards clearing or rearm.
With one request per camera the only such results are the one in flight
during a session or rule-set change. Frames carry the analysed frame's size
(the frame ring size, which the console also draws zones on), not the JPEG's.

The job's `threshold` is the lowest `min_confidence` among the camera's enabled
rules, within [0.1, 0.3] (the detector default 0.3 when no rule names the
camera); the worker reports detections from it and lets any of them start a
track (ByteTrack's own activation line is 0.1 above its activation threshold).
`max_gap_ns` is the longest `max_observation_gap_ns` among those rules: the
worker keeps track ids across pts gaps up to it (or its lost-track window when
longer), so a rule that tolerates a 5 s gap does not restart dwell on a new id
after a 2.5 s one.

## Worker supervision

`WorkerSupervisor` in the core starts `fovea-worker serve` as a child process:

- Binary discovery: env `FOVEA_WORKER_CMD` (a command line that runs the CLI;
  the serve arguments are appended), else `<app dir>/worker/.venv/bin/fovea-worker`
  (`Scripts\fovea-worker.exe` on Windows), else a dev tree found by walking up
  from the executable: `worker/.venv/bin/python -m fovea_worker.cli`.
- Arguments: `serve --backend dry --detector rfdetr --port 0 --token-file
  <data>/worker.token --info-file <data>/worker.json --warmup
  --exit-on-stdin-eof`. A fresh token is written (0600) on every start. The
  core reads the port from the info file. The core keeps the child's stdin
  open, so a core killed without cleanup still stops its worker. Output goes to
  `<data>/logs/worker.log`.
- Health: GET `/v1/health` every 2 s. States `starting | ready | failed |
  stopped | disabled` (`disabled`: no worker was found; every analysed frame is
  then unknown with reason `worker_unavailable`). `ready` means the worker
  answers health; the scheduler also waits for `detector_state` `ready` (or
  `unloaded`) and reports `detector_loading` otherwise. On exit, 3 failed
  health checks, a detector that reports `failed` in 3 consecutive checks (a
  failed load is only retried by a job, and the scheduler sends none while the
  detector is failed), or a detector lane inside one job for more than 30 s
  (`detector_busy_ms` in the worker's health; far above the 2.5 s job deadline
  and above a model rebuild on CPU after an MPS failure): kill, restart with
  backoff 1..30 s (reset after 30 s ready with a detector that has not failed);
  frames taken meanwhile are unknown (`worker_failed`, `worker_starting`).
- The child inherits no descriptor but stdin, stdout and stderr
  (`QProcess::UnixProcessFlag::CloseFileDescriptors`; Windows handles are not
  inheritable by default), so a restarted worker never pins a recording file
  that retention deletes.
- The worker is a child of the core only; closing the console does not affect it.
- The detector model loads lazily on the first job; the first job may take
  10 s and is excluded from turnaround p95.

## Storage (schema version 3)

Schema version 2 was taken by M2 (retention columns and `evidence_holds`), so
these tables are version 3. Deviations from the first draft: `evidence_refs`
has a `reason` column (the API reports why a ref is partial or deleted);
`rule_evaluations.id` and `analysis_coverage.id` are integer autoincrement keys
(append-only logs, like `audit_log`); `evidence_holds` gets a unique index on
`(segment_id, reason)` (the migration drops duplicates first) and holds of an
event use reason `event:<event_id>`.

| Table | Columns |
| --- | --- |
| `zones` | `id, camera_id, name, created_utc_ms, deleted_utc_ms` |
| `zone_revisions` | `zone_id, revision, points_json (normalized [[x,y],...]), anchor (foot/center), ref_width, ref_height, created_utc_ms` PK(zone_id, revision) |
| `rules` | `id, name, created_utc_ms, deleted_utc_ms, current_revision` |
| `rule_revisions` | `rule_id, revision, json (full RuleRevision incl. camera_id, zone_id, zone_revision, schedule, tz, class, confidence, dwell/gap/clear/rearm/ttl/evidence pre/post ns, vlm_role, enabled, severity, actions), created_utc_ms` PK(rule_id, revision) |
| `rule_evaluations` | `id, rule_id, rule_revision, camera_id, session_id, generation, window_start_pts_ns, window_end_pts_ns, utc_ms, before, after, quality, transition, event_id, track_ids_json, dwell_ns, note` (transitions only; `none` and `still_active` are not stored; lifecycle transitions always are; reports the evaluator repeats on every frame or tick (`unknown`, `suppressed`, `stale`, `out_of_schedule`, `zone_mismatch`, `disabled`) are stored once until the transition or the condition after it changes; an unknown row's note is the scheduler's reason) |
| `events` | `id, rule_id, rule_revision, camera_id, session_id, severity, condition (active/clearing/cleared), operator_state (new/acknowledged/resolved), opened_utc_ms, opened_pts_ns, trigger_pts_ns, cleared_utc_ms, title, detail, late (0/1)` |
| `event_reviews` | `id, event_id, label (confirmed/false_alarm/undecided), note, operator, utc_ms` (append-only; the latest row is the current label) |
| `alert_deliveries` | `id, event_id, channel (console/sound), state (pending/delivered/failed), attempts, last_error, created_utc_ms, delivered_utc_ms` |
| `evidence_refs` | `id, event_id, camera_id, from_utc_ms, to_utc_ms, state (pending/available/partial/deleted), reason, segment_ids_json, thumbnail_path, updated_utc_ms` |
| `evidence_holds` | `segment_id, until_utc_ms, reason` (created by the M2 migration; filled by `EvidenceService`) |
| `analysis_coverage` | `id, camera_id, session_id, from_utc_ms, to_utc_ms, frames_sent, frames_known, frames_unknown, detect_fps_target` (one row per camera per minute, and at a session change; `frames_sent` counts frames taken for analysis, including those that were unknown without a request) |

Operator actions (acknowledge, resolve, review, delete, export) are also
written to `audit_log`.

## Event lifecycle

- `Triggered` -> insert `events` (condition active, operator new), one
  `evidence_refs` row covering `[trigger_utc - evidence_pre, trigger_utc +
  evidence_post]`, an open-ended evidence hold on every segment already
  overlapping that window, `alert_deliveries` rows for `console` and (when the
  rule's `actions.sound` is set) `sound`, in one transaction; the audit row
  `event.open` records the trigger frame's receive age. The alert goes out
  immediately; it never waits for post-event footage. The `triggered`
  evaluation row is written only after that transaction committed; when it
  fails the evaluator forgets the event (no rearm) and the next frame with a
  completed dwell triggers again with a new id.
- Clearing counts observed absence only. A worker outage, a detector that
  cannot keep up (TTL-stale results) or a zone mismatch delivers frames that
  cannot be observed; they freeze the timers and also cover the span they
  arrived in (at most `max_observation_gap_ns` each), so they never end an
  occupancy: on resume a person still inside keeps the event open (dwell is
  measured again before `still_active`), and an empty zone starts clearing from
  the resume frame. Only time without any frame (a stopped source, including
  across a session change) longer than `clear_after_ns` ends it with note
  `observation gap`.
- `BecameClearing` / `StillActive` -> update `events.condition`.
- `Cleared` -> `condition cleared`, `cleared_utc_ms`.
- A trigger whose observation `recv_mono_ns` is older than the rule's
  `resultTtlNs` never reaches the evaluator as live; it is reported stale and
  nothing is opened. (Offline re-analysis that creates `late = 1` events is M4.)
- Operator state changes are independent of the condition: an event can be
  acknowledged while still active and resolved after it cleared. Acknowledge
  and resolve are idempotent.
- Evaluator state is not persisted: events a previous core run left `active`
  or `clearing` are cleared when the core starts, each with a `cleared`
  evaluation row and an audit row `event.clear` (note `core_restart`) in the
  same transaction as the condition change. Each rule's rearm then counts from
  its latest cleared event, so a person still inside after a restart is
  suppressed until rearm instead of alerting again after the dwell.
- Deleting a camera deletes its rules (an open event clears with note
  `camera_deleted`) and zones, and marks its evidence refs `deleted`.
- Rule edits: `PUT /v1/rules/{id}`, enable/disable and zone edits each store a
  new rule revision and apply it with `setRule`; `camera_id` cannot change.
  A zone revision moves every rule on the zone to a new rule revision with the
  new `zone_revision`; deleting a zone stores a disabled revision of each rule
  on it.

## Evidence

`EvidenceService` re-evaluates `pending` and `partial` refs every 5 s:

- Segments of the camera overlapping the window are collected.
- `available`: the window is fully covered by `finalized` segments and no
  receive gap intersects it.
  Finalized segments must reach both window edges exactly; adjacent segments
  of one session may join with a hole of up to 500 ms (their stored boundaries
  differ by splitmuxsink bookkeeping, 15 to 170 ms measured, while the media is
  contiguous); segments of different sessions never join.
- `partial`: some coverage exists but the end of the window is still in the
  future or inside a `recording` segment (`pending` while nothing is covered
  yet), or a gap or damaged segment intersects it. A partial ref whose window
  ended more than 2 minutes ago, with no segment still recording across it, and
  is still not fully covered stays `partial` permanently and records why.
- `deleted`: a covering segment was deleted. Retention marks the refs listing a
  segment inside the transaction that deletes it (reason names the segment);
  deleting a camera marks its refs (reason `camera deleted`). A deleted ref
  loses its thumbnail file and `GET /v1/evidence/{id}/thumbnail` answers 404
  `evidence_deleted`.
- The pass stops re-evaluating a `partial` ref once it has been evaluated more
  than 2 minutes after its window ended, unless a segment of its camera that
  started before the window ended is still recording (it is then re-evaluated
  until one pass after that segment finalized; a 600 s segment may finish long
  after the window).
- Retention never deletes a segment referenced by an evidence ref of an event
  whose operator state is not `resolved`, or that is younger than the
  evidence retention period (default 30 days). Ordinary recordings default to
  7 days. Retention (M2) checks only `evidence_holds(segment_id, until_utc_ms,
  reason)`, inside the IMMEDIATE transaction that marks the segment deleted, so
  a hold written before that transaction starts always wins. `EvidenceService`
  keeps the holds: whenever it collects the segments covering a ref it inserts
  a hold per segment with `until_utc_ms = 0` while the event is not resolved,
  and on resolve sets `until_utc_ms` to the event's open time plus the evidence
  retention period, in the transaction that changes the operator state and
  writes the audit row (a repeated resolve re-applies it). Segments already
  overlapping the window are held from the transaction that opens the event, so
  a max_bytes or disk-floor sweep in the first 5 s cannot take the pre-trigger
  footage; a segment that starts inside a live window is still recording until
  the next pass holds it.
- Thumbnail: the spool JPEG of the trigger frame is copied to
  `<data>/evidence/<event_id>.jpg` before the spool janitor removes it.

## Alert delivery

- The console polls `GET /v1/alerts/pending?console_id=...` every 1 s. The core
  marks the `console` delivery `delivered` when the console confirms with
  `POST /v1/alerts/{id}/delivered` after presenting it (the event is in the
  console's event list, which the badge, the live feed and the alert log
  render, and the camera popped when the rule asks), and `sound` delivered
  when the console confirms it played the sound. A poll that lists a delivery
  still being presented does not confirm it.
- No console confirmation within 10 s: `attempts++`, `last_error "no console
  connected"` (`"console did not confirm"` when a console polled within the
  last 10 s); after 3 attempts the delivery becomes `failed` and stays visible
  as failed in the alert log. An attempt is due 10 s after the later of the
  delivery's creation, its previous attempt and the core's start, so a restart
  or a stalled main thread never counts several attempts at once. A later console still receives it (pending list
  includes failed deliveries of unresolved events) and can mark it delivered.
  The confirmation counts as an attempt too.
- Dedupe is the evaluator's job (one event per occupancy, rearm); delivery
  never creates a second alert for the same event.

## API additions (v1)

| Method | Path | Purpose |
| --- | --- | --- |
| GET | /v1/zones?camera_id= | zones with their current revision |
| POST | /v1/zones | create zone `{camera_id, name, points, anchor, ref_width, ref_height}` |
| PUT | /v1/zones/{id} | new revision (points/anchor/ref size) |
| DELETE | /v1/zones/{id} | soft delete; rules using it are disabled |
| GET | /v1/rules | rules with current revision and runtime state per camera |
| POST | /v1/rules | create rule (body = RuleRevision fields) |
| PUT | /v1/rules/{id} | new revision; evaluator `setRule` keeps open events |
| DELETE | /v1/rules/{id} | soft delete; open events are cleared with note `rule_deleted` |
| POST | /v1/rules/{id}/enable, /disable | toggle |
| GET | /v1/events?state=&camera_id=&from_utc_ms=&to_utc_ms=&limit= | events + latest review + evidence + deliveries |
| GET | /v1/events/counts | `{unresolved, acknowledged, dismissed}` over all events (tab rules of the alert log) |
| GET | /v1/events/{id} | one event with evaluations timeline |
| POST | /v1/events/{id}/acknowledge, /resolve | operator state |
| POST | /v1/events/{id}/review | `{label, note}` |
| GET | /v1/alerts/pending | deliveries to show/play |
| POST | /v1/alerts/{id}/delivered | confirm |
| GET | /v1/cameras/{id}/detections/latest | newest detection frame for overlays `{camera_id, session_id, frame_id, pts_ns, utc_ms, width, height, detections[]}`; 404 when analytics is off |
| GET | /v1/analysis | worker state, per camera achieved fps, skips, p95 turnaround, unknown ratio |
| GET | /v1/evidence/{id}/thumbnail | JPEG |

Evidence playback reuses `POST /v1/playback {camera_id, at_utc_ms}`.

Payload fields the console reads (snake_case column names; unknown fields are ignored):

- Zone: `id, camera_id, name, revision, points [[x,y],...], anchor, ref_width, ref_height`.
  `GET /v1/zones` without `camera_id` lists the zones of every camera.
- Rule (list entry and POST/PUT response): `id, name, revision, enabled` plus the
  RuleRevision fields `camera_id, zone_id, zone_revision, schedule [{days (bit 0 =
  Monday), start_minute, end_minute}], time_zone, target_class, min_confidence,
  dwell_ns, max_observation_gap_ns, clear_after_ns, rearm_ns, result_ttl_ns,
  evidence_pre_ns, evidence_post_ns, vlm_role, severity (critical|review|info),
  actions {sound, pop_to_main_view}`. POST/PUT bodies carry the same revision fields.
- Event: `id, rule_id, rule_revision, camera_id, session_id, severity, condition,
  operator_state, opened_utc_ms, cleared_utc_ms, title, detail, late,
  review {label, note, operator, utc_ms} | null, evidence {id, state, reason,
  from_utc_ms, to_utc_ms} | null, deliveries [{id, event_id, channel, state,
  attempts, last_error, created_utc_ms, delivered_utc_ms}]`; `GET /v1/events/{id}`
  adds `evaluations [{utc_ms, rule_revision, transition, before, after, quality,
  dwell_ns, track_ids, note}]`. `evidence.reason` says why a ref is partial or deleted.
- Pending alerts: an array of delivery objects (as above). The console confirms
  with `POST /v1/alerts/{id}/delivered {console_id}`.
- Detection: `{track_id, cls, confidence, bbox [x1,y1,x2,y2]}`, bbox normalized to
  the detection frame.
- `GET /v1/evidence/{id}/thumbnail` takes the evidence ref id.

## Console

- Alerts & Analytics tab, Alert log sub-tab per the handoff: rules rail with the
  structured editor (camera, zone drawn on a live frame of that camera, schedule
  days + start/end + time zone, class, confidence, dwell s, severity, actions:
  sound, pop to main view), alert table with status tabs mapped to operator
  state (Unresolved = new, Acknowledged, Resolved), alert detail with evidence
  player, evidence state chip, review buttons (Confirmed, False alarm,
  Undecided), activity timeline from evaluations.
  - The design's "Dismissed" tab maps to review label `false_alarm` + resolved:
    it holds resolved events and events reviewed as false alarms in any
    operator state; Unresolved and Acknowledged leave false alarms out, and the
    tab badge counts the Unresolved tab.
  - "Test on last 24 h", webhook and email actions are shown as not implemented.
  - The editor keeps an existing rule on its camera (the API refuses a camera
    change) and sends confidence and dwell unchanged unless the operator edited
    them; every class the API accepts can be chosen.
  - The evidence player lists the window's recordings and plays across segment
    boundaries, starting at the first recorded time of the window.
  - The tab badge counts the Unresolved tab over all events
    (`GET /v1/events/counts`), not only the polled 7-day list. An alert whose
    event is older than the list is fetched on its own and stays in the list.
  - While the service is not ready the event lists read "service unavailable"
    and the badge is empty; the rules rail keeps its cards under a "not
    current" header.
- Monitor: live event feed panel (340) returns with real events. The
  provisional recordings panel moves into a toggle in the wall toolbar.
- Tiles draw detection boxes from `/detections/latest` (polled at the analysis
  rate, never at video rate) only when Overlays is on, and only boxes whose
  detection frame has the displayed frame's `session_id` and a `pts_ns` within
  1 s of it (the frame ring header carries session and pts, not the core's
  `frame_id`).
- Sound: bundled short WAV via QSoundEffect; the tab badge counts unresolved
  events.
- Statistics sub-tab and Model Train stay not implemented.

## Verification (scripts/verify_m3.py)

Inputs: `sample3.mp4` (classroom, four seated people for 33 s, looped) and
`sample2.mp4` (overhead parking lot, one person walking for 54 s).

1. Zone covering the seated people, dwell 10 s: exactly one event opens
   between 10 and 13 s after analysis starts; alert delivered to a console
   session; no second event while they stay (loop wraps keep one session).
2. Zone that nobody enters: no event for 60 s.
3. Worker killed during pending: evaluations report unknown, no event opens
   during the outage, dwell restarts after the worker is back; the seated
   event open at the kill stays open, and the restarted worker holds no
   recording file open.
4. Rule revision edited while the event is active: same event id stays open.
5. Source stopped: session change, event clears after clearAfterNs, the next
   occupancy is suppressed until rearm.
6. Evidence ref becomes `available` after the post window is recorded and the
   segment finalizes; playback at the trigger time shows frames.
7. Review saved and visible after a core restart.

Measured and recorded: trigger latency (condition satisfied in media time to
alert row), achieved detect fps, unknown ratio, detector p50/p95, events per
camera-hour on the negative zone.
