# M4 design: natural-language search over recordings

Scope: build a persistent embedding index of recorded footage, answer a
natural-language query with ranked time ranges filtered by camera and time,
play the original footage for each result, and measure retrieval quality on a
fixed question set. VLM re-checking of candidates and follow-up questions are
M5; M4 results are embedding similarity only and are labelled that way.

## What is indexed

- Unit: one sampled frame per `sample_interval_ms` (default 1000 ms) of every
  `finalized` recording segment of cameras with indexing enabled. Frames are
  decoded from the recording file, not from live video, so the index always
  points at footage that exists on disk.
- Stored per sample: `EmbeddingRecord {id, index_version, camera_id,
  session_id, segment_id, pts_ns, utc_ms, vector_file, vector_offset,
  thumbnail_path, deleted}`. The vector itself lives in an append-only binary
  file, the rest in SQLite.
- Sample density and coverage are recorded per segment in `index_jobs`
  (`frames_expected`, `frames_indexed`, `sample_interval_ms`), so a result list
  can say how much of the requested range was actually indexed. A sampled
  frame is never reported as if every frame had been analysed.

## Index versions

`index_version` = short hash of `{model_id, model_revision, preprocessing
(image size, resize mode, normalisation), vector_dims, dtype,
sample_interval_ms, prompt/instruction template}`. Rules:

- As implemented, the worker builds the descriptor `{name, model_id,
  model_revision, preprocessing, dims, dtype, sample_interval_ms,
  prompt_template}` (the version name is included so two names never share a
  hash) and returns it with every embed result; `index_version` is the first 12
  hex digits of the SHA-256 of its compact sorted-key JSON. Descriptors hold only
  ASCII strings, integers and booleans, because those serialise byte-identically
  in Python and in Qt's `QJsonDocument::Compact`, so the core can recompute and
  check the worker's hash. The core sends the version name and sample interval;
  the worker answers with the hash.

- A query runs against exactly one index version (the active one, setting
  `search.active_index_version`). Vectors of different versions are never
  compared, even when their dimensions match.
- Changing the model or preprocessing creates a new version and queues a
  re-index of every retained segment; the old version keeps answering until the
  new one covers the requested range, and the result reports which version and
  what coverage answered.
- Versions no longer active and fully superseded are deleted by an explicit
  API call, not automatically.

Initial versions:

| Version name | Model | Dims | Notes |
| --- | --- | --- | --- |
| `siglip2-b16-224` | google/siglip2-base-patch16-224 | 768 | Cost baseline; multilingual text tower |
| `qwen3vl-emb-2b-1024` | Qwen/Qwen3-VL-Embedding-2B (MRL truncated to 1024) | 1024 | Multilingual image/video retrieval candidate. Deviations from the model repository's script: frames are capped at 262144 px (256 visual tokens, 960x540 becomes 672x384) instead of 1843200, because full-resolution frames cost about 1.6 s per frame on the development Mac and could not index one camera at 1 sample/s in real time; compute is float16 on MPS/CUDA (float32 on CPU), which matched the card's float32 similarities within 0.0002 on MPS at half the memory (docs/verification/embed-bench-20260913-231945.md) |

Both are evaluated on the same question set before either becomes the default.
The rule for that choice, fixed before the evaluation ran: the default is the
version with the higher Recall@5 on the final question set, and on a tie the
one that costs less to index. Measured: `siglip2-b16-224` indexes at 227
compute s per footage hour on the development Mac and meets the throughput
target below; `qwen3vl-emb-2b-1024` needs 3 177 s per footage hour, which is
0.88x real time for one camera at 1 sample/s on that machine, so it does not.

## Pipeline

```
Store: segment finalized (or imported)
  -> index_jobs row {segment_id, index_version, state queued, attempts 0, generation}
IndexScheduler (core, 1 timer, low priority)
  picks the oldest queued job whose camera is indexing-enabled
  -> SegmentSampler: filesrc ! demux ! parse ! avdec ! videorate/sample by pts ! videoscale ! jpegenc ! appsink
     writes <data>/spool/index/<job>/<n>.jpg with pts and utc from the segment's session clock
  -> POST worker /v1/jobs {kind: embed_frames, index_version_name, sample_interval_ms, priority index, frames[<=32 per request], deadline_ms}
  <- index_version (descriptor hash) + descriptor + dims + count + vectors (float32 little-endian, base64) + frame_ids in request order
  -> contract checks (frame ids, dims, model matches the index version)
  -> VectorStore.append + embedding_records insert in one store-thread step
  -> index_jobs done | failed(attempts++, backoff) | skipped(reason)
```

