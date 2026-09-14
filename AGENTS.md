# Working on Fovea

Instructions for a coding agent (or a new contributor) picking this repository
up. Read this first, then `docs/STATUS.md` for what is actually verified today.

## What this is

Local video monitoring for existing CCTV cameras: watch, record, search the
recordings in natural language, and raise alerts from rules, each with an
evidence clip. Everything runs on one machine; no cloud, no upload.

Three processes (`docs/ARCHITECTURE.md`):

| Process | Binary | Role |
| --- | --- | --- |
| Console | `fovea` | Qt Widgets UI. Never decodes video, never runs models, never opens the database. |
| Core | `fovea-core` | GStreamer pipelines, recording, SQLite, HTTP API, rules, index and search. Keeps running when the console closes. |
| Worker | `fovea-worker` | Python. Detection, embeddings, VLM. A separate process so a model crash cannot stop recording. |

Console to core and core to worker are loopback HTTP with a per-install bearer
token. Frames move through shared-memory rings, never through HTTP.

## Read in this order

1. `docs/STATUS.md`: what is done, what is verified and by which run, what is
   explicitly not verified. The "Not verified" lists are as important as the
   rest; do not treat anything outside them as proven.
2. `docs/PLAN.md`: the milestones and their exit criteria.
3. The design document of the milestone you are working on: `docs/M3_DESIGN.md`
   (rules, events, evidence), `docs/M4_DESIGN.md` (index and search),
   `docs/M5_DESIGN.md` (VLM re-check, semantic rules, follow-up questions).
   Each design document is the contract for its milestone and carries an
   "As implemented" section with every deviation and its reason.
4. `docs/API.md` (HTTP contract), `docs/DATA_CONTRACTS.md` (SQLite schema and
   JSON shapes), `docs/DECISIONS.md` (choices that are not obvious from code),
   `docs/ENVIRONMENT.md` (machines, versions, pitfalls).

## Build and test

macOS, always source the environment script first (an Anaconda GStreamer 1.14
otherwise shadows Homebrew's 1.26):

```sh
cd <repo> && . ./scripts/env.sh
cmake --preset macos-dev -B build/dev -DFOVEA_BUILD_TOOLS=ON
cmake --build build/dev            # must finish with zero warnings
ctest --test-dir build/dev --output-on-failure
```

Windows (a "x64 Native Tools" prompt, Qt and GStreamer per `README.md`):

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc --output-on-failure
```

Worker:

```sh
./scripts/setup-worker.sh torch          # creates worker/.venv
./scripts/fetch-models.sh search         # SigLIP 2 for search; m0 / all add the VLMs
cd worker && python3 -m unittest discover -s tests          # no torch needed
cd worker && .venv/bin/python -m unittest discover -s tests # with real weights
```

Some worker tests only run when `FOVEA_TEST_FRAMES_DIR` points at a directory
of real CCTV frames (they are skipped otherwise, and one of them expects a car
in the first frames).

## Verification scripts

Integration runs against the real binaries. They write a JSON report and a log
into `docs/verification/`, and every "Verified" line in `docs/STATUS.md` names
the report it came from. Add to them rather than replacing them.

```sh
python3 scripts/verify_m1.py --bin-dir build/dev --source pattern
python3 scripts/verify_m2.py --bin-dir build/dev --minutes 3
python3 scripts/verify_m3.py --bin-dir build/dev --seated <clip> [--walking <clip>]
python3 scripts/verify_m4.py --bin-dir build/dev --classroom <clip> --walking <clip> --whitecar <clip>
python3 scripts/eval_search.py --bin-dir build/dev --video <clip>... --index-version-name siglip2-b16-224
```

Test media is never committed. The scripts take clip paths as arguments and
match evaluation videos by sha256, so the question sets in `eval/search/` are
reproducible without redistributing footage. Public sample clips come from the
`supervision` package's assets; the rest are private files on the developer's
machine.

## Rules for changes

- C++20 with Qt 6.9 and GStreamer 1.26 only. No vendored libraries. Python 3.12
  for the worker; scripts under `scripts/` are standard library only and must
  run on Windows as well.
- Zero compiler warnings. macOS builds with `-Wall -Wextra -Wpedantic -Wshadow
  -Wconversion`; MSVC with `/W4`. Windows must compile everything, so no
  POSIX-only calls without a `_WIN32` path.
- No emojis anywhere. No comments that restate the code; comment only what is
  not obvious, and say why rather than what.
- The core owns every SQLite write. The console asks the API. The worker never
  touches the database or the recordings.
- A deviation from a milestone design goes into that design document in the
  same change, with the reason. The documents are the contract; silent drift is
  worse than an ugly note.
- Never claim something works without naming the run that showed it. If a test
  was skipped, say so. `docs/STATUS.md` distinguishes "verified" from "not
  verified" and that distinction is the point of the file.

## Traps that have already cost time

- **GStreamer on the PATH**: source `scripts/env.sh` before anything, or an old
  Anaconda GStreamer wins and pipelines fail in confusing ways.
- **`qt_add_executable` on Windows** builds GUI-subsystem binaries whose stdout
  disappears; tests and tools call `fovea_console_executable(<target>)`.
- **Windows will not delete a file that is memory-mapped.** The index removes
  vector files only while no search is scanning; a test that maps a file must
  close its reader before asserting a removal.
- **Python `time.monotonic` is not C++ `steady_clock`** on macOS or Windows.
  The worker compares times through `fovea_worker.clock.mono_ns` only.
- **Two threads importing torch or transformers at once** made both imports
  fail and the worker restart in a loop. Model loads take one process-wide lock
  (`fovea_worker/modelload.py`); inference does not.
- **`Path.with_suffix` cuts a name at its last dot**, which silently overwrote
  report files whose names carried a decimal number. Append the extension.
- **GStreamer on Windows** finds its plugins at `<dll dir>/../lib/gstreamer-1.0`.
- Model weights are never downloaded at run time. `Setup-Worker.cmd` on Windows
  and `scripts/fetch-models.sh` on macOS put them in the Hugging Face cache.

## Publishing

`main` is the branch to work on. Releases are tags (`v*`) that make CI build,
test, package and attach `fovea-windows-x64.zip` as a prerelease. CI runs the
Windows build and tests, the macOS build and tests, the packaged M1
verification, and the worker tests on Ubuntu and Windows.

## Where the work continues

M0 to M4 are implemented and verified on macOS (`docs/STATUS.md`). M5 is
designed but not started: `docs/M5_DESIGN.md` is the contract and
`docs/M5_PLAN.md` breaks it into the order to build it in, with what each step
has to show before it counts as done.
