"""Embedding contract shared by the job protocol, the server and the embedders. Standard library only.

An index version is named by the core (index_version_name) and identified by
the short hash of its descriptor: {name, model_id, model_revision (Hugging Face
snapshot commit), preprocessing, dims, dtype (model compute dtype),
sample_interval_ms, prompt_template}. The hash is the first 12 hex digits of
the SHA-256 of the descriptor's compact JSON with sorted keys, which is the
byte string Qt's QJsonDocument::Compact writes for the same object as long as
the descriptor holds only ASCII strings, integers and booleans (no floats).

Vectors are float32 little-endian, L2-normalised, concatenated in input order
and base64 encoded.
"""
from __future__ import annotations

import base64
import hashlib
import json
import math
import os
import struct
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from .protocol import Job

EMBED_JOB_KINDS = ("embed_frames", "embed_text")
EMBED_MAX_FRAMES = 32
EMBED_MAX_TEXTS = 8
EMBED_MAX_TEXT_CHARS = 2000
PRIORITY_QUERY = "query"
PRIORITY_INDEX = "index"
EMBED_PRIORITIES = (PRIORITY_QUERY, PRIORITY_INDEX)
DEFAULT_SAMPLE_INTERVAL_MS = 1000
EMBED_STATUSES = ("ok", "error", "dry_run")
UNIT_NORM_TOLERANCE = 1e-3
HASH_HEX_DIGITS = 12
DESCRIPTOR_KEYS = ("name", "model_id", "model_revision", "preprocessing", "dims", "dtype", "sample_interval_ms",
                   "prompt_template")


@dataclass(frozen=True)
class IndexSpec:
    name: str
    model_id: str
    dims: int


INDEX_VERSIONS = {
    spec.name: spec
    for spec in (
        IndexSpec("siglip2-b16-224", "google/siglip2-base-patch16-224", 768),
        IndexSpec("qwen3vl-emb-2b-1024", "Qwen/Qwen3-VL-Embedding-2B", 1024),
    )
}


class ModelUnavailable(Exception):
    """The embedder for an index version could not be loaded (weights missing, import or device failure)."""


def default_priority(kind: str) -> str:
    return PRIORITY_QUERY if kind == "embed_text" else PRIORITY_INDEX


def canonical_json(value: dict) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False)


def descriptor_hash(descriptor: dict) -> str:
    return hashlib.sha256(canonical_json(descriptor).encode("ascii")).hexdigest()[:HASH_HEX_DIGITS]


def _portable(value: Any) -> bool:
    """True when value holds only ASCII strings, integers and booleans, which Python and Qt serialise identically."""
    if isinstance(value, dict):
        return all(isinstance(k, str) and k.isascii() and _portable(v) for k, v in value.items())
    if isinstance(value, list):
        return all(_portable(v) for v in value)
    if isinstance(value, str):
        return value.isascii()
    return isinstance(value, int)


def build_descriptor(name: str, model_id: str, model_revision: str, preprocessing: dict, dims: int, dtype: str,
                     sample_interval_ms: int, prompt_template: str) -> dict:
    descriptor = {"name": name, "model_id": model_id, "model_revision": model_revision,
                  "preprocessing": preprocessing, "dims": dims, "dtype": dtype,
                  "sample_interval_ms": sample_interval_ms, "prompt_template": prompt_template}
    if not _portable(descriptor):
        raise ValueError("an index version descriptor holds only ASCII strings, integers and booleans")
    return descriptor


def hf_hub_cache() -> Path:
    if os.environ.get("HF_HUB_CACHE"):
        return Path(os.environ["HF_HUB_CACHE"])
    if os.environ.get("HF_HOME"):
        return Path(os.environ["HF_HOME"]) / "hub"
    return Path.home() / ".cache" / "huggingface" / "hub"


def hf_snapshot(model_id: str, cache: Path | None = None) -> tuple[Path, str]:
    """(snapshot directory, commit hash) of the cached main revision. Raises ModelUnavailable when not cached."""
    repo = (cache or hf_hub_cache()) / ("models--" + model_id.replace("/", "--"))
    try:
        revision = (repo / "refs" / "main").read_text(encoding="ascii").strip()
    except OSError as e:
        raise ModelUnavailable(f"{model_id} is not in the Hugging Face cache ({type(e).__name__})") from e
    snapshot = repo / "snapshots" / revision
    if not (snapshot / "config.json").is_file():
        raise ModelUnavailable(f"{model_id} snapshot {revision} has no config.json")
    return snapshot, revision


