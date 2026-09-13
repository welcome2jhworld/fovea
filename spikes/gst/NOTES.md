# Spike 1: GStreamer record + decode pipeline (RTSP H.264 and local mp4)

Verified 2026-09-13 on macOS arm64 (M4), GStreamer 1.26.1 (Homebrew), ffmpeg 7.1.1, Apple clang 21.
Everything below was run on this machine; numbers are from those runs.

## Files

| file | purpose |
|---|---|
| `env.sh` | PATH/PKG_CONFIG_PATH for Homebrew GStreamer (the anaconda 1.14 on PATH shadows it). Source it first. |
| `rtsp_server.cpp` | gst-rtsp-server test server, `rtsp://127.0.0.1:8554/test`. `live` mode (videotestsrc + x264enc, keyframe every 25 frames, burnt-in time) or `file <clip.mp4>` (no re-encode, does not loop). |
| `pipe_probe.cpp` | Runs a launch string ending in `appsink name=sink`; prints caps, LATENCY query, per-frame PTS / running time / clock / monotonic time, rtpjitterbuffer stats; `--loop` implements gapless file looping. |
| `CMakeLists.txt` | Builds both. |
| `make_clip.sh` | Test clip generator (ffmpeg). |
| `record_rtsp.sh` | RTSP -> tee -> segments + decode, muxer variants, `UNBUFFERED=1`, INT or KILL stop. |
| `crash_test.sh` | kill -9 test for mp4 / fmp4 / mkv. |
| `file_input.sh` | Local mp4 paced at real time -> same tee. |
| `runfor.sh` | `timeout` replacement (macOS has none): run N seconds then SIGINT (clean EOS) or SIGKILL. |

Build (out of tree, scratch dir; ~400 KB):

```
source spikes/gst/env.sh
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S spikes/gst -B "$SCRATCH/build" && ninja -C "$SCRATCH/build"
"$SCRATCH/build/rtsp_server" live &
```

Toolchain caveats hit:
- Homebrew `libffi.pc` advertises `/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk/usr/include/ffi`, which does not exist; `pkg_check_modules(... IMPORTED_TARGET)` fails on it. The CMakeLists filters non-existent include dirs instead.
- zsh does not word-split unquoted `$VAR`, so a pipeline kept in a variable is passed as one argument ("erroneous pipeline: syntax error"). All scripts are `/bin/sh`.
- gst-launch messages come out in Korean; use `LC_ALL=C`. `identity silent=false` output only shows with `-v`.

## Test clip

`make_clip.sh clip.mp4`: `testsrc2` 640x360 25 fps 20 s, libx264 `-g 25 -keyint_min 25 -sc_threshold 0 -bf 0`, drawtext frame counter + pts (Arial.ttf is present at `/System/Library/Fonts/Supplemental/Arial.ttf`). Result: 2,199,023 bytes, 500 frames, 20 keyframes.

## RTSP server

gst-rtsp-server worked; the ffmpeg `-rtsp_flags listen` fallback was not needed. Launch string (live mode):

```
( videotestsrc is-live=true pattern=ball ! video/x-raw,width=640,height=360,framerate=25/1
  ! timeoverlay ! x264enc tune=zerolatency speed-preset=veryfast key-int-max=25 bframes=0 bitrate=800
  ! h264parse config-interval=1 ! rtph264pay name=pay0 pt=96 )
```

Shared factory (all clients get the same pipeline), TCP + UDP allowed. Client sees `video/x-h264, stream-format=avc, alignment=au, profile=constrained-baseline, 640x360, 25/1`.

File mode `( multifilesrc location=clip.mp4 loop=true ! qtdemux ! h264parse config-interval=1 ! rtph264pay name=pay0 pt=96 )` serves the clip without re-encoding but does not loop: qtdemux hits EOS after one pass (client got EOS at 21.0 s with latency=1000). Good for <= 20 s tests only; use live mode for long runs. `filesrc` behaves the same.

## RTSP -> tee -> segments + decoded BGRA (verified)

```
gst-launch-1.0 -e rtspsrc location=rtsp://127.0.0.1:8554/test protocols=tcp latency=1000 \
  ! rtph264depay ! h264parse ! tee name=t \
  t. ! queue ! splitmuxsink location=/path/seg_%05d.mkv max-size-time=10000000000 \
       muxer=matroskamux sink="filesink buffer-mode=unbuffered" \
  t. ! queue leaky=downstream max-size-buffers=5 max-size-time=0 max-size-bytes=0 \
     ! avdec_h264 ! videoconvert ! video/x-raw,format=BGRA ! appsink sync=false
```

