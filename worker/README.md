# fovea-worker

Model worker for fovea-core: detection with tracking, and VLM clip jobs, over a
loopback JSON API. Job and result contracts are in `fovea_worker/protocol.py`.

Setup: `scripts/setup-worker.sh [torch|mlx|torch,mlx]` on macOS,
`scripts\windows\setup-worker.ps1 [-Cpu]` on Windows (CUDA 12.8 torch wheels when
`nvidia-smi` lists a GPU). The Windows executable is `worker\.venv\Scripts\fovea-worker.exe`,
elsewhere `worker/.venv/bin/fovea-worker`.

## serve

```
fovea-worker serve --backend dry --detector rfdetr --port 0 \
  --token-file <data>/worker.token --info-file <data>/worker.json \
  [--warmup] [--exit-on-stdin-eof]
```

- Binds `127.0.0.1:<port>` (0 picks a free port). Every request needs
  `Authorization: Bearer <token>`.
- Once the socket is listening, the info file is written through a temporary
  file in the same directory and renamed, so a reader sees either no file or
  the whole JSON: `{port, pid, backend, model, detector, detector_model,
  started_utc_ms}`. The same JSON is the only line written to stdout; model
  libraries log to stderr. On a clean exit the file is removed if it still
  carries this pid. A parent should delete a stale file before starting the
  worker and compare `pid`.
- Stop: SIGTERM or SIGINT on POSIX, CTRL_BREAK_EVENT on Windows (the child
  must be started with `CREATE_NEW_PROCESS_GROUP`). The worker stops accepting
  connections, waits up to 5 s for jobs in a lane, and exits 0.
- `--exit-on-stdin-eof`: the worker also stops when its stdin reaches end of
  file. A parent that starts the worker with a stdin pipe and never writes to
  it ties the worker's lifetime to its own: closing the pipe stops the worker,
  and so does the parent dying for any reason, including SIGKILL or a crash,
  where no signal would be sent. This is the stop path to use on Windows when
  the parent has no console to deliver CTRL_BREAK.
- `--warmup`: after binding, load the detector and run one inference on a
  blank 960x540 frame in the background, so the first real job does not pay
  for model load (about 10 s on MPS) and kernel compilation. Jobs that arrive
  meanwhile wait in the detector lane within their deadline. Without it the
  detector loads on the first detect job.

## GET /v1/health

VLM lane keys are unprefixed, detector lane keys carry `detector_`:

| Key | Meaning |
| --- | --- |
| `backend` / `detector` | backend name |
| `model` / `detector_model` | model version |
| `state` / `detector_state` | `unloaded`, `loading`, `ready`, `failed` (last load raised; the next job retries), `unavailable` (no backend) |
| `loaded` / `detector_loaded` | `state == ready` |
| `device` / `detector_device` | `mps`, `cuda`, `cpu`, `mlx` or empty before load |
| `load_ms` / `detector_load_ms` | duration of the last successful load, warmup inference included |
| `load_error` / `detector_load_error` | redacted error of the last failed load, empty after a success |
| `busy_ms` / `detector_busy_ms` | how long the lane has been running its current job, model load excluded; 0 when idle (the core restarts a worker whose detector stays busy over 30 s) |
| `turnaround_ms` / `detector_turnaround_ms` | `{jobs, p50, p95}` over the last 200 jobs of the lane, from request accepted to result ready, lane wait included; the job that loaded the model is not counted |

## Tracking across jobs

The detector keeps one ByteTrack instance per `camera_id:session_id` and the
last pts it saw, so a stream of single-frame jobs tracks exactly like one job
carrying the same frames. The tracker's rate is the median of the last 5 pts
gaps across jobs (2 fps until 3 gaps are known). Track ids are
`<process token>.<epoch>-<id>`; a new epoch starts when pts goes backwards,
when the gap to the previous frame exceeds both the lost-track window at the
current rate and the job's `max_gap_ns` (the longest gap the camera's rules
bridge), and after 120 s without jobs for that key. A job's `threshold` is also
the lowest score that starts a track, so every reported detection can get an id.

Environment knobs: `FOVEA_DETECTOR_MODEL` (`nano`, `small`),
`FOVEA_DETECTOR_TRACE` (`0` keeps the eager model), `FOVEA_TRACK_ACTIVATION`,
`FOVEA_TRACK_LOST_BUFFER`, `FOVEA_TRACK_MATCH_THRESHOLD`,
`FOVEA_TRACK_BOX_BUFFER` (box padding for association, default 0.3).

## Benchmarks

`fovea-worker bench-serve <frames> [--cameras N] [--fps F] [--pace realtime|max]
[--stop signal|stdin] [--budget-p95-ms MS]` starts `serve` the way the core does
(`--warmup --exit-on-stdin-eof`, token and info files in a temporary directory),
sends one-frame detect jobs one at a time, round robin over N cameras, and
prints round-trip and worker latency, tracking per camera, health, and how the
worker exited.