def encode_vectors(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def decode_vectors(encoded: str, dims: int) -> list[tuple[float, ...]]:
    """Rows of a base64 float32 little-endian payload. Raises ValueError when it is not a whole number of rows."""
    if dims <= 0:
        raise ValueError(f"dims must be positive, got {dims}")
    raw = base64.b64decode(encoded, validate=True)
    row_bytes = dims * 4
    if len(raw) % row_bytes:
        raise ValueError(f"{len(raw)} vector bytes is not a multiple of {row_bytes} ({dims} float32 dims)")
    return [struct.unpack_from(f"<{dims}f", raw, offset) for offset in range(0, len(raw), row_bytes)]


def vector_problems(rows: list[tuple[float, ...]]) -> list[str]:
    problems = []
    for i, row in enumerate(rows):
        if not all(math.isfinite(v) for v in row):
            problems.append(f"vector {i}: not finite")
            continue
        norm = math.sqrt(math.fsum(v * v for v in row))
        if abs(norm - 1.0) > UNIT_NORM_TOLERANCE:
            problems.append(f"vector {i}: norm {norm:.4f} is not 1")
    return problems


@dataclass
class EmbedResult:
    job_id: str
    generation: int
    status: str
    model: str
    model_version: str
    index_version: str = ""
    descriptor: dict = field(default_factory=dict)
    dims: int = 0
    count: int = 0
    vectors: str = ""
    frame_ids: list[str] = field(default_factory=list)
    per_frame_ms: list[int] = field(default_factory=list)
    processing_ms: int = 0
    load_ms: int = 0
    device: str = ""
    error: str = ""
    notes: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def job_inputs(job: "Job") -> list[str]:
    """What an embed job embeds, in order: frame paths or texts."""
    return [f.path for f in job.frames] if job.kind == "embed_frames" else list(job.texts)


def validate_embed_result(result: EmbedResult, job: "Job") -> list[str]:
    """Contract violations of an embed result against its job. Empty list means acceptable."""
    problems: list[str] = []
    if result.job_id != job.job_id:
        problems.append("job_id mismatch")
    if result.generation != job.generation:
        problems.append("generation mismatch")
    if result.status not in EMBED_STATUSES:
        problems.append(f"unknown status {result.status!r}")
    if not result.model or not result.model_version:
        problems.append("model or model_version missing")
    if result.status == "error":
        if result.vectors:
            problems.append("error result carries vectors")
        return problems
    expected_ids = [f.frame_id for f in job.frames] if job.kind == "embed_frames" else []
    if result.frame_ids != expected_ids:
        problems.append(f"frame_ids {result.frame_ids} differ from the job's {expected_ids}")
    expected_count = len(job_inputs(job))
    if result.count != expected_count:
        problems.append(f"count {result.count} differs from {expected_count} inputs")
    if len(result.per_frame_ms) != expected_count:
        problems.append(f"per_frame_ms has {len(result.per_frame_ms)} entries for {expected_count} inputs")
    spec = INDEX_VERSIONS.get(job.index_version_name)
    d = result.descriptor
    if sorted(d) != sorted(DESCRIPTOR_KEYS):
        problems.append(f"descriptor keys {sorted(d)} differ from {sorted(DESCRIPTOR_KEYS)}")
        return problems
    if d["name"] != job.index_version_name:
        problems.append(f"descriptor name {d['name']!r} differs from {job.index_version_name!r}")
    if spec is not None and (d["model_id"], d["dims"]) != (spec.model_id, spec.dims):
        problems.append(f"descriptor model {d['model_id']} dims {d['dims']} differ from {spec.model_id} {spec.dims}")
    if d["sample_interval_ms"] != job.sample_interval_ms:
        problems.append(f"descriptor sample_interval_ms {d['sample_interval_ms']} differs from the job's "
                        f"{job.sample_interval_ms}")
    if result.dims != d["dims"]:
        problems.append(f"dims {result.dims} differ from descriptor dims {d['dims']}")
    if not _portable(d):
        problems.append("descriptor holds a value other than an ASCII string, integer or boolean")
    elif result.index_version != descriptor_hash(d):
        problems.append(f"index_version {result.index_version!r} is not the descriptor hash {descriptor_hash(d)!r}")
    try:
        rows = decode_vectors(result.vectors, result.dims)
    except ValueError as e:
        problems.append(f"vectors: {e}")
        return problems
    if len(rows) != expected_count:
        problems.append(f"{len(rows)} vectors for {expected_count} inputs")
    problems.extend(vector_problems(rows))
    return problems
