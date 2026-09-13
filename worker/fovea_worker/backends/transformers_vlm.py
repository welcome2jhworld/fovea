"""Reference VLM backend: Hugging Face Transformers, Qwen3-VL family.

Up to limits.max_frames frames, spread evenly over the job, are passed as an
explicit list of images with their timestamps written into the prompt, so the
model sees the same time information the core stores. Only those frames can be
cited. Loading happens once; run() is synchronous and honours
limits.max_new_tokens.
"""
from __future__ import annotations

import os
import time
from dataclasses import replace

from ..parsing import parse_json_observations
from ..protocol import Job, Result, sample_frames

DEFAULT_MODEL = os.environ.get("FOVEA_VLM_MODEL", "Qwen/Qwen3.5-4B")


class TransformersVlmBackend:
    name = "transformers"

    def __init__(self, model_id: str = DEFAULT_MODEL) -> None:
        self.model_id = model_id
        self.version = model_id
        self.model = None
        self.processor = None
        self.device = ""
        self.notes: list[str] = []

    def load(self) -> None:
        import torch
        from transformers import AutoModelForImageTextToText, AutoProcessor

        device = "cuda" if torch.cuda.is_available() else ("mps" if torch.backends.mps.is_available() else "cpu")
        dtype = torch.float16 if device != "cpu" else torch.float32
        self.processor = AutoProcessor.from_pretrained(self.model_id, local_files_only=True)
        self.model = AutoModelForImageTextToText.from_pretrained(
            self.model_id, torch_dtype=dtype, local_files_only=True
        ).to(device)
        self.device = device
        self.notes = ["no cuda or mps device available; running on cpu"] if device == "cpu" else []

    def _build_messages(self, job: Job) -> list[dict]:
        from PIL import Image

        content = []
        for f in job.frames:
            img = Image.open(f.path).convert("RGB")
            content.append({"type": "image", "image": img})
            content.append({"type": "text", "text": f"[frame {f.index} id={f.frame_id} t={f.pts_ns / 1e9:.2f}s]"})
        gaps = "; ".join(f"{g.from_pts_ns / 1e9:.2f}s-{g.to_pts_ns / 1e9:.2f}s ({g.reason})" for g in job.gaps) or "none"
        instruction = (
            f"{job.prompt}\n"
            f"Frames are in order with their times. Receive gaps: {gaps}.\n"
            "Answer as JSON: {\"observations\": [{\"text\": str, \"frame_ids\": [str], "
            "\"start_s\": float, \"end_s\": float, \"uncertainty\": \"low|medium|high\", \"labels\": [str]}]}. "
            "Cite only frame ids that appear above."
        )
        content.append({"type": "text", "text": instruction})
        return [{"role": "user", "content": content}]

    def run(self, job: Job) -> Result:
        import torch

        t0 = time.monotonic()
        shown = replace(job, frames=sample_frames(job.frames, job.limits.max_frames))
        messages = self._build_messages(shown)
        template_kwargs = dict(add_generation_prompt=True, tokenize=True, return_dict=True, return_tensors="pt")
        try:
            inputs = self.processor.apply_chat_template(messages, enable_thinking=False, **template_kwargs)
        except TypeError:
            inputs = self.processor.apply_chat_template(messages, **template_kwargs)
        inputs = inputs.to(self.model.device)
        with torch.inference_mode():
            out = self.model.generate(**inputs, max_new_tokens=job.limits.max_new_tokens, do_sample=False)
        text = self.processor.batch_decode(out[:, inputs["input_ids"].shape[1]:], skip_special_tokens=True)[0]
        outcome = parse_json_observations(text, shown)
        return Result(
            job_id=job.job_id,
            generation=job.generation,
            status=outcome.status,
            observations=outcome.observations,
            model=self.name,
            model_version=self.model_id,
            input_frames=len(shown.frames),
            processing_ms=int((time.monotonic() - t0) * 1000),
            error=outcome.error,
            raw_text=text[:4000],
            device=self.device,
            notes=self.notes + outcome.notes,
        )

