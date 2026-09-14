# Development status

Updated 2026-09-14. Categories: done / in progress / blocked. Each "verified"
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

## Integrated build (2026-09-14, M4 tree)

Core, worker and console with the M4 search code built and run together from
one tree on the development Mac (Apple M4, 16 GB, macOS 26.3, Qt 6.9.0,
GStreamer 1.26.1), while four review agents read the tree and, for part of the
time, other processes ran (1-minute load average 3.4 to 5.8).

- Verified: clean build of every target (`FOVEA_BUILD_TOOLS=ON`), 228 build
  steps, 0 compiler warnings.
- Verified: `ctest` 22 of 22 pass (0 skipped), including the new `test_vector_store`,
  `test_index` and `test_import_sample`, and the console's `test_search_logic`.
- Verified: worker unit tests, 145 tests: system Python 3.12 OK with 6 skipped
  (clock tests for other platforms, tests that need torch or supervision, the
  weights-dependent embedding test); worker venv with real weights and the
  parking-lot frames OK with 2 skipped (the other-platform clock tests).
- Verified on this tree: `scripts/verify_m1.py --source pattern` PASS
  (`docs/verification/m1-20260914-000407.{json,log}`), `scripts/verify_m2.py
  --minutes 3` PASS (`m2-20260914-000913`), `scripts/verify_m3.py` PASS
  (`m3-20260914-000606`), `scripts/verify_m4.py` PASS (`m4-20260914-000451`).
  M2 and M3 numbers are in their sections below.
- Verified again after the review fixes (2026-09-14, same build with the fixed
  core, worker and console): 22 of 22 ctest tests, 146 worker tests in the venv
  with real weights (2 skipped, both for other platforms), `verify_m1.py`
  (`m1-20260914-005020`), `verify_m4.py` (`m4-20260914-005102`) and
  `verify_m3.py` (`m3-20260914-005210`) all PASS. M3 turnaround was p50 103.3 /
  p95 126.3 ms on this run (the machine was quiet), against p50 120.2 / p95
  445.2 ms while four review agents were reading the tree.

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
- Verified again on the M4 tree (2026-09-14, `--minutes 3`, report
  `docs/verification/m2-20260914-000913.{json,log}`): PASS, 0 failed checks.
  All 4 cameras online in 1.29 s; fps 25 on every camera and sample, latency
  p95 at most 2.2 ms, 0 drops; kill -9 recovery to all online in 1.09 s
  (4 sessions closed, 4 segments finalized, 4 rings removed); console closed
  after 6.3 s with the core still recording; disk floor paused recording on all
  4 cameras at 25 fps; retention deleted by age with the held segment kept and
  the crash-left file purged. New in this run: the worker indexed every
  finalized 30 s segment during the soak (the cameras have indexing enabled by
  default), and the core's RSS went 87.7 MB -> 124.0 MB after the first minute
  -> 144.7 MB at 3 minutes (+16.7 % after the first minute, CPU average
  24.4 %), against +1.5 % on the M3 tree without indexing. A 10-minute soak on
  the same build is listed under M4.
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
- Verified again on the M4 tree (2026-09-14, report
  `docs/verification/m3-20260914-000606.{json,log}`): PASS, all checks, same
  scenarios. Event opened 10 440 ms after the first known detection; worker
  SIGKILL: ready 14.7 s after the kill, dwell restarted 14.7 s after the kill,
  event 23.4 s after the restarted pending, the open seated event stayed
  `active`; rearm event 20 520 ms after the clear; steady window 71.1 s at
  2.01 detect fps with unknown ratio 0.0; core turnaround p50 120.2 / p95
  445.2 ms and worker detector p50 111 / p95 373 ms over 75 jobs, slower than
  the 2026-09-13 run because four review agents were reading the tree during
  the run; core SIGKILL and analytics toggle as before.
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

## M4 — natural-language search over recordings

Done
- Worker: an `embed` lane separate from the detector and VLM lanes, two index
  versions (`siglip2-b16-224`, 768 dims float32; `qwen3vl-emb-2b-1024`, 1024
  dims MRL, float16 on MPS/CUDA), `embed_frames` and `embed_text` jobs with
  contract checks, a descriptor per index version whose hash the core
  recomputes, query jobs that take the lane between an index job's batches,
  `embed-bench` and `embed-query` CLIs. Bench and sanity check on real frames:
  `docs/verification/embed-bench-20260913-231945.md` (SigLIP 2 25 ms per frame at
  batch 16 on MPS, Qwen 0.8 to 0.9 s per frame at the 262 144 px cap; a Qwen
  text query answered in 433 ms while a 32-frame Qwen index request held the
  lane).
