from __future__ import annotations

import importlib

VLM_BACKENDS = ("dry", "transformers", "mlx")
DETECTOR_BACKENDS = ("rfdetr", "dry")


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
