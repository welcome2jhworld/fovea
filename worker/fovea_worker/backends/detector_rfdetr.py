"""RF-DETR detector with ByteTrack track ids.

The model runs once per frame; boxes above the job threshold are filtered to
the job's target classes and pushed through one ByteTrack instance per
camera+session so the rule engine sees stable track ids across jobs. A job
stops feeding frames once limits.deadline_ms has elapsed since the worker
accepted it and reports what it finished as partial. Torch,
rfdetr, supervision and PIL are imported lazily so the module can be imported
and unit-tested without them.
"""
from __future__ import annotations

import os
import threading
import time
import warnings
from typing import Any, Callable

from ..clock import mono_ns
from ..protocol import Job, Result
from ..tracking import RawDetection, TrackerRegistry, build_detection_frame, derive_fps, sort_frames

MODEL_VARIANTS = {"nano": "RFDETRNano", "small": "RFDETRSmall"}
DEFAULT_VARIANT = os.environ.get("FOVEA_DETECTOR_MODEL", "nano")
TRACK_ACTIVATION_THRESHOLD = float(os.environ.get("FOVEA_TRACK_ACTIVATION", "0.25"))
LOST_TRACK_BUFFER = int(os.environ.get("FOVEA_TRACK_LOST_BUFFER", "30"))
MINIMUM_MATCHING_THRESHOLD = float(os.environ.get("FOVEA_TRACK_MATCH_THRESHOLD", "0.8"))


def pick_device() -> str:
    import torch

    if torch.cuda.is_available():
        return "cuda"
    if torch.backends.mps.is_available():
        return "mps"
    return "cpu"


def _iou_matrix(a: Any, b: Any) -> Any:
    """Pairwise IoU of two (n,4) and (m,4) xyxy arrays -> (n,m)."""
    import numpy as np

    lt = np.maximum(a[:, None, :2], b[None, :, :2])
    rb = np.minimum(a[:, None, 2:], b[None, :, 2:])
    wh = np.clip(rb - lt, 0, None)
    inter = wh[..., 0] * wh[..., 1]
    area_a = (a[:, 2] - a[:, 0]) * (a[:, 3] - a[:, 1])
    area_b = (b[:, 2] - b[:, 0]) * (b[:, 3] - b[:, 1])
    union = area_a[:, None] + area_b[None, :] - inter
    return np.where(union > 0, inter / np.where(union > 0, union, 1), 0.0)


class ByteTrackAdapter:
    """supervision.ByteTrack behind the Tracker protocol used by TrackerRegistry.

    Tracks are read back through update_with_tensors and mapped to the input
    rows here. supervision's update_with_detections re-associates by IoU >= 0.5
    against the Kalman posterior box, which at 2 fps drops ids for objects that
    move a lot between frames even though the tracker matched them. A track
    updated this frame carries the matched detection's score, so the mapping
    below uses score equality first (IoU as tie-break) and falls back to IoU
    gated at the tracker's own matching threshold.
    """

    def __init__(self, fps: float) -> None:
        import supervision as sv

        self.fps = fps
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", FutureWarning)
            self.tracker = sv.ByteTrack(
                track_activation_threshold=TRACK_ACTIVATION_THRESHOLD,
                lost_track_buffer=LOST_TRACK_BUFFER,
                minimum_matching_threshold=MINIMUM_MATCHING_THRESHOLD,
                frame_rate=fps,
            )

    def set_fps(self, fps: float) -> None:
        self.fps = fps
        self.tracker.max_time_lost = int(fps / 30.0 * LOST_TRACK_BUFFER)

    def lost_window_ns(self) -> int:
        # ByteTrack drops a lost track after the update in which frames since its last match exceed
        # max_time_lost, so it can still be matched max_time_lost + 1 frames after that match.
        return int((self.tracker.max_time_lost + 1) * 1e9 / self.fps)

    def reset(self) -> None:
        self.tracker.reset()

    def update(self, detections: list[RawDetection]) -> list[str | None]:
        import numpy as np

        n = len(detections)
        tensors = np.array([[d.x1, d.y1, d.x2, d.y2, d.confidence] for d in detections], dtype=np.float32).reshape(n, 5)
        tracks = self.tracker.update_with_tensors(tensors)
        ids: list[str | None] = [None] * n
        if not tracks or n == 0:
            return ids
        track_boxes = np.array([t.tlbr for t in tracks], dtype=np.float32).reshape(len(tracks), 4)
        track_scores = np.array([t.score for t in tracks], dtype=np.float32)
        cost = 1.0 - _iou_matrix(tensors[:, :4], track_boxes)
        cost = np.where(tensors[:, 4:5] == track_scores[None, :], cost - 1.0, cost)
        pairs = sorted((float(cost[d, t]), d, t) for d in range(n) for t in range(len(tracks)))
        used_d: set[int] = set()
        used_t: set[int] = set()
        for c, d, t in pairs:
            if c > MINIMUM_MATCHING_THRESHOLD:
                break
            if d in used_d or t in used_t:
                continue
            used_d.add(d)
            used_t.add(t)
            ids[d] = str(int(tracks[t].external_track_id))
        return ids