- Core: schema v4 (`index_versions`, `index_jobs`, `embedding_records`,
  `search_sessions`, `imports`, camera `index_enabled`), `POST /v1/imports`
  (remux without re-encoding into `imported` segments of a camera, own session
  clock), segment sampling at every whole second of UTC, a persistent index
  queue with restart recovery, append-only vector files with a startup check
  and compaction, brute-force cosine search over the filtered range, range
  merging, coverage reporting, stored search sessions, thumbnails per record,
  retention and camera deletion propagating to records, jobs, thumbnails and
  stored sessions, `GET /v1/index`, `PUT /v1/index/active`,
  `DELETE /v1/index/versions/{hash}`. Deviations from the design are listed in
  `docs/M4_DESIGN.md`, "As implemented in the core".
- Console: Search tab (query bar, time and camera filters, index status line,
  results grid with async thumbnails and a relevance badge, stats line with
  coverage, inspector with the evidence player limited to the result, metadata,
  the permanent "Similarity search. Results are not verified." note, the four
  not-implemented actions disabled), searching / empty / error states, Ctrl+K,
  stub-core fixtures and screenshot views for the console tests.
- Evaluation set `eval/search/` (53 questions, 36 final and 17 tune, 28 Korean,
  12 negatives, 6 videos matched by sha256: 3 private local clips and 3
  supervision assets) and `scripts/eval_search.py`.
- Verified after the review fixes (`scripts/verify_m4.py` with the three local
  clips, report `docs/verification/m4-20260914-005102.{json,log}`): PASS, all
  checks, index version `b63a3a358815` (the descriptor now records the image
  processor, so the hash differs from the run before the fixes). Imports 15 to
  31 ms; the core killed after 32 of 54 records of a running job deleted that
  attempt's rows and reached coverage 1.0 again 16.4 s after the restart, 117
  live records for 117 expected instants, 0 instants with two records; index
  cost 179 compute s per footage hour (20x real time); each positive query
  ranked its own camera first (relevance 0.150 / 0.212 / 0.161) with total 17
  to 36 ms of which 0 to 2 ms scanning; playback at a result's time delivered
  24 frames in 2 s; retention removed the classroom camera's records (0 live,
  84 kept), its thumbnails (404) and its ranges from the stored session.
- Verified before the review fixes (`docs/verification/m4-20260914-000451.{json,log}`):
  PASS, all checks.
  - Imports: 3 files became 4 + 1 + 4 finalized `imported` segments anchored at
    the given `start_utc_ms` (import 18 to 36 ms each); a missing file answers
    400, overlapping footage 409.
  - The details below are from that first run; the run after the fixes checked
    the same scenarios with the numbers above.
  - Restart mid-index: the core was killed after the walking video's job had
    stored 32 of 54 records; after the restart the 32 records of the interrupted
    attempt were deleted, the job ran again (generation 1 -> 2), every camera
    reached coverage 1.0 in 17.4 s, 118 live records for 118 expected samples,
    0 instants with two records.
  - Index cost on this run: 246 compute s per hour of footage (14.6x real time
    for one camera at 1 sample/s), 118 rows, 364 KB of vectors, 919 KB of
    thumbnails.
  - Queries: each positive query ranked its own video's camera first
    (classroom, walking, white car), answered by `siglip2-b16-224` as similarity
    only, coverage 1.0, total 22 to 37 ms; camera and time filters hold; an
    unknown camera answers 400; the stored session returns the same ranges; the
    representative thumbnail is a JPEG.
  - Playback at a result's time delivered 24 frames in 2 s.
  - Retention (`FOVEA_RETENTION_SECONDS`) removed the classroom camera's
    footage: 0 live records left for it, 85 kept for the others, its 33
    thumbnails gone (404), the stored session lost its classroom ranges, a new
    query returns no classroom footage, the walking query still ranks its video
    first, 4 audit rows.
- Verified: evaluation of both index versions on the final set
  (`docs/verification/search-eval-siglip2-b16-224-final-cf0.3-20260914-005638.{json,md}`,
  `search-eval-qwen3vl-emb-2b-1024-final-cf0.3-20260914-005653.{json,md}`,
  comparison in `search-eval-summary-final-20260914-005952.md`). Headline set of
  24 questions: siglip2-b16-224 Recall@1 0.917, Recall@5 1.000, Recall@10 1.000,
  query total p50 15 ms, 226 compute s per footage hour; qwen3vl-emb-2b-1024
  Recall@1 0.875, Recall@5 0.917, Recall@10 0.958, p50 86 ms, 3 176 compute s
  per footage hour. Korean questions (13 of the 24) are where they differ most:
  SigLIP 2 answers 0.923 / 1.000 / 1.000 and Qwen 0.769 / 0.846 / 0.923, while
  Qwen is perfect on the 11 English ones. Negative queries: none of Qwen's 8
  score above the median positive top score, against one of SigLIP 2's. By the
  rule fixed before the runs (higher Recall@5, tie to the cheaper)
  `siglip2-b16-224` is the default; Qwen indexes at 0.88x real time on this Mac
  and misses the throughput target of the design. `candidate_fraction` 0.3 was
  chosen on the tune set, whose five runs are in `docs/verification/` too.