Priorities and budgets (the worker has separate lanes: detection must never
wait for indexing):

- Live detection (M3) keeps its own lane. Index and query embedding run on the
  `embed` lane with one request at a time.
- Index jobs yield to query embedding: a pending query is sent before the next
  index request. Addition in the worker: a query request that arrives while an
  index request is running starts before any waiting index request and takes
  the embed lane between the running request's batches (16 frames for SigLIP 2,
  2 for Qwen3-VL-Embedding), because a 32-frame Qwen request holds the lane for
  about 30 s and would otherwise miss the 3 s query deadline.
- Index throughput target: at least real time for one camera at 1 sample/s on
  the development Mac; measured and reported as seconds of compute per hour of
  footage.
- Restart: jobs left `running` return to `queued`; vectors already appended for
  a job that did not reach `done` are marked deleted before the job retries, so
  a crash never produces duplicate records.
- A job whose segment was deleted before it ran becomes `skipped
  (segment_deleted)`.

## Vector store

- File layout: `<data>/index/<index_version>/<camera_id>/<yyyymmdd>.vec`,
  header `{magic, version, dims, dtype}` then fixed-size records
  `{record_id uint64, utc_ms int64, vector float32[dims]}`. Append only.
- Deletion: retention marks `embedding_records.deleted = 1` in the same
  transaction that marks the segment deleted. Search skips deleted records via
  the SQLite id filter; files are compacted when more than half of a file is
  deleted.
- SQLite and vector files are not updated atomically. Consistency comes from:
  records referencing byte offsets that exist (checked on load), index_jobs
  generations, and a startup check that truncates a trailing partial record
  and drops records whose offset is past the end of the file.
- Search is brute-force cosine similarity over the filtered time and camera
  range in the core (normalised vectors, dot product, top-K heap). FAISS is
  adopted only when a measured query over the retained range exceeds the
  latency budget (DECISIONS D5).

## Query path

```
POST /v1/search {query, from_utc_ms, to_utc_ms, camera_ids[], limit, min_gap_ms, index_version?}
  -> resolve time and camera filter (validated: range <= retained range, cameras exist)
  -> worker embed_text {index_version_name, sample_interval_ms, texts[query], priority query} (embed lane, 3 s deadline)
  -> VectorStore scan of candidate files, top N samples (N = min(limit * 8, candidate_fraction * samples scanned), at least 1)
  -> merge samples of the same camera closer than min_gap_ms (default 3000) into ranges
     range score = max sample score, representative = best sample thumbnail
  -> drop ranges whose segments are deleted; mark partial when part of the range is deleted
  -> SearchSession row {id, query, filters, index_version, model, created_utc_ms, stats}
  <- {session_id, results[{camera_id, start_utc_ms, end_utc_ms, score, representative {utc_ms, thumbnail}, samples, evidence_state}], stats {samples_scanned, hours_scanned, coverage_ratio, embed_ms, scan_ms, total_ms}, index_version, model}
GET /v1/search/{session_id}
GET /v1/search/thumbnails/{record_id}
POST /v1/playback {camera_id, at_utc_ms}  (existing)
```

- `score` is a relevance score for ranking only. The API field is named
  `relevance` and the console never shows it as a probability or percentage of
  certainty.
- `coverage_ratio` = indexed samples in the filtered range / expected samples.
  Below 1.0 the console shows how much of the range was searched.
- Queries and filters are written to `audit_log` (`search.query`).

## Import (for investigation and evaluation)

`POST /v1/imports {path, camera_id, start_utc_ms}` copies an H.264/H.265 MP4 or
MKV into the camera's recordings as `imported` segments (remux, no re-encode,
split at keyframes by the configured segment length), with its own
StreamSession whose `capture_clock` is `imported`. Imported segments follow the
same retention, indexing and playback rules. This is how the evaluation corpus
enters the system through the real pipeline.

## Console: Search tab

Per the handoff (Screen 3): query bar with ASK label, filter chips for time
range and cameras, results grid with thumbnails and a relevance badge, stats
line (`N results · H hours searched · coverage C% · T s`), inspector with the
evidence player, metadata (camera, start, end, index version, model), and an
explicit "similarity only, not verified" note until M5 adds VLM checks.
Reference image search, "Save as alert rule" and "Track subject" are shown as
not implemented.

## Evaluation (scripts/eval_search.py)

- Corpus: videos passed by path (not stored in the repository), imported
  through `POST /v1/imports` into dedicated cameras, then indexed.
