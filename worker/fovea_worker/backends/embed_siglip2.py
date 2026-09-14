"""google/siglip2-base-patch16-224 image and text towers.

This is the fixed-resolution SigLIP 2 checkpoint (SiglipProcessor): every
image is squashed to 224x224 with bilinear resampling and normalised with mean
and std 0.5; max_num_patches only exists for the NaFlex checkpoints and does
not apply. Per the model card, text is lowercased and tokenised with
padding="max_length", max_length=64, the length the text tower was trained
with, and queries use the card's "this is a photo of {text}." template. Vectors
are the towers' pooled outputs (768 dims) in float32, L2-normalised.
"""
from __future__ import annotations

from typing import Any

from ..embedding import INDEX_VERSIONS, ModelUnavailable, build_descriptor, hf_snapshot
from .embed import l2_normalize, pick_device

TEXT_MAX_LENGTH = 64
QUERY_TEMPLATE = "this is a photo of {text}."


class Siglip2Embedder:
    spec = INDEX_VERSIONS["siglip2-b16-224"]
    lane_batch_size = 16
    dry_run = False
    dtype = "float32"

    def __init__(self, device: str | None = None) -> None:
        self.requested_device = device
        self.device = ""
        self.revision = ""
        self.model: Any = None
        self.processor: Any = None
        self.preprocessing: dict = {}

    def load(self) -> None:
        import torch
        from PIL import Image
        from transformers import AutoModel, AutoProcessor

        snapshot, self.revision = hf_snapshot(self.spec.model_id)
        device = self.requested_device or pick_device()
        processor = AutoProcessor.from_pretrained(snapshot, local_files_only=True)
        image = processor.image_processor
        if not type(image).__name__.startswith("SiglipImageProcessor"):
            raise ModelUnavailable(f"expected SiglipImageProcessor, got {type(image).__name__}")
        self.model = AutoModel.from_pretrained(snapshot, dtype=torch.float32, local_files_only=True).to(device).eval()
        self.processor = processor
        self.device = device
        self.preprocessing = {
            "image_size": f"{image.size['width']}x{image.size['height']}",
            "image_resize": f"squash {Image.Resampling(int(image.resample)).name.lower()}",
            "image_processor": type(image).__name__,
            "image_normalize": f"rescale 1/255 mean {image.image_mean} std {image.image_std}",
            "text": f"lowercase, padding max_length {TEXT_MAX_LENGTH}, truncation",
            "vector": "pooler_output, l2",
        }

    def descriptor(self, sample_interval_ms: int) -> dict:
        return build_descriptor(self.spec.name, self.spec.model_id, self.revision, self.preprocessing, self.spec.dims,
                                self.dtype, sample_interval_ms, "query: " + QUERY_TEMPLATE)

    def _features(self, method: Any, inputs: dict) -> Any:
        import torch

        with torch.inference_mode():
            out = method(**{k: v.to(self.device) for k, v in inputs.items()})
        return l2_normalize(out.pooler_output.float().cpu().numpy())

    def embed_images(self, paths: list[str]) -> Any:
        from PIL import Image

        images = []
        for path in paths:
            with Image.open(path) as image:
                images.append(image.convert("RGB"))
        return self._features(self.model.get_image_features, self.processor(images=images, return_tensors="pt"))

    def embed_text(self, texts: list[str]) -> Any:
        prompts = [QUERY_TEMPLATE.format(text=t.strip()).lower() for t in texts]
        inputs = self.processor(text=prompts, padding="max_length", max_length=TEXT_MAX_LENGTH, truncation=True,
                                return_tensors="pt")
        return self._features(self.model.get_text_features, inputs)
