# Fovea

Local video monitoring for existing CCTV cameras. Watch and record RTSP
cameras, search recordings in natural language, and get alerts for rules you
define, each with the evidence clip. Everything runs on one machine: no cloud
service, no remote video upload, no internet connection required at runtime.

**Status: early development.** Live view, recording, retention, the first
rule (person in a zone) with alerts and evidence clips, and natural-language
search over recordings work end to end. The VLM features are being built
milestone by milestone.
See [docs/STATUS.md](docs/STATUS.md) for what is verified today and
[docs/PLAN.md](docs/PLAN.md) for the plan.

## What works now

- Add RTSP cameras (or local video files), live wall 1x1 / 2x2 / 3x3, connection
  status, automatic reconnect with receive-gap tracking.
- Continuous recording in 10-600 s Matroska segments without re-encoding,
  crash-safe (a killed process leaves a playable file), startup recovery.
- Playback of recorded segments.
- Headless recording: closing the console keeps the service recording; four
  cameras at 25 fps verified with a 5 and a 10 minute soak, crash restart and a
  disk floor that pauses recording but keeps live view.
- Retention per camera by age (`retention_days`) and size (`max_bytes`), plus
  oldest-first deletion when the disk runs low; segments referenced by an
  event's evidence are held.
- Person detection and tracking (RF-DETR Nano + ByteTrack) in a supervised
  worker process that restarts on failure; frames without a result count as
  unknown, never as empty.
- Rules: "a person stays inside a zone for N seconds during a schedule", with
  one event per occupancy, clear and rearm, console alerts (live event feed,
  sound, optional jump to the camera), an evidence clip and thumbnail per
  event, and acknowledge, resolve and review saved in the local database.
  Detection boxes on live tiles.
- Natural-language search over recordings, in Korean or English: every
  finalized segment is sampled once a second and embedded (SigLIP 2 by default,
  Qwen3-VL-Embedding as a second index version) in the worker; a query returns
  ranked time ranges per camera with thumbnails, how much of the range was
  indexed, and plays the original footage. Results are embedding similarity
  only and the console says so; the index survives restarts and follows
  retention. Video files can be imported into a camera's recordings for
  investigation.
- Local RTSP test source (test pattern, video file loop, or this machine's
  webcam) for trying everything without a camera.

In progress: VLM re-check of search results and follow-up questions (M5),
Windows verification of M2 to M4.

## Try it on Windows (x64)

1. Download `fovea-windows-x64.zip` from the latest
   [CI run](../../actions/workflows/ci.yml) (Artifacts) or from
   [Releases](../../releases).
2. Unzip anywhere and run `Fovea.cmd` (or `bin\fovea.exe`). The service
   `fovea-core.exe` starts in the background; closing the window keeps
   recording.
3. No camera at hand? In a terminal in the unzipped folder:
   ```
   bin\rtsp-testsrc.exe --pattern ball      # rtsp://127.0.0.1:8554/test
   bin\rtsp-testsrc.exe --webcam            # your webcam as an RTSP camera
   ```
   Then **+ Add** in the console with that URL, or your camera's
   `rtsp://user:pass@ip:554/...` URL.
4. Detection, alerts and search (optional): run `Setup-Worker.cmd` once (needs
   Python 3.12 and internet). It installs PyTorch (CUDA build when an NVIDIA
   GPU is present) and the RF-DETR detector into `worker\.venv` and downloads
   the search embedding model (about 1.5 GB) into the Hugging Face cache; the
   service picks it up on its next start. Then enable "Run analytics on this
   camera" and create a rule with a zone under **Alerts & Analytics**, or type
   a question under **Search** once the index line shows coverage.
5. Self test (needs Python 3.10+): `python verify_m1.py --bin-dir bin --flat`

Data lives in `%LOCALAPPDATA%\Fovea` (override with `FOVEA_DATA_DIR`).

## Build from source

### Windows

Requirements: Visual Studio 2022 (Desktop C++), CMake 3.25+, Ninja,
Qt 6.9 for MSVC 2022 x64 with the Qt HTTP Server, WebSockets and Multimedia
add-ons, GStreamer 1.26 MSVC x86_64 runtime and development packages.

