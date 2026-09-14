# Decisions

Short records of choices that are not obvious from the code.

- D1 Qt Widgets, not QML. The design handoff targets Qt Widgets + QSS and no
  UI existed. Model/view with delegates covers the lists; two custom painted
  widgets (video tile, range timeline) are needed either way.
- D2 All media decode in the core, none in the console. Live tiles and
  recording playback both read shared-memory frame rings, so one display widget
  serves both and Qt Multimedia backend differences (AVFoundation on macOS,
  FFmpeg/WMF on Windows) never affect playback of our own files.
- D3 Loopback HTTP (QtHttpServer) for control, shared memory for frames, 1 Hz
  status polling in M1. Push events (WebSocket) arrive with alerts in M3.
- D4 Dependencies: Qt 6.9 and GStreamer 1.26 only, both from Homebrew on macOS.
  SQLite through QtSql, JSON through QJson, no vendored libraries. Windows uses
  the official Qt and GStreamer MSVC packages; versions pinned in
  docs/ENVIRONMENT.md.
- D5 Vector search starts as brute-force cosine over a flat float file (one
  hour at 1 fps is about 14 MB at 1024 dims). FAISS is adopted when a measured
  index exceeds what brute force answers within budget.
- D6 Credentials: SecretStore interface; M1 implementation is a 0600 file in
  the data directory, URLs are redacted in every log line. OS keychain backends
  are an M6 item.
- D7 Recording container: Matroska via splitmuxsink with muxer=matroskamux and
  sink="filesink buffer-mode=unbuffered". The M0 crash test (kill -9 mid
  segment) left plain mp4 unplayable, fragmented mp4 lost up to one fragment,
  and mkv lost at most the current ~500 ms cluster while staying playable
  without repair. Files lack duration/cues until finalized; playback reads the
  duration from the segment row. Details in spikes/gst/NOTES.md.
- D8 View branch uses appsink sync=false behind a 5-buffer leaky queue: a
  syncing sink holds each frame for the whole jitter latency and overflows the
  queue (5 to 12 fps observed). The console paints frames as they arrive.
- D9 Decoder: avdec_h264 by default (same CPU as vtdec at 640x360, about 50 ms
  earlier delivery). vtdec_hw is revisited for 1080p or many streams.
- D8 Recording pipeline shape. Sources link into `parse -> tee` with the
  record branch (`queue -> valve -> splitmuxsink`, matroskamux, unbuffered
  filesink, one file per `segment_seconds`) and the view branch (leaky queue
  -> avdec -> BGRA -> appsink -> frame ring). File sources loop with a
  non-flushing segment seek so running time continues across wraps (one
  session, continuous segments); their chain is built before the first state
  change because a paced (`clocksync`) chain added to a running pipeline
  never leaves PAUSED. Upstream duration queries are dropped at the tee so a
  fragment header never carries the source clip's length. Startup recovery
  measures a cut file by parsing it (no decode), since the demuxer's estimate
  for a header-less mkv counts from zero rather than the fragment's first
  timestamp.
- D9 Disk floor. Free space is checked before a session records and at every
  fragment boundary; below the floor the record valve drops and the camera
  reports `paused_disk`. Resuming mid-session would splice a non-keyframe
  into the open file, so recording resumes on the next session start.
- D10 Retention and the disk floor. Retention frees space from the floor plus
  headroom (10 %, at least 16 MB) rather than from the floor itself, so the
  pipelines, which pause below the floor and resume only above floor plus
  headroom, never flap between paused and recording while old segments exist.
  Space is freed across cameras only if every deletable segment together
  could reach that target; otherwise nothing is deleted and `/v1/storage`
  reports `floor_unreachable`, because wiping all recordings and still being
  unable to record helps nobody (a disk filled by something else, or a floor
  set above the volume size). One pass deletes at most 200 segments and
  continues 500 ms later, so a large backlog never holds the API thread for
  long. Evidence protection is a separate `evidence_holds` table keyed by
  segment rather than a query over M3's evidence refs, so retention does not
  depend on how evidence windows map to segments.
- D11 Default index version. Two embedding models were built behind one index
  version contract (`siglip2-b16-224`, `qwen3vl-emb-2b-1024`) and the default
  was chosen by a rule fixed before the evaluation ran (the milestone's run
  plan; it is repeated in docs/M4_DESIGN.md so the repository carries it): the
  higher Recall@5 on the final question set, on a tie the cheaper one to index.
  SigLIP 2 won (1.000 against 0.917) at one fourteenth of the index cost, so it
  is `search.active_index_version` by default; Qwen stays selectable per query
  and per install (`PUT /v1/index/active`), because it separates negative
  queries from positives by score and M5 may need that. Settings name index
  versions, never hashes: the worker reports the descriptor hash with every
  result, so a model revision change becomes a new version and a re-index
  without any configuration edit (`docs/M4_DESIGN.md`).
- D12 Indexing never waits for the live detector and vice versa. The worker's
  embed lane is separate from the detection lane, index requests yield the
  lane to a query between batches, and the core delays embed jobs until the
  detector warm-up has ended, because loading a second Transformers model
  while the detector's warm-up import runs in another thread broke both
  imports and restarted the worker in a loop.
- D13 Searches are bounded, not queued without limit. A scan streams the rows
  of its range (it never holds them all) and at most two run at once, with four
  more waiting and `503 search_busy` beyond that, because the console re-posts
  a query on every filter change and only the newest answer is ever painted:
  without the bound a burst of queries would each hold a mapping and a
  candidate heap for results nobody reads, and would starve compaction, which
  only runs while no scan does.
- D14 Counting how much of a range a version covers walks that version's index,
  so it never runs on the core thread: the choice between the active and the
  previous version happens on a pool thread, and a query that names no version
  and has no previous version to compare against counts nothing at all.
