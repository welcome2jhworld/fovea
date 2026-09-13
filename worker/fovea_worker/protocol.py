"""Job and result contracts between fovea-core and the model worker.

Every frame reference carries the application-issued frame id, its media time
and, when known, the capture UTC time. Results may only cite frame ids and time
ranges that were present in the request; the core rejects anything else.

Detection results (kind "detect_frames") report one DetectionFrame per input
frame. Boxes and anchors are normalized to the frame size (0..1) so the rule
engine can compare them with zone polygons without knowing the pixel size.
"""
from __future__ import annotations

from dataclasses import dataclass, field, asdict
from typing import Any


JOB_KINDS = ("vlm_clip", "embed_frames", "detect_frames", "ping")
FRAME_JOB_KINDS = ("vlm_clip", "embed_frames", "detect_frames")
RESULT_STATUSES = ("ok", "partial", "error", "unparsed", "dry_run")
DETECT_TARGET_CLASSES = ("person", "car", "truck", "bus", "motorcycle", "bicycle")
DETECT_DEFAULT_THRESHOLD = 0.3


@dataclass
class FrameRef:
    frame_id: str
    pts_ns: int
    recv_mono_ns: int
    capture_utc_ms: int
    path: str
    index: int


@dataclass
class ClipRange:
    session_id: str
    start_pts_ns: int
    end_pts_ns: int
    start_utc_ms: int
    end_utc_ms: int


@dataclass
class Gap:
    from_pts_ns: int
    to_pts_ns: int
    reason: str


@dataclass
class Limits:
    max_frames: int = 16
    max_pixels: int = 1280 * 720
    max_new_tokens: int = 256
    deadline_ms: int = 20000


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _positive_int(name: str, value: Any) -> int:
    if isinstance(value, float) and value.is_integer():
        value = int(value)
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(f"{name} must be a positive integer, got {value!r}")
    return value


def _threshold(value: Any) -> float:
    if value is None:
        return DETECT_DEFAULT_THRESHOLD
    if not _is_number(value) or not 0.0 < value <= 1.0:
        raise ValueError(f"threshold must be in (0, 1], got {value!r}")
    return float(value)


def _target_classes(value: Any) -> list[str]:
    if value is None:
        return list(DETECT_TARGET_CLASSES)
    if not isinstance(value, list) or not value or not all(isinstance(c, str) and c for c in value):
        raise ValueError(f"target_classes must be a non-empty list of class names, got {value!r}")
    return list(value)