- Question set: `eval/search/questions.json` in the repository with
  `{id, query, language (ko|en), video_sha256, ranges [{start_s, end_s}],
  negative: bool}`; video files are matched by sha256 so the set is
  reproducible without redistributing footage.
- Metrics per index version, as the script computes them:
  - Recall@1/5/10 over the headline set (every question that is not marked
    `ambiguous`; the ambiguous ones are reported separately): a query is a hit
    at K when one of the top K ranges overlaps a ground-truth range of its
    video or of an `also_matches` video by at least 1 s, or covers all of a
    ground-truth range shorter than 1 s.
  - Precision@5/10: overlapping ranges among the top K, divided by K. A query
    returns between one and a dozen ranges on this corpus, so P@10 is bounded
    by a tenth of the ranges returned; the reports print that count.
  - Temporal error: |start - gt start| and |end - gt end| of the highest-ranked
    overlapping range against the ground-truth range it overlaps most, median
    and p90. Percentiles are interpolated between neighbouring values.
  - Latency: embed, scan and total ms, p50 and p95.
  - Index cost: compute seconds per hour of footage, bytes per hour (vectors,
    thumbnails, SQLite rows).
  - Negative queries: top relevance distribution compared with positive
    queries; reported as the fraction of negatives whose top result scores
    above the median positive top score.
  - Failure split: "not retrieved" (no overlapping range among those returned)
    vs "ranked low" (retrieved beyond K).
- Question sets for tuning and for the final report are separate files; the
  final set is not used to pick thresholds or sampling intervals. Both sets
  describe the same footage (only the question texts differ), so the numbers
  compare index versions on this corpus rather than estimate field recall.
  Every report file name carries the split and the candidate fraction it ran
  with, so a tuning run cannot be mistaken for a final one.

## As implemented in the core

Deviations and additions, with the reason for each.

- Restart semantics, extended after review: a restart also counts an attempt on
  the job it requeues (a segment whose decode takes the core down would
  otherwise be picked first at every start, and the job fails at the attempt
  cap), and jobs of segments deleted while they were running are marked
  `skipped (segment_deleted)` at the next start, because nothing else would
  ever pick or skip them.
- A scan streams the rows of its range camera by camera in time order (the
  order the index stores them in, which groups them by vector file) and keeps
  only the candidate heap, so its memory does not grow with the retained
  footage. At most two scans run at once with four more waiting; beyond that a
  query is refused with 503 `search_busy`, because the console re-posts a query
  on every filter change and only the newest answer is painted.
- Counting how much of a range a version covers walks that version's index, so
  the comparison between the active and the previous version runs on a pool
  thread; a query that names no version and has no previous version counts
  nothing. A filter that leaves no footage answers at once, without taking the
  worker's embed lane.
- Records whose vector is not at the offset the database gives (a crash between
  the append and the commit that left stale bytes) are dropped when a scan
  meets them, and their segment is indexed again; the startup check does the
  same for rows past the end of their file. Both requeue the job, so the
  footage does not stay unindexed.
- The files of a version deleted before its removal could run are removed at
  the next start, and a version cannot be deleted while a search is scanning
  (409 `index_version_busy`), because the scan holds a mapping of its files.
- An import that fails removes the fragment files of its session that have no
  segment row, so startup recovery cannot adopt them as footage of an import
  that failed.

