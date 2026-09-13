# Development status

Updated 2026-09-13. Categories: done / in progress / blocked. Each "verified"
line names what actually ran.

## M0 — foundation and risk checks

Done
- Environment survey (`docs/ENVIRONMENT.md`), architecture, data contracts,
  API contract, plan, decisions.
- CMake project with macOS and Windows presets; common library (ids, clocks,
  paths, redaction, backoff, token, shared-memory frame ring, API JSON types);
  SQLite store with startup recovery; file secret store; core API server and
  entry point; Python worker protocol with a dry-run backend.
- Feasibility spikes: Qt shared memory + HttpServer (`spikes/qt/NOTES.md`),
  GStreamer record/decode pipeline (`spikes/gst/NOTES.md`), design handoff
  mapping (`docs/design/handoff-map.md`), model survey
  (`docs/models/candidates.md`).
- Verified: `ctest --preset macos-dev` (frame ring incl. torn-read check,
  backoff, redaction, token, API JSON, store recovery) and the worker protocol
  tests pass on this Mac.

- Real VLM smoke test (2026-09-13, Apple M4 16 GB, `scripts/vlm-smoke.sh` on an
  8 s parking-lot CCTV clip, 16 frames at 2 fps; raw output in
  `docs/verification/vlm-smoke-*.md`):
  - NemoStation/Marlin-2B-MLX-8bit via the mlx_vlm video path: 25 s wall,
    scene caption + 3 timed events (`<0.0 - 2.5>` reverse, `<2.5 - 3.5>`
    drive forward, `<3.5 - 7.5>` empty lot), all mapped to existing frame ids,
    0 contract violations. English output; Korean not yet evaluated.
  - Qwen/Qwen3.5-4B via Transformers 5.17 on MPS (bf16): load 130 s,
    generate 112 s for 2 Korean observations with frame ids and ranges,
    0 contract violations. Too slow on this Mac for live use; the target is
    the Windows NVIDIA box. Thinking mode is disabled in the chat template.
  - First attempt failed to parse (Marlin answered in its own caption format,
    Qwen answered before the JSON instruction took effect); the worker now
    keeps raw text, parses the Marlin format, and keeps prose answers as
    whole-clip observations labelled `evidence_unmapped`.

Blocked
- Windows: CI build and packaged M1 run on GitHub windows-2022 runners is being
  brought up; the target NVIDIA machine has not run Fovea yet.

## Integrated build (2026-09-13)

Core (M2 + M3), worker (M3) and console (M3) built and run together from one
tree on the development Mac (Apple M4, 16 GB, macOS 26.3, Qt 6.9.0,
GStreamer 1.26.1). The machine was shared with unrelated work during every run
(1-minute load average 4.5 to 9.9), so timings are not isolated measurements.

- Verified: clean build of every target (`FOVEA_BUILD_TOOLS=ON`), 196 build
  steps, 0 compiler warnings under `-Wall -Wextra -Wpedantic -Wshadow
  -Wconversion`.
- Verified: `ctest` 18 of 18 pass (0 skipped), including `test_store`,
  `test_retention`, `test_rule_engine`, `test_evidence`, `test_analysis`,
  `test_alert_logic`, `test_console_flows` and `console_smoke`.
- Verified: worker unit tests, 113 tests: system Python 3.12 OK with 5 skipped
  (2 clock tests for other platforms, 2 detector tests without torch, 1
  ByteTrack test without supervision); worker venv with real RF-DETR frames
  (5 frames of the parking-lot clip from 5.5 s, one car) OK with 2 skipped (the
  other-platform clock tests).

## M1 — single input video path

Done
- Verified after the review fixes (`scripts/verify_m1.py --source pattern`,
  report `docs/verification/m1-20260913-141250.{json,log}`): PASS. Camera online
  0.51 s after it was added at 24 fps, status latency p50 0.81 / p95 0.96 ms;
  shared-memory ring 77 frames in 3 s (25.7 fps, read latency p95 6.71 ms);
  2 finalized segments of 9.85 s (549 078 and 547 550 bytes); source stop
  detected at once (state `reconnecting`, last frame 17 ms old); reconnect
  2.33 s after the source restarted, 1 receive gap, closed; playback playing at
  2.48 s with 51 ring frames in 2 s; after an API shutdown and restart 4
  finalized, 0 damaged, 0 stale `recording` rows. The earlier run before the
  fixes (`docs/verification/m1-20260913-123134.{json,log}`) also passed.
- Earlier `scripts/verify-m1.sh` runs with the file-loop source also passed
  (`docs/verification/m1-20260913-10*.log`), including SIGKILL recovery of the
  open segment.

Not verified
- The webcam source in a verification run.

## M2 — four inputs, headless recording, retention

