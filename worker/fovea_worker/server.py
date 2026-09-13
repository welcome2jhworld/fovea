"""Loopback JSON API for fovea-core. Standard library only.

POST /v1/jobs      run one job synchronously; kind detect_frames goes to the
                   detector lane, vlm_clip to the VLM lane. Each lane
                   serializes its own backend, so a detection job does not wait
                   behind a VLM clip and both models stay resident. A job that
                   cannot start within limits.deadline_ms gets 503
                   deadline_exceeded. ping is answered without entering a lane;
                   embed_frames is 501 until a backend implements it.
GET  /v1/health    per lane (VLM keys unprefixed, detector keys prefixed
                   "detector_"): backend name, model, state
                   (unloaded|loading|ready|failed|unavailable), loaded, device,
                   load_ms, load_error, busy_ms (how long the lane has been
                   running its current job past any model load, 0 when idle),
                   and turnaround_ms {jobs, p50, p95} over the last
                   TURNAROUND_WINDOW jobs. Turnaround runs from the worker
                   accepting the request to the result, lane wait included; a
                   job that had to load the model is excluded.
"""
from __future__ import annotations

import contextlib
import json
import secrets
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .clock import mono_ns
from .protocol import Job, Result, validate_result_against_job
from .redact import redact_text
from .stats import percentile

VLM_LANE = "vlm"
DETECT_LANE = "detect"
TURNAROUND_WINDOW = 200


def _error(code: str, message: str) -> dict:
    return {"error": {"code": code, "message": message}}


def _log(message: str) -> None:
    print(f"fovea-worker: {redact_text(message)}", file=sys.stderr, flush=True)