- `-e` matters: SIGINT then becomes EOS and splitmuxsink finalizes the open file. Without it the last file is truncated exactly like a crash.
- 25 s runs, all three containers: `seg_00000` 250 frames / 10.000 s, `seg_00001` 250 frames / 10.000 s, `seg_00002` 4.5-4.8 s partial finalized on EOS. Every file starts on a keyframe (ffprobe `key_frame=1` on frame 0) and probes cleanly. With a 1 s GOP the cut lands exactly on 10.000 s; with longer GOPs the cut is the first keyframe after `max-size-time` (`send-keyframe-requests` only helps when an encoder is in the pipeline).
- Decode branch: `videoconvert ! video/x-raw,format=BGRA` negotiates after avdec_h264 (I420), vtdec and vtdec_hw (NV12).

### splitmuxsink muxer selection (trap)

gst-inspect: `muxer` is "Valid only for async-finalize = FALSE"; `muxer-factory`, `muxer-preset`, `muxer-properties` are "Valid only for async-finalize = TRUE". Verified consequences:

| syntax | result |
|---|---|
| `muxer-factory=mp4mux muxer-properties=properties,fragment-duration=1000` (default async-finalize=false) | silently ignored: files identical to plain mp4mux, 0 `moof` |
| `muxer="mp4mux fragment-duration=1000"` | works: 10 `moof` per 10 s file |
| `muxer=matroskamux` | works |
| `async-finalize=true muxer-factory=mp4mux muxer-properties="properties,fragment-duration=1000"` | works (10 moof) |
| `async-finalize=true muxer-factory=matroskamux` | works, but in one run seg_00000 had 225 frames / 9 keyframes (one GOP missing at start); not investigated, prefer the `muxer=` form |

In C++: `g_object_set(splitmux, "muxer", gst_element_factory_make("matroskamux", nullptr), "sink", filesink, nullptr)`.

## Crash test (kill -9 at 16 s, second file in progress)

Expected content of the in-progress file at the kill: about 4.5-5 s (112-125 frames). Completed 10 s files were intact (250 frames) in every case.

| container | default filesink (64 KB userspace buffer) | `sink="filesink buffer-mode=unbuffered"` |
|---|---|---|
| mp4mux (plain) | unplayable: `moov atom not found` (132 KB on disk) | unplayable (141 KB) |
| mp4mux `fragment-duration=1000` | playable, 56 frames / 3.0 s (3 moof) | playable, 100 frames / 4.0 s (4 moof) |
| matroskamux | playable, 100 frames (duration N/A, no cues) | playable, 125 frames (duration N/A) |

Judged with `ffprobe -count_frames`. filesink's default buffering loses up to 64 KB (2+ s at this bitrate) on a kill; `buffer-mode=unbuffered` hands every buffer to the kernel, which survives a process crash (not a power cut; `sync=true` would fsync per buffer).

Recommendation: **matroskamux + unbuffered filesink** for the live recording. It loses at most the current cluster (default `min-cluster-duration` 500 ms) and needs no repair to play. The file lacks duration/cues until finalized; `ffmpeg -i in.mkv -c copy out.mkv` or a remux to mp4 fixes that offline. fMP4 (`fragment-duration=500..1000`) is the alternative when mp4 is required: loses up to one fragment. Plain mp4mux is unusable for a recorder that may die. Not tested: mp4mux `reserved-max-duration` + `reserved-moov-update-period` (periodically rewritten moov).

## Decoders

| element | rank | output | `videoconvert ! BGRA` | CPU, 15 s of 640x360@25 to BGRA, appsink sync=false | RSS |
|---|---|---|---|---|---|
| avdec_h264 | primary (256) | I420, bt601 | ok | 0.58 s user + 0.12 s sys (390 frames) ~4.4% of one core | 40 MB |
| vtdec | secondary (128) | NV12 | ok | 0.57 s user + 0.12 s sys (369 frames) ~4.6% | 33 MB |
| vtdec_hw | primary+1 (257) | NV12 | ok | same as vtdec | |

At this resolution they are indistinguishable; videoconvert to BGRA is likely the larger cost. vtdec accepts `video/x-h264, stream-format=avc, alignment=au` (h264parse converts) and also H.265 hev1/hvc1. In simultaneous runs vtdec delivered frames ~50 ms later than avdec_h264 (VideoToolbox is asynchronous), avdec_h264 within 1-6 ms of the buffer's running time. `decodebin`/`playbin` would autoplug vtdec_hw first; pick explicitly.

### appsink sync (important)

With `appsink sync=true` (or `fakesink sync=true`) every decoded frame is held in the sink until `running time + pipeline latency` (= rtspsrc `latency`). The decoder is blocked while the sink waits, so the 5-buffer leaky queue in front of it overflows and drops:

| config (rtspsrc latency=300) | delivered |
|---|---|
| leaky queue 5 -> avdec_h264 -> sink sync=true | 5.4 fps (another run: 12.5 fps), every dropped frame a PTS gap |
| leaky queue 5 -> vtdec -> sink sync=true | ~19 fps |
| plain `queue` -> avdec_h264 -> sink sync=true | 283 of 287 frames |
| leaky queue 50 -> avdec_h264 -> sink sync=true | 24.3 fps, 0 gaps |
| leaky queue 5 -> avdec_h264 -> appsink sync=false | 24.8 fps, 0 gaps, delivered 1-6 ms after running time |

For the analysis branch use `appsink sync=false` (frames as soon as decoded; the leaky queue then only drops when the consumer really is slower than the stream). If a sink must sync (display), size its queue to at least `latency / frame period` buffers. Side effect: with `sync=false` the LATENCY query reports `live=0 min=0` (basesink only takes part when syncing); harmless.

## rtspsrc / rtpjitterbuffer properties (1.26.1)

`add-reference-timestamp-meta` exists on rtspsrc (Boolean, default false): "Add Reference Timestamp Meta to buffers with the original clock timestamp before any adjustments when syncing to an RFC7273 clock". Only meaningful when the SDP carries an RFC 7273 clock.

rtspsrc, relevant properties with defaults:
- `protocols` flags, default `tcp+udp-mcast+udp` (0x7); `protocols=tcp` forces interleaved TCP.
- `latency` ms, default 2000 (jitterbuffer size).
- `timeout` us, default 5,000,000: retry over TCP after this UDP timeout. `tcp-timeout` us, default 20,000,000: fail after this on TCP. `teardown-timeout` ns, default 100 ms.
- `retry` default 20: RTP port allocation retries (not reconnection). `udp-reconnect` default true. There is no built-in reconnect for TCP: handle ERROR/EOS on the bus and rebuild.
- `drop-on-latency` default false; `do-retransmission` default true (RTX, needs server support); `ntp-sync` default false; `ntp-time-source` default ntp; `buffer-mode` default auto; `max-ts-offset` 3 s; `do-rtsp-keep-alive` true; `is-live` true; `onvif-mode` false; `user-agent`.
- Signals: `new-manager` (rtpbin), `select-stream`, `on-sdp`, `before-send`, `handle-request`, `accept-certificate`.

rtpjitterbuffer: `stats` (GstStructure `application/x-rtp-jitterbuffer-stats`: `num-pushed`, `num-lost`, `num-late`, `num-duplicates`, `avg-jitter` ns, `rtx-count`, `rtx-success-count`, `rtx-per-packet`, `rtx-rtt`), `latency`, `drop-on-latency`, `post-drop-messages` + `drop-messages-interval` (bus messages per dropped packet), `max-dropout-time` (default 60000 ms), `faststart-min-packets`, `rtx-stats-timeout`. Access from rtspsrc: connect `new-manager`, then the manager's `new-jitterbuffer` signal (done in `pipe_probe`). Sample after 13 s TCP loopback: `num-pushed=1942 num-lost=1 num-late=0 num-duplicates=0 avg-jitter=342024`.

## Delay measurements (pipe_probe, TCP loopback)

Programmatically available per sample: buffer PTS, `gst_segment_to_running_time(sample segment, PTS)`, clock running time `gst_clock_get_time(pipeline clock) - base_time`, `g_get_monotonic_time()`, plus the pipeline LATENCY query. `age = clock running time - buffer running time` is the delay between the stream timeline and delivery.

| rtspsrc latency | appsink | time to first decoded frame after PLAYING | age of delivered frames | LATENCY query |
|---|---|---|---|---|
| 300 | sync=false | 330 ms | 1-6 ms (avdec) | live=0 min=0 (sync=false) |
| 1000 | sync=false | 1029 ms | 4-7 ms (avdec) | live=0 min=0 |
| 300 | sync=true | 476 ms | 320-325 ms | live=1 min=320 ms (300 jitterbuffer + 20 ms parse/decode) |

