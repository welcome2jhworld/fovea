"""Model output parsers. Every observation must cite frames the model was shown.

Callers pass the job restricted to the frames the model actually received.
Text that cannot be tied to those frames produces no observation; it stays in
Result.raw_text and the drop is recorded in the notes or the parse error.
"""
from __future__ import annotations

import json
import math
import re
from dataclasses import dataclass, field
from typing import Any

from .protocol import Evidence, FrameRef, Job, Observation

THINK_RE = re.compile(r"<think>.*?</think>", re.DOTALL)
EVENT_RE = re.compile(r"^\s*<\s*(\d+(?:\.\d+)?)\s*-\s*(\d+(?:\.\d+)?)\s*>\s*(.+?)\s*$")
UNCERTAINTIES = ("low", "medium", "high")


@dataclass
class ParseOutcome:
    observations: list[Observation] = field(default_factory=list)
    error: str = ""
    notes: list[str] = field(default_factory=list)

    @property
    def status(self) -> str:
        if self.error:
            return "unparsed"
        return "partial" if self.notes else "ok"


class UnknownFrameIds(ValueError):
    def __init__(self, ids: list[str]) -> None:
        super().__init__(f"cites frame ids the model was not shown: {ids}")
        self.ids = ids


def strip_thinking(text: str) -> str:
    """Remove reasoning blocks. A block left open means generation stopped before any answer."""
    text = THINK_RE.sub("", text)
    if "</think>" in text:
        text = text.rsplit("</think>", 1)[1]
    if "<think>" in text:
        return ""
    return text.strip()


def clip_start_ns(job: Job) -> int:
    """Media time of second 0 in the clip file: the clip range start, else the earliest frame."""
    return job.clip.start_pts_ns if job.clip else min(f.pts_ns for f in job.frames)


def frames_in_range(frames: list[FrameRef], start_s: float, end_s: float, base_ns: int = 0) -> list[FrameRef]:
    lo, hi = base_ns + round(start_s * 1e9), base_ns + round(end_s * 1e9)
    return [f for f in frames if lo <= f.pts_ns <= hi]


def _evidence(frames: list[FrameRef]) -> Evidence:
    pts = [f.pts_ns for f in frames]
    return Evidence(frame_ids=[f.frame_id for f in frames], start_pts_ns=min(pts), end_pts_ns=max(pts))


def _seconds(value: Any) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float, str)):
        raise ValueError(f"time {value!r} is not a number")
    seconds = float(value)
    if not math.isfinite(seconds) or seconds < 0:
        raise ValueError(f"time {value!r} is not a non-negative number")
    return seconds


def _json_item(item: Any, frames: list[FrameRef], time_base_ns: int) -> Observation:
    if not isinstance(item, dict):
        raise ValueError("not an object")
    text = item.get("text")
    if not isinstance(text, str) or not text.strip():
        raise ValueError("no text")
    raw_ids = item.get("frame_ids") or []
    labels = item.get("labels") or []
    if not isinstance(raw_ids, list) or not isinstance(labels, list):
        raise ValueError("frame_ids and labels must be lists")
    by_id = {f.frame_id: f for f in frames}
    ids = list(dict.fromkeys(str(i) for i in raw_ids))
    unknown = [i for i in ids if i not in by_id]
    if unknown:
        raise UnknownFrameIds(unknown)
    cited = [by_id[i] for i in ids]
    if not cited and "start_s" in item and "end_s" in item:
        cited = frames_in_range(frames, _seconds(item["start_s"]), _seconds(item["end_s"]), time_base_ns)
    if not cited:
        raise ValueError("cites no shown frame")
    uncertainty = item.get("uncertainty")
    return Observation(
        text=text.strip(),
        evidence=_evidence(cited),
        uncertainty=uncertainty if uncertainty in UNCERTAINTIES else "high",
        labels=[str(x) for x in labels],
    )


def parse_json_observations(text: str, job: Job, time_base_ns: int = 0) -> ParseOutcome:
    """Structured protocol: {"observations": [{"text", "frame_ids" or "start_s"/"end_s", ...}]}.

    start_s/end_s are seconds after time_base_ns. An answer that cites a frame id
    the model was not shown is rejected whole; items that are malformed or cite
    no frame are dropped individually and noted.
    """
    clean = strip_thinking(text)
    if not clean:
        return ParseOutcome(error="empty model output")
    start, end = clean.find("{"), clean.rfind("}")
    try:
        data = json.loads(clean[start : end + 1]) if 0 <= start < end else None
    except json.JSONDecodeError:
        data = None
    if not isinstance(data, dict):
        return ParseOutcome(error="no JSON object in model output")
    items = data.get("observations")
    if not isinstance(items, list):
        return ParseOutcome(error="JSON output has no observations list")
    outcome = ParseOutcome()
    for n, item in enumerate(items):
        try:
            outcome.observations.append(_json_item(item, job.frames, time_base_ns))
        except UnknownFrameIds as e:
            return ParseOutcome(error=f"no localisable observations: observation {n} {e}")
        except (TypeError, ValueError) as e:
            outcome.notes.append(f"observation {n} dropped: {e}")
    if items and not outcome.observations:
        outcome.error = "no localisable observations"
    return outcome


def parse_marlin_caption(text: str, job: Job) -> ParseOutcome:
    """Marlin caption format: 'Scene: <paragraph>' then 'Events:' with '<X.X - Y.Y> text' lines.

    Event times are seconds from the start of the clip file, so they are offset
    by clip_start_ns. The scene line describes the whole clip the model watched.
    """
    clean = strip_thinking(text)
    if not clean:
        return ParseOutcome(error="empty model output")
    if not job.frames:
        return ParseOutcome(error="job has no frames")
    base = clip_start_ns(job)
    outcome = ParseOutcome()
    scene = ""
    for line in clean.splitlines():
        if line.strip().lower().startswith("scene:"):
            scene = line.split(":", 1)[1].strip()
            continue
        m = EVENT_RE.match(line)
        if not m:
            continue
        cited = frames_in_range(job.frames, float(m.group(1)), float(m.group(2)), base)
        if not cited:
            outcome.notes.append(f"event <{m.group(1)} - {m.group(2)}> matches no frame; dropped")
            continue
        outcome.observations.append(Observation(text=m.group(3), evidence=_evidence(cited), uncertainty="medium",
                                                labels=["event"]))
    if scene:
        outcome.observations.insert(0, Observation(text=scene, evidence=_evidence(job.frames), uncertainty="medium",
                                                   labels=["scene"]))
    if not outcome.observations:
        outcome.error = "no localisable observations"
    return outcome
