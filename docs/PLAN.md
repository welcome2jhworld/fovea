# Plan

Milestones follow the development brief. A milestone is done only when its
completion checks pass on this machine; checks that need hardware, models or a
design that is not available stay listed as unverified.

| Milestone | Scope | Done when |
| --- | --- | --- |
| M0 | Environment survey, build/test entry points, architecture and data contracts, feasibility spikes, real VLM smoke test | Reproducible minimal build; VLM smoke test result or a concrete blocker record |
| M1 | Single input (file, local RTSP, webcam RTSP): display, status, reconnect, segmented recording, playback | Fresh run: register input -> real frames on screen -> segment finalized -> playback; disconnect/reconnect shows state and gap correctly |
| M2 | 4 inputs, headless recording with UI closed, retention, resource metrics, crash/restart/disk-full tests | 4-input profile passes; UI close, worker kill, service restart, disk-full handled |
| M3 | First rule (person in zone >= 10 s in schedule), Event/Alert states, evidence clips, operator review | Alert on condition, dedupe, unknown during gaps, stale results ignored, review persisted |
| M4 | Persistent index, embeddings, filter + vector candidate search, evidence playback | Recall@K on a fixed question set; index resumes after restart; deleted sources reflected |
| M5 | VLM re-check of candidates, evidence validation, natural-language rules, follow-up questions | Question -> search -> VLM -> evidence works; missing evidence never becomes a positive |
| M6 | Design polish, Windows installer, offline run, update/recovery | One icon on a clean Windows box reproduces the main flows |

## M1 work breakdown

1. common: ids, clocks, paths, redaction, frame ring, API types.
2. core: store + schema + startup recovery; camera pipeline (rtsp/file) with
   tee -> record/view; session + gap tracking; reconnect backoff; playback
   channels; HTTP API; metrics.
3. console: theme tokens + QSS, frameless window, tab bar, Monitor screen
   (camera rail, wall 1x1/2x2/3x3, tile chrome), Camera settings dialog
   (Connection + Recording tabs real, others marked), recordings list and
   playback (provisional, not in handoff), service launcher.
4. tools: local RTSP test source (pattern, file loop, webcam).
5. tests: unit (ring, session mapping, backoff, redaction, store recovery,
   auth) + integration script producing a verification report.