class RfDetrTracker:
    name = "rfdetr"

    def __init__(self, variant: str = DEFAULT_VARIANT, device: str | None = None,
                 registry: TrackerRegistry | None = None, clock: Callable[[], int] = mono_ns) -> None:
        if variant not in MODEL_VARIANTS:
            raise ValueError(f"unknown detector variant {variant!r}; expected one of {sorted(MODEL_VARIANTS)}")
        self.variant = variant
        self.version = f"rf-detr-{variant}"
        self.requested_device = device
        self.device = ""
        self.model: Any = None
        self.class_names: dict[int, str] = {}
        self.registry = registry or TrackerRegistry(ByteTrackAdapter)
        self.notes: list[str] = []
        self.lock = threading.Lock()
        self.clock = clock

    def load(self) -> None:
        from rfdetr.assets.coco_classes import COCO_CLASSES

        self.class_names = dict(COCO_CLASSES)
        self._build(self.requested_device or pick_device())

    def _build(self, device: str) -> None:
        import rfdetr

        model_cls = getattr(rfdetr, MODEL_VARIANTS[self.variant])
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", FutureWarning)
            self.model = model_cls(device=device)
        self.device = device

    def _load_image(self, path: str) -> tuple[Any, int, int]:
        from PIL import Image

        image = Image.open(path).convert("RGB")
        return image, image.width, image.height

    def _predict(self, image: Any, threshold: float) -> list[RawDetection]:
        import torch

        with torch.inference_mode():
            det = self.model.predict(image, threshold=threshold, include_source_image=False)
        raws: list[RawDetection] = []
        for (x1, y1, x2, y2), conf, class_id in zip(det.xyxy.tolist(), det.confidence.tolist(), det.class_id.tolist()):
            raws.append(RawDetection(x1, y1, x2, y2, float(conf), self.class_names.get(int(class_id), "")))
        return raws

    def _predict_with_fallback(self, image: Any, threshold: float) -> list[RawDetection]:
        try:
            return self._predict(image, threshold)
        except (RuntimeError, NotImplementedError) as e:
            if self.device != "mps":
                raise
            self.notes.append(f"mps inference failed, fell back to cpu: {type(e).__name__}: {str(e)[:200]}")
            self._build("cpu")
            return self._predict(image, threshold)

    def run(self, job: Job) -> Result:
        with self.lock:
            return self._run(job)

    def _run(self, job: Job) -> Result:
        t0 = time.monotonic()
        deadline_ns = job.deadline_mono_ns(self.clock())
        if self.model is None:
            self.load()
        target = set(job.target_classes)
        unknown = sorted(target - set(self.class_names.values()))
        if unknown:
            return Result(job.job_id, job.generation, "error", [], self.name, self.version, len(job.frames),
                          int((time.monotonic() - t0) * 1000), error=f"unknown target classes {unknown}",
                          device=self.device, notes=list(self.notes))
        frames = sort_frames(job.frames)
        key = TrackerRegistry.key(job.camera_id, job.session_id)
        notes: list[str] = []
        errors: list[str] = []
        out_frames = []
        per_frame_ms: list[int] = []
        entry, tracker_note = self.registry.acquire(key, frames[0].pts_ns, derive_fps(frames))
        if tracker_note:
            notes.append(f"tracker {key}: {tracker_note}")
        for i, frame in enumerate(frames):
            if self.clock() >= deadline_ns:
                errors.append(f"deadline {job.limits.deadline_ms} ms exceeded after {i} of {len(frames)} frames")
                break
            t1 = time.monotonic()
            try:
                image, width, height = self._load_image(frame.path)
            except OSError as e:
                errors.append(f"{frame.frame_id}: {e}")
                continue
            reset_note = self.registry.check_continuity(entry, frame.pts_ns)
            if reset_note:
                notes.append(f"tracker {key}: {reset_note} at {frame.frame_id}")
            raws = [r for r in self._predict_with_fallback(image, job.threshold) if r.cls in target]
            track_ids = entry.track_ids(entry.tracker.update(raws))
            out_frames.append(build_detection_frame(frame, width, height, raws, track_ids))
            per_frame_ms.append(int((time.monotonic() - t1) * 1000))
            self.registry.commit(key, frame.pts_ns)
        status = "ok" if not errors else ("partial" if out_frames else "error")
        return Result(
            job_id=job.job_id,
            generation=job.generation,
            status=status,
            observations=[],
            model=self.name,
            model_version=self.version,
            input_frames=len(job.frames),
            processing_ms=int((time.monotonic() - t0) * 1000),
            error="; ".join(errors),
            frames=out_frames,
            per_frame_ms=per_frame_ms,
            device=self.device,
            notes=self.notes + notes,
        )
