"""Tracker bookkeeping shared by detector backends. Standard library only.

One tracker per camera and stream session, keyed "camera_id:session_id".
Frames are fed strictly in pts order, one at a time, whether they arrive as one
multi-frame job or as a sequence of single-frame jobs: the registry keeps the
last pts of every key across jobs, so both produce the same tracker calls.

A frame whose pts is not newer than the last pts the tracker saw (a replay or a
reused session id), or that follows it by more than both the tracker's
lost-track window and the job's max_gap_ns (the gap the camera's rules bridge),
resets the tracker, so track ids never bridge two timelines. The tracker counts
updates, not time, so a track lost for one update is still matched after any
gap that did not reset it. Every
tracker creation and reset opens a new epoch that is unique within the registry
and prefixed with a per-process token; emitted track ids carry it, so a reset,
an eviction or a worker restart never reuses an id.

The sampling rate is the median of the most recent pts gaps between
consecutive frames of the key, across job boundaries. Gaps that caused a
pts_gap reset are kept as samples: a single outage does not move the median,
while a feed that is slower than the assumed rate stops resetting once enough
of its gaps are known. Until MIN_RATE_SAMPLES gaps exist the tracker runs at
DEFAULT_FPS. Idle trackers are evicted so a worker serving many cameras does
not keep state for streams that stopped.
"""
from __future__ import annotations

import itertools
import secrets
import statistics
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Callable, Protocol

from .protocol import Detection, DetectionFrame, FrameRef

DEFAULT_FPS = 2.0
TRACKER_IDLE_S = 120.0
FPS_CHANGE_TOLERANCE = 0.05
RATE_WINDOW = 5
MIN_RATE_SAMPLES = 3


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

    def set_threshold(self, threshold: float) -> None:
        """Lowest detection confidence the job reports; such a detection may start a track."""
        ...

    def lost_window_ns(self) -> int:
        """Longest pts gap after which a lost track can still be matched at the current rate."""
        ...


@dataclass
class TrackerEntry:
    tracker: Any
    fps: float
    epoch: str
    last_pts_ns: int = -1
    gaps_ns: deque[int] = field(default_factory=lambda: deque(maxlen=RATE_WINDOW))
    last_used: float = 0.0
    resets: int = 0

    def track_ids(self, raw_ids: list[str | None]) -> list[str | None]:
        return [None if t is None else f"{self.epoch}-{t}" for t in raw_ids]

    def sampled_fps(self, next_pts_ns: int) -> float | None:
        """Rate from the recent gaps plus the gap to next_pts_ns; None while too few gaps are known."""
        gaps = list(self.gaps_ns)
        if 0 <= self.last_pts_ns < next_pts_ns:
            gaps.append(next_pts_ns - self.last_pts_ns)
        gaps = gaps[-RATE_WINDOW:]
        if len(gaps) < MIN_RATE_SAMPLES:
            return None
        return 1e9 / statistics.median(gaps)


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

    def acquire(self, key: str) -> tuple[TrackerEntry, str]:
        """Return the entry for key and "created" when it is new, "" when an existing tracker continues."""
        now = self.clock()
        self.evict_idle(now)
        entry = self.entries.get(key)
        if entry is None:
            entry = TrackerEntry(tracker=self.factory(DEFAULT_FPS), fps=DEFAULT_FPS, epoch=self._next_epoch())
            self.entries[key] = entry
            note = "created"
        else:
            note = ""
        entry.last_used = now
        return entry, note

    def advance(self, entry: TrackerEntry, pts_ns: int, max_gap_ns: int = 0) -> str:
        """Prepare the tracker for the frame at pts_ns: update its rate, then reset it if the timeline broke.

        Returns "" or ";"-joined notes: "fps:<old>-><new>", "reset:pts_backwards", "reset:pts_gap".
        """
        notes: list[str] = []
        fps = entry.sampled_fps(pts_ns)
        if fps is not None and abs(fps - entry.fps) > FPS_CHANGE_TOLERANCE * entry.fps:
            entry.tracker.set_fps(fps)
            notes.append(f"fps:{entry.fps:.3g}->{fps:.3g}")
            entry.fps = fps
        reset = self._timeline_break(entry, pts_ns, max_gap_ns)
        if reset:
            entry.tracker.reset()
            entry.resets += 1
            entry.epoch = self._next_epoch()
            notes.append(reset)
        return ";".join(notes)

    @staticmethod
    def _timeline_break(entry: TrackerEntry, pts_ns: int, max_gap_ns: int) -> str:
        if entry.last_pts_ns < 0:
            return ""
        if pts_ns <= entry.last_pts_ns:
            return "reset:pts_backwards"
        if pts_ns - entry.last_pts_ns > max(entry.tracker.lost_window_ns(), max_gap_ns):
            return "reset:pts_gap"
        return ""

    def commit(self, entry: TrackerEntry, pts_ns: int) -> None:
        """Record that the frame at pts_ns went through the tracker."""
        if 0 <= entry.last_pts_ns < pts_ns:
            entry.gaps_ns.append(pts_ns - entry.last_pts_ns)
        entry.last_pts_ns = pts_ns
        entry.last_used = self.clock()


def sort_frames(frames: list[FrameRef]) -> list[FrameRef]:
    return sorted(frames, key=lambda f: (f.pts_ns, f.index))


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
