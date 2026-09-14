"""embed-bench and embed-query: run one index version's embedder directly on a directory of frames.

embed-bench times embed_images over full batches after one warmup batch
(per-frame ms = batch ms / batch size), single-text query embedding, model load
and peak memory. embed-query embeds every image under --against (recursively)
and prints the best-scoring frames for each query, plus the best score per
first-level subdirectory so a directory per video shows which video wins.
Scores are cosine similarities for ranking only.
"""
from __future__ import annotations

import argparse
import contextlib
import importlib
import json
import sys
import time
from pathlib import Path

from .backends.embed import create_embedder
from .embedding import DEFAULT_SAMPLE_INTERVAL_MS, INDEX_VERSIONS, descriptor_hash
from .stats import percentile

IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png"}
BENCH_QUERIES = ("a person walking in a parking lot", "a white car", "사람이 앉아 있는 교실", "빨간 트럭")
QUERY_REPEAT = 5


def image_paths(directory: Path) -> list[Path]:
    return sorted(p for p in directory.rglob("*") if p.suffix.lower() in IMAGE_SUFFIXES and p.is_file())


def peak_rss_mb() -> float | None:
    try:
        import resource
    except ImportError:
        return None
    peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return round(peak / 1e6 if sys.platform == "darwin" else peak * 1024 / 1e6, 1)


def device_memory_mb(device: str) -> float | None:
    import torch

    if device == "mps":
        return round(torch.mps.driver_allocated_memory() / 1e6, 1)
    if device == "cuda":
        return round(torch.cuda.max_memory_allocated() / 1e6, 1)
    return None


def _timed(fn, *args):
    t0 = time.monotonic()
    value = fn(*args)
    return value, (time.monotonic() - t0) * 1000


def _load(name: str):
    t0 = time.monotonic()
    for module in ("torch", "transformers"):
        importlib.import_module(module)
    import_ms = (time.monotonic() - t0) * 1000
    embedder = create_embedder(name)
    _, load_ms = _timed(embedder.load)
    return embedder, round(import_ms), round(load_ms)


def _summary(values: list[float]) -> dict:
    return {"n": len(values), "p50": percentile(values, 50), "p95": percentile(values, 95),
            "mean": round(sum(values) / len(values), 1) if values else None}


def cmd_embed_bench(args: argparse.Namespace) -> int:
    paths = [str(p) for p in image_paths(Path(args.frames_dir))]
    if len(paths) < 2 * args.batch:
        print(f"need at least {2 * args.batch} frames (one warmup batch and one timed batch), found {len(paths)}",
              file=sys.stderr)
        return 2
    with contextlib.redirect_stdout(sys.stderr):
        embedder, import_ms, load_ms = _load(args.index)
        batches = [paths[i:i + args.batch] for i in range(0, len(paths) - args.batch + 1, args.batch)]
        _, first_batch_ms = _timed(embedder.embed_images, batches[0])
        per_frame: list[float] = []
        for _ in range(args.repeat):
            for batch in batches[1:]:
                _, ms = _timed(embedder.embed_images, batch)
                per_frame.extend([round(ms / len(batch), 1)] * len(batch))
        embedder.embed_text([BENCH_QUERIES[0]])
        query_ms = [round(_timed(embedder.embed_text, [q])[1], 1) for _ in range(QUERY_REPEAT) for q in BENCH_QUERIES]
        memory = device_memory_mb(embedder.device)
    descriptor = embedder.descriptor(args.sample_interval_ms)
    dims = descriptor["dims"]
    frame_ms = _summary(per_frame)
    out = {
        "index_version_name": args.index,
        "index_version": descriptor_hash(descriptor),
        "descriptor": descriptor,
        "device": embedder.device,
        "frames_in_dir": len(paths),
        "batch": args.batch,
        "timed_batches": len(batches) - 1,
        "repeat": args.repeat,
        "import_ms": import_ms,
        "load_ms": load_ms,
        "first_batch_ms": round(first_batch_ms),
        "per_frame_ms": frame_ms,
        "query_text_ms": _summary(query_ms),
        "compute_s_per_footage_hour_at_1_sample_per_s": round(frame_ms["mean"] * 3.6, 1),
        "peak_rss_mb": peak_rss_mb(),
        "device_memory_mb": memory,
        "vector_bytes_per_frame": dims * 4,
        "vector_record_bytes_per_frame": 16 + dims * 4,
        "base64_bytes_per_frame": 4 * ((dims * 4 + 2) // 3),
    }
    print(json.dumps(out, ensure_ascii=False, indent=2))
    return 0


def cmd_embed_query(args: argparse.Namespace) -> int:
    import numpy as np

    root = Path(args.against)
    paths = image_paths(root)
    if not paths:
        print("no frames found", file=sys.stderr)
        return 2
    with contextlib.redirect_stdout(sys.stderr):
        embedder, _, load_ms = _load(args.index)
        chunks = []
        t0 = time.monotonic()
        for i in range(0, len(paths), args.batch):
            chunks.append(embedder.embed_images([str(p) for p in paths[i:i + args.batch]]))
        frames_ms = (time.monotonic() - t0) * 1000
        frame_vectors = np.concatenate(chunks)
        query_vectors, query_ms = _timed(embedder.embed_text, args.texts)
    scores = np.asarray(query_vectors) @ frame_vectors.T
    relative = [p.relative_to(root) for p in paths]
    queries = []
    for text, row in zip(args.texts, scores):
        order = np.argsort(-row)
        best_by_dir: dict[str, float] = {}
        for rel, score in zip(relative, row.tolist()):
            key = rel.parts[0] if len(rel.parts) > 1 else "."
            best_by_dir[key] = max(best_by_dir.get(key, -1.0), score)
        queries.append({
            "text": text,
            "top": [{"frame": relative[i].as_posix(), "score": round(float(row[i]), 4)} for i in order[:args.top]],
            "best_by_dir": {k: round(v, 4) for k, v in sorted(best_by_dir.items(), key=lambda kv: -kv[1])},
        })
    descriptor = embedder.descriptor(args.sample_interval_ms)
    out = {"index_version_name": args.index, "index_version": descriptor_hash(descriptor), "device": embedder.device,
           "load_ms": load_ms, "frames": len(paths), "frames_embed_ms": round(frames_ms),
           "queries_embed_ms": round(query_ms), "queries": queries}
    print(json.dumps(out, ensure_ascii=False, indent=2))
    return 0


def add_parsers(sub) -> None:
    b = sub.add_parser("embed-bench", help="time an index version's embedder over a directory of frames")
    b.add_argument("frames_dir")
    b.add_argument("--index", required=True, choices=tuple(INDEX_VERSIONS))
    b.add_argument("--batch", type=int, default=16)
    b.add_argument("--repeat", type=int, default=1, help="timed passes over the frames after the warmup batch")
    b.add_argument("--sample-interval-ms", type=int, default=DEFAULT_SAMPLE_INTERVAL_MS)
    b.set_defaults(func=cmd_embed_bench)
    q = sub.add_parser("embed-query", help="rank the frames under a directory against text queries")
    q.add_argument("texts", nargs="+")
    q.add_argument("--against", required=True, help="directory searched recursively for frames")
    q.add_argument("--index", required=True, choices=tuple(INDEX_VERSIONS))
    q.add_argument("--top", type=int, default=5)
    q.add_argument("--batch", type=int, default=16)
    q.add_argument("--sample-interval-ms", type=int, default=DEFAULT_SAMPLE_INTERVAL_MS)
    q.set_defaults(func=cmd_embed_query)
