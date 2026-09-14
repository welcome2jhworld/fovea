from __future__ import annotations

import importlib

VLM_BACKENDS = ("dry", "transformers", "mlx")
DETECTOR_BACKENDS = ("rfdetr", "dry")
EMBED_BACKENDS = ("transformers", "dry", "none")


def load_backend(name: str):
    if name == "dry":
        from .dry import DryBackend
        return DryBackend()
    if name == "transformers":
        mod = importlib.import_module("fovea_worker.backends.transformers_vlm")
        return mod.TransformersVlmBackend()
    if name == "mlx":
        mod = importlib.import_module("fovea_worker.backends.mlx_vlm_backend")
        return mod.MlxVlmBackend()
    raise ValueError(f"unknown backend {name!r}")


def load_detector(name: str):
    if name == "dry":
        from .dry import DryBackend
        return DryBackend()
    if name == "rfdetr":
        mod = importlib.import_module("fovea_worker.backends.detector_rfdetr")
        return mod.RfDetrTracker()
    raise ValueError(f"unknown detector {name!r}")


def load_embed_backend(name: str, preload: tuple[str, ...] = ()):
    """The embed lane backend, or None for "none". Models load per index version on first use or at warmup."""
    if name == "none":
        return None
    from .embed import DryEmbedder, EmbedBackend, create_embedder
    if name == "dry":
        return EmbedBackend(DryEmbedder, preload=preload)
    if name == "transformers":
        return EmbedBackend(create_embedder, preload=preload)
    raise ValueError(f"unknown embed backend {name!r}")
