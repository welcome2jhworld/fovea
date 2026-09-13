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

## M1 — single input video path

In progress (see git log for the latest).

## Next
- Finish M1 pipeline + console, run `scripts/verify-m1.sh` (file source and
  webcam source), record results here.
