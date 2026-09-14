"""Embed results built with the standard library, for tests that must not need numpy or torch."""
import struct

from fovea_worker.embedding import (INDEX_VERSIONS, EmbedResult, build_descriptor, descriptor_hash, encode_vectors,
                                    job_inputs)

REVISION = "0123456789abcdef0123456789abcdef01234567"


def descriptor(name="siglip2-b16-224", sample_interval_ms=1000, revision=REVISION):
    spec = INDEX_VERSIONS[name]
    return build_descriptor(name, spec.model_id, revision, {"image_size": "224x224", "text_max_length": 64},
                            spec.dims, "float32", sample_interval_ms, "query: {text}")


def unit_vectors(count, dims):
    return b"".join(struct.pack(f"<{dims}f", *[1.0 if j == i % dims else 0.0 for j in range(dims)])
                    for i in range(count))


def ok_result(job):
    d = descriptor(job.index_version_name, job.sample_interval_ms)
    count = len(job_inputs(job))
    return EmbedResult(job.job_id, job.generation, "ok", job.index_version_name, d["model_id"],
                       index_version=descriptor_hash(d), descriptor=d, dims=d["dims"], count=count,
                       vectors=encode_vectors(unit_vectors(count, d["dims"])),
                       frame_ids=[f.frame_id for f in job.frames] if job.kind == "embed_frames" else [],
                       per_frame_ms=[1] * count, device="fake")


def frame_dicts(n, prefix="f"):
    return [{"frame_id": f"{prefix}{i}", "pts_ns": i * 1_000_000_000, "recv_mono_ns": 0, "capture_utc_ms": 0,
             "path": f"/spool/{prefix}{i}.jpg", "index": i} for i in range(n)]


def embed_frames_dict(n=2, **overrides):
    d = {"job_id": "e1", "kind": "embed_frames", "generation": 4, "index_version_name": "siglip2-b16-224",
         "frames": frame_dicts(n), "limits": {}}
    d.update(overrides)
    return d


def embed_text_dict(texts=("a white car",), **overrides):
    d = {"job_id": "q1", "kind": "embed_text", "generation": 0, "index_version_name": "siglip2-b16-224",
         "texts": list(texts), "limits": {}}
    d.update(overrides)
    return d
