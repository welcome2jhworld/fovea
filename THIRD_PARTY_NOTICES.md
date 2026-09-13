# Third-party notices

Fovea is licensed under Apache-2.0 (see LICENSE). Binary packages bundle or
depend on the components below, each under its own license.

| Component | Version | License | Use |
| --- | --- | --- | --- |
| Qt (Core, Gui, Widgets, Network, Sql, HttpServer, WebSockets, Svg, Multimedia) | 6.9.0 | LGPL-3.0, dynamically linked | UI, HTTP API, SQLite driver |
| GStreamer core, plugins base/good/bad | 1.26 | LGPL-2.1 | Camera input, recording, decoding |
| GStreamer libav plugin + FFmpeg libraries | 1.26 | LGPL-2.1 | H.264/H.265 software decoding |
| x264 (GStreamer x264 plugin) | bundled with GStreamer | GPL-2.0 | Only used by the rtsp-testsrc test tool |
| OpenH264 | bundled with GStreamer | BSD-2-Clause | Test tool encoder fallback |
| SQLite | bundled with Qt | Public domain | Metadata store |
| IBM Plex Sans / IBM Plex Mono | 6.x | SIL OFL 1.1 (src/console/resources/fonts/LICENSE.txt) | UI fonts |

Model worker (optional, installed separately, not bundled):

| Component | License |
| --- | --- |
| PyTorch | BSD-3-Clause |
| Hugging Face Transformers | Apache-2.0 |
| mlx, mlx-vlm (macOS) | MIT |
| rfdetr (RF-DETR Nano/Small code and weights) | Apache-2.0 |
| supervision (ByteTrack) | MIT |

Model weights are downloaded by the user from their publishers and keep their
own licenses: Qwen/Qwen3.5-4B and Qwen/Qwen3-VL-Embedding-2B (Apache-2.0),
NemoStation/Marlin-2B (Apache-2.0, gated), google/siglip2-base-patch16-224
(Apache-2.0), RF-DETR Nano/Small (Apache-2.0).

A distribution that includes the x264 plugin is subject to the GPL for that
plugin. Production packages should drop it (the test tool falls back to
OpenH264 or the platform encoder).