- Version resolution. Settings hold version names
  (`search.active_index_version`, default `siglip2-b16-224`, and
  `search.previous_index_version`) plus `index.sample_interval_ms`; hashes are
  never configured. The worker only reports a descriptor with a result, so the
  core sends an `embed_text` probe for the active name (text "index version
  probe") before its first job in each run, stores the reported descriptor in
  `index_versions` and queues the jobs of that hash. A reply whose hash differs
  from the job's (a new model revision in the cache) registers the new version
  for the name, marks the job `skipped (index_version_changed)` and queues the
  re-index; search uses the hash the query embedding reports.
- Sample instants are the multiples of `sample_interval_ms` in UTC inside
  `[segment start, segment end)`, so consecutive segments never sample the same
  instant; each instant takes the frame on screen at that time (the latest
  frame at or before it). `frames_expected` is the number of instants.
  Sampling decodes with `filesrc ! demux ! parse ! avdec ! appsink` on a pool
  thread and encodes only the chosen frames (at most 960 px wide for the
  worker, 320 px wide thumbnails) with `gst_video_convert_sample`.
- Thumbnails are kept per record under
  `<data>/index/<version>/thumbs/<camera>/<segment>/g<generation>-<n>.jpg`
  (about 12 KB each at 320 px). They are removed when the record is deleted,
  and the whole directory of a segment when an attempt ends without `done`.
- Job failures. A worker that is not ready, a timeout and HTTP 503
  `deadline_exceeded` requeue the job without counting an attempt and pause
  indexing with backoff (1 to 60 s); any other failure (decode, contract
  violation, `backend_load_failed`, store) counts an attempt and retries after
  10 s, 30 s, 90 s, ... (at most 10 min), up to 5 attempts. `dry_run` vectors
  are refused. Embed jobs wait until the worker's detector warm-up has ended
  (detector state `ready`, `failed` or `unavailable`): loading an embedder
  while the detector warm-up imports `transformers` in another thread made
  both imports fail in the worker (`cannot import name 'AutoBackbone'`), which
  restarted the worker in a loop. The worker now takes one
  process-wide lock around every model load, and the core also passes no
  `--embed-preload`, so a query never waits behind two model loads.
- Vector files. A search scans on a pool thread with its own read-only SQLite
  connection and memory-maps the files. Compaction (checked every 60 s while no
  scan runs) seals the file, so later appends for that day start a new
  `<yyyymmdd>-<n>.vec`, copies the live records into another new file on a pool
  thread, moves the rows in one transaction and removes the old file once no
  scan is running. Files without live rows are removed the same way. The
  startup check also removes files that no row references (a crash between
  the append and the commit) and deletes the rows of files that are missing or
  unreadable.
- Search candidates. N is also capped at `candidate_fraction` (request field,
  default 0.3) of the samples scanned. With N = limit * 8 alone, a query over
  less footage than N samples takes every sample as a candidate and merges each
  camera into one range covering all of its footage. The default was chosen on
  `eval/search/questions.tune.json` with siglip2-b16-224 (12 unambiguous
  positives, limit 50); the five runs are in `docs/verification/` as
  `search-eval-siglip2-b16-224-tune-cf*`:

  | candidate_fraction | R@1 | R@5 | R@10 | P@5 | Temporal error median s (start / end) | p90 s (start / end) | Not retrieved |
  | --- | --- | --- | --- | --- | --- | --- | --- |
  | 1 (N = limit * 8 only) | 0.833 | 1.000 | 1.000 | 0.217 | 0.717 / 1.081 | 37.930 / 10.250 | 0 |
  | 0.3 | 0.833 | 1.000 | 1.000 | 0.250 | 0.709 / 0.084 | 10.780 / 7.880 | 0 |
  | 0.15 | 0.750 | 0.917 | 0.917 | 0.333 | 0.717 / 0.167 | 5.301 / 11.101 | 1 |
  | 0.1 | 0.750 | 0.833 | 0.833 | 0.317 | 0.717 / 0.167 | 4.020 / 13.701 | 2 |
  | 0.05 | 0.750 | 0.833 | 0.833 | 0.283 | 0.717 / 0.423 | 5.898 / 23.600 | 2 |

  Over hours of footage the cap is far above limit * 8 and changes nothing.
- A range ends at its last sample plus one interval, but never after the end
  of the footage it lies in. The time filter is clamped to the finalized
  footage of the selected cameras. Without `index_version` a query uses the
  active version unless the previous one covers strictly more of the range.
  `GET /v1/search/{id}` checks the stored ranges against deletions again, so a
  deleted segment removes its results from old sessions too.
- Retention and camera deletion also mark queued and failed jobs of the
  deleted segments `skipped (segment_deleted)` in the same transaction; a
  running job notices when its next batch cannot be stored.
- Imports. `POST /v1/imports` checks the path (absolute, a readable `.mp4`,
  `.m4v`, `.mov` or `.mkv`) and the camera at once, probes the file on a pool
  thread and answers 202 with the queued import, or 400 (`unsupported_codec`,
  `unreadable_media`, ...) or 409 `footage_overlap` when the camera already
  has footage in that span. One import runs at a time. The session is anchored
  at `first_pts_ns = 0`, so `start_utc_ms` is the time of media time zero of
  the file. The demuxer's first video track feeds `splitmuxsink` with
  `matroskamux` (set before its video pad is requested). An import that fails
  or is interrupted (stop, crash) deletes the segments it wrote (reason
  `import_failed`). Imports go to cameras of any kind; the evaluation and
  verification cameras are disabled `file` cameras, so no live pipeline runs.

## Done when

- Index survives restart and resumes without duplicates.
- Deleting a segment removes its results immediately.
- Both index versions evaluated on the same question set with the metrics
  above recorded in `docs/verification/`.
- Search tab returns real results and plays the original footage.