def sample_frames(frames: list[FrameRef], limit: int) -> list[FrameRef]:
    """At most limit frames spread evenly over the list, first and last included."""
    if len(frames) <= limit:
        return list(frames)
    if limit == 1:
        return [frames[0]]
    return [frames[i * (len(frames) - 1) // (limit - 1)] for i in range(limit)]


@dataclass
class Job:
    job_id: str
    kind: str
    camera_id: str
    generation: int
    frames: list[FrameRef]
    clip: ClipRange | None
    gaps: list[Gap]
    limits: Limits
    prompt: str = ""
    language: str = "ko"
    rule_revision: str = ""
    clip_path: str = ""
    session_id: str = ""
    threshold: float = DETECT_DEFAULT_THRESHOLD
    target_classes: list[str] = field(default_factory=lambda: list(DETECT_TARGET_CLASSES))
    # Worker-side receive time on fovea_worker.clock.mono_ns(); 0 when the job did not come through the server.
    accepted_mono_ns: int = 0

    def deadline_mono_ns(self, start_ns: int) -> int:
        return (self.accepted_mono_ns or start_ns) + self.limits.deadline_ms * 1_000_000

    @staticmethod
    def from_dict(d: dict[str, Any]) -> "Job":
        kind = d.get("kind")
        if kind not in JOB_KINDS:
            raise ValueError(f"unsupported job kind: {kind!r}")
        frames = [FrameRef(**f) for f in d.get("frames", [])]
        ids = [f.frame_id for f in frames]
        if len(set(ids)) != len(ids):
            raise ValueError("duplicate frame ids")
        if kind in FRAME_JOB_KINDS and not frames:
            raise ValueError(f"{kind} job has no frames")
        clip = ClipRange(**d["clip"]) if d.get("clip") else None
        gaps = [Gap(**g) for g in d.get("gaps", [])]
        raw_limits = asdict(Limits(**(d.get("limits") or {})))
        limits = Limits(**{k: _positive_int(f"limits.{k}", v) for k, v in raw_limits.items()})
        camera_id = str(d.get("camera_id", ""))
        session_id = str(d.get("session_id", ""))
        if clip is not None and clip.session_id and session_id and clip.session_id != session_id:
            raise ValueError(f"clip.session_id {clip.session_id!r} differs from session_id {session_id!r}")
        if kind == "detect_frames":
            if not camera_id or not session_id:
                raise ValueError("detect_frames requires camera_id and session_id")
            if len(frames) > limits.max_frames:
                raise ValueError(f"detect_frames job has {len(frames)} frames, limits.max_frames is {limits.max_frames}")
        return Job(
            job_id=str(d["job_id"]),
            kind=kind,
            camera_id=camera_id,
            generation=int(d.get("generation", 0)),
            frames=frames,
            clip=clip,
            gaps=gaps,
            limits=limits,
            prompt=d.get("prompt", ""),
            language=d.get("language", "ko"),
            rule_revision=d.get("rule_revision", ""),
            clip_path=d.get("clip_path", ""),
            session_id=session_id,
            threshold=_threshold(d.get("threshold")),
            target_classes=_target_classes(d.get("target_classes")),
        )


@dataclass
class Evidence:
    frame_ids: list[str]
    start_pts_ns: int
    end_pts_ns: int


@dataclass
class Observation:
    text: str
    evidence: Evidence
    uncertainty: str
    labels: list[str] = field(default_factory=list)


@dataclass
class Detection:
    track_id: str | None
    cls: str
    confidence: float
    bbox: list[float]
    anchor_foot: list[float]
    anchor_center: list[float]


@dataclass
class DetectionFrame:
    frame_id: str
    pts_ns: int
    width: int
    height: int
    detections: list[Detection] = field(default_factory=list)


@dataclass
class Result:
    job_id: str
    generation: int
    status: str
    observations: list[Observation]
    model: str
    model_version: str
    input_frames: int
    processing_ms: int
    error: str = ""
    raw_text: str = ""
    frames: list[DetectionFrame] = field(default_factory=list)
    per_frame_ms: list[int] = field(default_factory=list)
    device: str = ""
    notes: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def _unit(values: list[float]) -> bool:
    return all(_is_number(v) and 0.0 <= float(v) <= 1.0 for v in values)


def citable_frames(job: Job) -> list[FrameRef]:
    """Frames a result may cite. Image VLM jobs show the model only sample_frames(frames, max_frames)."""
    if job.kind == "vlm_clip" and not job.clip_path:
        return sample_frames(job.frames, job.limits.max_frames)
    return job.frames


def _validate_detection_frames(result: Result, job: Job) -> list[str]:
    problems: list[str] = []
    pts_by_id = {f.frame_id: f.pts_ns for f in job.frames}
    target = set(job.target_classes)
    detect = job.kind == "detect_frames"
    if detect and result.status == "ok" and len(result.frames) != len(job.frames):
        problems.append(f"status ok with {len(result.frames)} of {len(job.frames)} frames")
    seen: set[str] = set()
    for i, frame in enumerate(result.frames):
        if frame.frame_id not in pts_by_id:
            problems.append(f"frame {i}: unknown frame id {frame.frame_id!r}")
        else:
            if frame.pts_ns != pts_by_id[frame.frame_id]:
                problems.append(f"frame {i}: pts {frame.pts_ns} differs from job pts {pts_by_id[frame.frame_id]}")
            if frame.frame_id in seen:
                problems.append(f"frame {i}: duplicate frame id {frame.frame_id!r}")
            seen.add(frame.frame_id)
        if not all(isinstance(v, int) and not isinstance(v, bool) and v > 0 for v in (frame.width, frame.height)):
            problems.append(f"frame {i}: invalid size {frame.width}x{frame.height}")
        for j, det in enumerate(frame.detections):
            where = f"frame {i} detection {j}"
            if not det.cls:
                problems.append(f"{where}: empty class")
            elif detect and det.cls not in target:
                problems.append(f"{where}: class {det.cls!r} not in target_classes")
            if not (_is_number(det.confidence) and 0.0 <= det.confidence <= 1.0):
                problems.append(f"{where}: confidence {det.confidence!r} outside [0,1]")
            if len(det.bbox) != 4 or not _unit(det.bbox):
                problems.append(f"{where}: bbox {det.bbox!r} outside [0,1]")
            elif det.bbox[0] > det.bbox[2] or det.bbox[1] > det.bbox[3]:
                problems.append(f"{where}: bbox {det.bbox!r} inverted")
            if len(det.anchor_foot) != 2 or not _unit(det.anchor_foot):
                problems.append(f"{where}: anchor_foot {det.anchor_foot!r} outside [0,1]")
            if len(det.anchor_center) != 2 or not _unit(det.anchor_center):
                problems.append(f"{where}: anchor_center {det.anchor_center!r} outside [0,1]")
            if det.track_id is not None and not isinstance(det.track_id, str):
                problems.append(f"{where}: track_id must be a string or null")
    if result.per_frame_ms and len(result.per_frame_ms) != len(result.frames):
        problems.append("per_frame_ms length differs from frames")
    return problems


def validate_result_against_job(result: Result, job: Job) -> list[str]:
    """Return a list of contract violations. Empty list means the result is acceptable."""
    problems: list[str] = []
    citable = {f.frame_id: f.pts_ns for f in citable_frames(job)}
    lo = min((f.pts_ns for f in job.frames), default=0)
    hi = max((f.pts_ns for f in job.frames), default=0)
    if result.job_id != job.job_id:
        problems.append("job_id mismatch")
    if result.generation != job.generation:
        problems.append("generation mismatch")
    if result.status not in RESULT_STATUSES:
        problems.append(f"unknown status {result.status!r}")
    if not result.model or not result.model_version:
        problems.append("model or model_version missing")
    for i, obs in enumerate(result.observations):
        ev = obs.evidence
        unknown = [fid for fid in ev.frame_ids if fid not in citable]
        if unknown:
            problems.append(f"observation {i}: unknown frame ids {unknown}")
        if not ev.frame_ids:
            problems.append(f"observation {i}: no evidence frames")
        elif not unknown:
            pts = [citable[fid] for fid in ev.frame_ids]
            if (ev.start_pts_ns, ev.end_pts_ns) != (min(pts), max(pts)):
                problems.append(f"observation {i}: evidence range differs from cited frames")
        if ev.start_pts_ns < lo or ev.end_pts_ns > hi:
            problems.append(f"observation {i}: evidence range outside clip")
        if ev.start_pts_ns > ev.end_pts_ns:
            problems.append(f"observation {i}: evidence range inverted")
    problems.extend(_validate_detection_frames(result, job))
    return problems
