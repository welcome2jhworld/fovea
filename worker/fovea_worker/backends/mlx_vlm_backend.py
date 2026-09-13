"""Mac-only adapter using mlx-vlm. Experimental; results are compared against the
Transformers backend before any conclusion is drawn from them.

Two paths: with job.clip_path the clip is passed as a video through the
mlx_vlm CLI (the path documented for Marlin, where the processor writes frame
timestamps into the prompt) and the Scene/Events caption format is parsed;
without a clip up to limits.max_frames frames, spread evenly over the job, are
sent as images with explicit timestamps and only those frames can be cited.
"""
from __future__ import annotations

import os
import subprocess
import sys
import time
from dataclasses import replace

from ..parsing import clip_start_ns, parse_json_observations, parse_marlin_caption
from ..protocol import Job, Result, sample_frames

DEFAULT_MODEL = os.environ.get("FOVEA_MLX_MODEL", "NemoStation/Marlin-2B-MLX-8bit")
STAT_PREFIXES = ("Prompt:", "Generation:", "Peak memory:", "=====", "Files:", "Fetching", "Calling")


class MlxVlmBackend:
    name = "mlx"
    device = "mlx"

    def __init__(self, model_id: str = DEFAULT_MODEL) -> None:
        self.model_id = model_id
        self.version = model_id
        self.model = None
        self.processor = None
        self.config = None
        self.marlin = "marlin" in model_id.lower()

    def load(self) -> None:
        if self.marlin:
            return None
        from mlx_vlm import load
        from mlx_vlm.utils import load_config

        self.model, self.processor = load(self.model_id)
        self.config = load_config(self.model_id)

    def run(self, job: Job) -> Result:
        t0 = time.monotonic()
        if job.clip_path:
            input_frames = min(len(job.frames), job.limits.max_frames)
            text, error = self._run_video_cli(job)
            if self.marlin:
                outcome = parse_marlin_caption(text, job)
            else:
                outcome = parse_json_observations(text, job, time_base_ns=clip_start_ns(job))
        else:
            shown = replace(job, frames=sample_frames(job.frames, job.limits.max_frames))
            input_frames = len(shown.frames)
            text, error = self._run_images(shown)
            outcome = parse_json_observations(text, shown)
        return Result(
            job_id=job.job_id,
            generation=job.generation,
            status="error" if error else outcome.status,
            observations=[] if error else outcome.observations,
            model=self.name,
            model_version=self.model_id,
            input_frames=input_frames,
            processing_ms=int((time.monotonic() - t0) * 1000),
            error=error or outcome.error,
            raw_text=text[:4000],
            device=self.device,
            notes=[] if error else outcome.notes,
        )

    def _run_video_cli(self, job: Job) -> tuple[str, str]:
        fps = 2.0
        if len(job.frames) >= 2:
            span = (job.frames[-1].pts_ns - job.frames[0].pts_ns) / 1e9
            if span > 0:
                fps = round((len(job.frames) - 1) / span, 2)
        prompt = job.prompt if not self.marlin else "Describe the video."
        cmd = [sys.executable, "-m", "mlx_vlm.generate", "--model", self.model_id, "--video", job.clip_path,
               "--fps", str(fps), "--video-max-frames", str(job.limits.max_frames), "--prompt", prompt,
               "--max-tokens", str(job.limits.max_new_tokens), "--temperature", "0"]
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=max(30, job.limits.deadline_ms / 1000))
        except subprocess.TimeoutExpired:
            return "", "mlx_vlm generate timed out"
        if proc.returncode != 0:
            return proc.stdout, f"mlx_vlm generate failed: {proc.stderr.strip()[-500:]}"
        lines = [l for l in proc.stdout.splitlines() if not l.startswith(STAT_PREFIXES)]
        return "\n".join(lines).strip(), ""

    def _run_images(self, job: Job) -> tuple[str, str]:
        from mlx_vlm import generate
        from mlx_vlm.prompt_utils import apply_chat_template

        if self.model is None:
            self.load()
        images = [f.path for f in job.frames]
        times = " ".join(f"[frame {f.index} id={f.frame_id} t={f.pts_ns / 1e9:.2f}s]" for f in job.frames)
        prompt = (
            f"{job.prompt}\n{times}\n"
            "Answer as JSON: {\"observations\": [{\"text\": str, \"frame_ids\": [str], "
            "\"start_s\": float, \"end_s\": float, \"uncertainty\": \"low|medium|high\", \"labels\": [str]}]}."
        )
        try:
            formatted = apply_chat_template(self.processor, self.config, prompt, num_images=len(images), enable_thinking=False)
        except TypeError:
            formatted = apply_chat_template(self.processor, self.config, prompt, num_images=len(images))
        out = generate(self.model, self.processor, formatted, images, max_tokens=job.limits.max_new_tokens, verbose=False)
        return (out.text if hasattr(out, "text") else str(out)), ""