class JobRefused(Exception):
    def __init__(self, status: int, code: str, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.code = code


@dataclass
class LaneStatus:
    state: str = "unloaded"
    load_ms: int | None = None
    load_error: str = ""
    busy_since_ns: int = 0
    turnaround_ns: deque[int] = field(default_factory=lambda: deque(maxlen=TURNAROUND_WINDOW))


class WorkerState:
    def __init__(self, backend, token: str, detector=None) -> None:
        self.backends = {VLM_LANE: backend, DETECT_LANE: detector}
        self.token = token
        self.locks = {VLM_LANE: threading.Lock(), DETECT_LANE: threading.Lock()}
        self.status = {VLM_LANE: LaneStatus(), DETECT_LANE: LaneStatus()}
        self.status_lock = threading.Lock()
        self.idle = threading.Condition()
        self.in_flight = 0

    @property
    def backend(self):
        return self.backends[VLM_LANE]

    @property
    def detector(self):
        return self.backends[DETECT_LANE]

    @staticmethod
    def lane_for(kind: str) -> str:
        return DETECT_LANE if kind == "detect_frames" else VLM_LANE

    def _lane_health(self, lane: str) -> dict:
        backend = self.backends[lane]
        status = self.status[lane]
        samples = [ns // 1_000_000 for ns in status.turnaround_ns]
        return {
            "model": backend.version if backend is not None else None,
            "loaded": status.state == "ready",
            "state": status.state if backend is not None else "unavailable",
            "device": getattr(backend, "device", ""),
            "load_ms": status.load_ms,
            "load_error": status.load_error,
            "busy_ms": (mono_ns() - status.busy_since_ns) // 1_000_000 if status.busy_since_ns else 0,
            "turnaround_ms": {"jobs": len(samples), "p50": percentile(samples, 50), "p95": percentile(samples, 95)},
        }

    def health(self) -> dict:
        with self.status_lock:
            vlm = self._lane_health(VLM_LANE)
            detect = self._lane_health(DETECT_LANE)
        detector = self.detector
        body = {"backend": self.backend.name, **vlm, "detector": detector.name if detector is not None else None}
        body.update({f"detector_{k}": v for k, v in detect.items()})
        return body

    def _ensure_loaded(self, lane: str, warm: bool) -> bool:
        """Load the lane's backend unless it is ready. The caller holds the lane lock. True when this call loaded it."""
        status = self.status[lane]
        if status.state == "ready":
            return False
        backend = self.backends[lane]
        with self.status_lock:
            status.state = "loading"
        t0 = time.monotonic()
        try:
            backend.load()
            warmup = getattr(backend, "warmup", None)
            if warm and warmup is not None:
                warmup()
        except Exception as e:
            message = redact_text(f"{type(e).__name__}: {e}")
            _log(f"{backend.name} load failed: {message}")
            with self.status_lock:
                status.state = "failed"
                status.load_error = message
            raise JobRefused(503, "backend_load_failed", message) from e
        with self.status_lock:
            status.state = "ready"
            status.load_ms = int((time.monotonic() - t0) * 1000)
            status.load_error = ""
        return True

    def warm_up(self, lane: str) -> bool:
        """Load the lane's backend and run its warmup inference now. False when it failed (see health)."""
        if self.backends[lane] is None:
            return False
        with self._busy(), self.locks[lane]:
            try:
                self._ensure_loaded(lane, warm=True)
            except JobRefused:
                return False
        return True

    @contextlib.contextmanager
    def _busy(self):
        with self.idle:
            self.in_flight += 1
        try:
            yield
        finally:
            with self.idle:
                self.in_flight -= 1
                self.idle.notify_all()

    def wait_idle(self, timeout_s: float) -> bool:
        """Wait until no job or warmup is inside a lane. False when one is still running after timeout_s."""
        with self.idle:
            return self.idle.wait_for(lambda: self.in_flight == 0, timeout_s)

    def run(self, job: Job) -> Result:
        """Run job in its lane. Raises JobRefused when no result can be produced."""
        if job.kind == "ping":
            return Result(job.job_id, job.generation, "ok", [], self.backend.name, self.backend.version, 0, 0)
        if job.kind == "embed_frames":
            raise JobRefused(501, "not_implemented", "no backend implements embed_frames")
        lane = self.lane_for(job.kind)
        if self.backends[lane] is None:
            raise JobRefused(503, "backend_unavailable", f"no backend for {job.kind}")
        with self._busy():
            return self._run_in_lane(lane, job)

    def _run_in_lane(self, lane: str, job: Job) -> Result:
        backend = self.backends[lane]
        now = mono_ns()
        accepted_ns = job.accepted_mono_ns or now
        wait_s = max(0.0, (job.deadline_mono_ns(now) - now) / 1e9)
        if not self.locks[lane].acquire(timeout=wait_s):
            raise JobRefused(503, "deadline_exceeded", f"{lane} lane busy past the {job.limits.deadline_ms} ms deadline")
        try:
            loaded_now = self._ensure_loaded(lane, warm=False)
            t0 = time.monotonic()
            with self.status_lock:
                self.status[lane].busy_since_ns = mono_ns()
            try:
                result = backend.run(job)
            except Exception as e:
                message = redact_text(f"{type(e).__name__}: {e}")
                _log(f"job {job.job_id} ({job.kind}) failed in {backend.name}: {message}")
                result = Result(job.job_id, job.generation, "error", [], backend.name, backend.version,
                                len(job.frames), int((time.monotonic() - t0) * 1000), error=message)
        finally:
            with self.status_lock:
                self.status[lane].busy_since_ns = 0
            self.locks[lane].release()
        if not loaded_now:
            with self.status_lock:
                self.status[lane].turnaround_ns.append(mono_ns() - accepted_ns)
        return result


def make_handler(state: WorkerState):
    class Handler(BaseHTTPRequestHandler):
        def _authorized(self) -> bool:
            auth = self.headers.get("Authorization", "")
            return auth.startswith("Bearer ") and secrets.compare_digest(auth[7:], state.token)

        def _send(self, code: int, body: dict) -> None:
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, fmt, *args):
            return None

        def do_GET(self):
            if not self._authorized():
                return self._send(401, _error("unauthorized", "bad token"))
            if self.path == "/v1/health":
                return self._send(200, state.health())
            return self._send(404, _error("not_found", self.path))

        def do_POST(self):
            accepted_ns = mono_ns()
            if not self._authorized():
                return self._send(401, _error("unauthorized", "bad token"))
            if self.path != "/v1/jobs":
                return self._send(404, _error("not_found", self.path))
            try:
                length = int(self.headers.get("Content-Length", "0"))
                job = Job.from_dict(json.loads(self.rfile.read(length) or b"{}"))
            except (ValueError, KeyError, TypeError, AttributeError) as e:
                return self._send(400, _error("bad_request", str(e)))
            job.accepted_mono_ns = accepted_ns
            try:
                result = state.run(job)
            except JobRefused as e:
                return self._send(e.status, _error(e.code, str(e)))
            body = result.to_dict()
            body["contract_violations"] = validate_result_against_job(result, job)
            return self._send(200, body)

    return Handler


class WorkerServer(ThreadingHTTPServer):
    def __init__(self, address: tuple[str, int], state: WorkerState) -> None:
        self.state = state
        super().__init__(address, make_handler(state))


def serve(backend, host: str, port: int, token: str, detector=None) -> WorkerServer:
    """Bind the loopback server; the socket is listening when this returns."""
    return WorkerServer((host, port), WorkerState(backend, token, detector))