Done
- Verified after the review fixes (`scripts/verify_m2.py --inputs 4 --minutes 3`,
  report `docs/verification/m2-20260913-141335.{json,log}`): PASS, 0 failed
  checks. 4 x pattern 640x360 25 fps, 30 s segments. The core also supervised
  the development worker (RF-DETR loaded at start, no analysed camera).
  - All 4 cameras online in 1.04 s. Soak 180 s: fps mean 25 / 25 / 25 / 25,
    minimum sample 25; latency p95 median 1.67 / 1.69 / 1.66 / 1.97 ms, max
    2.9 ms; 0 drops; queue depth max 1; 5 finalized segments per camera (1.67
    per minute).
  - Core RSS 83.5 MB at start, 66.8 MB after 1 min, 67.8 MB at the end
    (+1.5 %); CPU average 12.7 % of one core.
  - kill -9 while recording: all online 1.25 s after the restart; recovery
    closed 4 sessions, finalized 4 segments, removed 4 orphan frame rings; 0
    rings, 0 open old sessions and 0 stale `recording` rows left.
  - Console opened headless and closed: exit 0 after 6.6 s; the same core kept
    running and all 4 cameras finalized segments afterwards.
  - Disk floor above free space: all cameras `online` / `paused_disk` at 25
    fps, ring frames still flowing (51 in 2 s), 0 segment rows created,
    `floor_unreachable` true, 0 deletions; recording resumed 1.16 s after a
    normal restart.
  - Retention with `FOVEA_RETENTION_SECONDS=120`: 15 segments (23.9 MB) deleted
    by age with 0 files left; the held segment kept (finalized, file present);
    the file left by a simulated crash after the row commit removed at startup
    (purged 1); audit rows match; `retention_days` 3 accepted, invalid value
    rejected with 400; `max_bytes` deleted all 5 finalized segments of that
    camera.
- Verified before the review fixes (`--minutes 5`, report
  `docs/verification/m2-20260913-123234.{json,log}`): PASS.
- Verified earlier by the core lane with a 10 min soak
  (`docs/verification/m2-20260913-110452.{json,log}`): PASS, RSS -4.5 % after
  the first minute, CPU average 13.1 %.

Not verified
- Worker kill with 4 recording inputs (worker kill is covered by M3 on 2
  analysed cameras); cross-camera disk-floor deletion on a really full disk
  (unit test only); retention over very large segment tables; Windows build of
  the M2 code; playback-channel rings after a crash are not reclaimed.

## M3 — first rule, events, evidence, alerts

Done
- Review fixes (2026-09-13), each with unit tests: a worker outage, TTL-stale
  or generation-stale results no longer end an open event (only time without
  any frame counts towards the `observation gap` close) and generation-stale
  results freeze their span; a failed event insert no longer leaves the
  evaluator holding an event without a row (the `triggered` row is written after
  the event commits, the next frame triggers again); a core restart writes a
  `cleared` evaluation (note `core_restart`) and seeds rearm from the last clear;
  delivery attempts are spaced 10 s from the previous attempt or the core start;
  deleting a camera deletes its rules, zones and evidence refs; resolve, acknowledge
  and review commit with their audit row; evidence of a window inside a long
  recording segment becomes available once the segment finalizes; window edges
  must be covered exactly; holds are written when the event opens; retention marks
  refs inside the deletion transaction and removes their thumbnails (the route
  answers 404 `evidence_deleted`); an adopted fragment without a predecessor end
  time gets no start time; the worker is restarted when its detector stays
  `failed` for 3 checks or one job runs over 30 s, and inherits no descriptors;
  latency p95 counts failed and timed-out requests; jobs carry `threshold` and
  `max_gap_ns` (tracks start at the job threshold, ids survive gaps the rules
  bridge). Console: deliveries are confirmed only after they were presented (one
  events refresh per poll), the evidence player plays across segments, the badge
  counts all unresolved events (`GET /v1/events/counts`), alerts older than the
  list stay pinned, stale lists read "service unavailable", the editor keeps an
  existing rule's camera and unedited confidence and dwell, all six classes are
  selectable, zones stop at 32 points, "deleted rule" only when rules are loaded,
  the feed never moves an unresolved selection, wrapping schedules read "next day".