So with in-order TCP delivery the `latency` value is paid once at start (initial buffering) and, for syncing sinks, as a constant hold in the sink; it does not delay a `sync=false` consumer per frame. The absolute age offset varies a few tens of ms between runs (it is set by the jitterbuffer's initial base), so compare decoders only within simultaneous runs. Glass-to-glass was not measured; the server burns its running time into the frames (`timeoverlay`) if OCR-based measurement is wanted later.

## Local mp4 input, real-time paced (verified)

```
gst-launch-1.0 -e filesrc location=clip.mp4 ! qtdemux ! h264parse ! identity sync=true ! tee name=t \
  t. ! queue ! splitmuxsink location=seg_%05d.mkv max-size-time=10000000000 muxer=matroskamux \
  t. ! queue leaky=downstream max-size-buffers=5 max-size-time=0 max-size-bytes=0 \
     ! avdec_h264 ! videoconvert ! video/x-raw,format=BGRA ! fakesink sync=false
```

500 frames over 19.97 s, two 250-frame 10.000 s MKV files, both starting on a keyframe; 1.15 s user CPU over 20 s. `clocksync` in place of `identity sync=true` behaves the same (used in the loop runs). Both return NO_PREROLL, i.e. the pipeline becomes live-like: `gst_element_get_state` does not wait for preroll and there is no ASYNC_DONE.

### Looping the file (pipe_probe --loop, verified)

Recipe, 32 s run on the 20 s clip: 795 frames at 25.0 fps, 0 PTS gaps, `seg_00000..2` exactly 250 frames each (the wrap at 20 s is inside seg_00002), `seg_00003` 45 frames finalized on EOS. MKV files carry running time (start 0/10/20/30 s); mp4mux files each start at 0.

1. Name the demuxer (`qtdemux name=demux`) and send seeks to it, not to the pipeline: `gst_element_seek(pipeline)` returns FALSE because splitmuxsink refuses seek events and the bin ANDs the sink results.
2. Set PAUSED. On qtdemux `no-more-pads` (hop to the main loop with `g_idle_add`) do a flushing seek with `GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_SEGMENT` to 0, then set PLAYING. In PAUSED the first buffer is still blocked in clocksync, so nothing has reached the muxer yet and the flush is harmless.
3. On `GST_MESSAGE_SEGMENT_DONE` seek again with `GST_SEEK_FLAG_SEGMENT` only (no flush) to 0: running time continues (PTS 0 arrived at running time 20000 ms), splitmuxsink keeps counting.
4. To stop: `gst_element_send_event(pipeline, eos)`; the source then returns EOS to qtdemux, which in segment mode posts SEGMENT_DONE instead of pushing EOS. On that SEGMENT_DONE push an EOS event out of the demuxer src pads (`gst_element_foreach_src_pad`), splitmuxsink finalizes, the pipeline posts EOS.

Pitfalls found on the way:
- Seeking right after `set_state(PAUSED)` fails: with NO_PREROLL `get_state` returns immediately and the demuxer has no pads yet (seek created at 0.202 s, qtdemux parsed the file at 0.206 s).
- A flushing seek while data flows corrupts the splitmuxsink file in progress (seg_00000 unreadable, ffprobe garbage), and any seek issued after data flows records the first ~1 s twice (seg_00000 9.04 s / 227 frames).
- Re-seeking on SEGMENT_DONE after EOS was sent loops forever (SEGMENT_DONE storm, 172 MB of log in a minute). Hence the `stopping` flag.
- A STREAM_START bus message never arrives in PAUSED with clocksync (the muxer emits stream-start only after its first buffer), so it cannot be the trigger.

Server-side looping of an mp4 over RTSP is unsolved (`multifilesrc loop=true ! qtdemux` ends after one pass); the same segment-seek approach inside a custom GstRTSPMedia would be the fix if ever needed.

## Reproduce

```
source spikes/gst/env.sh; export LC_ALL=C
spikes/gst/make_clip.sh "$SCRATCH/clip.mp4"
cmake -G Ninja -S spikes/gst -B "$SCRATCH/build" && ninja -C "$SCRATCH/build"
"$SCRATCH/build/rtsp_server" live &
UNBUFFERED=1 spikes/gst/record_rtsp.sh mkv "$SCRATCH/rec" 25 INT          # segments + decode
UNBUFFERED=1 spikes/gst/crash_test.sh "$SCRATCH"                          # kill -9 table
spikes/gst/file_input.sh "$SCRATCH/clip.mp4" "$SCRATCH/file_rec"          # local file, paced
"$SCRATCH/build/pipe_probe" --loop --seconds 32 "filesrc location=$SCRATCH/clip.mp4 ! qtdemux name=demux ! h264parse ! clocksync ! tee name=t  t. ! queue ! splitmuxsink location=$SCRATCH/loop/seg_%05d.mkv max-size-time=10000000000 muxer=matroskamux  t. ! queue leaky=downstream max-size-buffers=5 max-size-time=0 max-size-bytes=0 ! avdec_h264 ! videoconvert ! video/x-raw,format=BGRA ! appsink name=sink sync=false"
pkill -f build/rtsp_server
```
