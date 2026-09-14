"""Qwen/Qwen3-VL-Embedding-2B with Matryoshka truncation to 1024 dims.

Follows scripts/qwen3_vl_embedding.py from the model repository: each input is a
chat with the instruction as the system message and the image or text as the
user message, rendered with the processor's chat template (generation prompt
added), images resized by qwen_vl_utils (multiples of 32 px within
[min_pixels, max_pixels], aspect kept) and passed with do_resize=False, right
padding, and the last non-padding token of the final hidden state as the
embedding. Frames use the model's default instruction; queries use a retrieval
instruction in English, as the card recommends for multilingual queries.

Deviations from the script, both recorded in the descriptor: max_pixels is
262144 (256 visual tokens, 960x540 becomes 672x384) instead of 1843200, which
halves per-frame cost on the development Mac; the pooled vector is cast to
float32, truncated to its first 1024 dims and L2-normalised again (MRL). The
compute dtype is float16 on MPS and CUDA (the card's accelerated example) and
float32 on CPU.
"""
from __future__ import annotations

from typing import Any

from ..embedding import INDEX_VERSIONS, build_descriptor, hf_snapshot
from .embed import pick_device

FRAME_INSTRUCTION = "Represent the user's input."
QUERY_INSTRUCTION = "Retrieve surveillance camera frames that show what the user describes."
MIN_PIXELS = 4096
MAX_PIXELS = 262144
IMAGE_PATCH_SIZE = 16
MAX_LENGTH = 8192


class Qwen3VlEmbedder:
    spec = INDEX_VERSIONS["qwen3vl-emb-2b-1024"]
    lane_batch_size = 2
    dry_run = False

    def __init__(self, device: str | None = None) -> None:
        self.requested_device = device
        self.device = ""
        self.dtype = ""
        self.revision = ""
        self.model: Any = None
        self.processor: Any = None

    def load(self) -> None:
        import torch
        from transformers import AutoProcessor
        from transformers.models.qwen3_vl.modeling_qwen3_vl import Qwen3VLModel

        snapshot, self.revision = hf_snapshot(self.spec.model_id)
        device = self.requested_device or pick_device()
        dtype = torch.float32 if device == "cpu" else torch.float16
        self.processor = AutoProcessor.from_pretrained(snapshot, padding_side="right", local_files_only=True)
        self.model = Qwen3VLModel.from_pretrained(snapshot, dtype=dtype, attn_implementation="sdpa",
                                                  local_files_only=True).to(device).eval()
        self.device = device
        self.dtype = str(dtype).removeprefix("torch.")

    def descriptor(self, sample_interval_ms: int) -> dict:
        preprocessing = {
            "image_resize": f"qwen_vl_utils smart_resize factor {IMAGE_PATCH_SIZE * 2} min_pixels {MIN_PIXELS} "
                            f"max_pixels {MAX_PIXELS}, processor do_resize false",
            "image_processor": type(self.processor.image_processor).__name__,
            "chat": "system instruction, user content, chat template with generation prompt, right padding",
            "max_length": MAX_LENGTH,
            "pooling": "last token",
            "mrl_dims": self.spec.dims,
            "vector": "float32 truncate then l2",
        }
        template = f"frame: system '{FRAME_INSTRUCTION}' user image; query: system '{QUERY_INSTRUCTION}' user text"
        return build_descriptor(self.spec.name, self.spec.model_id, self.revision, preprocessing, self.spec.dims,
                                self.dtype, sample_interval_ms, template)

    def _embed(self, conversations: list[list[dict]]) -> Any:
        import torch
        import torch.nn.functional as F
        from qwen_vl_utils import process_vision_info

        text = self.processor.apply_chat_template(conversations, add_generation_prompt=True, tokenize=False)
        images, _, _ = process_vision_info(conversations, image_patch_size=IMAGE_PATCH_SIZE,
                                           return_video_metadata=True, return_video_kwargs=True)
        inputs = self.processor(text=text, images=images, truncation=True, max_length=MAX_LENGTH, padding=True,
                                do_resize=False, return_tensors="pt")
        inputs = {k: v.to(self.device) for k, v in inputs.items()}
        with torch.inference_mode():
            hidden = self.model(**inputs).last_hidden_state
            mask = inputs["attention_mask"]
            last = mask.shape[1] - mask.flip(dims=[1]).argmax(dim=1) - 1
            pooled = hidden[torch.arange(hidden.shape[0], device=hidden.device), last].float()
            vectors = F.normalize(pooled[:, :self.spec.dims], p=2, dim=-1)
        return vectors.cpu().numpy()

    def embed_images(self, paths: list[str]) -> Any:
        return self._embed([
            [{"role": "system", "content": [{"type": "text", "text": FRAME_INSTRUCTION}]},
             {"role": "user", "content": [{"type": "image", "image": "file://" + path, "min_pixels": MIN_PIXELS,
                                           "max_pixels": MAX_PIXELS}]}]
            for path in paths])

    def embed_text(self, texts: list[str]) -> Any:
        return self._embed([
            [{"role": "system", "content": [{"type": "text", "text": QUERY_INSTRUCTION}]},
             {"role": "user", "content": [{"type": "text", "text": text.strip()}]}]
            for text in texts])