- Verified after the review fixes (`scripts/verify_m3.py` with the classroom clip,
  transcoded to 960x540 25 fps, and the parking-lot clip; report
  `docs/verification/m3-20260913-140841.{json,log}`): PASS, all checks. Core ran
  the RF-DETR Nano worker on MPS through `FOVEA_WORKER_CMD`; load average 9.9 at
  the start, 6.2 at the end.
  - Worker: detector load 13 899 ms, core start to known results 17.1 s.
  - Dwell 10 s on the seated zone: exactly 1 event, opened 10 080 ms after the
    first known detection; trigger frame receive to event row 133 ms; dwell
    satisfied in media time to event row 213 ms; console and sound deliveries
    pending, confirmed, then off the pending list; still 1 event 60 s later.
  - Zone nobody enters: 0 events in 0.0411 camera-hours.
  - Worker SIGKILL 5 s into a pending 20 s dwell: 1 unknown row (note
    `worker_failed`), 0 events during the outage, 1 restart with a new pid,
    detector ready 14.7 s after the kill, dwell restarted 15.2 s after the kill,
    event 20 440 ms after the restarted pending. New checks: the seated event
    open at the kill stayed `active` through the outage (2 events on that rule
    before and after), and the restarted worker held 0 recording files open
    (`lsof`).
  - Rule revised while the event was active: revision 2, same event id.
  - Source stopped 8 s: new session, generation 4 -> 5, event cleared
    (`observation gap`) 15.46 s after the stop, 1 `suppressed` row 10.0 s after
    the clear, next event 20 000 ms after the clear (rearm 20 s).
  - Evidence `available` 26.0 s after the trigger, 20 s window over 3
    segments with 3 open-ended holds, thumbnail JPEG 160 657 bytes, playback at
    the trigger time 76 frames in 3 s.
  - Core SIGKILL: its worker exited (stdin closed); after the restart the event
    is still `acknowledged` with review `confirmed`, deliveries `delivered`,
    rule revision 2, the 2 events left open cleared at startup, audit rows for
    acknowledge and review. Analytics off: not analysed, detections 404; on
    again: the rule reports `became_pending` on known frames, 0 stale rows. A
    clean shutdown stopped the worker and removed `worker.json`.
  - Classroom camera steady window 70.7 s: 1.99 detect fps, unknown ratio 0.0;
    whole run 0.1865 (58 of 311 frames, detector load and outage included);
    0 skips. Core turnaround p50 111.3 / p95 153.5 ms (request only 59.1 / 80.9
    ms; both now include failed and timed-out requests); worker detector p50 58
    / p95 76 ms over 79 jobs. Parking lot camera 2 fps, unknown ratio 0.167,
    turnaround p50 67.8 / p95 93.7 ms.
- Verified earlier, before the review fixes
  (`docs/verification/m3-20260913-123858.{json,log}`, scenario details in
  `docs/verification/m3-20260913-122156.md`): PASS with the same scenarios.
- Verified before the review fixes: console against the integrated core and
  real worker (a scratch script, not in the repo: `rtsp-testsrc` looping the
  transcoded classroom clip, `fovea-core` with `FOVEA_WORKER_CMD`, one camera
  with analytics, the seated zone and a 10 s dwell rule with sound, then the
  console headless with
  `FOVEA_SCREENSHOT_VIEW` = `alerts`, `monitor-feed`, `alert-detail`). Final
  run: known results 15.2 s after the camera was added, event 10.2 s after the
  rule, evidence `available` 19.2 s after the event, thumbnail 163 433 bytes;
  each console run exited 0 after 9.0 s with the core still running. The
  screenshots show the real event in the alert table with its thumbnail and
  the unresolved badge, the live tile with RF-DETR person boxes and scores
  (0.88-0.92) plus the event in the live feed, and the detail panel with the
  evidence clip playing, the `EVIDENCE AVAILABLE` chip, the window
  21:49:23-21:49:43 and "shown on console" in the activity. The console
  confirmed both the console and sound deliveries (`delivered`, 2 attempts),
  and the evidence playback channel it opened was closed when it exited.
- Defect found by the integration run and fixed: closing the console with an
  alert detail open left its evidence playback channel (pipeline and frame
  ring) open in the core until the core restarted, because the player releases
  the channel when hidden and the window hid it after the exit drain. The main
  window now hides its screens before draining (`MainWindow::closeEvent`).
  Before the fix 1 of 1 channel stayed open after the console exited; after it
  0 of 1.

Not verified
- Windows: MSVC build of the M3 core and console, `Scripts\fovea-worker.exe`
  discovery, worker stop through stdin, QSoundEffect playback.
- Four analysed cameras, long soak with analysis, memory of the tap copies
  under load; stale results on a live run beyond the generation bump (unit
  tests only); schedule windows excluding real traffic; zone mismatch on a
  resolution change; retention deleting evidence after the 30-day hold on real
  recordings (unit tests only).
- Console flows against the real core: creating a rule and drawing a zone in
  the editor, the acknowledge / resolve / review buttons (acknowledge and
  review are covered by `test_console_flows` against the stub core), whether
  the sound was audible; after the review fixes the console ran only against
  the stub core (`test_console_flows`, `console_smoke`), so evidence playback
  across a segment boundary, the counts badge and the stale-list state are not
  verified against the real core.
- A detector that stays `failed` or a detector job that hangs on a live run
  (restart rules covered by unit tests only); a worker outage longer than
  `clear_after_ns` during clearing on a live run (unit tests only).

Known issues
- Labels of neighbouring detection boxes overlap on the tile (cosmetic).

## Next
- M4: persistent index, embeddings, filter + vector candidate search.
- Windows: M2 and M3 verification scripts on the target NVIDIA machine.
