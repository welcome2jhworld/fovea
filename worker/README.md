# fovea-worker

Model worker for fovea-core: detection with tracking, frame and query embeddings
for search, and VLM clip jobs, over a loopback JSON API. Job and result contracts
are in `fovea_worker/protocol.py` and `fovea_worker/embedding.py`.

Setup: `scripts/setup-worker.sh [torch|mlx|torch,mlx]` on macOS,
`scripts\windows\setup-worker.ps1 [-Cpu]` on Windows (CUDA 12.8 torch wheels when
`nvidia-smi` lists a GPU). The Windows executable is `worker\.venv\Scripts\fovea-worker.exe`,
elsewhere `worker/.venv/bin/fovea-worker`.

## serve

```
fovea-worker serve --backend dry --detector rfdetr --port 0 \
  --token-file <data>/worker.token --info-file <data>/worker.json \
  [--warmup] [--exit-on-stdin-eof] [--embedder transformers|dry|none] \
  [--embed-preload siglip2-b16-224,qwen3vl-emb-2b-1024]
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
- `--embedder`: the embed lane backend. `transformers` (default) loads each
  index version's model from the Hugging Face cache on its first embed job
  (never downloads); `dry` returns deterministic unit vectors with status
  `dry_run` and model revision `dry` (the core does not store them); `none`
  answers embed jobs with 503 `backend_unavailable`.
- `--embed-preload`: load these index versions and embed one text right after
  binding, so the first query does not pay for model load. Model loads are
  serialised process-wide (one lock), because importing torch and transformers
  from two threads at once made both imports fail; inference is not.

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

The embed lane reports the same keys prefixed `embed_` (`embedder` is its
backend name), plus `embed_versions`: per enabled index version `{model_id,
dims, state, load_ms, load_error, device}`, where state is that model's
`unloaded`, `loading`, `ready` or `failed`. Because index version models load
inside their first job, `embed_busy_ms` of that job includes the load.

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

## Embedding

Index versions (`fovea_worker/embedding.py`):

| Name | Model | Dims | Compute dtype | Frames | Queries |
| --- | --- | --- | --- | --- | --- |
| `siglip2-b16-224` | google/siglip2-base-patch16-224 | 768 | float32 | squashed to 224x224, pooled image tower | lowercased `this is a photo of {text}.`, padding max_length 64 |
| `qwen3vl-emb-2b-1024` | Qwen/Qwen3-VL-Embedding-2B | 1024 (MRL) | float16 on MPS/CUDA, float32 on CPU | default instruction, at most 262144 px (256 visual tokens) | retrieval instruction, last-token pooling |

Jobs:

- `embed_frames {job_id, generation, index_version_name, frames[1..32],
  sample_interval_ms (default 1000), priority (default "index"), limits.deadline_ms}`
- `embed_text {job_id, generation, index_version_name, texts[1..8],
  sample_interval_ms, priority (default "query"), limits.deadline_ms}`

Result (HTTP 200): `{job_id, generation, status (ok|error|dry_run), model,
model_version (<model_id>@<revision>), index_version, descriptor, dims, count,
vectors, frame_ids, per_frame_ms, processing_ms, load_ms, device, error, notes,
contract_violations}`. `vectors` is base64 of `count x dims` float32
little-endian L2-normalised rows in input order; `frame_ids` repeats the job's
frame ids in order (empty for texts); `per_frame_ms` has one entry per input
(its batch's compute time split evenly, JPEG decode included); `load_ms` is
non-zero when this job loaded the model.

`descriptor` is `{name, model_id, model_revision (Hugging Face snapshot
commit), preprocessing, dims, dtype, sample_interval_ms, prompt_template}` and
`index_version` is the first 12 hex digits of the SHA-256 of its compact JSON
with sorted keys. Descriptors hold only ASCII strings, integers and booleans,
so Qt's `QJsonDocument::Compact` output of the same object hashes identically.
A text query must name the same index version and sample interval as the
frames it searches, or its hash differs.

Scheduling: the embed lane is separate from the detector and VLM lanes. Waiting
`query` jobs start before waiting `index` jobs, and a running job hands the lane
to a waiting query between batches (16 frames for SigLIP 2, 2 for
Qwen3-VL-Embedding), so a query waits for at most one batch of a running job
(plus a model load that is already in progress). A model that cannot
load answers 503 `backend_load_failed`; an unreadable frame or invalid output
gives status `error` with no vectors.

```
fovea-worker embed-bench <frames-dir> --index siglip2-b16-224|qwen3vl-emb-2b-1024 [--batch 16] [--repeat 1]
fovea-worker embed-query "<text>" ["<text>" ...] --against <frames-dir> --index <name> [--top 5]
```

`embed-bench` prints per-frame ms p50/p95 over full batches after a warmup
batch, single-text query ms, import and load ms, peak RSS and device memory, and
vector bytes per frame. `embed-query` embeds every image under the directory
(recursively), prints the top frames per query and the best score per
first-level subdirectory. Scores are cosine similarities for ranking only.

## Benchmarks

`fovea-worker bench-serve <frames> [--cameras N] [--fps F] [--pace realtime|max]
[--stop signal|stdin] [--budget-p95-ms MS]` starts `serve` the way the core does
(`--warmup --exit-on-stdin-eof`, token and info files in a temporary directory),
sends one-frame detect jobs one at a time, round robin over N cameras, and
prints round-trip and worker latency, tracking per camera, health, and how the
worker exited.