```powershell
# from a "x64 Native Tools" PowerShell
pip install aqtinstall
aqt install-qt windows desktop 6.9.0 win64_msvc2022_64 -O C:\Qt -m qthttpserver qtwebsockets qtmultimedia
./scripts/windows/install-gstreamer.ps1            # official MSIs, checksums pinned
$env:QT_ROOT = "C:\Qt\6.9.0\msvc2022_64"
$env:GSTREAMER_1_0_ROOT_MSVC_X86_64 = "C:\gstreamer\1.0\msvc_x86_64\"
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
./scripts/windows/package.ps1                       # -> dist\fovea
```

### macOS

```
brew install cmake ninja pkgconf qt gstreamer ffmpeg
./scripts/build.sh
./scripts/test.sh
python3 scripts/verify_m1.py --bin-dir build/macos-dev
./scripts/setup-worker.sh torch && ./scripts/fetch-models.sh search   # detection and search
```

`scripts/env.sh` puts Homebrew's GStreamer first on `PATH` (an Anaconda
install may otherwise shadow it with an old GStreamer).

## Architecture

Three processes on one machine:

| Process | Role |
| --- | --- |
| `fovea` | Qt Widgets console: live wall, camera settings, playback, alerts. Never decodes video or touches the database. |
| `fovea-core` | Headless service: GStreamer camera pipelines, recording, shared-memory frame distribution, SQLite metadata, loopback HTTP API, rules and alerts. |
| `fovea-worker` | Python model worker: detection and tracking (RF-DETR + ByteTrack), VLM, embeddings. A separate process so a model crash never stops recording. |

Working on the code: [AGENTS.md](AGENTS.md) has the build, test and
verification commands, the rules for changes and the traps that have already
cost time.

Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md),
[docs/API.md](docs/API.md), [docs/DATA_CONTRACTS.md](docs/DATA_CONTRACTS.md),
[docs/M3_DESIGN.md](docs/M3_DESIGN.md), [docs/M4_DESIGN.md](docs/M4_DESIGN.md),
[docs/DECISIONS.md](docs/DECISIONS.md).

## Model worker (needed for detection and rules)

```
./scripts/setup-worker.sh torch        # macOS/Linux; mlx extra for Apple silicon
./scripts/fetch-models.sh search       # SigLIP 2 for search (1.5 GB); m0 / all add the VLMs
worker/.venv/bin/fovea-worker detect-frames <frames-dir>
worker/.venv/bin/fovea-worker embed-query "a white car" --against <frames-dir> --index siglip2-b16-224
```

Models are not bundled; see [docs/models/candidates.md](docs/models/candidates.md)
for ids, sizes and licenses.

## Tests

- `ctest`: C++ unit tests (frame ring, store and recovery, session clock,
  media probe, rule evaluator, console smoke).
- `cd worker && python -m unittest discover -s tests`: worker contracts,
  parsing and tracking.
- `scripts/verify_m1.py`: integration run against real binaries
  (source -> core -> frames -> segments -> reconnect -> playback -> recovery).
  CI runs it on Windows against the packaged zip and on macOS.
- `scripts/verify_m2.py`: 4-input soak, kill -9 recovery, console closed,
  disk floor and retention.
- `scripts/verify_m3.py --seated <clip> [--walking <clip>]`: zone rule, events,
  worker kill, evidence and review against the real detector worker.
- `scripts/verify_m4.py --classroom <clip> --walking <clip> --whitecar <clip>`:
  imports, index resume after a kill without duplicates, queries and filters,
  playback of a result, retention removing results.
- `scripts/eval_search.py --video <clip>... --index-version-name <name>`:
  retrieval metrics on `eval/search/questions.json` (Recall@K, precision,
  temporal error, latency, index cost, negatives); reports land in
  `docs/verification/`.

## Security notes

The core API binds to 127.0.0.1 only and requires a per-install token
(`core.token`, owner-only permissions). Camera passwords are stored separately
from camera URLs and are redacted from logs. The current credential store is a
permission-restricted file; OS keychain integration is planned.

## License

Apache-2.0. Third-party components: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