- Verified: console against the real core (a scratch script, not in the repo:
  `fovea-core` with the venv worker, three cameras with the local clips
  imported through `POST /v1/imports`, index coverage 1.0 on all three, then
  the console headless with `FOVEA_SCREENSHOT_VIEW=search-results`). The
  screenshot shows the real query "person walking across the gate apron" with
  7 results over two cameras (relevance 0.12 down to 0.07), real thumbnails
  (the walker is visible in the first one), the index line `siglip2-b16-224 ·
  Parking Lot 100% · Lot Entrance 100% · Classroom 100% · queue empty`, the
  stats line `7 results · < 0.1 hours searched · coverage 100% · 0.03 s`, and
  the inspector playing the selected 14 s range with its metadata (index
  version `b63a3a358815`, model google/siglip2-base-patch16-224) and the
  permanent "Similarity search. Results are not verified." note above the four
  disabled actions. The console exited 0 and the core shut down cleanly.
- Verified: the M4 core, console and worker compile with MSVC and pass the
  Windows unit tests in CI (run on the first push of this code; one test failed
  because it removed a vector file while a reader still mapped it, which
  Windows refuses and which the scheduler already avoids by removing files only
  while no search is scanning; the test now closes the reader first).
- Windows packaging: `Setup-Worker.cmd` now also downloads the SigLIP 2
  weights (about 1.5 GB) into the Hugging Face cache, because the worker never
  downloads at run time and the Search tab needs them; `scripts/fetch-models.sh
  search` does the same on macOS.
- Review fixes (2026-09-14), each with a unit test unless noted. Core: a job
  left running on a segment that was deleted is skipped at the next start
  instead of sitting in the queue for ever, and a crash now counts an attempt
  so a segment that takes the core down cannot hold the head of the queue; a
  scan streams its rows instead of loading every row of the range (memory is
  the candidate heap, not the footage), at most two scans run at once with four
  waiting and `503 search_busy` beyond that; the coverage counts that choose
  between the active and the previous version run on a pool thread, and a query
  with no previous version counts nothing; a filter that leaves no footage
  answers at once without asking the worker; a segment that ends exactly where
  a range starts no longer marks it partial; records whose vector is not at
  their offset are dropped and their segment is indexed again (at startup and
  when a scan finds them); the files of a version deleted before its removal
  ran are removed at the next start, and a version cannot be deleted while a
  search is scanning; an import that fails removes its fragment files that have
  no segment row. Worker: model loads are serialised process-wide, so
  `--warmup` with `--embed-preload` no longer makes two threads import torch at
  once; both descriptors record the image-processor class, so a change of
  resize implementation becomes a new index version. Console: a search and its
  thumbnail fetches are aborted when the screen is hidden or destroyed (the
  exit drain no longer waits for them, and the stale fetches no longer queue in
  front of the next query); the stub core answers with the documented error
  codes, limits and fields. Evaluation: percentiles are interpolated (the
  "median" was a single order statistic picked by banker's rounding, which
  moved the negatives figure), report file names carry the split and the
  candidate fraction, and the reports state what precision divides by and how
  many ranges a query returned.

Not verified
- Windows: MSVC build and run of the M4 core and console, the worker embed lane
  on CUDA, `Setup-Worker.cmd` with the model download, Qwen3-VL-Embedding on a
  GPU, a query arriving while a Qwen index request holds the lane on a GPU.
- The scan bound (`search_busy`) and the streaming scan under real load: unit
  tests and the verification run cover the paths, but no run has had more than
  a few thousand samples or more than one query at a time.
- Vector bytes that a power loss leaves stale inside a file are found only when
  a search reads them (the row is then dropped and its segment indexed again);
  the startup check reads offsets, not record ids.
- Scale: the index over hours or days of footage (brute-force scan latency,
  compaction on real data, thumbnail volume of about 44 MB per footage hour),
  and memory over a long soak with indexing (see the M2 3-minute soak: RSS
  +16.7 % after the first minute while 24 segments were indexed; the 10-minute
  soak result is added below when it finishes).
- Field recall: the corpus is 152 s of footage with 24 headline questions, and
  the tune and final splits share the same footage; the numbers separate the
  two models on this footage only.
- Console against the real core beyond the screenshot: custom time range and
  camera multi-select, keyboard navigation, a search that fails or is cancelled
  by a newer query, thumbnails for many results, a partially deleted range.
- Imports of H.265 and MKV files and of files longer than a few minutes; a
  second index version active at the same time as a re-index.
- Korean queries were evaluated only on SigLIP 2's and Qwen's multilingual
  text towers with the question set above; no native speaker rated the ranking.

## Next
- M5: VLM re-check of search results, semantic rules, drafting and follow-up
  questions (`docs/M5_DESIGN.md`).
- Windows: M2, M3 and M4 verification scripts on the target NVIDIA machine.
