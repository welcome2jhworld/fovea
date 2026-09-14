"""The embed lane backend: one embedder per index version, loaded on first use or by warmup.

An embedder turns JPEG paths or texts into an (N, dims) float32 array whose
rows are L2-normalised, and describes its index version (embedding.py). The
lane runs a job's inputs in batches of the embedder's lane_batch_size and calls
between_batches() before every batch after the first, which is where the
server lets a waiting query job take the lane. per_frame_ms splits each batch's
compute time evenly over its inputs.
"""
from __future__ import annotations

import hashlib
import importlib
import struct
import threading
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Protocol

from ..embedding import (EMBED_JOB_KINDS, INDEX_VERSIONS, UNIT_NORM_TOLERANCE, EmbedResult, IndexSpec,
                         ModelUnavailable, build_descriptor, descriptor_hash, encode_vectors, job_inputs)
from ..modelload import MODEL_LOAD_LOCK
from ..protocol import Job
from ..redact import redact_text

EMBEDDER_CLASSES = {
    "siglip2-b16-224": ("fovea_worker.backends.embed_siglip2", "Siglip2Embedder"),
    "qwen3vl-emb-2b-1024": ("fovea_worker.backends.embed_qwen3vl", "Qwen3VlEmbedder"),
}
WARMUP_TEXT = "warmup"


class Embedder(Protocol):
    spec: IndexSpec
    device: str
    lane_batch_size: int
    dry_run: bool

    def load(self) -> None: ...

    def embed_images(self, paths: list[str]) -> Any: ...

    def embed_text(self, texts: list[str]) -> Any: ...

    def descriptor(self, sample_interval_ms: int) -> dict: ...


def create_embedder(name: str) -> Embedder:
    if name not in EMBEDDER_CLASSES:
        raise ValueError(f"unknown index version {name!r}; expected one of {sorted(EMBEDDER_CLASSES)}")
    module, cls = EMBEDDER_CLASSES[name]
    return getattr(importlib.import_module(module), cls)()


def pick_device() -> str:
    import torch

    if torch.cuda.is_available():
        return "cuda"
    if torch.backends.mps.is_available():
        return "mps"
    return "cpu"


def normalized_rows(matrix: Any, count: int, dims: int) -> Any:
    """matrix as a float32 (count, dims) array. Raises ValueError unless every row is finite and unit length."""
    import numpy as np

    rows = np.asarray(matrix, dtype=np.float32)
    if rows.shape != (count, dims):
        raise ValueError(f"embedder returned shape {rows.shape}, expected {(count, dims)}")
    if not np.isfinite(rows).all():
        raise ValueError("embedder returned a non-finite vector")
    norms = np.linalg.norm(rows.astype(np.float64), axis=1)
    if count and float(np.abs(norms - 1.0).max()) > UNIT_NORM_TOLERANCE:
        raise ValueError(f"embedder returned vectors with norms {norms.min():.4f}..{norms.max():.4f}")
    return rows


def l2_normalize(matrix: Any) -> Any:
    import numpy as np

    rows = np.asarray(matrix, dtype=np.float64)
    return (rows / np.linalg.norm(rows, axis=1, keepdims=True)).astype(np.float32)


class DryEmbedder:
    """Deterministic pseudo-random unit vectors from input bytes, no model. Results are dry_run, never evidence."""

    dry_run = True
    lane_batch_size = 16

    def __init__(self, name: str) -> None:
        self.spec = INDEX_VERSIONS[name]
        self.device = "none"

    def load(self) -> None:
        return None

    def _vector(self, data: bytes) -> list[float]:
        values: list[float] = []
        block = 0
        while len(values) < self.spec.dims:
            digest = hashlib.sha256(block.to_bytes(4, "little") + data).digest()
            values.extend(v / 2**31 - 1.0 for v in struct.unpack("<8I", digest))
            block += 1
        return values[:self.spec.dims]

    def embed_images(self, paths: list[str]) -> Any:
        return l2_normalize([self._vector(Path(p).read_bytes()) for p in paths])

    def embed_text(self, texts: list[str]) -> Any:
        return l2_normalize([self._vector(t.encode("utf-8")) for t in texts])

    def descriptor(self, sample_interval_ms: int) -> dict:
        return build_descriptor(self.spec.name, self.spec.model_id, "dry", {"input": "raw bytes sha256"},
                                self.spec.dims, "float32", sample_interval_ms, "none")


@dataclass
class VersionStatus:
    state: str = "unloaded"
    load_ms: int | None = None
    load_error: str = ""
    device: str = ""


