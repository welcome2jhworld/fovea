# Fovea

Local video monitoring for existing CCTV cameras. Watch and record RTSP
cameras, search recordings in natural language, and get alerts for rules you
define, each with the evidence clip. Everything runs on one machine: no cloud
service, no remote video upload, no internet connection required at runtime.

**Status: early development.** The live view and recording path works end to
end. Rules, search and the VLM features are being built milestone by milestone.
See [docs/STATUS.md](docs/STATUS.md) for what is verified today and
[docs/PLAN.md](docs/PLAN.md) for the plan.

## What works now

- Add RTSP cameras (or local video files), live wall 1x1 / 2x2 / 3x3, connection
  status, automatic reconnect with receive-gap tracking.
- Continuous recording in 10-600 s Matroska segments without re-encoding,
  crash-safe (a killed process leaves a playable file), startup recovery.
- Playback of recorded segments.
- Local RTSP test source (test pattern, video file loop, or this machine's
  webcam) for trying everything without a camera.

In progress: person-in-zone rule with evidence clips and alerts (M3), 4-camera
soak and retention (M2), natural-language search (M4).

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
4. Self test (needs Python 3.10+): `python verify_m1.py --bin-dir bin --flat`

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

Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md),
[docs/API.md](docs/API.md), [docs/DATA_CONTRACTS.md](docs/DATA_CONTRACTS.md),
[docs/M3_DESIGN.md](docs/M3_DESIGN.md), [docs/DECISIONS.md](docs/DECISIONS.md).

## Model worker (optional today)

```
./scripts/setup-worker.sh torch        # macOS/Linux; mlx extra for Apple silicon
./scripts/fetch-models.sh minimal      # downloads pinned weights to the HF cache
worker/.venv/bin/fovea-worker detect-frames <frames-dir>
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

## Security notes

The core API binds to 127.0.0.1 only and requires a per-install token
(`core.token`, owner-only permissions). Camera passwords are stored separately
from camera URLs and are redacted from logs. The current credential store is a
permission-restricted file; OS keychain integration is planned.

## License

Apache-2.0. Third-party components: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
