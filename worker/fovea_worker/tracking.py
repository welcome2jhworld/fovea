"""Tracker bookkeeping shared by detector backends. Standard library only.

One tracker per camera and stream session, keyed "camera_id:session_id".
Frames are fed strictly in pts order. A frame whose pts is not newer than the
last pts the tracker saw (a replay or a reused session id), or that follows it
by more than the tracker's lost-track window, resets the tracker, so track ids
never bridge two timelines. Every tracker creation and reset opens a new epoch
that is unique within the registry and prefixed with a per-process token;
emitted track ids carry it, so a reset, an eviction or a worker restart never
reuses an id. Idle trackers are evicted so a worker serving many cameras does
not keep state for streams that stopped.
"""
from __future__ import annotations

import itertools
import secrets
import statistics
import time
from dataclasses import dataclass
from typing import Any, Callable, Protocol

from .protocol import Detection, DetectionFrame, FrameRef

DEFAULT_FPS = 2.0
TRACKER_IDLE_S = 120.0
FPS_CHANGE_TOLERANCE = 0.05


@dataclass
class RawDetection:
    """One detector output in pixel coordinates of the source frame."""

    x1: float
    y1: float
    x2: float
    y2: float
    confidence: float
    cls: str


class Tracker(Protocol):
    def update(self, detections: list[RawDetection]) -> list[str | None]: ...

    def reset(self) -> None: ...

    def set_fps(self, fps: float) -> None: ...

    def lost_window_ns(self) -> int:
        """Longest pts gap after which a lost track can still be matched at the current rate."""
        ...


@dataclass
class TrackerEntry:
    tracker: Any
    fps: float
    epoch: str
    last_pts_ns: int = -1
    last_used: float = 0.0
    resets: int = 0

    def track_ids(self, raw_ids: list[str | None]) -> list[str | None]:
        return [None if t is None else f"{self.epoch}-{t}" for t in raw_ids]


class TrackerRegistry:
    def __init__(
        self,
        factory: Callable[[float], Any],
        idle_s: float = TRACKER_IDLE_S,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.factory = factory
        self.idle_s = idle_s
        self.clock = clock
        self.entries: dict[str, TrackerEntry] = {}
        self.instance = secrets.token_hex(3)
        self.epochs = itertools.count(1)

    @staticmethod
    def key(camera_id: str, session_id: str) -> str:
        return f"{camera_id}:{session_id}"

    def _next_epoch(self) -> str:
        return f"{self.instance}.{next(self.epochs)}"

    def evict_idle(self, now: float | None = None) -> list[str]:
        now = self.clock() if now is None else now
        stale = [k for k, e in self.entries.items() if now - e.last_used > self.idle_s]
        for k in stale:
            del self.entries[k]
        return stale

    def acquire(self, key: str, first_pts_ns: int, fps: float) -> tuple[TrackerEntry, str]:
        """Return the entry for key, ready for a job starting at first_pts_ns.

        The note is "" when an existing tracker continues, "created" for a new
        one, a check_continuity reset note, and/or "fps:<old>-><new>" when the
        sampling rate changed and the tracker was told.
        """
        now = self.clock()
        self.evict_idle(now)
        entry = self.entries.get(key)
        if entry is None:
            entry = TrackerEntry(tracker=self.factory(fps), fps=fps, epoch=self._next_epoch(), last_used=now)
            self.entries[key] = entry
            return entry, "created"
        note = self.check_continuity(entry, first_pts_ns)
        if abs(fps - entry.fps) > FPS_CHANGE_TOLERANCE * entry.fps:
            entry.tracker.set_fps(fps)
            note = (note + ";" if note else "") + f"fps:{entry.fps:g}->{fps:g}"
            entry.fps = fps
        entry.last_used = now
        return entry, note

    def check_continuity(self, entry: TrackerEntry, pts_ns: int) -> str:
        """Reset the tracker when pts_ns cannot continue its timeline.

        Returns "reset:pts_backwards", "reset:pts_gap" or "" when it continues.
        """
        if entry.last_pts_ns < 0:
            return ""
        if pts_ns <= entry.last_pts_ns:
            note = "reset:pts_backwards"
        elif pts_ns - entry.last_pts_ns > entry.tracker.lost_window_ns():
            note = "reset:pts_gap"
        else:
            return ""
        entry.tracker.reset()
        entry.resets += 1
        entry.last_pts_ns = -1
        entry.epoch = self._next_epoch()
        return note

    def commit(self, key: str, last_pts_ns: int) -> None:
        entry = self.entries.get(key)
        if entry is not None:
            entry.last_pts_ns = max(entry.last_pts_ns, last_pts_ns)
            entry.last_used = self.clock()


def sort_frames(frames: list[FrameRef]) -> list[FrameRef]:
    return sorted(frames, key=lambda f: (f.pts_ns, f.index))


def derive_fps(frames: list[FrameRef], default: float = DEFAULT_FPS) -> float:
    """Sampling rate from consecutive pts (median gap); default when it cannot be derived."""
    ordered = sort_frames(frames)
    gaps = [b.pts_ns - a.pts_ns for a, b in zip(ordered, ordered[1:]) if b.pts_ns > a.pts_ns]
    if not gaps:
        return default
    return 1e9 / statistics.median(gaps)


def _unit(value: float) -> float:
    return round(min(1.0, max(0.0, value)), 5)


def build_detection_frame(
    frame: FrameRef,
    width: int,
    height: int,
    raws: list[RawDetection],
    track_ids: list[str | None],
) -> DetectionFrame:
    """Normalize pixel boxes to 0..1 and derive the foot and center anchors."""
    detections: list[Detection] = []
    for raw, track_id in zip(raws, track_ids):
        x1, x2 = sorted((_unit(raw.x1 / width), _unit(raw.x2 / width)))
        y1, y2 = sorted((_unit(raw.y1 / height), _unit(raw.y2 / height)))
        cx = _unit((x1 + x2) / 2)
        detections.append(Detection(
            track_id=track_id,
            cls=raw.cls,
            confidence=_unit(raw.confidence),
            bbox=[x1, y1, x2, y2],
            anchor_foot=[cx, y2],
            anchor_center=[cx, _unit((y1 + y2) / 2)],
        ))
    return DetectionFrame(frame_id=frame.frame_id, pts_ns=frame.pts_ns, width=width, height=height, detections=detections)