class EmbedBackend:
    name = "embed"

    def __init__(self, factory: Callable[[str], Embedder] = create_embedder,
                 names: Sequence[str] = tuple(INDEX_VERSIONS), preload: Sequence[str] = ()) -> None:
        unknown = sorted(set(names) - set(INDEX_VERSIONS)) + sorted(set(preload) - set(names))
        if unknown:
            raise ValueError(f"unknown or disabled index versions {unknown}")
        self.version = ",".join(names)
        self.device = ""
        self.names = tuple(names)
        self.preload = tuple(preload)
        self._factory = factory
        self._embedders: dict[str, Embedder] = {}
        self._status = {name: VersionStatus() for name in self.names}
        self._status_lock = threading.Lock()

    def load(self) -> None:
        return None

    def warmup(self) -> None:
        for name in self.preload:
            embedder, _ = self._embedder(name)
            embedder.embed_text([WARMUP_TEXT])

    def versions_health(self) -> dict:
        with self._status_lock:
            return {name: {"model_id": INDEX_VERSIONS[name].model_id, "dims": INDEX_VERSIONS[name].dims,
                           "state": s.state, "load_ms": s.load_ms, "load_error": s.load_error, "device": s.device}
                    for name, s in self._status.items()}

    def _embedder(self, name: str) -> tuple[Embedder, int]:
        """The loaded embedder for name and the ms this call spent loading it. Raises ModelUnavailable."""
        if name not in self._status:
            raise ModelUnavailable(f"index version {name!r} is not enabled in this worker")
        if name in self._embedders:
            return self._embedders[name], 0
        status = self._status[name]
        with self._status_lock:
            status.state = "loading"
        t0 = time.monotonic()
        try:
            # One model loads at a time in this process: two concurrent imports
            # of torch and transformers fail (fovea_worker.modelload).
            with MODEL_LOAD_LOCK:
                embedder = self._factory(name)
                embedder.load()
        except Exception as e:
            message = redact_text(f"{type(e).__name__}: {e}")
            with self._status_lock:
                status.state = "failed"
                status.load_error = message
            raise ModelUnavailable(f"{name}: {message}") from e
        load_ms = int((time.monotonic() - t0) * 1000)
        self._embedders[name] = embedder
        with self._status_lock:
            status.state = "ready"
            status.load_ms = load_ms
            status.load_error = ""
            status.device = embedder.device
        self.device = embedder.device
        return embedder, load_ms

    def run(self, job: Job, between_batches: Callable[[], None] | None = None) -> EmbedResult:
        """Embed the job's inputs. Raises ModelUnavailable when the index version's model cannot be loaded."""
        import numpy as np

        if job.kind not in EMBED_JOB_KINDS:
            raise ValueError(f"{job.kind} is not an embed job")
        t0 = time.monotonic()
        embedder, load_ms = self._embedder(job.index_version_name)
        spec = embedder.spec
        inputs = job_inputs(job)
        embed = embedder.embed_images if job.kind == "embed_frames" else embedder.embed_text
        result = EmbedResult(job.job_id, job.generation, "error", spec.name, spec.model_id, load_ms=load_ms,
                             device=embedder.device)
        try:
            descriptor = embedder.descriptor(job.sample_interval_ms)
            batches = []
            per_frame_ms: list[int] = []
            for start in range(0, len(inputs), embedder.lane_batch_size):
                if start and between_batches is not None:
                    between_batches()
                chunk = inputs[start:start + embedder.lane_batch_size]
                t1 = time.monotonic()
                batches.append(normalized_rows(embed(chunk), len(chunk), spec.dims))
                per_frame_ms.extend([round((time.monotonic() - t1) * 1000 / len(chunk))] * len(chunk))
        except Exception as e:
            result.error = redact_text(f"{type(e).__name__}: {e}")
            result.processing_ms = int((time.monotonic() - t0) * 1000)
            return result
        matrix = np.concatenate(batches)
        result.status = "dry_run" if embedder.dry_run else "ok"
        result.model_version = f"{spec.model_id}@{descriptor['model_revision']}"
        result.descriptor = descriptor
        result.index_version = descriptor_hash(descriptor)
        result.dims = spec.dims
        result.count = len(inputs)
        result.vectors = encode_vectors(matrix.astype("<f4").tobytes())
        result.frame_ids = [f.frame_id for f in job.frames] if job.kind == "embed_frames" else []
        result.per_frame_ms = per_frame_ms
        result.processing_ms = int((time.monotonic() - t0) * 1000)
        return result
