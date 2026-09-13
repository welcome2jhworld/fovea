# Environment (surveyed 2026-09-13)

## Development machine (macOS)

| Item | Value |
| --- | --- |
| Hardware | MacBook Pro, Apple M4 (4P+6E), 16 GB unified memory |
| OS | macOS (Darwin 25.3.0), Xcode 26.6, Apple clang 21 |
| CMake / Ninja | 4.0.1 / 1.12.1 (Homebrew) |
| Qt | 6.9.0 (Homebrew `qt`, /opt/homebrew/opt/qt): Core Gui Widgets Network Sql HttpServer WebSockets Test Concurrent Svg Multimedia Charts |
| GStreamer | 1.26.1 (Homebrew `gstreamer`, monolithic build with base/good/bad/ugly/libav/rs plugins, gst-rtsp-server) at /opt/homebrew/opt/gstreamer |
| ffmpeg | 7.1.1 (libx264, libx265, videotoolbox) |
| SQLite | 3.51.0 (Homebrew); QtSql bundles its own driver |
| Python | /opt/anaconda3/bin/python3 3.13.5 with torch 2.7.1 (MPS) and transformers 4.46.3; Homebrew python 3.12/3.13 bare; `uv` available |
| GPU | Apple M4 GPU (Metal/MPS). No CUDA. |

Pitfalls:

- `PATH` puts anaconda's GStreamer 1.14.1 before Homebrew. Every script exports
  `/opt/homebrew/opt/gstreamer/bin` first and sets `PKG_CONFIG_PATH`
  (`scripts/env.sh`).
- Homebrew plugin `libgstadaptivedemux2` fails to load (missing nettle). Not
  used by Fovea; the warning is harmless.
- Model weights need 3 to 20 GB of free disk depending on the set
  (`scripts/fetch-models.sh minimal|m0|all`).

## Target machine (Windows x86-64, NVIDIA 24 GB)

CI builds, tests and packages on GitHub-hosted `windows-2022` runners (no GPU)
with Qt 6.9.0 MSVC 2022 x64 (aqtinstall) and GStreamer 1.26.11 MSVC x86_64
(official MSIs, checksums pinned in `scripts/windows/install-gstreamer.ps1`).
A CI pass is a build and CPU-path check, not a validation on the target GPU
machine; those results are recorded separately in STATUS.md.

## Pinned versions

| Component | Version | License |
| --- | --- | --- |
| Qt | 6.9.0 | LGPL-3.0 (dynamic linking) |
| GStreamer | 1.26.1 (macOS), 1.26.11 (Windows) | LGPL-2.1 (plugins vary; x264 is GPL and is used only by the local test tool) |
| SQLite | 3.51.0 | Public domain |
| IBM Plex Sans / Mono | 6.4.0 (planned bundle) | OFL-1.1 |

Model pins live in `docs/models/candidates.md` once the survey completes.
